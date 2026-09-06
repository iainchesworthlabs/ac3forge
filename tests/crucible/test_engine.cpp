#include <catch2/catch_test_macros.hpp>

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

// The whole engine - its frame loop, on its own thread - over the fakes for
// every seam it has (fake_devices.hpp for the machine's audio, fake_services.hpp
// for the session list and the foreground). Nothing here needs audio hardware,
// a window manager or a driver, so it runs on every CI leg.
//
// What these cases hold is one rule: a tap is opened only while the output
// stage has an endpoint to play to. See Impl::sync_taps() in
// apps/crucible/engine/engine.cpp for why that matters on macOS, where the
// Core Audio process tap mutes the application it taps, and why it changes
// nothing audible on Windows or Linux, where a tap is a pure capture.

using namespace ac3::crucible;
using namespace ac3::crucible::testing;

namespace {

AppSession playing(AppId app, std::string name) {
    return AppSession{.app = app,
                      .name = std::move(name),
                      .active = true,
                      .has_window = true,
                      .has_session = true,
                      .session_pids = {app}};
}

// The engine runs on its own thread at its own pace, so every assertion
// about it is "this becomes true", never "this is true now".
template <typename Predicate>
bool eventually(Predicate predicate,
                std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// The same, for a change to the fake device list, which only reaches the
// engine through a probe. The request is repeated rather than made once
// because reprobe() is absorbed when an enumeration is already in flight,
// and the device watcher on a real CI machine can start one at any moment.
template <typename Predicate>
bool eventually_after_reprobe(Engine& engine, Predicate predicate,
                              std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
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
    const std::lock_guard lock(devices.mutex);
    TapCensus counted;
    counted.created = devices.taps.size();
    for (const auto& tap : devices.taps) {
        const std::lock_guard tap_lock(tap->mutex);
        if (tap->started && !tap->stopped) {
            ++counted.live;
        }
    }
    return counted;
}

void set_endpoints(FakeDevices& devices, std::vector<DeviceFacts> endpoints) {
    const std::lock_guard lock(devices.mutex);
    devices.devices = std::move(endpoints);
}

// Two applications playing, and whatever endpoints the case wants.
struct Rig {
    std::shared_ptr<FakeDevices> devices = std::make_shared<FakeDevices>();
    std::shared_ptr<FakeSessionMonitor> sessions = std::make_shared<FakeSessionMonitor>();
    std::shared_ptr<FakeForeground> foreground = std::make_shared<FakeForeground>();
    std::unique_ptr<Engine> engine;

    void start(std::vector<DeviceFacts> endpoints) {
        set_endpoints(*devices, std::move(endpoints));
        sessions->set_apps({playing(1001, "player"), playing(1002, "browser")});
        EngineConfig config;
        config.devices = devices;
        config.sessions = sessions;
        config.foreground = foreground;
        engine = std::make_unique<Engine>(std::move(config));
        REQUIRE(engine->start().has_value());
    }

    // The engine has taken a session list and applied its first probe. The
    // reason is the signal for the second: the output stage leaves it empty
    // until an apply() has run, whatever that apply decided.
    [[nodiscard]] bool listed_and_probed() const {
        const auto status = engine->status();
        return status.apps.size() == 2 && !status.output_reason.empty();
    }

    // Lets `frames` more frames go by, so that a thing asserted absent has
    // had every chance to happen.
    void run_on(std::uint64_t frames) const {
        const auto until = engine->status().frames_encoded + frames;
        REQUIRE(eventually([&] { return engine->status().frames_encoded >= until; }));
    }
};

}  // namespace

TEST_CASE("no application is tapped while the output stage has no endpoint", "[crucible][engine]") {
    // A machine with no render endpoint at all: the policy answers kNone, so
    // there is nowhere for tapped audio to go.
    Rig rig;
    rig.start({});
    REQUIRE(eventually([&] { return rig.listed_and_probed(); }));
    REQUIRE(rig.engine->status().mode == OutputMode::kNone);

    // The session list is taken earlier in the frame than a probe is applied,
    // and the first probe is only requested at construction, so this is the
    // ordering the rule exists for: without it the very first frame would
    // have tapped both applications before looking for an output at all.
    rig.run_on(8);
    const auto counted = census(*rig.devices);
    CHECK(counted.created == 0);
    CHECK(counted.live == 0);
    for (const auto& app : rig.engine->status().apps) {
        CHECK_FALSE(app.tapped);
    }
}

TEST_CASE("taps open when the output has an endpoint and are released when it goes",
          "[crucible][engine]") {
    // One plain stereo endpoint, which is also the default: the policy
    // decodes to Lo/Ro on it.
    Rig rig;
    rig.start({realtek_default()});
    REQUIRE(eventually([&] { return rig.listed_and_probed(); }));
    REQUIRE(rig.engine->status().mode == OutputMode::kStereo);
    REQUIRE(eventually([&] { return census(*rig.devices).live == 2; }));

    // The endpoint goes away - unplugged, or switched off at the receiver.
    set_endpoints(*rig.devices, {});
    REQUIRE(eventually_after_reprobe(
        *rig.engine, [&] { return rig.engine->status().mode == OutputMode::kNone; }));
    REQUIRE(eventually([&] { return census(*rig.devices).live == 0; }));

    // And stays away: no tap is opened again while there is nowhere to play,
    // however many session refreshes go by.
    rig.run_on(8);
    CHECK(census(*rig.devices).created == 2);
    for (const auto& app : rig.engine->status().apps) {
        CHECK_FALSE(app.tapped);
    }

    // It comes back with the endpoint, without waiting for the session
    // monitor: the gate is checked on every frame, not only when a new list
    // arrives.
    set_endpoints(*rig.devices, {realtek_default()});
    REQUIRE(eventually_after_reprobe(
        *rig.engine, [&] { return rig.engine->status().mode == OutputMode::kStereo; }));
    REQUIRE(eventually([&] { return census(*rig.devices).live == 2; }));
    CHECK(census(*rig.devices).created == 4);
}

TEST_CASE("a sink that refuses to start releases the taps too", "[crucible][engine]") {
    // The other way the output stage ends up with no endpoint: the policy
    // chose one and the sink would not open on it. The stage reports kNone
    // for that as well, and the rule is the same - a tap held open then
    // would mute the application on macOS for a mode that never started.
    Rig rig;
    rig.start({realtek_default()});
    REQUIRE(eventually([&] { return rig.listed_and_probed(); }));
    REQUIRE(eventually([&] { return census(*rig.devices).live == 2; }));

    // A different endpoint, so the stage tears the stereo sink down and
    // starts a new one on it - which refuses, and goes on refusing however
    // many times the stage retries.
    {
        const std::lock_guard lock(rig.devices->mutex);
        rig.devices->devices = {hdmi_avr()};
        rig.devices->refuse_sink_starts = true;
    }
    REQUIRE(eventually_after_reprobe(
        *rig.engine, [&] { return rig.engine->status().mode == OutputMode::kNone; }));
    REQUIRE(eventually([&] { return census(*rig.devices).live == 0; }));
    rig.run_on(8);
    CHECK(census(*rig.devices).created == 2);
}
