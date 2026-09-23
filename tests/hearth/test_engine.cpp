#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/routing.hpp"
#include "engine_thread.hpp"
#include "pcm_sink.hpp"

// ac3::hearth::Engine (apps/hearth/engine/engine_thread.cpp): the player on a thread
// of its own. The device here has a clock that a second thread runs, as a
// real device's render thread would, so the engine, the device and the test's
// own thread - posting commands and reading snapshots - all run at once.
// Tagged [concurrency]: this is what the TSan leg is for.

using namespace std::chrono_literals;

namespace {

using ac3::hearth::Engine;
using ac3::hearth::EngineStatus;
using ac3::hearth::EngineTiming;
using ac3::hearth::FailurePolicy;
using ac3::hearth::ItemLoader;
using ac3::hearth::LoadedItem;
using ac3::hearth::OpenOutputFormat;
using ac3::hearth::OutputMode;
using ac3::hearth::PcmSink;
using ac3::hearth::PlayPosition;
using ac3::hearth::Player;
using ac3::hearth::QueueItem;
using ac3::hearth::TransportState;

// A device whose clock another thread runs. Everything is under one lock:
// the engine thread submits and reads the position, the clock thread plays.
class ClockedDevice final : public PcmSink {
public:
    struct State {
        mutable std::mutex mutex;
        bool open = false;
        bool paused = false;
        std::uint64_t submitted = 0;
        std::uint64_t heard = 0;
        std::uint64_t clock = 0;
        std::uint64_t submitted_total = 0;
        std::uint64_t heard_total = 0;
        std::uint32_t opens = 0;
        std::uint32_t closes = 0;
        std::uint32_t flushes = 0;
        std::size_t capacity = 16384;
        // Set by lose_at_next_submit(), cleared by the submit that finds it.
        bool losing = false;

        // The device's thread: `frames` more of the clock, what is held
        // heard first.
        void tick(std::uint64_t frames) {
            const std::scoped_lock lock(mutex);
            if (!open || paused) {
                return;
            }
            const std::uint64_t now = std::min(submitted - heard, frames);
            heard += now;
            heard_total += now;
            clock += frames;
        }

        [[nodiscard]] bool is_open() const {
            const std::scoped_lock lock(mutex);
            return open;
        }

        // The device goes away under the next submit, which is refused: the
        // sink stops itself without being closed, as ac3::audio's sinks do.
        // Taken there rather than from this thread so that the engine is in
        // a pump when it happens, and has to pump again to find out.
        void lose_at_next_submit() {
            const std::scoped_lock lock(mutex);
            losing = true;
        }

        [[nodiscard]] std::uint64_t heard_so_far() const {
            const std::scoped_lock lock(mutex);
            return heard_total;
        }
    };

    explicit ClockedDevice(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        const std::scoped_lock lock(state_->mutex);
        state_->open = true;
        state_->paused = false;
        state_->submitted = 0;
        state_->heard = 0;
        state_->clock = 0;
        ++state_->opens;
        return OpenOutputFormat{.sample_rate = format.sample_rate,
                                .channels = static_cast<std::uint16_t>(format.layout.slots()),
                                .mode = OutputMode::kLocalPcm};
    }

    void close() override {
        const std::scoped_lock lock(state_->mutex);
        state_->open = false;
        ++state_->closes;
    }

    [[nodiscard]] bool is_open() const override { return state_->is_open(); }

    bool submit(std::span<const std::span<const float>> /*slots*/, std::size_t frames) override {
        const std::scoped_lock lock(state_->mutex);
        if (state_->losing) {
            state_->losing = false;
            state_->open = false;
        }
        if (!state_->open || state_->submitted - state_->heard + frames > state_->capacity) {
            return false;
        }
        state_->submitted += frames;
        state_->submitted_total += frames;
        return true;
    }

    [[nodiscard]] std::optional<ac3::audio::MonitorPosition> position() const override {
        const std::scoped_lock lock(state_->mutex);
        if (!state_->open) {
            return std::nullopt;
        }
        return ac3::audio::MonitorPosition{.frames_played = state_->clock,
                                           .frames_queued = state_->submitted - state_->heard,
                                           .latency_frames = 0};
    }

    void flush() override {
        const std::scoped_lock lock(state_->mutex);
        ++state_->flushes;
        state_->submitted = 0;
        state_->heard = 0;
        state_->clock = 0;
    }

    bool pause() override {
        const std::scoped_lock lock(state_->mutex);
        state_->paused = true;
        return state_->open;
    }

    bool resume() override {
        const std::scoped_lock lock(state_->mutex);
        state_->paused = false;
        return state_->open;
    }

private:
    std::shared_ptr<State> state_;
};

// The device's render thread: `frames` of the clock every millisecond, a
// hundred times real time by default.
class ClockThread {
public:
    explicit ClockThread(std::shared_ptr<ClockedDevice::State> state, std::uint64_t frames = 4800)
        : thread_([state = std::move(state), frames](const std::stop_token& stop) {
              while (!stop.stop_requested()) {
                  std::this_thread::sleep_for(1ms);
                  state->tick(frames);
              }
          }) {}

private:
    std::jthread thread_;
};

std::vector<std::byte> eac3_stream(int frames) {
    ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    ac3::eac3::FrameEncoder encoder{config};
    std::vector<std::byte> out;
    for (int f = 0; f < frames; ++f) {
        std::vector<float> samples(ac3::kSamplesPerFrame);
        for (std::size_t n = 0; n < samples.size(); ++n) {
            samples[n] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * 440.0 *
                               static_cast<double>(n + (static_cast<std::size_t>(f) * 1536)) /
                               48000.0));
        }
        const std::vector<std::span<const float>> views(2, samples);
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.insert(out.end(), frame->begin(), frame->end());
    }
    return out;
}

// Streams by name. Filled before an engine starts and only read after, so the
// engine thread's loader needs no lock.
struct Library {
    std::map<std::string, std::vector<std::byte>> files;

    [[nodiscard]] ItemLoader loader() const {
        return [this](const std::string& path) -> std::expected<LoadedItem, std::string> {
            const auto found = files.find(path);
            if (found == files.end()) {
                return std::unexpected("no such file: " + path);
            }
            return LoadedItem{.bytes = found->second};
        };
    }
};

QueueItem item(const std::string& path) {
    QueueItem entry;
    entry.path = path;
    entry.title = path;
    return entry;
}

std::unique_ptr<Engine> make_engine(const Library& library,
                                    const std::shared_ptr<ClockedDevice::State>& state) {
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    return std::make_unique<Engine>(std::make_unique<ClockedDevice>(state), library.loader(),
                                    *layout, ac3::hearth::DecoderSettings{},
                                    EngineTiming{.period = 1ms, .budget = 4800});
}

// Polls until `done` holds, or gives up after a generous while.
bool eventually(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return done();
}

}  // namespace

TEST_CASE("engine: a queue plays to its end on the engine's thread while the device's runs",
          "[hearth][concurrency]") {
    Library library;
    library.files["a"] = eac3_stream(8);
    library.files["b"] = eac3_stream(5);
    library.files["c"] = eac3_stream(6);
    auto state = std::make_shared<ClockedDevice::State>();
    const auto engine = make_engine(library, state);
    const ClockThread clock{state};

    engine->add({item("a"), item("b"), item("c")});
    engine->play();
    // Read from this thread the whole time the other two run.
    std::uint64_t reads = 0;
    const bool finished = eventually([&] {
        const EngineStatus status = engine->status();
        static_cast<void>(engine->position());
        ++reads;
        return status.state == TransportState::kStopped && status.history.size() == 3 &&
               !state->is_open();
    });
    REQUIRE(finished);
    CHECK(reads > 0);

    engine->sync();
    const EngineStatus status = engine->status();
    const std::vector<std::uint64_t> expected{8 * 1536, 5 * 1536, 6 * 1536};
    std::uint64_t total = 0;
    for (std::size_t k = 0; k < 3; ++k) {
        INFO("item " << k);
        CHECK(status.history[k].frames == expected[k]);
        CHECK(status.history[k].first_frame == total);
        total += status.history[k].frames;
    }
    CHECK(status.output_opens == 1);
    CHECK(state->heard_so_far() == total);
}

TEST_CASE("engine: commands from several threads all take effect, each thread's in its order",
          "[hearth][concurrency]") {
    Library library;
    library.files["long"] = eac3_stream(40);
    auto state = std::make_shared<ClockedDevice::State>();
    const auto engine = make_engine(library, state);
    // Ten times real time, so the queue is still playing when it is checked.
    const ClockThread clock{state, 480};

    constexpr int kWriters = 4;
    constexpr int kEach = 6;
    std::atomic<bool> writing{true};
    std::vector<std::jthread> threads;
    for (int writer = 0; writer < kWriters; ++writer) {
        threads.emplace_back([&engine, writer] {
            for (int n = 0; n < kEach; ++n) {
                QueueItem entry = item("long");
                entry.title = std::to_string(writer) + "/" + std::to_string(n);
                engine->add({entry});
                if (n == 1) {
                    engine->play();
                }
            }
        });
    }
    // Transport commands, and a settings change, from another thread.
    threads.emplace_back([&engine] {
        for (int n = 0; n < 20; ++n) {
            engine->pause();
            engine->play();
            if (n % 5 == 0) {
                ac3::hearth::DecoderSettings settings;
                settings.mix_levels.loro_clev = 0.5 + (0.01 * n);
                engine->set_decoder_settings(settings);
            }
        }
    });
    // A reader the whole time. Catch2's assertions are not for other threads,
    // so what it sees is kept and checked here.
    std::atomic<bool> went_back{false};
    std::jthread reader([&engine, &writing, &went_back] {
        std::uint64_t last = 0;
        while (writing.load()) {
            const EngineStatus status = engine->status();
            if (status.generation < last) {
                went_back.store(true);
            }
            last = status.generation;
            static_cast<void>(engine->position());
        }
    });
    for (std::jthread& thread : threads) {
        thread.join();
    }
    engine->sync();
    writing.store(false);
    reader.join();
    CHECK_FALSE(went_back.load());

    const EngineStatus status = engine->status();
    REQUIRE(status.queue.size() == static_cast<std::size_t>(kWriters * kEach));
    // Each writer's items keep the order that writer added them in.
    for (int writer = 0; writer < kWriters; ++writer) {
        int expected = 0;
        for (const QueueItem& entry : status.queue) {
            const std::string prefix = std::to_string(writer) + "/";
            if (entry.title.starts_with(prefix)) {
                CHECK(entry.title == prefix + std::to_string(expected));
                ++expected;
            }
        }
        CHECK(expected == kEach);
    }
    CHECK(status.state == TransportState::kPlaying);
    // The last of the four settings changes, made at n == 15.
    CHECK(status.settings.mix_levels.loro_clev == std::optional<double>{0.5 + (0.01 * 15)});
    engine->stop();
    engine->sync();
    CHECK(engine->status().state == TransportState::kStopped);
}

TEST_CASE("engine: sync waits for the effect of every command made before it",
          "[hearth][concurrency]") {
    Library library;
    library.files["long"] = eac3_stream(40);
    auto state = std::make_shared<ClockedDevice::State>();
    const auto engine = make_engine(library, state);
    const ClockThread clock{state};

    CHECK(engine->status().output.sample_rate == 0);
    engine->add({item("long")});
    engine->play();
    engine->pause();
    engine->sync();
    EngineStatus status = engine->status();
    CHECK(status.state == TransportState::kPaused);
    CHECK(status.queue.size() == 1);
    CHECK(state->is_open());
    // What the output is open at.
    CHECK(status.output.sample_rate == 48000);
    CHECK(status.output.channels == 2);
    CHECK(status.output.mode == OutputMode::kLocalPcm);

    engine->play();
    engine->sync();
    CHECK(engine->status().state == TransportState::kPlaying);

    // A command that has something to say says it.
    engine->next();
    engine->sync();
    status = engine->status();
    CHECK_FALSE(status.note.empty());
}

TEST_CASE("engine: the position moves with the device's clock and stands while paused",
          "[hearth][concurrency]") {
    Library library;
    library.files["long"] = eac3_stream(200);
    auto state = std::make_shared<ClockedDevice::State>();
    const auto engine = make_engine(library, state);
    // Ten times real time: the item's 6.4 s must outlast however long this
    // thread is kept waiting before it pauses and plays again.
    const ClockThread clock{state, 480};

    CHECK_FALSE(engine->meters().has_value());
    engine->add({item("long")});
    engine->play();
    REQUIRE(eventually([&] { return engine->position().heard > 50ms; }));
    CHECK(engine->position().item == 0);
    CHECK(engine->position().duration.count() == 200 * 1536 * 1000 / 48000);
    REQUIRE(eventually([&] { return engine->meters().has_value(); }));
    CHECK(engine->meters()->levels.size() == 2);
    REQUIRE(eventually([&] { return engine->unit_report().has_value(); }));
    CHECK(engine->unit_report()->layout.count == 2);

    engine->pause();
    engine->sync();
    // One period for the engine to read the clock after the device paused.
    std::this_thread::sleep_for(20ms);
    const auto paused = engine->position().heard;
    std::this_thread::sleep_for(30ms);
    CHECK(engine->position().heard == paused);

    engine->play();
    REQUIRE(eventually([&] { return engine->position().heard > paused + 20ms; }));

    // Stopped, there is nothing to meter or report, and no output.
    engine->stop();
    engine->sync();
    CHECK_FALSE(engine->meters().has_value());
    CHECK_FALSE(engine->unit_report().has_value());
    CHECK(engine->status().output.sample_rate == 0);
}

TEST_CASE("engine: changes are reported on the engine's thread, every one, in order",
          "[hearth][concurrency]") {
    Library library;
    library.files["short"] = eac3_stream(4);
    auto state = std::make_shared<ClockedDevice::State>();
    // Declared before the engine, whose thread calls back into them until it
    // stops.
    std::mutex seen_mutex;
    std::vector<std::uint64_t> generations;
    std::vector<std::thread::id> threads;
    bool heard_end = false;
    const auto engine = make_engine(library, state);
    const ClockThread clock{state};

    engine->on_change([&](const EngineStatus& status) {
        const std::scoped_lock lock(seen_mutex);
        generations.push_back(status.generation);
        threads.push_back(std::this_thread::get_id());
        heard_end = heard_end ||
                    (status.history.size() == 2 && status.state == TransportState::kStopped);
    });

    engine->add({item("short"), item("short")});
    engine->play();
    // A snapshot is stored before its callback runs, so status() can show the
    // queue's end before the callback has heard it: what the callback heard is
    // waited for. It hears the end twice - the queue finishing, then the
    // output closing once the device has played the last item out - after
    // the commands and the first item's start: three publications at least.
    // The device closes before the last of them is made, so its closing is no
    // sign that the callback has heard everything.
    REQUIRE(eventually([&] {
        const std::scoped_lock lock(seen_mutex);
        return heard_end && generations.size() >= 3;
    }));
    REQUIRE(eventually([&] { return !state->is_open(); }));

    const std::scoped_lock lock(seen_mutex);
    REQUIRE(generations.size() >= 3);
    for (std::size_t k = 1; k < generations.size(); ++k) {
        CHECK(generations[k] == generations[k - 1] + 1);
    }
    for (const std::thread::id id : threads) {
        CHECK(id == threads.front());
        CHECK(id != std::this_thread::get_id());
    }
}

TEST_CASE("engine: a playing engine that goes away stops, and closes its output",
          "[hearth][concurrency]") {
    Library library;
    library.files["long"] = eac3_stream(200);
    auto state = std::make_shared<ClockedDevice::State>();
    const ClockThread clock{state};
    {
        const auto engine = make_engine(library, state);
        engine->add({item("long")});
        engine->play();
        REQUIRE(eventually([&] { return state->heard_so_far() > 0; }));
    }
    CHECK_FALSE(state->is_open());
    const std::scoped_lock lock(state->mutex);
    CHECK(state->closes == 1);
}

TEST_CASE("engine: an output whose device goes away stops playback, and the status says why",
          "[hearth][concurrency]") {
    Library library;
    library.files["long"] = eac3_stream(200);
    auto state = std::make_shared<ClockedDevice::State>();
    const auto engine = make_engine(library, state);
    // Real time, so the item is still playing when its device goes.
    const ClockThread clock{state, 48};
    engine->add({item("long")});
    engine->play();
    // Published as playing first, so a stopped status below is news.
    engine->sync();
    REQUIRE(engine->status().state == TransportState::kPlaying);
    REQUIRE(eventually([&] { return state->heard_so_far() > 0; }));

    state->lose_at_next_submit();
    EngineStatus status;
    REQUIRE(eventually([&] {
        status = engine->status();
        return status.state == TransportState::kStopped;
    }));
    CHECK(status.error == "Playback stopped: the output device went away.");
    CHECK(status.note == status.error);
    CHECK(status.history.size() == 1);
    const std::scoped_lock lock(state->mutex);
    CHECK(state->closes == 1);
}

TEST_CASE("engine: the diagnostics ring hears each command, then what playback did about it",
          "[hearth][concurrency][diagnostics]") {
    Library library;
    library.files["a"] = eac3_stream(4);
    auto state = std::make_shared<ClockedDevice::State>();
    // Outlives the engine, which writes to it until it has stopped.
    ac3::hearth::DiagnosticLog diagnostics;
    ac3::hearth::DecoderSettings rf;
    rf.mode = ac3::OperatingMode::kRf;
    {
        const auto layout = ac3::render::OutputLayout::parse("2.0");
        REQUIRE(layout.has_value());
        const auto engine = std::make_unique<Engine>(
            std::make_unique<ClockedDevice>(state), library.loader(), *layout,
            ac3::hearth::DecoderSettings{}, EngineTiming{.period = 1ms, .budget = 4800},
            &diagnostics);
        const ClockThread clock{state};
        // The window reads the ring whenever it likes.
        std::atomic<bool> reading{true};
        std::jthread reader([&diagnostics, &reading] {
            while (reading.load()) {
                static_cast<void>(diagnostics.lines());
            }
        });

        // An item whose loader names its path when it cannot read it.
        QueueItem gone = item("C:\\Private\\gone.ec3");
        gone.title = "gone";
        engine->add({item("a"), gone});
        engine->set_gapless(false);
        engine->set_gapless(false);
        engine->play();
        REQUIRE(eventually([&] {
            const EngineStatus status = engine->status();
            return status.state == TransportState::kStopped && status.history.size() == 1 &&
                   !state->is_open();
        }));
        // Asked for by hand, the item that cannot be played says why, and
        // where it lives stays out of the ring.
        engine->play_item(1);
        engine->remove(5);
        engine->next();
        engine->set_decoder_settings(rf);
        engine->set_decoder_settings(rf);
        engine->sync();
        reading.store(false);
    }

    std::vector<std::string> notes;
    std::string all;
    for (const std::string& line : diagnostics.lines()) {
        notes.push_back(line.substr(ac3::hearth::DiagnosticLog::kStampBytes));
        all += notes.back() + "\n";
    }
    INFO(all);
    const std::vector<std::string> expected{
        "engine started: layout 2.0 (2 slots), " + describe(ac3::hearth::DecoderSettings{}),
        "add 2 items to a queue of 0",
        "gapless off",
        "play",
        "output opened: local PCM, 48000 Hz, 2 channels (open 1)",
        "item 1 \"a\" started: E-AC-3, 48000 Hz, 2 channels, 0.128 s",
        "item 2 \"gone\" cannot be played: no such file: <withheld>\\gone.ec3",
        "playback ends once the output has played out: The queue has finished.",
        "output closed",
        "play item 2 \"gone\"",
        "transport: \"gone\" cannot be played here: no such file: <withheld>\\gone.ec3",
        "remove item 6 (no such item)",
        "next",
        "transport: Nothing after this in the queue.",
        "decoder settings: " + describe(rf),
        "engine stopped",
    };
    CHECK(notes == expected);
}

TEST_CASE("engine: a restored queue waits at its item and position until asked to play",
          "[hearth][concurrency]") {
    Library library;
    library.files["a"] = eac3_stream(40);
    library.files["b"] = eac3_stream(200);
    auto state = std::make_shared<ClockedDevice::State>();
    // No clock runs, so the device hears nothing and a position is where
    // playback started.
    const auto engine = make_engine(library, state);

    // Whatever was playing stops.
    engine->add({item("a")});
    engine->play();
    engine->set_on_failure(FailurePolicy::kStop);
    engine->restore({item("a"), item("b")}, 1, 3000ms);
    engine->sync();
    EngineStatus status = engine->status();
    CHECK(status.state == TransportState::kStopped);
    CHECK_FALSE(state->is_open());
    REQUIRE(status.queue.size() == 2);
    CHECK(status.queue[1].path == "b");
    CHECK(status.current == 1);
    CHECK(status.on_failure == FailurePolicy::kStop);

    // Asked to play, it starts at the unit that covers the position: 3 s is
    // 144000 samples, in the unit of 1536 that starts at 142848.
    engine->play();
    PlayPosition first;
    REQUIRE(eventually([&] {
        first = engine->position();
        return first.item == 1;
    }));
    CHECK(first.heard == 2976ms);
    engine->stop();

    // A current item the queue does not have is none: the queue starts at
    // its front, from the beginning.
    engine->restore({item("a")}, 5, 1000ms);
    engine->play();
    REQUIRE(eventually([&] {
        first = engine->position();
        return first.item == 0;
    }));
    CHECK(first.heard == 0ms);
    engine->stop();
    engine->sync();
    CHECK(engine->status().current == 0);
}

TEST_CASE("engine: the speaker setup commands reach EngineStatus, and a refused one leaves it "
          "alone but says so",
          "[hearth][engine]") {
    Library library;
    auto state = std::make_shared<ClockedDevice::State>();
    const auto engine = make_engine(library, state);

    engine->set_trim_db(0, -3.0);
    engine->set_delay_ms(0, 5.0);
    engine->set_crossover_hz(100.0);
    engine->identify_start(1);
    engine->set_identify_level_db(-30.0);
    engine->sync();

    const EngineStatus applied = engine->status();
    REQUIRE(applied.trim_db.size() == 2);  // make_engine's layout is "2.0"
    CHECK(applied.trim_db[0] == 3.0 * -1.0);
    CHECK(applied.delay_ms[0] == 5.0);
    CHECK(applied.crossover_hz == 100.0);
    CHECK(applied.identify_slot == 1);
    CHECK(applied.identify_level_db == -30.0);

    // No slot 2 on a 2.0 layout: refused, noted, and nothing changes.
    engine->set_trim_db(2, -1.0);
    engine->sync();
    const EngineStatus after_refusal = engine->status();
    CHECK(after_refusal.note.find("refused") != std::string::npos);
    CHECK(after_refusal.trim_db[0] == -3.0);
    CHECK(after_refusal.trim_db.size() == 2);

    // Same rule for identify: an out-of-range slot is refused, noted, and
    // changes nothing, but identify_stop() always succeeds.
    engine->identify_start(2);
    engine->sync();
    const EngineStatus identify_refused = engine->status();
    CHECK(identify_refused.note.find("refused") != std::string::npos);
    CHECK(identify_refused.identify_slot == 1);
    engine->identify_stop();
    engine->sync();
    CHECK(engine->status().identify_slot == ac3::hearth::Queue::kNone);

    // No PCM sink here supports routing (ClockedDevice, like FakeDevice,
    // takes PcmSink's own inert defaults): refused, and EngineStatus's
    // routing/device facts stay at their own defaults.
    engine->set_routing(ac3::render::Routing{});
    engine->sync();
    const EngineStatus routing_status = engine->status();
    CHECK(routing_status.note.find("refused") != std::string::npos);
    CHECK(routing_status.device_name.empty());
    CHECK(routing_status.speaker_mask == 0);
}

TEST_CASE("engine: the volume command reaches EngineStatus, and a refused one leaves it alone "
          "but says so",
          "[hearth][engine]") {
    Library library;
    auto state = std::make_shared<ClockedDevice::State>();
    const auto engine = make_engine(library, state);

    CHECK(engine->status().volume_db == 0.0);  // unity by default

    engine->set_volume_db(-9.0);
    engine->sync();
    CHECK(engine->status().volume_db == -9.0);

    // Outside [Player::kMinVolumeDb, Player::kMaxVolumeDb]: refused, noted,
    // and nothing changes.
    engine->set_volume_db(Player::kMaxVolumeDb + 1.0);
    engine->sync();
    const EngineStatus after_refusal = engine->status();
    CHECK(after_refusal.note.find("refused") != std::string::npos);
    CHECK(after_refusal.volume_db == -9.0);
}

TEST_CASE("engine: set_layout changes EngineStatus's layout and resets the speaker setup, "
          "and a refused layout leaves it alone",
          "[hearth][engine]") {
    Library library;
    auto state = std::make_shared<ClockedDevice::State>();
    const auto engine = make_engine(library, state);  // "2.0"

    engine->set_trim_db(0, -3.0);
    engine->sync();
    REQUIRE(engine->status().trim_db[0] == -3.0);

    const auto layout = ac3::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    engine->set_layout(*layout);
    engine->sync();
    const EngineStatus after = engine->status();
    CHECK(after.layout.text() == "5.1");
    CHECK(after.trim_db.size() == 6);  // "5.1" is L C R Ls Rs LFE
    CHECK(after.trim_db[0] == 0.0);  // reset: a different speaker under the new layout
    CHECK(after.note.find("refused") == std::string::npos);

    // No slots at all: refused, noted, and the layout stays "5.1".
    engine->set_layout(ac3::render::OutputLayout{});
    engine->sync();
    const EngineStatus refused = engine->status();
    CHECK(refused.note.find("refused") != std::string::npos);
    CHECK(refused.layout.text() == "5.1");
}

TEST_CASE("engine: set_layout while playing reopens the output at the new width, on the "
          "engine's own thread",
          "[hearth][concurrency]") {
    Library library;
    library.files["long"] = eac3_stream(200);
    auto state = std::make_shared<ClockedDevice::State>();
    const auto engine = make_engine(library, state);  // "2.0"
    // Ten times real time, not the default hundred: the item still has to be
    // mid-play by the time this test reads status() well after the reopen -
    // real work (posting the command, sync()'s wait) happens between the
    // eventually() below and that read, and a hundred-times clock could race
    // a short item to its end inside that gap, especially with TSan's own
    // overhead slowing this thread down. Same reasoning as "engine: commands
    // from several threads..." above.
    const ClockThread clock{state, 480};

    engine->add({item("long")});
    engine->play();
    REQUIRE(eventually([&] { return engine->position().heard > 50ms; }));

    const auto layout = ac3::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    engine->set_layout(*layout);
    engine->sync();

    const EngineStatus status = engine->status();
    CHECK(status.layout.text() == "5.1");
    CHECK(status.output.channels == 6);
    CHECK(state->opens == 2);
    // Still playing the one item, through an output that reopened - not
    // stopped, and not a fresh queue position.
    CHECK(status.state == TransportState::kPlaying);
    CHECK(status.current == 0);

    REQUIRE(eventually([&] {
        const EngineStatus final_status = engine->status();
        return final_status.state == TransportState::kStopped && !state->is_open();
    }));
}