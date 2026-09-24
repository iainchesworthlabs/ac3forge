#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "platform/process.hpp"

#include "diagnostics.hpp"
#include "engine.hpp"
#include "fake_devices.hpp"
#include "fake_services.hpp"
#include "output_policy.hpp"
#include "session_monitor.hpp"

// The engine's commands, over the same three fakes test_engine.cpp uses:
// what positioning, splitting, sizing and pairing an application does to the
// status the UI reads back; the full-screen rule; the output commands (pin,
// preferred endpoint, bypass); the signing key loaded, refused and cleared;
// and the frame loop's own bookkeeping - starved taps, a tap that will not
// open, and the catch-up that bounds the PCM sink's queue - as the
// diagnostics log and the status report them.
//
// Every case carries [concurrency] for the reason test_engine.cpp gives: the
// commands are posted from this thread and applied on the frame thread, and
// the ThreadSanitizer leg should see both sides. Every wait is a poll on the
// status snapshot with a generous deadline that a passing run never reaches.

using namespace ac3::crucible;
using namespace ac3::crucible::testing;

namespace {

using Clock = std::chrono::steady_clock;

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

struct Rig {
    std::shared_ptr<FakeDevices> devices = std::make_shared<FakeDevices>();
    std::shared_ptr<FakeSessionMonitor> sessions = std::make_shared<FakeSessionMonitor>();
    std::shared_ptr<FakeForeground> foreground = std::make_shared<FakeForeground>();
    DiagnosticLog log;

    Rig() { devices->devices = {realtek_default(), hdmi_avr()}; }

    [[nodiscard]] EngineConfig config() {
        EngineConfig c;
        c.devices = devices;
        c.sessions = sessions;
        c.foreground = foreground;
        c.low_latency = true;
        c.diagnostics = &log;
        return c;
    }

    // Whether any diagnostics line so far contains `what`.
    [[nodiscard]] bool noted(const std::string& what) const {
        return std::ranges::any_of(log.lines(),
                                   [&what](const std::string& line) { return line.find(what) != std::string::npos; });
    }
};

AppSession playing(AppId app, std::string name) {
    AppSession session;
    session.app = app;
    session.name = std::move(name);
    session.active = true;
    session.has_window = true;
    return session;
}

std::optional<AppStatus> app_in(const EngineStatus& status, AppId app) {
    for (const auto& a : status.apps) {
        if (a.app == app) {
            return a;
        }
    }
    return std::nullopt;
}

// Polls until the engine's status of `app` satisfies `predicate`.
template <typename Predicate>
bool app_becomes(const Engine& engine, AppId app, Predicate predicate) {
    return wait_for([&] {
        const auto a = app_in(engine.status(), app);
        return a.has_value() && predicate(*a);
    });
}

bool near(double a, double b) {
    return std::abs(a - b) < 1e-6;
}

bool tapped_and_running(const Engine& engine, std::size_t apps) {
    const auto s = engine.status();
    return s.mode != OutputMode::kNone && s.apps.size() == apps &&
           std::ranges::all_of(s.apps, [](const AppStatus& a) { return a.tapped; });
}

std::string scratch_pid_suffix() {
    return ac3::test::platform::process_id();
}

// This process's environment only (tests/platform/process.hpp's seam).
void set_env(const char* name, const char* value) {
    ac3::test::platform::set_environment(name, value);
}

void unset_env(const char* name) { ac3::test::platform::unset_environment(name); }

// A scratch directory of this process's own, for key files.
std::filesystem::path scratch() {
    const auto dir = std::filesystem::path{AC3FORGE_TEST_SCRATCH_DIR} /
                     ("crucible_engine_" + scratch_pid_suffix());
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path write_file(const std::string& name, const std::string& contents) {
    const auto path = scratch() / name;
    std::ofstream out{path, std::ios::binary};
    out << contents;
    return path;
}

}  // namespace

TEST_CASE("crucible engine commands: a positioned application takes a slot at its position and unposition returns it to the bed",
          "[crucible][engine][concurrency]") {
    Rig rig;
    rig.sessions->set_apps({playing(1001U, "player"), playing(1002U, "browser")});
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&] { return tapped_and_running(engine, 2); }));
    CHECK_FALSE(app_in(engine.status(), 1001U)->slot.has_value());

    engine.position(1001U, {0.2, 0.8, 0.0});
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) {
        return a.slot.has_value() && near(a.position.x, 0.2) && near(a.position.y, 0.8);
    }));
    {
        const auto a = *app_in(engine.status(), 1001U);
        CHECK(a.width == 1);
        CHECK(a.level_dbfs > -60.0F);  // the fake tap's tone reaches the meter
        CHECK_FALSE(app_in(engine.status(), 1002U)->slot.has_value());
    }
    // The monitor is asked to keep the placed application listed.
    REQUIRE(wait_for([&] {
        const auto keep = rig.sessions->last_keep();
        return std::ranges::find(keep, 1001U) != keep.end();
    }));

    engine.unposition(1001U);
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) { return !a.slot.has_value(); }));
    engine.stop();
}

TEST_CASE("crucible engine commands: a split pair sits at the spread, a side placed alone makes it custom, and reset restores it",
          "[crucible][engine][concurrency]") {
    Rig rig;
    rig.sessions->set_apps({playing(1001U, "player")});
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&] { return tapped_and_running(engine, 1); }));

    engine.set_split(1001U, true);
    engine.position(1001U, {0.5, 0.5, 0.0});
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) {
        return a.slot.has_value() && a.width == 2 && near(a.position.x, 0.5);
    }));
    {
        const auto a = *app_in(engine.status(), 1001U);
        CHECK_FALSE(a.pair_custom);
        CHECK(a.left.x == Catch::Approx(0.35));  // the default spread of 0.15 either side
        CHECK(a.right.x == Catch::Approx(0.65));
    }

    // One side on its own: the pair becomes custom and its centre follows.
    engine.position_side(1001U, 0, {0.1, 0.5, 0.0});
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) { return a.pair_custom && near(a.left.x, 0.1); }));
    CHECK(app_in(engine.status(), 1001U)->right.x == Catch::Approx(0.65));

    // An out-of-range side is ignored.
    engine.position_side(1001U, 2, {0.9, 0.9, 0.0});
    // Moving a custom pair moves both objects by the same amount, clamped to the room.
    engine.position(1001U, {0.475, 0.5, 0.0});  // centre was 0.375: +0.1
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) { return near(a.left.x, 0.2); }));
    {
        const auto a = *app_in(engine.status(), 1001U);
        CHECK(a.right.x == Catch::Approx(0.75));
        CHECK(a.left.y == Catch::Approx(0.5));
    }
    engine.position(1001U, {1.0, 1.0, 1.0});
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) { return near(a.right.x, 1.0); }));
    {
        const auto a = *app_in(engine.status(), 1001U);
        CHECK(a.right.y == Catch::Approx(1.0));
        CHECK(a.right.z == Catch::Approx(1.0));
        CHECK(a.left.x == Catch::Approx(0.725));
    }

    engine.reset_pair(1001U);
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) { return !a.pair_custom; }));
    CHECK(app_in(engine.status(), 1001U)->left.x == Catch::Approx(0.85));
    CHECK(app_in(engine.status(), 1001U)->right.x == Catch::Approx(1.0));  // clamped at the wall

    engine.set_split(1001U, false);
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) { return a.width == 1; }));
    engine.stop();
}

TEST_CASE("crucible engine commands: placing one side of an application that was never split makes it a pair",
          "[crucible][engine][concurrency]") {
    Rig rig;
    rig.sessions->set_apps({playing(1001U, "player")});
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&] { return tapped_and_running(engine, 1); }));

    engine.position_side(1001U, 1, {0.9, 0.3, 0.0});
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) {
        return a.width == 2 && a.slot.has_value() && a.pair_custom;
    }));
    const auto a = *app_in(engine.status(), 1001U);
    CHECK(a.right.x == Catch::Approx(0.9));
    CHECK(a.left.x == Catch::Approx(0.35));  // the unplaced side stays at the spread around the room's centre
    engine.stop();
}

TEST_CASE("crucible engine commands: split by default makes every application a pair",
          "[crucible][engine][concurrency]") {
    Rig rig;
    rig.sessions->set_apps({playing(1001U, "player")});
    auto config = rig.config();
    config.split_by_default = true;
    config.split_spread = 0.25;
    Engine engine(std::move(config));
    REQUIRE(engine.start().has_value());
    engine.position(1001U, {0.5, 0.5, 0.0});
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) { return a.width == 2 && a.slot.has_value(); }));
    const auto a = *app_in(engine.status(), 1001U);
    CHECK(a.left.x == Catch::Approx(0.25));
    CHECK(a.right.x == Catch::Approx(0.75));
    engine.stop();
}

TEST_CASE("crucible engine commands: an object's size is clamped to the unit range",
          "[crucible][engine][concurrency]") {
    Rig rig;
    rig.sessions->set_apps({playing(1001U, "player")});
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    engine.position(1001U, {0.5, 0.5, 0.0});
    engine.set_size(1001U, 3.0);
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) { return near(a.size, 1.0); }));
    engine.set_size(1001U, -1.0);
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) { return near(a.size, 0.0); }));
    engine.set_size(1001U, 0.4);
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) { return near(a.size, 0.4); }));
    engine.stop();
}

TEST_CASE("crucible engine commands: the full-screen application is marked, matched by any pid in its tree",
          "[crucible][engine][concurrency]") {
    Rig rig;
    auto game = playing(1001U, "game");
    game.session_pids = {1001U, 4242U};
    rig.sessions->set_apps({game, playing(1002U, "browser")});
    rig.foreground->set_fullscreen_pid(4242U);
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) { return a.fullscreen; }));
    CHECK_FALSE(app_in(engine.status(), 1002U)->fullscreen);
    CHECK(engine.status().fullscreen_rule_available);
    CHECK(engine.status().fullscreen_rule_reason.empty());

    rig.foreground->set_fullscreen_pid(1002U);  // matched by the application's own id
    REQUIRE(app_becomes(engine, 1002U, [](const AppStatus& a) { return a.fullscreen; }));
    CHECK_FALSE(app_in(engine.status(), 1001U)->fullscreen);

    rig.foreground->set_unsupported("no way to tell here");
    REQUIRE(wait_for([&] { return !engine.status().fullscreen_rule_available; }));
    CHECK(engine.status().fullscreen_rule_reason == "no way to tell here");
    REQUIRE(app_becomes(engine, 1002U, [](const AppStatus& a) { return !a.fullscreen; }));
    engine.stop();
}

TEST_CASE("crucible engine commands: an application that leaves is dropped from the plan and its slot freed",
          "[crucible][engine][concurrency]") {
    Rig rig;
    rig.sessions->set_apps({playing(1001U, "player"), playing(1002U, "browser")});
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    engine.position(1002U, {0.3, 0.3, 0.0});
    engine.set_size(1002U, 0.5);
    REQUIRE(app_becomes(engine, 1002U, [](const AppStatus& a) { return a.slot.has_value(); }));

    rig.sessions->set_apps({playing(1001U, "player")});
    REQUIRE(wait_for([&] {
        const auto s = engine.status();
        return s.apps.size() == 1 && s.apps[0].app == 1001U;
    }));
    // Back again, it starts over: unplaced, a point.
    rig.sessions->set_apps({playing(1001U, "player"), playing(1002U, "browser")});
    REQUIRE(app_becomes(engine, 1002U, [](const AppStatus& a) { return a.tapped; }));
    const auto back = *app_in(engine.status(), 1002U);
    CHECK_FALSE(back.slot.has_value());
    CHECK(back.size == 0.0);
    engine.stop();
}

TEST_CASE("crucible engine commands: pin, preferred endpoint and bypass reach the output stage",
          "[crucible][engine][concurrency]") {
    Rig rig;
    // One-block frames (the rig's low_latency): the DD 5.1 leg re-encodes the
    // bed with an AC-3 encoder that takes whole 1536-sample frames only, so
    // the output stage gathers six 256-sample beds into each one - the pin
    // must reach an AC-3 burst on the wire, not an encoder assertion.
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&] { return engine.status().mode == OutputMode::kDdPlus51; }));

    engine.pin(OutputMode::kDd51);
    REQUIRE(wait_for([&] { return engine.status().mode == OutputMode::kDd51; }));
    CHECK(rig.noted("output: "));
    REQUIRE(wait_for([&] {
        const std::lock_guard devices_lock(rig.devices->mutex);
        for (const auto& sink : rig.devices->burst_sinks) {
            const std::lock_guard lock(sink->mutex);
            if (sink->started && !sink->eac3 && sink->submits > 0) {
                return true;
            }
        }
        return false;
    }));
    engine.pin(std::nullopt);
    REQUIRE(wait_for([&] { return engine.status().mode == OutputMode::kDdPlus51; }));

    engine.prefer_endpoint("realtek");
    REQUIRE(wait_for([&] {
        const auto s = engine.status();
        return s.mode == OutputMode::kStereo && s.endpoint_name == "Speakers (Realtek)";
    }));
    CHECK_FALSE(engine.status().codec_bypassed);
    engine.set_bypass(true);
    REQUIRE(wait_for([&] { return engine.status().codec_bypassed; }));
    engine.set_bypass(false);
    REQUIRE(wait_for([&] { return !engine.status().codec_bypassed; }));
    engine.stop();
}

TEST_CASE("crucible engine commands: a signing key turns objects on and clearing it turns them off",
          "[crucible][engine][concurrency]") {
    Rig rig;
    const auto key = write_file("engine_key.b64", "AAECA/8=\n");
    auto config = rig.config();
    config.signing_key_path = key.string();
    Engine engine(std::move(config));
    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&] { return engine.status().mode == OutputMode::kAtmos; }));
    CHECK(engine.status().objects_enabled);
    CHECK(engine.status().signing.find("signing key loaded") != std::string::npos);
    CHECK(rig.noted("signing: objects on (key from a file)"));
    // The note names no file.
    CHECK_FALSE(rig.noted(key.filename().string()));

    engine.clear_signing_key();
    REQUIRE(wait_for([&] { return engine.status().mode == OutputMode::kDdPlus51; }));
    CHECK_FALSE(engine.status().objects_enabled);
    CHECK(engine.status().signing == "signing key cleared: objects off, streaming the 5.1 bed only");
    CHECK(rig.noted("signing: key cleared, 5.1 bed only"));

    engine.load_signing_key(key.string());
    REQUIRE(wait_for([&] { return engine.status().mode == OutputMode::kAtmos; }));
    CHECK(engine.status().objects_enabled);
    engine.stop();
    std::filesystem::remove(key);
}

TEST_CASE("crucible engine commands: a key that will not load says why in a note that names no file",
          "[crucible][engine][concurrency]") {
    Rig rig;
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());

    const auto empty = write_file("engine_empty.key", "");
    const auto malformed = write_file("engine_malformed.key", "56, 6c, ef, 66\n");
    const auto missing = scratch() / "engine_missing.key";

    engine.load_signing_key(missing.string());
    REQUIRE(wait_for([&] { return rig.noted("signing: key not loaded (unreadable), 5.1 bed only"); }));
    engine.load_signing_key(empty.string());
    REQUIRE(wait_for([&] { return rig.noted("signing: key not loaded (empty), 5.1 bed only"); }));
    engine.load_signing_key(malformed.string());
    REQUIRE(wait_for([&] { return rig.noted("signing: key not loaded (malformed), 5.1 bed only"); }));
    CHECK_FALSE(engine.status().objects_enabled);
    CHECK(engine.status().signing.find("signing key not loaded") != std::string::npos);
    CHECK_FALSE(rig.noted("engine_malformed"));
    engine.stop();
    std::filesystem::remove(empty);
    std::filesystem::remove(malformed);
}

TEST_CASE("crucible engine commands: an empty key path takes the key from the environment",
          "[crucible][engine][concurrency]") {
    Rig rig;
    // Only this process's environment, and only for this case: the variables
    // are put back as they were before the case ends.
    const char* had_inline = std::getenv("AC3FORGE_SIGNING_KEY");
    const std::optional<std::string> saved_inline =
        had_inline != nullptr ? std::optional<std::string>{had_inline} : std::nullopt;
    const char* had_file = std::getenv("AC3FORGE_SIGNING_KEY_FILE");
    const std::optional<std::string> saved_file =
        had_file != nullptr ? std::optional<std::string>{had_file} : std::nullopt;
    unset_env("AC3FORGE_SIGNING_KEY_FILE");
    unset_env("AC3FORGE_SIGNING_KEY");

    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&] { return rig.noted("signing: no key, 5.1 bed only"); }));

    set_env("AC3FORGE_SIGNING_KEY", "AAECA/8=");
    engine.load_signing_key("");
    REQUIRE(wait_for([&] { return engine.status().objects_enabled; }));
    CHECK(rig.noted("signing: objects on (key from the environment)"));
    CHECK(engine.status().signing == "signing key loaded from environment: object container will be signed");
    engine.stop();

    unset_env("AC3FORGE_SIGNING_KEY");
    if (saved_inline) {
        set_env("AC3FORGE_SIGNING_KEY", saved_inline->c_str());
    }
    if (saved_file) {
        set_env("AC3FORGE_SIGNING_KEY_FILE", saved_file->c_str());
    }
}

TEST_CASE("crucible engine commands: starved taps are counted and read as silence on the meter",
          "[crucible][engine][concurrency]") {
    Rig rig;
    rig.sessions->set_apps({playing(1001U, "player")});
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&] { return tapped_and_running(engine, 1); }));
    {
        const std::lock_guard<std::mutex> lock(rig.devices->mutex);
        REQUIRE_FALSE(rig.devices->taps.empty());
        for (const auto& tap : rig.devices->taps) {
            const std::lock_guard<std::mutex> tap_lock(tap->mutex);
            tap->starve = true;
        }
    }
    REQUIRE(wait_for([&] { return engine.status().starved_reads > 0; }));
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) { return a.level_dbfs <= -120.0F; }));
    engine.stop();
    CHECK(rig.noted("starved reads"));
}

TEST_CASE("crucible engine commands: a tap that refuses to open is noted once with the application's name",
          "[crucible][engine][concurrency]") {
    Rig rig;
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&] { return engine.status().mode != OutputMode::kNone; }));
    {
        const std::lock_guard<std::mutex> lock(rig.devices->mutex);
        rig.devices->refuse_next_start = true;
    }
    rig.sessions->set_apps({playing(1001U, "stubborn")});
    REQUIRE(wait_for([&] { return rig.noted("tap refused for app 1001 (stubborn)"); }));
    // The next refresh retries and, the refusal being one-shot, opens it.
    REQUIRE(app_becomes(engine, 1001U, [](const AppStatus& a) { return a.tapped; }));
    engine.stop();
    const auto lines = rig.log.lines();
    CHECK(std::ranges::count_if(lines, [](const std::string& l) {
              return l.find("tap refused for app 1001") != std::string::npos;
          }) == 1);
}

TEST_CASE("crucible engine commands: a PCM sink queue past its bound is caught up by dropping tap audio",
          "[crucible][engine][concurrency]") {
    Rig rig;
    rig.devices->devices = {realtek_default()};
    rig.sessions->set_apps({playing(1001U, "player")});
    Engine engine(rig.config());
    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&] { return tapped_and_running(engine, 1); }));
    REQUIRE(engine.status().mode == OutputMode::kStereo);
    {
        const std::lock_guard<std::mutex> lock(rig.devices->mutex);
        REQUIRE(rig.devices->pcm_sinks.size() == 1);
        const std::lock_guard<std::mutex> sink_lock(rig.devices->pcm_sinks[0]->mutex);
        rig.devices->pcm_sinks[0]->queued_frames = 48000;
    }
    REQUIRE(wait_for([&] { return engine.status().catchups > 0; }));
    CHECK(engine.status().sink_queue_ms == Catch::Approx(1000.0));
    CHECK(rig.noted("sink queue catch-up #1: dropped"));
    engine.stop();
    CHECK(rig.noted("catch-ups"));
}

TEST_CASE("crucible engine commands: an explicit bitrate is what the engine reports it encodes at",
          "[crucible][engine][concurrency]") {
    Rig rig;
    auto config = rig.config();
    config.low_latency = false;
    config.bitrate_kbps = 640;
    Engine engine(std::move(config));
    REQUIRE(engine.start().has_value());
    REQUIRE(wait_for([&] { return engine.status().frames_encoded > 0; }));
    engine.stop();
    CHECK(rig.noted("engine started: 6-block frames, 640 kb/s, taps 2ch"));
}
