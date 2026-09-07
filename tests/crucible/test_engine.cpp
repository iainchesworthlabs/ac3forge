#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "engine.hpp"
#include "fake_devices.hpp"
#include "fake_services.hpp"
#include "output_policy.hpp"
#include "session_monitor.hpp"
#include "slots.hpp"

// The engine's frame loop over the three seams EngineConfig injects
// (apps/crucible/engine/engine.hpp): it starts, it brings the output stage
// up on the endpoint the policy chose, it taps every application the
// session monitor lists, and it stops.
//
// The stop case is the one with a bug behind it. The probe runs
// `output->enumerate()` on its own thread and was never joined, so a stop
// that landed while an enumeration was in flight destroyed the OutputStage
// out from under the thread inside it (docs/crucible/promotion.md, "the
// probe thread outlived what it was enumerating"). `loop()` now joins the
// probe before it stops the watcher or the output, which makes the ordering
// observable from here: with an enumeration parked, `stop()` must not
// return until it is let go. The use-after-free itself only fires under a
// sanitizer, so the CI leg that would report it as a memory error is
// config-linux-llvm-asan-ubsan (CMakePresets.json's sanitize-asan-ubsan
// base); what this file pins on every leg is the wait, which is the fix's
// whole shape.
//
// The three cases after it hold a second rule of the same loop: a tap is
// opened only while the output stage has an endpoint to play to. See
// Impl::sync_taps() in apps/crucible/engine/engine.cpp for why that matters
// on macOS, where the Core Audio process tap mutes the application it taps,
// and why it changes nothing audible on Windows or Linux, where a tap is a
// pure capture.

using namespace ac3::crucible;
using namespace ac3::crucible::testing;

namespace {

using Clock = std::chrono::steady_clock;

// Polls `predicate` until it holds. Everything here is driven by a frame
// loop on another thread, so a case waits for what it asked for rather than
// sleeping a guessed interval; the deadline only exists so a regression
// fails the case instead of hanging the suite.
template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
    const auto deadline = Clock::now() + limit;
    while (Clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// The three seams, built together because every case needs all three and
// only ever scripts one of them. Two endpoints and no null sink, which is
// the machine every case here starts from.
struct Rig {
    std::shared_ptr<FakeDevices> devices = std::make_shared<FakeDevices>();
    std::shared_ptr<FakeSessionMonitor> sessions = std::make_shared<FakeSessionMonitor>();
    std::shared_ptr<FakeForeground> foreground = std::make_shared<FakeForeground>();

    Rig() { devices->devices = {realtek_default(), hdmi_avr()}; }

    [[nodiscard]] EngineConfig config() const {
        EngineConfig c;
        c.devices = devices;
        c.sessions = sessions;
        c.foreground = foreground;
        return c;
    }
};

// Field by field rather than a designated initialiser: AppSession has
// eleven members and clang's -Wmissing-designated-field-initializers is an
// error here, which is the same shape tests/crucible/test_platform_seams.cpp
// already uses.
AppSession playing(AppId app, std::string name) {
    AppSession session;
    session.app = app;
    session.name = std::move(name);
    session.active = true;
    session.has_window = true;
    return session;
}

// The applications a status names, in a fixed order: the engine's own list
// follows the slot plan, which is not an order a case should depend on.
std::vector<std::string> names_of(const std::vector<AppStatus>& apps) {
    std::vector<std::string> names;
    names.reserve(apps.size());
    for (const auto& app : apps) {
        names.push_back(app.name);
    }
    std::ranges::sort(names);
    return names;
}


// wait_for, for a change to the fake device list: it reaches the engine only
// through a probe. The request is repeated rather than made once because
// reprobe() is absorbed when an enumeration is already in flight, and the
// device watcher on a real CI machine can start one at any moment.
template <typename Predicate>
bool eventually_after_reprobe(Engine& engine, Predicate predicate,
                              std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
    const auto deadline = Clock::now() + limit;
    while (!predicate()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        engine.reprobe();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return true;
}

struct TapCensus {
    std::size_t created = 0;  // taps the engine asked the machine for, ever
    std::size_t live = 0;     // started and not since stopped
};

TapCensus census(FakeDevices& devices) {
    const std::lock_guard<std::mutex> lock(devices.mutex);
    TapCensus counted;
    counted.created = devices.taps.size();
    for (const auto& tap : devices.taps) {
        const std::lock_guard<std::mutex> tap_lock(tap->mutex);
        if (tap->started && !tap->stopped) {
            ++counted.live;
        }
    }
    return counted;
}

void set_endpoints(FakeDevices& devices, std::vector<DeviceFacts> endpoints) {
    const std::lock_guard<std::mutex> lock(devices.mutex);
    devices.devices = std::move(endpoints);
}

// The engine has taken a session list and applied its first probe. The reason
// is the signal for the second: the output stage leaves it empty until an
// apply() has run, whatever that apply decided.
bool listed_and_probed(const Engine& engine) {
    const auto status = engine.status();
    return status.apps.size() == 2 && !status.output_reason.empty();
}

// Lets `frames` more frames go by, so that a thing asserted absent has had
// every chance to happen.
void run_on(const Engine& engine, std::uint64_t frames) {
    const auto until = engine.status().frames_encoded + frames;
    REQUIRE(wait_for([&] { return engine.status().frames_encoded >= until; }));
}

// The two applications every tap case below plays.
void two_playing(Rig& rig) {
    rig.sessions->set_apps({playing(1001U, "player"), playing(1002U, "browser")});
}
}  // namespace

TEST_CASE("crucible engine: start brings the output up on the endpoint the policy chose", "[crucible][engine]") {
    Rig rig;
    Engine engine(rig.config());

    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&engine] { return engine.status().mode != OutputMode::kNone; }));

    const auto status = engine.status();
    CHECK(status.running);
    // No signing key, so the AVR takes DD+ 5.1 rather than Atmos - the same
    // choice tests/crucible/test_output_stage.cpp makes over the stage alone.
    CHECK(status.mode == OutputMode::kDdPlus51);
    CHECK(status.endpoint_name == "AVR (HDMI)");
    CHECK_FALSE(status.objects_enabled);

    std::shared_ptr<SinkRecord> sink;
    {
        const std::lock_guard<std::mutex> lock(rig.devices->mutex);
        REQUIRE(rig.devices->burst_sinks.size() == 1);
        sink = rig.devices->burst_sinks.front();
    }
    {
        const std::lock_guard<std::mutex> lock(sink->mutex);
        CHECK(sink->started);
        CHECK(sink->device_id == "avr");
        CHECK(sink->eac3);
    }

    engine.stop();

    CHECK_FALSE(engine.status().running);
    const std::lock_guard<std::mutex> lock(sink->mutex);
    CHECK(sink->stopped);
}

TEST_CASE("crucible engine: a second start is refused and stop is idempotent", "[crucible][engine]") {
    Rig rig;
    Engine engine(rig.config());

    REQUIRE(engine.start().has_value());
    const auto again = engine.start();
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == "already running");

    engine.stop();
    engine.stop();  // the second is a no-op, not a second join
    CHECK_FALSE(engine.status().running);
}

TEST_CASE("crucible engine: every listed application with a session gets a tap", "[crucible][engine]") {
    Rig rig;
    rig.sessions->set_apps({playing(1234U, "chrome"), playing(5678U, "vlc")});
    Engine engine(rig.config());

    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&engine] {
        const auto s = engine.status();
        return s.apps.size() == 2 &&
               std::ranges::all_of(s.apps, [](const AppStatus& a) { return a.tapped; });
    }));

    const auto status = engine.status();
    CHECK(status.tap_channels == 2);  // no null sink in the list: the default width
    CHECK(names_of(status.apps) == std::vector<std::string>{"chrome", "vlc"});

    engine.stop();

    const std::lock_guard<std::mutex> lock(rig.devices->mutex);
    REQUIRE(rig.devices->taps.size() == 2);
    std::vector<std::uint32_t> tapped;
    for (const auto& tap : rig.devices->taps) {
        const std::lock_guard<std::mutex> tap_lock(tap->mutex);
        CHECK(tap->started);
        CHECK(tap->channels == 2);
        CHECK(tap->stopped);
        tapped.push_back(tap->process_id);
    }
    std::ranges::sort(tapped);
    CHECK(tapped == std::vector<std::uint32_t>{1234U, 5678U});
}

TEST_CASE("crucible engine: stop waits for an enumeration still in flight", "[crucible][engine]") {
    Rig rig;
    auto& devices = *rig.devices;
    Engine engine(rig.config());

    REQUIRE(engine.start().has_value());
    // The engine probes once of its own accord on the first frame. Let that
    // one through, so what this case holds up is a probe it asked for.
    REQUIRE(wait_for([&devices] { return devices.enumerations_finished() >= 1; }));

    devices.hold_enumerations();
    // Asked for on every pass rather than once: the engine absorbs a request
    // that arrives while a probe is still in flight (one enumeration is as
    // fresh as any that could follow it), and the first probe's thread is
    // still winding up for a moment after its enumeration answered. Asking
    // until one parks is what makes this a fact rather than a race.
    const bool parked = wait_for([&engine, &devices] {
        engine.reprobe();
        return devices.enumerations_parked() >= 1;
    });
    if (!parked) {
        // Nothing is holding the gate, but leaving it shut would hang the
        // engine's destructor on the join this case exists to check.
        devices.release_enumerations();
    }
    REQUIRE(parked);

    // From here the enumeration answers only when this thread says so, and
    // it says so `kHold` after asking the engine to stop. If stop() returns
    // before that, it returned while a thread was still inside
    // output->enumerate() - which is the shutdown that destroyed the
    // OutputStage under it.
    constexpr auto kHold = std::chrono::milliseconds(300);
    const auto asked = Clock::now();
    Clock::time_point released{};
    std::thread releaser([&devices, &released, asked, kHold] {
        std::this_thread::sleep_until(asked + kHold);
        released = Clock::now();
        devices.release_enumerations();
    });

    engine.stop();
    const auto returned = Clock::now();
    releaser.join();

    CHECK(returned >= released);
    CHECK(returned - asked >= kHold);
    // And the probe that was parked did finish, rather than being abandoned:
    // the one from the first frame, plus this one.
    CHECK(devices.enumerations_finished() >= 2);
    CHECK_FALSE(engine.status().running);
}


TEST_CASE("crucible engine: no application is tapped while the output stage has no endpoint",
          "[crucible][engine]") {
    // A machine with no render endpoint at all: the policy answers kNone, so
    // there is nowhere for tapped audio to go.
    Rig rig;
    rig.devices->devices = {};  // set before start(), so nothing else is running yet
    two_playing(rig);
    Engine engine(rig.config());
    // start() refuses on exactly this machine, and says why: nothing here can
    // carry the stream (test_engine_start.cpp). That is incidental to the rule
    // below rather than a different subject - the frame loop runs on after a
    // refusal, deliberately, so what it does about taps is still observable.
    const auto started = engine.start();
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().find("no render endpoint can carry any mode") != std::string::npos);
    REQUIRE(wait_for([&engine] { return listed_and_probed(engine); }));
    REQUIRE(engine.status().mode == OutputMode::kNone);

    // The session list is taken earlier in the frame than a probe is applied,
    // and the first probe is only requested at construction, so this is the
    // ordering the rule exists for: without it the very first frame would have
    // tapped both applications before looking for an output at all.
    run_on(engine, 8);
    const auto counted = census(*rig.devices);
    CHECK(counted.created == 0);
    CHECK(counted.live == 0);
    for (const auto& app : engine.status().apps) {
        CHECK_FALSE(app.tapped);
    }
    engine.stop();
}

TEST_CASE("crucible engine: taps open with an endpoint and are released when it goes",
          "[crucible][engine]") {
    // One plain stereo endpoint, which is also the default: the policy decodes
    // to Lo/Ro on it.
    Rig rig;
    rig.devices->devices = {realtek_default()};
    two_playing(rig);
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&engine] { return listed_and_probed(engine); }));
    REQUIRE(engine.status().mode == OutputMode::kStereo);
    REQUIRE(wait_for([&rig] { return census(*rig.devices).live == 2; }));

    // The endpoint goes away - unplugged, or switched off at the receiver.
    set_endpoints(*rig.devices, {});
    REQUIRE(eventually_after_reprobe(
        engine, [&engine] { return engine.status().mode == OutputMode::kNone; }));
    REQUIRE(wait_for([&rig] { return census(*rig.devices).live == 0; }));

    // And stays away: no tap is opened again while there is nowhere to play,
    // however many session refreshes go by.
    run_on(engine, 8);
    CHECK(census(*rig.devices).created == 2);
    for (const auto& app : engine.status().apps) {
        CHECK_FALSE(app.tapped);
    }

    // It comes back with the endpoint, without waiting for the session monitor:
    // the gate is checked on every frame, not only when a new list arrives.
    set_endpoints(*rig.devices, {realtek_default()});
    REQUIRE(eventually_after_reprobe(
        engine, [&engine] { return engine.status().mode == OutputMode::kStereo; }));
    REQUIRE(wait_for([&rig] { return census(*rig.devices).live == 2; }));
    CHECK(census(*rig.devices).created == 4);
    engine.stop();
}

TEST_CASE("crucible engine: a sink that refuses to start releases the taps too",
          "[crucible][engine]") {
    // The other way the output stage ends up with no endpoint: the policy chose
    // one and the sink would not open on it. The stage reports kNone for that as
    // well, and the rule is the same - a tap held open then would mute the
    // application on macOS for a mode that never started.
    Rig rig;
    rig.devices->devices = {realtek_default()};
    two_playing(rig);
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&engine] { return listed_and_probed(engine); }));
    REQUIRE(wait_for([&rig] { return census(*rig.devices).live == 2; }));

    // A different endpoint, so the stage tears the stereo sink down and starts a
    // new one on it - which refuses, and goes on refusing however many times the
    // stage retries.
    {
        const std::lock_guard<std::mutex> lock(rig.devices->mutex);
        rig.devices->devices = {hdmi_avr()};
        rig.devices->refuse_sink_starts = true;
    }
    REQUIRE(eventually_after_reprobe(
        engine, [&engine] { return engine.status().mode == OutputMode::kNone; }));
    REQUIRE(wait_for([&rig] { return census(*rig.devices).live == 0; }));
    run_on(engine, 8);
    CHECK(census(*rig.devices).created == 2);
    engine.stop();
}
