#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/render/layout.hpp"
#include "container_input.hpp"
#include "matroska/matroska.hpp"
#include "mp4/mp4.hpp"
#include "pcm_sink.hpp"
#include "player.hpp"

// ac3::hearth::Player (apps/hearth/engine/player.cpp) against a fake device.
//
// A3's exit: "a queue of mixed containers plays to a fake device gaplessly,
// with the expected sample count at every join". The device here has a clock
// of its own - the test advances it - and records what it was given, so the
// whole queue plays deterministically with nothing but the pump loop between
// the items. The streams are encoded in the test and wrapped in MP4 and
// Matroska by the project's own muxers, so nothing depends on a fixture file.

namespace {

using ac3::hearth::ItemLoader;
using ac3::hearth::OpenOutputFormat;
using ac3::hearth::OutputMode;
using ac3::hearth::PcmSink;
using ac3::hearth::Player;
using ac3::hearth::QueueItem;
using ac3::hearth::TransportState;

// A device whose clock the test runs. Like the real backends
// (src/audio/src/backend/*/monitor.cpp), the clock keeps running when there
// is nothing to play, and the silence it runs through counts as played: that
// is what makes an underrun visible in the figures the player reads.
class FakeDevice final : public PcmSink {
public:
    struct Log {
        std::uint32_t opens = 0;
        std::uint32_t closes = 0;
        std::uint32_t flushes = 0;
        std::vector<std::uint32_t> rates{};
        // Since the last open or flush: frames submitted, frames of them
        // heard, and frames the clock has run through, silence included.
        std::uint64_t submitted = 0;
        std::uint64_t heard = 0;
        std::uint64_t clock = 0;
        // Where the clock stood when the last frame submitted was heard, or
        // nothing while some are still held.
        std::optional<std::uint64_t> drained_at{};
        // What the device says its output path adds.
        std::uint32_t latency = 0;
        // Frames submitted over the device's whole life, and a running hash
        // of every sample in them, block by block and slot by slot - equal
        // for two runs only if the same audio arrived in the same order.
        std::uint64_t submitted_total = 0;
        std::uint64_t hash = 14695981039346656037ULL;
        // At each open: the frames of the old output that had been heard,
        // and how far the clock had run past the last of them - for a
        // reopen, proof that the old item was heard out first.
        std::vector<std::uint64_t> heard_when_opened{};
        std::vector<std::uint64_t> slack_when_opened{};
        bool open = false;
        bool paused = false;
        bool refuse_open = false;
    };

    FakeDevice(std::shared_ptr<Log> log, std::size_t capacity)
        : log_(std::move(log)), capacity_(capacity) {}

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        if (log_->refuse_open) {
            return std::unexpected(std::string{"The fake device refused to open."});
        }
        log_->heard_when_opened.push_back(log_->heard);
        log_->slack_when_opened.push_back(log_->drained_at ? log_->clock - *log_->drained_at : 0);
        ++log_->opens;
        log_->rates.push_back(format.sample_rate);
        log_->open = true;
        log_->paused = false;
        restart();
        width_ = static_cast<std::uint16_t>(format.layout.slots());
        return OpenOutputFormat{.sample_rate = format.sample_rate,
                                .channels = width_,
                                .mode = OutputMode::kLocalPcm};
    }

    void close() override {
        ++log_->closes;
        log_->open = false;
    }

    [[nodiscard]] bool is_open() const override { return log_->open; }

    bool submit(std::span<const std::span<const float>> slots, std::size_t frames) override {
        if (!log_->open || log_->submitted - log_->heard + frames > capacity_) {
            return false;
        }
        REQUIRE(slots.size() == width_);
        for (const auto slot : slots) {
            REQUIRE(slot.size() == frames);
            for (const float sample : slot) {
                const auto bits = std::bit_cast<std::uint32_t>(sample);
                for (int shift = 0; shift < 32; shift += 8) {
                    log_->hash = (log_->hash ^ ((bits >> shift) & 0xFFU)) * 1099511628211ULL;
                }
            }
        }
        log_->submitted += frames;
        log_->submitted_total += frames;
        log_->drained_at.reset();
        return true;
    }

    [[nodiscard]] std::optional<ac3::audio::MonitorPosition> position() const override {
        if (!log_->open) {
            return std::nullopt;
        }
        return ac3::audio::MonitorPosition{.frames_played = log_->clock,
                                           .frames_queued = log_->submitted - log_->heard,
                                           .latency_frames = log_->latency};
    }

    void flush() override {
        ++log_->flushes;
        restart();
    }

    bool pause() override {
        log_->paused = true;
        return log_->open;
    }

    bool resume() override {
        log_->paused = false;
        return log_->open;
    }

private:
    void restart() {
        log_->submitted = 0;
        log_->heard = 0;
        log_->clock = 0;
        log_->drained_at.reset();
    }

    std::shared_ptr<Log> log_;
    std::size_t capacity_;
    std::uint16_t width_ = 0;
};

// The device's clock runs on by `frames`: what it holds is heard first, and
// the rest of the time is silence.
void advance(FakeDevice::Log& log, std::uint64_t frames) {
    if (!log.open || log.paused) {
        return;
    }
    const std::uint64_t held = log.submitted - log.heard;
    const std::uint64_t heard = std::min(held, frames);
    if (held != 0 && heard == held) {
        log.drained_at = log.clock + heard;
    }
    log.heard += heard;
    log.clock += frames;
}

std::vector<float> tone(double hz, std::uint32_t rate, std::size_t offset) {
    std::vector<float> out(ac3::kSamplesPerFrame);
    for (std::size_t n = 0; n < out.size(); ++n) {
        out[n] = static_cast<float>(
            0.3 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n + offset) / rate));
    }
    return out;
}

std::vector<std::byte> eac3_stream(int frames, ac3::SampleRate rate = ac3::SampleRate::k48000) {
    ac3::eac3::FrameConfig config;
    config.sample_rate = rate;
    config.bitrate_kbps = 384;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = true;
    ac3::eac3::FrameEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::byte> out;
    for (int f = 0; f < frames; ++f) {
        const auto samples = tone(440.0, ac3::sample_rate_hz(rate),
                                  static_cast<std::size_t>(f) * ac3::kSamplesPerFrame);
        const std::vector<std::span<const float>> views(channels, samples);
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.insert(out.end(), frame->begin(), frame->end());
    }
    return out;
}

std::vector<std::byte> ac3_stream(int frames, ac3::SampleRate rate = ac3::SampleRate::k48000) {
    ac3::EncoderConfig config;
    config.sample_rate = rate;
    config.bitrate_kbps = 384;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = true;
    ac3::FrameEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::byte> out;
    for (int f = 0; f < frames; ++f) {
        const auto samples = tone(660.0, ac3::sample_rate_hz(rate),
                                  static_cast<std::size_t>(f) * ac3::kSamplesPerFrame);
        const std::vector<std::span<const float>> views(channels, samples);
        const auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.insert(out.end(), frame->begin(), frame->end());
    }
    return out;
}

// A stream wrapped the way the container writers wrap one.
std::vector<std::byte> in_mp4(const std::vector<std::byte>& stream) {
    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    mp4::AudioTrack track;
    track.codec_id = std::string{scanned->kind == ac3::io::StreamKind::kAc3 ? mp4::kCodecAc3
                                                                            : mp4::kCodecEac3};
    track.sample_rate = ac3::sample_rate_hz(scanned->sample_rate);
    track.channels = scanned->channels;
    track.codec_config = ac3::io::build_codec_config_box(*scanned);
    const auto muxed = mp4::mux(track, std::span<const std::span<const std::byte>>(scanned->access_units));
    REQUIRE(muxed.has_value());
    return *muxed;
}

std::vector<std::byte> in_mkv(const std::vector<std::byte>& stream) {
    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    matroska::AudioTrack track;
    track.codec_id = std::string{scanned->kind == ac3::io::StreamKind::kAc3 ? matroska::kCodecAc3
                                                                            : matroska::kCodecEac3};
    track.sample_rate = ac3::sample_rate_hz(scanned->sample_rate);
    track.channels = scanned->channels;
    const auto muxed =
        matroska::mux(track, std::span<const std::span<const std::byte>>(scanned->access_units));
    REQUIRE(muxed.has_value());
    return *muxed;
}

// "Files" in memory, read the way the application reads a real one: the
// container, if any, is demuxed by apps/common's container input.
struct Library {
    std::map<std::string, std::vector<std::byte>> files;

    [[nodiscard]] ItemLoader loader() const {
        return [this](const std::string& path) -> std::expected<std::vector<std::byte>, std::string> {
            const auto found = files.find(path);
            if (found == files.end()) {
                return std::unexpected("no such file: " + path);
            }
            auto stream = ac3::apps::elementary_stream_from_bytes(found->second);
            if (!stream.error.empty()) {
                return std::unexpected(stream.error);
            }
            return std::move(stream.bytes);
        };
    }
};

QueueItem item(const std::string& path) {
    QueueItem entry;
    entry.path = path;
    entry.title = path;
    return entry;
}

// Plays until the queue has finished and the output has closed, advancing
// the device's clock a period per pump. Returns false if it never finishes.
bool play_out(Player& player, FakeDevice::Log& log, std::size_t period = 480) {
    for (int step = 0; step < 200000; ++step) {
        player.pump();
        advance(log, period);
        if (player.transport().state() == TransportState::kStopped && !log.open) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<Player> make_player(const Library& library, const std::shared_ptr<FakeDevice::Log>& log,
                                    std::size_t capacity = 8192) {
    const auto layout = ac3::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    return std::make_unique<Player>(std::make_unique<FakeDevice>(log, capacity), library.loader(),
                                    *layout);
}

}  // namespace

TEST_CASE("player: a queue of mixed containers plays gaplessly, every join exactly where it belongs",
          "[hearth][player]") {
    Library library;
    library.files["one.ec3"] = eac3_stream(24);
    library.files["two.mp4"] = in_mp4(eac3_stream(17));
    library.files["three.mkv"] = in_mkv(ac3_stream(9));
    library.files["four.ac3"] = ac3_stream(13);

    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log);
    for (const char* path : {"one.ec3", "two.mp4", "three.mkv", "four.ac3"}) {
        player->queue().add(item(path));
    }

    REQUIRE(player->play().state == TransportState::kPlaying);
    REQUIRE(play_out(*player, *log));

    // One output for the whole queue: every item is 48 kHz and decoded to the
    // same layout, so each one joined the last rather than reopening.
    CHECK(log->opens == 1);
    CHECK(player->output_opens() == 1);
    // Closed once, at the end, after everything had been heard.
    CHECK(log->closes == 1);

    const auto& history = player->history();
    REQUIRE(history.size() == 4);
    const std::vector<std::uint64_t> expected{24 * 1536, 17 * 1536, 9 * 1536, 13 * 1536};
    std::uint64_t total = 0;
    for (std::size_t k = 0; k < history.size(); ++k) {
        INFO("item " << k << ": " << history[k].title);
        // Every sample of every item, and no more.
        CHECK(history[k].expected_frames == expected[k]);
        CHECK(history[k].frames == expected[k]);
        // And each one starts exactly where the one before it ended: no
        // silence inserted, nothing overlapping, at every join.
        CHECK(history[k].first_frame == total);
        CHECK(history[k].output_opens == 1);
        total += history[k].frames;
    }
    CHECK(log->submitted_total == total);
    CHECK(log->heard == total);

    // And the same audio, sample for sample, as the same queue played with an
    // output of its own for every item: the joins added, dropped and changed
    // nothing.
    auto separate = std::make_shared<FakeDevice::Log>();
    const auto reference = make_player(library, separate);
    reference->set_gapless(false);
    for (const char* path : {"one.ec3", "two.mp4", "three.mkv", "four.ac3"}) {
        reference->queue().add(item(path));
    }
    reference->play();
    REQUIRE(play_out(*reference, *separate));
    CHECK(separate->opens == 4);
    CHECK(separate->submitted_total == total);
    CHECK(separate->hash == log->hash);
}

TEST_CASE("player: a rate change reopens the output, after the old item has been heard",
          "[hearth][player]") {
    Library library;
    library.files["48k.ec3"] = eac3_stream(10, ac3::SampleRate::k48000);
    library.files["44k1.ac3"] = ac3_stream(8, ac3::SampleRate::k44100);

    auto log = std::make_shared<FakeDevice::Log>();
    // An output path that holds on to the audio for a while after the
    // device has played it: the reopen waits for that as well. Longer than
    // the clock's step in play_out(), so a reopen that ignored it could not
    // land this far past the last frame by chance.
    log->latency = 1000;
    const auto player = make_player(library, log);
    player->queue().add(item("48k.ec3"));
    player->queue().add(item("44k1.ac3"));

    player->play();
    REQUIRE(play_out(*player, *log));

    CHECK(log->opens == 2);
    CHECK(log->rates == std::vector<std::uint32_t>{48000, 44100});
    // The second open waited for every frame of the first item to be heard,
    // and for the output path's delay after the last of them.
    REQUIRE(log->heard_when_opened.size() == 2);
    CHECK(log->heard_when_opened[1] == 10 * 1536);
    CHECK(log->slack_when_opened[1] >= 1000);

    const auto& history = player->history();
    REQUIRE(history.size() == 2);
    CHECK(history[0].frames == 10 * 1536);
    CHECK(history[1].frames == 8 * 1536);
    // A new output starts a new timeline.
    CHECK(history[1].first_frame == 0);
    CHECK(history[1].output_opens == 2);
}

TEST_CASE("player: an underrun before a reopen does not cut the old item short",
          "[hearth][player]") {
    Library library;
    library.files["48k.ec3"] = eac3_stream(10, ac3::SampleRate::k48000);
    library.files["44k1.ac3"] = ac3_stream(8, ac3::SampleRate::k44100);

    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log);
    player->queue().add(item("48k.ec3"));
    player->queue().add(item("44k1.ac3"));

    player->play();
    player->pump();
    // The engine stalls for a second while the device runs on: it plays what
    // it was given and then a long stretch of silence, all of which its clock
    // counts. A player that took "the clock has passed every frame submitted"
    // as "heard" would now reopen with most of the first item unheard.
    advance(*log, 48000);
    REQUIRE(log->clock > 10 * 1536);
    REQUIRE(log->heard < 10 * 1536);

    REQUIRE(play_out(*player, *log));
    REQUIRE(log->heard_when_opened.size() == 2);
    CHECK(log->heard_when_opened[1] == 10 * 1536);
    CHECK(player->history()[0].frames == 10 * 1536);
}

TEST_CASE("player: the block ring grows without reordering what it holds", "[hearth][player]") {
    Library library;
    library.files["long.ec3"] = eac3_stream(40);

    // The reference: ordinary pumps into an ordinary device.
    auto plain = std::make_shared<FakeDevice::Log>();
    const auto reference = make_player(library, plain);
    reference->queue().add(item("long.ec3"));
    reference->play();
    REQUIRE(play_out(*reference, *plain));

    // A device that takes one block at a time, and pumps that ask for more
    // each time: the ring fills while its oldest block moves round it, so it
    // has to grow from part-way round - three times over.
    auto narrow = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, narrow, 256);
    player->queue().add(item("long.ec3"));
    player->play();
    bool finished = false;
    for (std::size_t step = 1; step < 20000 && !finished; ++step) {
        player->pump(512 * step);
        advance(*narrow, 480);
        finished = player->transport().state() == TransportState::kStopped && !narrow->open;
    }
    REQUIRE(finished);
    CHECK(narrow->submitted_total == 40 * 1536);
    CHECK(narrow->hash == plain->hash);
}

TEST_CASE("player: with gapless off, every item gets an output of its own", "[hearth][player]") {
    Library library;
    library.files["a.ec3"] = eac3_stream(6);
    library.files["b.ec3"] = eac3_stream(6);
    library.files["c.ec3"] = eac3_stream(6);

    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log);
    player->set_gapless(false);
    for (const char* path : {"a.ec3", "b.ec3", "c.ec3"}) {
        player->queue().add(item(path));
    }

    player->play();
    REQUIRE(play_out(*player, *log));
    CHECK(log->opens == 3);
    CHECK(log->heard_when_opened == std::vector<std::uint64_t>{0, 6 * 1536, 6 * 1536});
    for (const auto& played : player->history()) {
        CHECK(played.frames == 6 * 1536);
        CHECK(played.first_frame == 0);
    }
}

TEST_CASE("player: an item that cannot be read is skipped, and says why", "[hearth][player]") {
    Library library;
    library.files["good.ec3"] = eac3_stream(5);
    library.files["noise.ec3"] = std::vector<std::byte>(4096, std::byte{0x33});
    library.files["also-good.ec3"] = eac3_stream(5);

    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log);
    for (const char* path : {"good.ec3", "noise.ec3", "missing.ec3", "also-good.ec3"}) {
        player->queue().add(item(path));
    }

    player->play();
    REQUIRE(play_out(*player, *log));

    // The two readable items played, and joined across the two that were not.
    const auto& history = player->history();
    REQUIRE(history.size() == 2);
    CHECK(history[0].title == "good.ec3");
    CHECK(history[1].title == "also-good.ec3");
    CHECK(history[1].first_frame == history[0].frames);
    CHECK(log->opens == 1);

    // The two that were not are marked, with the reason kept for the list.
    const auto items = player->queue().items();
    CHECK_FALSE(items[1].playable());
    CHECK_FALSE(items[1].facts.unplayable_because.empty());
    CHECK_FALSE(items[2].playable());
    CHECK(items[2].facts.unplayable_because.find("missing.ec3") != std::string::npos);
}

TEST_CASE("player: a device that will not open stops playback without blaming the item",
          "[hearth][player]") {
    Library library;
    library.files["fine.ec3"] = eac3_stream(4);

    auto log = std::make_shared<FakeDevice::Log>();
    log->refuse_open = true;
    const auto player = make_player(library, log);
    player->queue().add(item("fine.ec3"));

    player->play();
    CHECK(player->transport().state() == TransportState::kStopped);
    CHECK(player->last_error().find("refused") != std::string::npos);
    // The item is still playable: it was the device that said no.
    CHECK(player->queue().items()[0].playable());

    // And once the device will open, the same item plays.
    log->refuse_open = false;
    player->play();
    REQUIRE(play_out(*player, *log));
    REQUIRE(player->history().size() == 1);
    CHECK(player->history()[0].frames == 4 * 1536);
}

TEST_CASE("player: pause holds the output still, and seek drops what was queued",
          "[hearth][player]") {
    Library library;
    library.files["long.ec3"] = eac3_stream(40);

    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log);
    player->queue().add(item("long.ec3"));

    player->play();
    for (int step = 0; step < 20; ++step) {
        player->pump();
        advance(*log, 480);
    }
    REQUIRE(log->submitted > 0);

    // Paused: nothing more is submitted, and the device's clock stands.
    player->pause();
    CHECK(log->paused);
    const std::uint64_t submitted = log->submitted;
    const std::uint64_t clock = log->clock;
    for (int step = 0; step < 20; ++step) {
        CHECK(player->pump().frames_submitted == 0);
        advance(*log, 480);
    }
    CHECK(log->submitted == submitted);
    CHECK(log->clock == clock);

    // Resumed, then sought: the device is flushed and the stream restarts at
    // the unit covering the new position.
    player->play();
    CHECK_FALSE(log->paused);
    player->seek(std::chrono::milliseconds{500});
    CHECK(log->flushes == 1);
    CHECK(log->submitted == 0);

    REQUIRE(play_out(*player, *log));
    // From 500 ms on: the unit covering sample 24000 is unit 15, so 25 units
    // of 40 are left.
    CHECK(log->submitted_total == submitted + (25 * 1536));
}

TEST_CASE("player: a seek made while stopped lands when that item starts, and only then",
          "[hearth][player]") {
    Library library;
    library.files["first.ec3"] = eac3_stream(40);
    library.files["second.ec3"] = eac3_stream(12);

    SECTION("the item it was made on starts there") {
        auto log = std::make_shared<FakeDevice::Log>();
        const auto player = make_player(library, log);
        player->queue().add(item("first.ec3"));
        player->queue().add(item("second.ec3"));

        // Nothing is open, so there is nothing to flush: the position waits.
        player->seek(std::chrono::milliseconds{500});
        CHECK(log->opens == 0);
        player->play();
        REQUIRE(play_out(*player, *log));

        // Unit 15 of 40 onwards for the first item, all of the second.
        const auto& history = player->history();
        REQUIRE(history.size() == 2);
        CHECK(history[0].frames == 25 * 1536);
        CHECK(history[1].frames == 12 * 1536);
        CHECK(history[1].first_frame == 25 * 1536);
    }

    SECTION("another item starting drops it") {
        auto log = std::make_shared<FakeDevice::Log>();
        const auto player = make_player(library, log);
        player->queue().add(item("first.ec3"));
        player->queue().add(item("second.ec3"));

        player->seek(std::chrono::milliseconds{500});
        // Next, from stopped, starts the second item - from its beginning,
        // since the kept position was the first item's.
        player->next();
        REQUIRE(play_out(*player, *log));
        REQUIRE(player->history().size() == 1);
        CHECK(player->history()[0].title == "second.ec3");
        CHECK(player->history()[0].frames == 12 * 1536);
    }
}
