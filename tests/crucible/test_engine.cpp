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
//
// The last holds a third rule of the same loop, and the reason the stop case
// above can now ask once: a re-probe that arrives while an enumeration is
// running is kept for the next frame, not spent on the one in flight.
//
// Every case here carries [concurrency], so the ThreadSanitizer leg reaches
// them through `ctest -L concurrency` (tests/CMakeLists.txt's note on the
// label). The engine is the shape that label is for: three threads - the
// frame loop, the session monitor and the probe - over three mutexes, four
// atomics and an OutputStage, none of which any other gate can see into. The
// probe's own arming is the sharpest of them, and unlocked on purpose: the
// frame loop reads `probing` and then takes `want_reprobe`, which is safe
// only because it is the sole reader of the one and the sole writer that
// sets the other true (engine.cpp's own comment says so). That is an
// argument, and this is the leg that checks it.
//
// Every thread here is this project's own code. The one platform thread the
// engine would start is the device watcher's, and on the Linux legs that is
// the ALSA backend, which refuses start() with kNoBackend and starts nothing
// - so no uninstrumented library runs here and tsan.supp stays as empty as
// its own header asks it to be.
//
// test_engine_start.cpp is deliberately NOT on that leg, though it drives
// the same engine: its cases assert that frames flow within two seconds,
// which is an upper bound on how long the engine may take. TSan would break
// that, and widening it to survive would delete the thing it checks. The
// deadlines here are the other kind - a lower bound on stop(), and polls
// that a slow run only reaches sooner than their limit.

using namespace ac3::crucible;
using namespace ac3::crucible::testing;

namespace {

using Clock = std::chrono::steady_clock;

// Polls `predicate` until it holds. Everything here is driven by a frame
// loop on another thread, so a case waits for what it asked for rather than
// sleeping a guessed interval; the deadline only exists so a regression
// fails the case instead of hanging the suite.
//
// Thirty seconds, not the five a passing run needs. The poll returns the
// moment the predicate holds, so a long deadline costs a passing case
// nothing and only lengthens the wait before a genuine failure is reported.
// What it buys is the ThreadSanitizer leg below, where shadow memory makes
// every frame several times slower on a runner shared with other jobs.
template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds limit = std::chrono::milliseconds(30000)) {
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
// through a probe. One request is enough however busy the machine is - a
// re-probe asked for while an enumeration is in flight stays armed until the
// frame loop can start a fresh one - so nothing here has to repeat it.
// Thirty seconds for the same reason wait_for takes thirty: the wait ends the
// moment the predicate holds, and the slack is there for the ThreadSanitizer
// leg's slower frames.
template <typename Predicate>
bool eventually_after_reprobe(Engine& engine, Predicate predicate,
                              std::chrono::milliseconds limit = std::chrono::milliseconds(30000)) {
    engine.reprobe();
    return wait_for(std::move(predicate), limit);
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

TEST_CASE("crucible engine: start brings the output up on the endpoint the policy chose", "[crucible][engine][concurrency]") {
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

TEST_CASE("crucible engine: a second start is refused and stop is idempotent", "[crucible][engine][concurrency]") {
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

TEST_CASE("crucible engine: every listed application with a session gets a tap", "[crucible][engine][concurrency]") {
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

TEST_CASE("crucible engine: stop waits for an enumeration still in flight", "[crucible][engine][concurrency]") {
    Rig rig;
    auto& devices = *rig.devices;
    Engine engine(rig.config());

    REQUIRE(engine.start().has_value());
    // The engine probes once of its own accord on the first frame. Let that
    // one through, so what this case holds up is a probe it asked for.
    REQUIRE(wait_for([&devices] { return devices.enumerations_finished() >= 1; }));

    devices.hold_enumerations();
    // One request is enough, however busy the loop is. It used to take one
    // per pass, because a request that arrived while a probe was still in
    // flight was consumed and dropped - and the first probe's thread is still
    // winding up for a moment after its enumeration has answered, so landing
    // in that window was likely. A request now stays armed until the frame
    // loop can act on it, so this asks once and waits for the probe it asked
    // for to park (see the case below, which is about that rule alone).
    engine.reprobe();
    const bool parked = wait_for([&devices] { return devices.enumerations_parked() >= 1; });
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
          "[crucible][engine][concurrency]") {
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
          "[crucible][engine][concurrency]") {
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
          "[crucible][engine][concurrency]") {
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

TEST_CASE("crucible engine: a re-probe asked for during an enumeration is not lost",
          "[crucible][engine][concurrency]") {
    // The case reprobe() exists for. An enumeration is slow, so the world can
    // change after the running one has read it: the user moves the default
    // output to the silent device, or switches a receiver on, and only then
    // asks. The engine has to follow that from one request - the enumeration
    // in flight cannot answer it, because it looked before the change.
    Rig rig;
    rig.devices->devices = {realtek_default()};  // set before start(): nothing else is running
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&engine] { return engine.status().mode == OutputMode::kStereo; }));
    REQUIRE(engine.status().endpoint_name == "Speakers (Realtek)");

    // Park the next enumeration. It reads the list before it parks, so what it
    // is holding - one stereo endpoint - is what it reports when it is let go,
    // whatever happens to the machine meanwhile.
    const auto parked_before = rig.devices->enumerations_parked();
    rig.devices->hold_enumerations();
    engine.reprobe();
    const bool parked = wait_for(
        [&rig, parked_before] { return rig.devices->enumerations_parked() > parked_before; });
    if (!parked) {
        // Leaving the gate shut would hang the engine's destructor on the join.
        rig.devices->release_enumerations();
    }
    REQUIRE(parked);

    // The receiver goes on. The parked enumeration cannot see it.
    set_endpoints(*rig.devices, {realtek_default(), hdmi_avr()});

    // Exactly one request, made while that enumeration is still in flight.
    // This is the one that used to be dropped - neither served nor kept.
    engine.reprobe();

    // Frames have to go by with the request standing and the enumeration still
    // parked, because losing it was the frame loop's doing: it cleared
    // want_reprobe to test it, found `probing` already true, and started
    // nothing. Releasing the gate first would let the probe finish and the
    // request be served by the frame after it - which the old code did too, so
    // the case would pass against the bug it exists for.
    run_on(engine, 3);

    // Let the stale enumeration finish. It reports the world as it was, so on
    // its own it leaves the stage on the Realtek endpoint.
    rig.devices->release_enumerations();

    // The request outlived it: a fresh enumeration follows and finds the
    // receiver, which takes the output exclusively for DD+ 5.1 - no signing
    // key here, so not Atmos.
    REQUIRE(wait_for([&engine] { return engine.status().endpoint_name == "AVR (HDMI)"; }));
    CHECK(engine.status().mode == OutputMode::kDdPlus51);
    engine.stop();
}
