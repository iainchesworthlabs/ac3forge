#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
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
// The last case is the one with a bug behind it. The probe runs
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
