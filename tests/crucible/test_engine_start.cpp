#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "engine.hpp"
#include "fake_devices.hpp"
#include "fake_services.hpp"

// What Engine::start() answers, over a scripted machine.
//
// It used to answer `{}` every time: it spawned the worker and returned,
// while everything that can actually fail - the output stage, the first
// enumeration, opening a sink - happened on that thread afterwards. A
// machine with no audio endpoint at all therefore reported a running
// engine with no error, the window's status strip had nothing to say, and
// the "the engine did not start here" skip in the QML suites was dead
// code. These cases hold the refusal open: start() now waits for the
// worker to say which it is, and says so.
//
// Every case runs one-block frames (`low_latency`), which is not about
// latency here: it makes the frame loop's first pass 5 ms rather than 32,
// so the first probe's verdict reaches start() well inside its deadline
// even on a loaded host.

using namespace ac3::crucible;
using namespace ac3::crucible::testing;

namespace {

EngineConfig config_over(std::shared_ptr<FakeDevices> devices) {
    EngineConfig config;
    config.devices = std::move(devices);
    config.sessions = std::make_shared<FakeSessionMonitor>();
    config.foreground = std::make_shared<FakeForeground>();
    config.low_latency = true;
    return config;
}

// The engine publishes its status every other frame, so a fresh one has
// counted nothing yet; this waits for it rather than sleeping a guess.
bool frames_flow(const Engine& engine, std::chrono::milliseconds within) {
    const auto deadline = std::chrono::steady_clock::now() + within;
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.status().frames_encoded > 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

}  // namespace

TEST_CASE("engine start: a machine with no render endpoint is refused, not started",
          "[crucible][engine]") {
    auto devices = std::make_shared<FakeDevices>();  // nothing to enumerate
    Engine engine(config_over(devices));

    const auto started = engine.start();
    REQUIRE_FALSE(started.has_value());
    // The policy's own sentence, not the deadline's: this refusal is the
    // first probe reporting, which is what distinguishes it from a worker
    // that never came up.
    CHECK(started.error().find("no render endpoint can carry any mode") != std::string::npos);
    // Nothing was opened to play into, which is the state the sentence
    // above describes. `status().running` is deliberately not asserted
    // here: it reports the worker's own lifecycle, and a refusal reports
    // rather than stopping the loop.
    {
        const std::lock_guard lock(devices->mutex);
        CHECK(devices->burst_sinks.empty());
        CHECK(devices->pcm_sinks.empty());
        CHECK(devices->object_sinks.empty());
    }
    engine.stop();
}

TEST_CASE("engine start: an endpoint that can carry the stream starts and runs frames",
          "[crucible][engine]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    Engine engine(config_over(devices));

    REQUIRE(engine.start().has_value());
    CHECK(frames_flow(engine, std::chrono::milliseconds(2000)));
    CHECK(engine.status().mode != OutputMode::kNone);
    engine.stop();
}

TEST_CASE("engine start: a sink that refuses to open is a refusal with the sink's reason",
          "[crucible][engine]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    devices->refuse_next_start = true;  // the burst sink the AVR would get
    Engine engine(config_over(devices));

    const auto started = engine.start();
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().find("could not start: refused by the test") != std::string::npos);
    // The sink was asked and said no, which is a different shape of refusal
    // from the case above: one was offered and refused, rather than none
    // being there to offer.
    {
        const std::lock_guard lock(devices->mutex);
        REQUIRE(devices->burst_sinks.size() == 1);
        CHECK_FALSE(devices->burst_sinks[0]->started);
    }
    engine.stop();
}

TEST_CASE("engine start: starting a running engine is refused without disturbing it",
          "[crucible][engine]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    Engine engine(config_over(devices));

    REQUIRE(engine.start().has_value());
    const auto again = engine.start();
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == "already running");
    CHECK(frames_flow(engine, std::chrono::milliseconds(2000)));
    engine.stop();
}

TEST_CASE("engine start: a refused engine can be started again once the machine has an endpoint",
          "[crucible][engine]") {
    auto devices = std::make_shared<FakeDevices>();
    Engine refused(config_over(devices));
    REQUIRE_FALSE(refused.start().has_value());
    refused.stop();

    // The same fakes, with something to play into this time: nothing about
    // the first refusal is sticky.
    devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    Engine engine(config_over(devices));
    REQUIRE(engine.start().has_value());
    CHECK(frames_flow(engine, std::chrono::milliseconds(2000)));
    engine.stop();
}

TEST_CASE("engine start: the same engine restarted answers for this run, not the last one",
          "[crucible][engine]") {
    auto devices = std::make_shared<FakeDevices>();
    Engine engine(config_over(devices));

    // Refused: nothing to play into.
    REQUIRE_FALSE(engine.start().has_value());
    engine.stop();

    // The AVR arrives while the engine is stopped. A second start() has to
    // wait for the new worker rather than hand back the verdict the last
    // one left behind.
    devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    REQUIRE(engine.start().has_value());
    CHECK(frames_flow(engine, std::chrono::milliseconds(2000)));
    engine.stop();

    // And the other way round, so a stale success cannot pass either: the
    // endpoints go away, and the restart refuses.
    devices->devices.clear();
    const auto after = engine.start();
    REQUIRE_FALSE(after.has_value());
    CHECK(after.error().find("no render endpoint can carry any mode") != std::string::npos);
    engine.stop();
}
