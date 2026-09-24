#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "diagnostic_log.hpp"
#include "engine_thread.hpp"
#include "output_selector.hpp"
#include "pcm_sink.hpp"

// The engine's queue and transport commands that test_engine.cpp's cases leave out - previous,
// seek, insert, move, clear and repeat - and the output commands' answers on an engine that was
// given its outputs' decisions and one that decides them itself, as its diagnostics ring and its
// status report them. Nothing here plays: the device opens and never runs, so every command is
// answered while the queue stands still, and sync() is the only wait.

using namespace std::chrono_literals;

namespace {

using ac3::hearth::DiagnosticLog;
using ac3::hearth::Engine;
using ac3::hearth::EngineOutputs;
using ac3::hearth::EngineStatus;
using ac3::hearth::EngineTiming;
using ac3::hearth::LoadedItem;
using ac3::hearth::OpenOutputFormat;
using ac3::hearth::OutputMode;
using ac3::hearth::PcmSink;
using ac3::hearth::QueueItem;

// A device that opens and takes nothing: enough for an engine that is never asked to play.
class IdleDevice final : public PcmSink {
public:
    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        open_ = true;
        return OpenOutputFormat{.sample_rate = format.sample_rate,
                                .channels = static_cast<std::uint16_t>(format.layout.slots()),
                                .mode = OutputMode::kLocalPcm};
    }
    void close() override { open_ = false; }
    [[nodiscard]] bool is_open() const override { return open_; }
    bool submit(std::span<const std::span<const float>> /*slots*/, std::size_t /*frames*/) override { return false; }
    [[nodiscard]] std::optional<ac3::audio::MonitorPosition> position() const override { return std::nullopt; }
    void flush() override {}
    bool pause() override { return open_; }
    bool resume() override { return open_; }

private:
    bool open_ = false;
};

QueueItem item(const std::string& title) {
    QueueItem entry;
    entry.path = title + ".ec3";
    entry.title = title;
    return entry;
}

std::expected<LoadedItem, std::string> no_files(const std::string& path) {
    return std::unexpected("no such file: " + path);
}

ac3::render::OutputLayout stereo() {
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    return *layout;
}

// The ring's notes without their time stamps, from the second on (the first is the start).
std::vector<std::string> notes(const DiagnosticLog& log) {
    std::vector<std::string> out;
    for (const std::string& line : log.lines()) {
        out.push_back(line.substr(DiagnosticLog::kStampBytes));
    }
    if (!out.empty()) {
        out.erase(out.begin());
    }
    return out;
}

std::vector<std::string> titles(const EngineStatus& status) {
    std::vector<std::string> out;
    for (const QueueItem& entry : status.queue) {
        out.push_back(entry.title);
    }
    return out;
}

}  // namespace

TEST_CASE("engine queue commands: insert, move and clear edit the queue and say what they did",
          "[hearth][engine]") {
    DiagnosticLog log;
    {
        Engine engine(std::make_unique<IdleDevice>(), no_files, stereo(), {}, EngineTiming{.period = 1ms, .budget = 4800},
                      &log);
        engine.add({item("a"), item("b")});
        engine.insert(1, item("c"));
        engine.insert(9, item("d"));  // past the end: appended
        engine.sync();
        CHECK(titles(engine.status()) == std::vector<std::string>{"a", "c", "b", "d"});

        engine.move(0, 2);
        engine.move(1, 7);  // no such place
        engine.sync();
        CHECK(titles(engine.status()).size() == 4);
        CHECK(titles(engine.status())[2] == "a");

        engine.clear();
        engine.sync();
        CHECK(engine.status().queue.empty());
        engine.clear();
        engine.sync();
    }
    const std::vector<std::string> expected{
        "add 2 items to a queue of 0",
        "insert item 2 \"c\" into a queue of 2",
        "insert item 4 \"d\" into a queue of 3",
        "move item 1 \"a\" to 3",
        "move item 2 \"b\" to 8 (no such place)",
        "clear a queue of 4 items",
        "clear a queue of 0 items",
        "engine stopped",
    };
    CHECK(notes(log) == expected);
}

TEST_CASE("engine queue commands: previous and seek on a stopped queue are noted, and repeat is noted on a change only",
          "[hearth][engine]") {
    DiagnosticLog log;
    {
        Engine engine(std::make_unique<IdleDevice>(), no_files, stereo(), {}, EngineTiming{.period = 1ms, .budget = 4800},
                      &log);
        engine.previous();
        engine.seek(1500ms);
        engine.set_repeat(true);
        engine.set_repeat(true);
        engine.sync();
        CHECK(engine.status().repeat);
        engine.set_repeat(false);
        engine.sync();
        CHECK_FALSE(engine.status().repeat);
    }
    const auto seen = notes(log);
    const auto has = [&seen](const std::string& line) {
        for (const std::string& note : seen) {
            if (note == line) {
                return true;
            }
        }
        return false;
    };
    CHECK(has("previous"));
    CHECK(has("seek to 1.500 s"));
    CHECK(has("repeat on"));
    CHECK(has("repeat off"));
    std::size_t repeats_on = 0;
    for (const std::string& note : seen) {
        repeats_on += note == "repeat on" ? 1U : 0U;
    }
    CHECK(repeats_on == 1);
}

TEST_CASE("engine queue commands: out-of-range speaker settings are refused and say so",
          "[hearth][engine]") {
    Engine engine(std::make_unique<IdleDevice>(), no_files, stereo(), {}, EngineTiming{.period = 1ms, .budget = 4800});
    engine.set_delay_ms(0, 7.0);
    engine.sync();
    REQUIRE(engine.status().delay_ms.size() == 2);
    CHECK(engine.status().delay_ms[0] == 7.0);

    engine.set_delay_ms(5, 1.0);
    engine.sync();
    CHECK(engine.status().note == "delay refused: slot 5 at 1.0 ms");
    CHECK(engine.status().delay_ms[0] == 7.0);

    engine.set_identify_level_db(1000.0);
    engine.sync();
    CHECK(engine.status().note == "identify level refused: 1000 dB");
}

TEST_CASE("engine queue commands: an engine given its outputs' decisions refuses output choices and ignores a refresh",
          "[hearth][engine]") {
    DiagnosticLog log;
    {
        Engine engine(std::make_unique<IdleDevice>(), no_files, stereo(), {}, EngineTiming{.period = 1ms, .budget = 4800},
                      &log);
        engine.refresh_outputs();
        engine.set_output_preferences({});
        engine.sync();
        CHECK(engine.status().note == "This engine's outputs are chosen by its owner, not here.");
    }
    const auto seen = notes(log);
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.front() == "output choices refused: this engine was given its outputs' decisions");
}

TEST_CASE("engine queue commands: an engine that decides its outputs ignores unchanged choices and rereads on a refresh",
          "[hearth][engine]") {
    DiagnosticLog log;
    {
        EngineOutputs outputs;
        outputs.pcm = std::make_unique<IdleDevice>();
        outputs.endpoints = [](std::uint32_t /*sample_rate*/) { return std::vector<ac3::hearth::EndpointReading>{}; };
        Engine engine(std::move(outputs), no_files, stereo(), {}, EngineTiming{.period = 1ms, .budget = 4800}, &log);
        engine.set_output_preferences({});  // the defaults it already has: nothing to do
        engine.refresh_outputs();
        engine.sync();
    }
    const auto seen = notes(log);
    bool refreshed = false;
    for (const std::string& note : seen) {
        CHECK(note.find("output choices") == std::string::npos);
        refreshed = refreshed || note == "outputs changed: reading them again";
    }
    CHECK(refreshed);
}
