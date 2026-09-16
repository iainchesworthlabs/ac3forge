#include <catch2/catch_approx.hpp>
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

using ac3::hearth::DecoderSettings;
using ac3::hearth::ItemLoader;
using ac3::hearth::LoadedItem;
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
        // With `keep` set, every sample submitted, one vector per slot,
        // emptied by a flush.
        bool keep = false;
        std::vector<std::vector<float>> kept{};
        // At each open: the frames of the old output that had been heard,
        // and how far the clock had run past the last of them - for a
        // reopen, proof that the old item was heard out first.
        std::vector<std::uint64_t> heard_when_opened{};
        std::vector<std::uint64_t> slack_when_opened{};
        bool open = false;
        bool paused = false;
        bool refuse_open = false;
        bool refuse_pause = false;
        // A device that plays what it is given as soon as it has it: the
        // player is always behind it, as a slow machine can leave it.
        bool instant = false;
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
        if (log_->keep) {
            log_->kept.resize(std::max(log_->kept.size(), slots.size()));
            for (std::size_t slot = 0; slot < slots.size(); ++slot) {
                log_->kept[slot].insert(log_->kept[slot].end(), slots[slot].begin(),
                                        slots[slot].end());
            }
        }
        log_->submitted += frames;
        log_->submitted_total += frames;
        log_->drained_at.reset();
        if (log_->instant && !log_->paused) {
            log_->heard += frames;
            log_->clock += frames;
            log_->drained_at = log_->clock;
        }
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
        for (std::vector<float>& slot : log_->kept) {
            slot.clear();
        }
    }

    bool pause() override {
        if (log_->refuse_pause) {
            return false;
        }
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

std::vector<float> tone(double hz, std::uint32_t rate, std::size_t offset, double level = 0.3) {
    std::vector<float> out(ac3::kSamplesPerFrame);
    for (std::size_t n = 0; n < out.size(); ++n) {
        out[n] = static_cast<float>(
            level * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n + offset) / rate));
    }
    return out;
}

// `dither` off where a test compares a decode that started part-way through
// the stream - after a seek or a change of settings - with an unbroken one
// sample for sample. §7.3.4's dither generator runs on across frames, so a
// decoder that starts late draws different values for the same bins; the
// difference is some 95 dB down, but it is not zero.
std::vector<std::byte> eac3_stream(int frames, ac3::SampleRate rate = ac3::SampleRate::k48000,
                                   bool dither = true, double level = 0.3, int dialnorm = 31) {
    ac3::eac3::FrameConfig config;
    config.sample_rate = rate;
    config.bitrate_kbps = 384;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = true;
    config.dither = dither;
    config.dialnorm = dialnorm;
    ac3::eac3::FrameEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    std::vector<std::byte> out;
    for (int f = 0; f < frames; ++f) {
        const auto samples = tone(440.0, ac3::sample_rate_hz(rate),
                                  static_cast<std::size_t>(f) * ac3::kSamplesPerFrame, level);
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

// A stream wrapped the way the container writers wrap one, with an edit list
// when `edit` is given.
std::vector<std::byte> in_mp4(const std::vector<std::byte>& stream,
                              std::optional<mp4::MuxOptions::Edit> edit = std::nullopt) {
    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    mp4::AudioTrack track;
    track.codec_id = std::string{scanned->kind == ac3::io::StreamKind::kAc3 ? mp4::kCodecAc3
                                                                            : mp4::kCodecEac3};
    track.sample_rate = ac3::sample_rate_hz(scanned->sample_rate);
    track.channels = scanned->channels;
    track.codec_config = ac3::io::build_codec_config_box(*scanned);
    mp4::MuxOptions options;
    options.edit = edit;
    const auto muxed = mp4::mux(
        track, std::span<const std::span<const std::byte>>(scanned->access_units), options);
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
        return [this](const std::string& path) -> std::expected<LoadedItem, std::string> {
            const auto found = files.find(path);
            if (found == files.end()) {
                return std::unexpected("no such file: " + path);
            }
            auto stream = ac3::apps::elementary_stream_from_bytes(found->second);
            if (!stream.error.empty()) {
                return std::unexpected(stream.error);
            }
            return LoadedItem{.bytes = std::move(stream.bytes),
                              .skip_samples = stream.trim.start,
                              .play_samples = stream.trim.length,
                              .note = std::move(stream.trim_note)};
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
                                    std::size_t capacity = 8192, const char* layout_name = "5.1",
                                    const DecoderSettings& settings = {}) {
    const auto layout = ac3::render::OutputLayout::parse(layout_name);
    REQUIRE(layout.has_value());
    return std::make_unique<Player>(std::make_unique<FakeDevice>(log, capacity), library.loader(),
                                    *layout, settings);
}

using Slots = std::vector<std::vector<float>>;

// What one item gives, every sample of it, played on its own.
Slots played_alone(const Library& library, const std::string& path, const char* layout_name = "5.1",
                   const DecoderSettings& settings = {}) {
    auto log = std::make_shared<FakeDevice::Log>();
    log->keep = true;
    const auto player = make_player(library, log, 8192, layout_name, settings);
    player->queue().add(item(path));
    player->play();
    REQUIRE(play_out(*player, *log));
    return log->kept;
}

// A stereo E-AC-3 programme of `frames` frames of one tone, as independent
// substream `substreamid`.
std::vector<std::vector<std::byte>> programme_frames(int frames, double hz, int substreamid) {
    ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    config.substreamid = substreamid;
    config.dither = false;  // see eac3_stream()
    ac3::eac3::FrameEncoder encoder{config};
    std::vector<std::vector<std::byte>> out;
    for (int f = 0; f < frames; ++f) {
        const auto samples = tone(hz, 48000, static_cast<std::size_t>(f) * ac3::kSamplesPerFrame);
        const std::vector<std::span<const float>> views(2, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

std::vector<std::byte> joined_frames(const std::vector<std::vector<std::byte>>& frames) {
    std::vector<std::byte> out;
    for (const auto& frame : frames) {
        out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}

// Two programmes in one stream, a frame of each per period: a 440 Hz one as
// independent substream 0 and a 1 kHz one as substream 1.
std::vector<std::byte> two_programmes(int frames) {
    const auto first = programme_frames(frames, 440.0, 0);
    const auto second = programme_frames(frames, 1000.0, 1);
    std::vector<std::byte> out;
    for (std::size_t f = 0; f < first.size(); ++f) {
        out.insert(out.end(), first[f].begin(), first[f].end());
        out.insert(out.end(), second[f].begin(), second[f].end());
    }
    return out;
}

// `count` frames of `slots` from `from`, slot by slot.
Slots part(const Slots& slots, std::size_t from, std::size_t count) {
    Slots out;
    for (const std::vector<float>& slot : slots) {
        REQUIRE(from + count <= slot.size());
        out.emplace_back(std::next(slot.begin(), static_cast<std::ptrdiff_t>(from)),
                         std::next(slot.begin(), static_cast<std::ptrdiff_t>(from + count)));
    }
    return out;
}

Slots joined(const Slots& first, const Slots& second) {
    REQUIRE(first.size() == second.size());
    Slots out = first;
    for (std::size_t slot = 0; slot < out.size(); ++slot) {
        out[slot].insert(out[slot].end(), second[slot].begin(), second[slot].end());
    }
    return out;
}

// Where two runs first differ, as slot * 1'000'000'000 + frame, or kSame -
// a number rather than the vectors, so a failure does not print megabytes.
constexpr std::size_t kSame = static_cast<std::size_t>(-1);

// The largest difference between two runs of the same length.
double max_difference(const Slots& a, const Slots& b) {
    REQUIRE(a.size() == b.size());
    double worst = 0.0;
    for (std::size_t slot = 0; slot < a.size(); ++slot) {
        REQUIRE(a[slot].size() == b[slot].size());
        for (std::size_t frame = 0; frame < a[slot].size(); ++frame) {
            worst = std::max(worst, std::abs(static_cast<double>(a[slot][frame]) -
                                             static_cast<double>(b[slot][frame])));
        }
    }
    return worst;
}

std::size_t first_difference(const Slots& a, const Slots& b) {
    if (a.size() != b.size()) {
        return 0;
    }
    for (std::size_t slot = 0; slot < a.size(); ++slot) {
        const std::size_t common = std::min(a[slot].size(), b[slot].size());
        for (std::size_t frame = 0; frame < common; ++frame) {
            if (std::bit_cast<std::uint32_t>(a[slot][frame]) !=
                std::bit_cast<std::uint32_t>(b[slot][frame])) {
                return (slot * 1'000'000'000) + frame;
            }
        }
        if (a[slot].size() != b[slot].size()) {
            return (slot * 1'000'000'000) + common;
        }
    }
    return kSame;
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
    // And no longer than that, give or take the pumps it takes to notice.
    CHECK(log->slack_when_opened[1] < 1000 + (3 * 480));

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

// Edit lists. The priming and padding an MP4's edit list names are decoded -
// the decoder needs them - and never played; what does play is sample for
// sample the same stretch of an untrimmed decode.

TEST_CASE("player: an MP4 edit list's priming and padding are decoded but not played",
          "[hearth][player]") {
    const std::vector<std::byte> stream = eac3_stream(12);
    const std::uint64_t kept = (12 * 1536) - 256 - 1000;
    Library library;
    library.files["raw.ec3"] = stream;
    library.files["edited.mp4"] = in_mp4(stream, mp4::MuxOptions::Edit{.start_samples = 256,
                                                                       .duration_samples = kept});

    const Slots whole = played_alone(library, "raw.ec3");

    auto log = std::make_shared<FakeDevice::Log>();
    log->keep = true;
    const auto player = make_player(library, log);
    player->queue().add(item("edited.mp4"));
    player->play();
    // The duration the queue shows is the edited one.
    REQUIRE(player->queue().items()[0].facts.duration.has_value());
    CHECK(player->queue().items()[0].facts.duration->count() == static_cast<std::int64_t>(kept * 1000 / 48000));
    REQUIRE(play_out(*player, *log));

    REQUIRE(player->history().size() == 1);
    CHECK(player->history()[0].expected_frames == kept);
    CHECK(player->history()[0].frames == kept);
    CHECK(first_difference(log->kept, part(whole, 256, kept)) == kSame);
}

TEST_CASE("player: two edited items join with nothing of either encoder's between them",
          "[hearth][player]") {
    const std::vector<std::byte> first = eac3_stream(10);
    const std::vector<std::byte> second = ac3_stream(8);
    const std::uint64_t first_kept = (10 * 1536) - 256 - 700;
    const std::uint64_t second_kept = (8 * 1536) - 256 - 300;
    Library library;
    library.files["first.ec3"] = first;
    library.files["second.ac3"] = second;
    library.files["first.mp4"] = in_mp4(
        first, mp4::MuxOptions::Edit{.start_samples = 256, .duration_samples = first_kept});
    library.files["second.mp4"] = in_mp4(
        second, mp4::MuxOptions::Edit{.start_samples = 256, .duration_samples = second_kept});

    const Slots expected =
        joined(part(played_alone(library, "first.ec3"), 256, first_kept),
               part(played_alone(library, "second.ac3"), 256, second_kept));

    auto log = std::make_shared<FakeDevice::Log>();
    log->keep = true;
    const auto player = make_player(library, log);
    player->queue().add(item("first.mp4"));
    player->queue().add(item("second.mp4"));
    player->play();
    REQUIRE(play_out(*player, *log));

    CHECK(log->opens == 1);
    const auto& history = player->history();
    REQUIRE(history.size() == 2);
    CHECK(history[0].frames == first_kept);
    CHECK(history[1].first_frame == first_kept);
    CHECK(history[1].frames == second_kept);
    CHECK(first_difference(log->kept, expected) == kSame);
}

TEST_CASE("player: a seek in an edited item counts from what the item plays", "[hearth][player]") {
    const std::vector<std::byte> stream = eac3_stream(40, ac3::SampleRate::k48000, /*dither=*/false);
    const std::uint64_t kept = (40 * 1536) - 256 - 512;
    Library library;
    library.files["raw.ec3"] = stream;
    library.files["edited.mp4"] = in_mp4(stream, mp4::MuxOptions::Edit{.start_samples = 256,
                                                                       .duration_samples = kept});
    const Slots whole = played_alone(library, "raw.ec3");

    auto log = std::make_shared<FakeDevice::Log>();
    log->keep = true;
    const auto player = make_player(library, log);
    player->queue().add(item("edited.mp4"));
    player->play();
    for (int step = 0; step < 10; ++step) {
        player->pump();
        advance(*log, 480);
    }

    SECTION("back to the start plays the priming no more than the first time") {
        player->seek(std::chrono::milliseconds{0});
        REQUIRE(play_out(*player, *log));
        CHECK(first_difference(log->kept, part(whole, 256, kept)) == kSame);
    }

    SECTION("a position lands on the unit covering it, counted past the priming") {
        // 507 ms in is 24,336 samples into what the item plays, so stream
        // sample 24,592: inside unit 16 (24,576 to 26,112), where a seek that
        // forgot the priming would have landed in unit 15. The unit plays from
        // its start to the edit's end.
        player->seek(std::chrono::milliseconds{507});
        REQUIRE(play_out(*player, *log));
        const std::size_t from = 16 * 1536;
        // The decoder was primed with unit 15, so even unit 16's first block
        // is what an unbroken decode gives.
        CHECK(first_difference(log->kept, part(whole, from, 256 + kept - from)) == kSame);
    }
}

TEST_CASE("player: what a loader says about an item is kept, and an item with nothing left is skipped",
          "[hearth][player]") {
    const std::vector<std::byte> stream = eac3_stream(4);
    auto log = std::make_shared<FakeDevice::Log>();
    const auto layout = ac3::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    const ItemLoader loader = [&stream](const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (path == "noted") {
            return LoadedItem{.bytes = stream,
                              .skip_samples = 0,
                              .play_samples = std::nullopt,
                              .note = "The file's edit list has 2 edits with audio in them."};
        }
        // Everything skipped: an edit list that leaves nothing to play.
        return LoadedItem{.bytes = stream,
                          .skip_samples = 4 * 1536,
                          .play_samples = std::nullopt,
                          .note = {}};
    };
    Player player{std::make_unique<FakeDevice>(log, 8192), loader, *layout};
    player.queue().add(item("noted"));
    player.queue().add(item("empty"));
    player.queue().add(item("noted"));

    player.play();
    CHECK(player.queue().items()[0].facts.note.find("2 edits") != std::string::npos);
    std::vector<std::string> notes;
    for (int step = 0; step < 200000; ++step) {
        const auto report = player.pump();
        if (report.item_started) {
            notes.push_back(report.note);
        }
        advance(*log, 480);
        if (player.transport().state() == TransportState::kStopped && !log->open) {
            break;
        }
    }
    REQUIRE(player.history().size() == 2);
    CHECK(player.history()[1].first_frame == 4 * 1536);
    CHECK_FALSE(player.queue().items()[1].playable());
    CHECK(player.queue().items()[1].facts.unplayable_because.find("nothing left to play") !=
          std::string::npos);
    // The joining item's note came with the pump that started it.
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("2 edits") != std::string::npos);
}

// Decoder settings. A 5.1 programme into a two-speaker room, so a change is
// one the fold makes audible: the centre's level in it.

TEST_CASE("player: a settings change reaches the playing item at a unit, losing and repeating "
          "nothing",
          "[hearth][player]") {
    Library library;
    library.files["long.ec3"] = eac3_stream(30, ac3::SampleRate::k48000, /*dither=*/false);
    const std::size_t total = 30 * 1536;
    const DecoderSettings before;
    DecoderSettings after;
    after.mix_levels.loro_clev = 0.25;

    // Each setting's decode of the whole item.
    const Slots old_whole = played_alone(library, "long.ec3", "2.0", before);
    const Slots new_whole = played_alone(library, "long.ec3", "2.0", after);
    REQUIRE(first_difference(old_whole, new_whole) != kSame);

    // Where the output stops being the old decode and becomes the new one,
    // if it does so at a unit boundary with nothing lost or repeated.
    const auto boundary_in = [&](const Slots& played) -> std::optional<std::size_t> {
        if (played.empty() || played[0].size() != total) {
            return std::nullopt;
        }
        for (std::size_t unit = 0; unit <= 30; ++unit) {
            const std::size_t at = unit * 1536;
            if (first_difference(part(played, 0, at), part(old_whole, 0, at)) == kSame &&
                first_difference(part(played, at, total - at), part(new_whole, at, total - at)) ==
                    kSame) {
                return at;
            }
        }
        return std::nullopt;
    };

    auto log = std::make_shared<FakeDevice::Log>();
    log->keep = true;
    const auto player = make_player(library, log, 8192, "2.0", before);
    player->queue().add(item("long.ec3"));
    player->play();

    SECTION("while playing") {
        for (int step = 0; step < 6; ++step) {
            player->pump();
            advance(*log, 480);
        }
        const std::uint64_t submitted = log->submitted;
        player->set_decoder_settings(after);
        CHECK(player->decoder_settings() == after);
        REQUIRE(play_out(*player, *log));
        REQUIRE(player->history().size() == 1);
        CHECK(player->history()[0].frames == total);
        const auto boundary = boundary_in(log->kept);
        REQUIRE(boundary.has_value());
        // What was already decoded when the change came plays out as it was,
        // and the change lands after it, part-way through the item.
        CHECK(*boundary >= submitted);
        CHECK(*boundary > 0);
        CHECK(*boundary < total);
    }

    SECTION("while paused") {
        for (int step = 0; step < 6; ++step) {
            player->pump();
            advance(*log, 480);
        }
        player->pause();
        player->set_decoder_settings(after);
        player->play();
        REQUIRE(play_out(*player, *log));
        const auto boundary = boundary_in(log->kept);
        REQUIRE(boundary.has_value());
        CHECK(*boundary > 0);
        CHECK(*boundary < total);
    }

    SECTION("while stopped, for the next play") {
        player->stop();
        player->set_decoder_settings(after);
        for (std::vector<float>& slot : log->kept) {
            slot.clear();
        }
        player->play();
        REQUIRE(play_out(*player, *log));
        CHECK(first_difference(log->kept, new_whole) == kSame);
    }

    SECTION("the same settings again change nothing") {
        player->set_decoder_settings(before);
        REQUIRE(play_out(*player, *log));
        CHECK(first_difference(log->kept, old_whole) == kSame);
    }
}

TEST_CASE("player: with dither in the stream, a settings change is still only the change",
          "[hearth][player]") {
    // This stream's frames do use §7.3.4 dither, so the decoder that takes
    // over draws other values for those bins than an unbroken decode would -
    // some 95 dB down. Anything else going wrong at the handover (a missing
    // overlap, a unit lost or played twice) is of the order of the signal.
    Library library;
    library.files["long.ec3"] = eac3_stream(30);
    const std::size_t total = 30 * 1536;
    DecoderSettings after;
    after.mix_levels.loro_clev = 0.25;
    const Slots old_whole = played_alone(library, "long.ec3", "2.0");
    const Slots new_whole = played_alone(library, "long.ec3", "2.0", after);

    auto log = std::make_shared<FakeDevice::Log>();
    log->keep = true;
    const auto player = make_player(library, log, 8192, "2.0");
    player->queue().add(item("long.ec3"));
    player->play();
    for (int step = 0; step < 6; ++step) {
        player->pump();
        advance(*log, 480);
    }
    player->set_decoder_settings(after);
    REQUIRE(play_out(*player, *log));
    REQUIRE(log->kept.size() == old_whole.size());
    REQUIRE(log->kept[0].size() == total);

    std::optional<std::size_t> boundary;
    for (std::size_t unit = 1; unit < 30 && !boundary; ++unit) {
        const std::size_t at = unit * 1536;
        if (first_difference(part(log->kept, 0, at), part(old_whole, 0, at)) == kSame &&
            max_difference(part(log->kept, at, total - at), part(new_whole, at, total - at)) <
                1e-4) {
            boundary = at;
        }
    }
    CHECK(boundary.has_value());
}

TEST_CASE("player: the programme setting picks one of a stream's programmes when an item starts",
          "[hearth][player]") {
    Library library;
    library.files["both.ec3"] = two_programmes(8);
    library.files["first.ec3"] = joined_frames(programme_frames(8, 440.0, 0));
    library.files["second.ec3"] = joined_frames(programme_frames(8, 1000.0, 0));
    const Slots first = played_alone(library, "first.ec3");
    const Slots second = played_alone(library, "second.ec3");
    REQUIRE(first_difference(first, second) != kSame);

    DecoderSettings settings;
    SECTION("unset plays the first") {
        CHECK(first_difference(played_alone(library, "both.ec3", "5.1", settings), first) == kSame);
    }

    SECTION("an id plays that programme") {
        settings.programme = 1;
        CHECK(first_difference(played_alone(library, "both.ec3", "5.1", settings), second) ==
              kSame);
    }

    SECTION("an id the stream lacks plays the first, and says so") {
        settings.programme = 7;
        auto log = std::make_shared<FakeDevice::Log>();
        log->keep = true;
        const auto player = make_player(library, log, 8192, "5.1", settings);
        player->queue().add(item("both.ec3"));
        player->play();
        CHECK(player->queue().items()[0].facts.note.find("no programme 7") != std::string::npos);
        REQUIRE(play_out(*player, *log));
        CHECK(first_difference(log->kept, first) == kSame);
    }

    SECTION("a change reaches the next item, and the playing one plays on") {
        // Long enough that one pump leaves most of the first item still to
        // decode when the setting changes.
        library.files["long_both.ec3"] = two_programmes(20);
        library.files["long_first.ec3"] = joined_frames(programme_frames(20, 440.0, 0));
        library.files["long_second.ec3"] = joined_frames(programme_frames(20, 1000.0, 0));
        const Slots long_first = played_alone(library, "long_first.ec3");
        const Slots long_second = played_alone(library, "long_second.ec3");

        auto log = std::make_shared<FakeDevice::Log>();
        log->keep = true;
        const auto player = make_player(library, log);
        player->queue().add(item("long_both.ec3"));
        player->queue().add(item("long_both.ec3"));
        player->play();
        player->pump();
        advance(*log, 480);
        REQUIRE(log->submitted < 20 * 1536);
        settings.programme = 1;
        player->set_decoder_settings(settings);
        REQUIRE(play_out(*player, *log));
        REQUIRE(player->history().size() == 2);
        CHECK(player->history()[0].frames == 20 * 1536);
        CHECK(player->history()[1].first_frame == 20 * 1536);
        CHECK(first_difference(log->kept, joined(long_first, long_second)) == kSame);
    }
}

// Queue edits while playing, and the play position.

TEST_CASE("player: removing the playing item goes on to the next, and the history follows the list",
          "[hearth][player]") {
    Library library;
    library.files["a.ec3"] = eac3_stream(10);
    library.files["b.ec3"] = eac3_stream(4);
    library.files["c.ec3"] = eac3_stream(4);
    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log);
    for (const char* path : {"a.ec3", "b.ec3", "c.ec3"}) {
        player->add(item(path));
    }
    player->play();
    for (int step = 0; step < 3; ++step) {
        player->pump();
        advance(*log, 480);
    }

    player->remove(0);
    REQUIRE(player->queue().size() == 2);
    CHECK(player->transport().state() == TransportState::kPlaying);
    REQUIRE(play_out(*player, *log));

    const auto& history = player->history();
    REQUIRE(history.size() == 3);
    // The removed item played part of the way, and has no place in the list.
    CHECK(history[0].title == "a.ec3");
    CHECK(history[0].queue_index == ac3::hearth::Queue::kNone);
    CHECK(history[0].frames < 10 * 1536);
    // The next one started on an output of its own, since the one playing
    // stopped part-way; the one after joined it.
    CHECK(history[1].title == "b.ec3");
    CHECK(history[1].queue_index == 0);
    CHECK(history[1].frames == 4 * 1536);
    CHECK(history[1].output_opens == 2);
    CHECK(history[2].queue_index == 1);
    CHECK(history[2].first_frame == 4 * 1536);
    CHECK(history[2].output_opens == 2);
}

TEST_CASE("player: an edit while a reopen waits leaves the reopen on its item", "[hearth][player]") {
    Library library;
    library.files["48k.ec3"] = eac3_stream(6);
    library.files["44k1.ac3"] = ac3_stream(4, ac3::SampleRate::k44100);
    library.files["new.ec3"] = eac3_stream(2);
    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log);
    player->add(item("48k.ec3"));
    player->add(item("44k1.ac3"));
    player->play();

    // Until the first item has been decoded and the transport has moved on
    // to the second, whose reopen now waits for the first to be heard.
    for (int step = 0; step < 1000 && player->queue().current_index() != 1; ++step) {
        player->pump();
        advance(*log, 480);
    }
    REQUIRE(player->queue().current_index() == 1);
    REQUIRE(player->history().size() == 1);
    REQUIRE(log->open);

    // An item in front moves the waiting one to index 2.
    player->insert(0, item("new.ec3"));
    REQUIRE(player->queue().current_index() == 2);
    REQUIRE(play_out(*player, *log));

    const auto& history = player->history();
    REQUIRE(history.size() == 2);
    CHECK(history[0].queue_index == 1);
    CHECK(history[1].title == "44k1.ac3");
    CHECK(history[1].queue_index == 2);
    CHECK(history[1].frames == 4 * 1536);
    CHECK(log->rates == std::vector<std::uint32_t>{48000, 44100});
}

TEST_CASE("player: the position follows the device's clock through a join and a seek",
          "[hearth][player]") {
    // The second item is long enough to be still decoding when the seek
    // comes: the player decodes ahead of the clock, and an item decoded to
    // its end at the end of the queue has nothing left to seek in.
    Library library;
    library.files["one.ec3"] = eac3_stream(10);
    library.files["two.ec3"] = eac3_stream(40);
    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log);
    player->add(item("one.ec3"));
    player->add(item("two.ec3"));
    CHECK(player->position().item == ac3::hearth::Queue::kNone);

    player->play();
    auto at = player->position();
    CHECK(at.item == 0);
    CHECK(at.heard.count() == 0);
    CHECK(at.duration.count() == 10 * 1536 * 1000 / 48000);

    const auto run_until = [&](std::uint64_t clock) {
        for (int step = 0; step < 100000 && log->clock < clock; ++step) {
            player->pump();
            advance(*log, 480);
        }
        REQUIRE(log->clock >= clock);
    };
    // Part-way through the first item: what the clock has passed.
    run_until(12000);
    at = player->position();
    CHECK(at.item == 0);
    CHECK(at.heard.count() == static_cast<std::int64_t>(log->clock * 1000 / 48000));

    // Past the join, the second item, counted from its own first frame.
    run_until((10 * 1536) + 4800);
    at = player->position();
    CHECK(at.item == 1);
    CHECK(at.heard.count() == static_cast<std::int64_t>((log->clock - (10 * 1536)) * 1000 / 48000));

    // A seek restarts the clock at the unit the item now plays from: 100 ms
    // is inside unit 3, which starts at 4,608.
    player->seek(std::chrono::milliseconds{100});
    at = player->position();
    CHECK(at.item == 1);
    CHECK(at.heard.count() == 4608 * 1000 / 48000);
    player->pump();
    advance(*log, 480);
    CHECK(player->position().heard.count() == (4608 + 480) * 1000 / 48000);

    // Paused, it stands.
    player->pause();
    const auto paused = player->position();
    for (int step = 0; step < 5; ++step) {
        player->pump();
        advance(*log, 480);
    }
    CHECK(player->position().heard == paused.heard);

    // Stopped, there is nothing playing.
    player->stop();
    CHECK(player->position().item == ac3::hearth::Queue::kNone);
}

TEST_CASE("player: choosing an item plays it from its start, and clearing the queue stops",
          "[hearth][player]") {
    Library library;
    library.files["a.ec3"] = eac3_stream(10);
    library.files["b.ec3"] = eac3_stream(3);
    library.files["c.ec3"] = eac3_stream(10);
    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log);
    for (const char* path : {"a.ec3", "b.ec3", "c.ec3"}) {
        player->add(item(path));
    }
    player->play();
    player->pump();
    advance(*log, 480);

    const auto chosen = player->play_item(2);
    CHECK(chosen.state == TransportState::kPlaying);
    CHECK(player->queue().current_index() == 2);
    CHECK(player->position().item == 2);
    CHECK(player->position().heard.count() == 0);
    player->pump();
    advance(*log, 480);

    player->clear();
    CHECK(player->transport().state() == TransportState::kStopped);
    CHECK_FALSE(log->open);
    CHECK(player->position().item == ac3::hearth::Queue::kNone);
    REQUIRE(player->history().size() == 2);
    CHECK(player->history()[1].title == "c.ec3");
    CHECK(player->history()[1].queue_index == ac3::hearth::Queue::kNone);
    CHECK(log->opens == 2);

    // Out of range is said, not done.
    CHECK_FALSE(player->play_item(5).note.empty());
}

TEST_CASE("player: a meter reading waits until the device has played what it describes",
          "[hearth][player]") {
    // A 5.1 tone, the same in every channel, folded to two speakers: the fold
    // keeps its level, 0.3 of full scale.
    Library library;
    library.files["tone.ec3"] = eac3_stream(20);
    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log, 8192, "2.0");
    player->add(item("tone.ec3"));
    ac3::hearth::MeterSnapshot latest;
    CHECK_FALSE(player->meters(latest));

    player->play();
    player->pump();
    // Decoded and queued, but nothing heard.
    CHECK_FALSE(player->meters(latest));
    // The first reading describes the audio up to the end of the tenth block.
    advance(*log, 2400);
    CHECK_FALSE(player->meters(latest));
    advance(*log, 480);
    REQUIRE(player->meters(latest));
    CHECK(latest.output_frame == 2560);
    REQUIRE(latest.levels.size() == 2);
    CHECK(latest.levels[0].hold_db == Catch::Approx(-10.46).margin(1.0));
    CHECK(latest.levels[1].hold_db == Catch::Approx(-10.46).margin(1.0));

    // A seek throws away what was waiting: with nothing decoded since, the
    // clock can pass every frame the old readings were stamped with and
    // release none of them.
    player->pump();
    player->seek(std::chrono::milliseconds{200});
    advance(*log, 16384);
    CHECK_FALSE(player->meters(latest));
}

TEST_CASE("player: the report of the unit being heard waits for the device", "[hearth][player]") {
    // Two items told apart by their dialnorm, joined.
    Library library;
    library.files["a.ec3"] = eac3_stream(20);
    library.files["b.ec3"] = eac3_stream(20, ac3::SampleRate::k48000, true, 0.3, 20);
    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log, 8192, "2.0");
    player->add(item("a.ec3"));
    player->add(item("b.ec3"));
    ac3::hearth::UnitReport report;
    CHECK_FALSE(player->unit_report(report));

    player->play();
    player->pump();
    // Decoded and queued, but nothing heard.
    CHECK_FALSE(player->unit_report(report));
    advance(*log, 1);
    REQUIRE(player->unit_report(report));
    CHECK(report.dialnorm == 31);
    CHECK(report.layout.count == 6);
    // The next unit's report once the clock passes its first frame, and not
    // before.
    advance(*log, 1535);
    CHECK_FALSE(player->unit_report(report));
    advance(*log, 1);
    CHECK(player->unit_report(report));

    // Up to the join, the first item's words; past it, the second's.
    const std::uint64_t join = 20 * ac3::kSamplesPerFrame;
    for (int step = 0; step < 100000 && log->clock < join; ++step) {
        player->pump();
        advance(*log, std::min<std::uint64_t>(480, join - log->clock));
    }
    REQUIRE(log->clock == join);
    REQUIRE(log->opens == 1);
    static_cast<void>(player->unit_report(report));
    CHECK(report.dialnorm == 31);
    player->pump();
    advance(*log, 1);
    REQUIRE(player->unit_report(report));
    CHECK(report.dialnorm == 20);

    // A seek throws away what was waiting, so the next report is the new
    // position's as soon as it is heard.
    player->pump();
    player->seek(std::chrono::milliseconds{100});
    player->pump();
    advance(*log, 1);
    REQUIRE(player->unit_report(report));
    CHECK(report.dialnorm == 20);
}

TEST_CASE("player: a join starts the next item's loudness, and momentary loudness runs on",
          "[hearth][player]") {
    // A tone, then silence, joined.
    Library library;
    library.files["tone.ec3"] = eac3_stream(60);
    library.files["silence.ec3"] = eac3_stream(60, ac3::SampleRate::k48000, /*dither=*/false, 0.0);
    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log, 8192, "2.0");
    player->add(item("tone.ec3"));
    player->add(item("silence.ec3"));
    player->play();
    const std::uint64_t join = 60 * ac3::kSamplesPerFrame;
    const auto play_until = [&](std::uint64_t frame) {
        for (int step = 0; step < 100000 && log->clock < frame; ++step) {
            player->pump();
            advance(*log, 480);
        }
        REQUIRE(log->clock >= frame);
    };

    // 200 ms into the silence, half the momentary window is still the tone.
    ac3::hearth::MeterSnapshot latest;
    play_until(join + 9600);
    REQUIRE(log->opens == 1);
    REQUIRE(player->meters(latest));
    REQUIRE(latest.output_frame > join);
    REQUIRE(latest.momentary_lkfs.has_value());
    CHECK(*latest.momentary_lkfs > -20.0);
    CHECK_FALSE(latest.integrated_lkfs.has_value());

    // Over a second in, the programme readings are the silence's own: no
    // integrated loudness, and no true peak to speak of.
    play_until(join + 57600);
    REQUIRE(player->meters(latest));
    CHECK_FALSE(latest.integrated_lkfs.has_value());
    CHECK((!latest.true_peak_dbtp || *latest.true_peak_dbtp < -60.0));
    CHECK(log->opens == 1);
}

namespace {

// What the ring holds, without the stamps.
std::vector<std::string> notes_in(const ac3::hearth::DiagnosticLog& diagnostics) {
    std::vector<std::string> notes;
    for (const std::string& line : diagnostics.lines()) {
        notes.push_back(line.substr(ac3::hearth::DiagnosticLog::kStampBytes));
    }
    return notes;
}

std::string all_of(const std::vector<std::string>& notes) {
    std::string out;
    for (const std::string& note : notes) {
        out += note;
        out += '\n';
    }
    return out;
}

}  // namespace

TEST_CASE("player: the diagnostics ring hears what playback did, and not where a file lives",
          "[hearth][player][diagnostics]") {
    Library library;
    const std::string folder = "C:\\Users\\Someone\\Music\\";
    library.files[folder + "a.ec3"] = eac3_stream(4);
    library.files[folder + "b.ec3"] = eac3_stream(3);
    library.files["/home/someone/c.ec3"] = eac3_stream(2, ac3::SampleRate::k44100);
    auto log = std::make_shared<FakeDevice::Log>();
    ac3::hearth::DiagnosticLog diagnostics;
    const auto layout = ac3::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    Player player{std::make_unique<FakeDevice>(log, 8192), library.loader(), *layout,
                  DecoderSettings{}, &diagnostics};
    const auto add = [&player](const std::string& path, const std::string& title) {
        QueueItem entry = item(path);
        entry.title = title;
        player.add(entry);
    };
    add(folder + "a.ec3", "Song A");
    add(folder + "missing.ec3", "Song M");
    add(folder + "b.ec3", "Song B");
    add("/home/someone/c.ec3", "Song C");
    player.play();
    REQUIRE(play_out(player, *log));

    const auto notes = notes_in(diagnostics);
    INFO(all_of(notes));
    const std::vector<std::string> expected{
        "output opened: local PCM, 48000 Hz, 6 channels (open 1)",
        "item 1 \"Song A\" started: E-AC-3, 48000 Hz, 6 channels, 0.128 s",
        "item 2 \"Song M\" cannot be played: no such file: <withheld>\\missing.ec3",
        "item 3 \"Song B\" joined the open output: E-AC-3, 48000 Hz, 6 channels, 0.096 s",
        "item 4 \"Song C\" is next, once the output has played out and reopened: \"Song C\" is "
        "44100 Hz and the output is open at 48000 Hz, so it reopens - there is a gap.",
        "output closed",
        "output opened: local PCM, 44100 Hz, 6 channels (open 2)",
        "item 4 \"Song C\" started: E-AC-3, 44100 Hz, 6 channels, 0.069 s",
        "playback ends once the output has played out: The queue has finished.",
        "output closed",
    };
    CHECK(notes == expected);
    CHECK(diagnostics.dropped() == 0);
}

TEST_CASE("player: units that will not decode are noted once, then counted",
          "[hearth][player][diagnostics]") {
    // The middle of six units damaged, where only their CRC notices.
    auto damaged = eac3_stream(12);
    {
        const auto scanned = ac3::io::scan(damaged);
        REQUIRE(scanned.has_value());
        REQUIRE(scanned->access_units.size() == 12);
        for (std::size_t k = 3; k <= 8; ++k) {
            const auto unit = scanned->access_units[k];
            const auto at = static_cast<std::size_t>(unit.data() - damaged.data()) + (unit.size() / 2);
            damaged[at] ^= std::byte{0xFF};
        }
    }
    Library library;
    library.files["damaged.ec3"] = damaged;
    library.files["clean.ec3"] = eac3_stream(2);
    auto log = std::make_shared<FakeDevice::Log>();
    ac3::hearth::DiagnosticLog diagnostics;
    const auto layout = ac3::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    // Concealment would hide the damage from the player.
    DecoderSettings settings;
    settings.concealment = ac3::ConcealmentPolicy::kNone;
    Player player{std::make_unique<FakeDevice>(log, 8192), library.loader(), *layout, settings,
                  &diagnostics};
    player.add(item("damaged.ec3"));
    player.add(item("clean.ec3"));
    player.play();
    REQUIRE(play_out(player, *log));
    CHECK_FALSE(player.last_error().empty());

    const auto notes = notes_in(diagnostics);
    INFO(all_of(notes));
    REQUIRE(notes.size() == 7);
    CHECK(notes[1].starts_with("item 1 \"damaged.ec3\" started: "));
    CHECK(notes[2] == "item 1 \"damaged.ec3\" has a unit that could not be decoded: An E-AC-3 "
                      "access unit could not be decoded: the frame's CRC does not check out.");
    CHECK(notes[3] == "item 1 \"damaged.ec3\" had 5 more units that could not be decoded");
    CHECK(notes[4].starts_with("item 2 \"clean.ec3\" joined the open output: "));
    CHECK(notes[6] == "output closed");
}

TEST_CASE("player: an output that will not open or pause is noted", "[hearth][player][diagnostics]") {
    Library library;
    library.files["a.ec3"] = eac3_stream(40);
    auto log = std::make_shared<FakeDevice::Log>();
    ac3::hearth::DiagnosticLog diagnostics;
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    Player player{std::make_unique<FakeDevice>(log, 8192), library.loader(), *layout,
                  DecoderSettings{}, &diagnostics};
    player.add(item("a.ec3"));
    log->refuse_open = true;
    player.play();
    log->refuse_open = false;
    player.play();
    player.pump();
    log->refuse_pause = true;
    player.pause();
    log->refuse_pause = false;
    player.play();
    player.stop();

    const auto notes = notes_in(diagnostics);
    INFO(all_of(notes));
    const std::vector<std::string> expected{
        "item 1 \"a.ec3\" could not start: the output would not open: The fake device refused to "
        "open.",
        "output opened: local PCM, 48000 Hz, 2 channels (open 1)",
        "item 1 \"a.ec3\" started: E-AC-3, 48000 Hz, 6 channels, 1.280 s",
        "the output would not pause",
        "output closed",
    };
    CHECK(notes == expected);
}

TEST_CASE("player: an item that fails can stop playback at it", "[hearth][player]") {
    Library library;
    library.files["first.ec3"] = eac3_stream(4);
    library.files["third.ec3"] = eac3_stream(3);
    const auto layout = ac3::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());

    // The item before it plays to its end, and playback stops at it.
    auto log = std::make_shared<FakeDevice::Log>();
    ac3::hearth::DiagnosticLog diagnostics;
    Player player{std::make_unique<FakeDevice>(log, 8192), library.loader(), *layout,
                  DecoderSettings{}, &diagnostics};
    player.set_on_failure(ac3::hearth::FailurePolicy::kStop);
    player.add(item("first.ec3"));
    QueueItem missing = item("D:\\Private\\missing.ec3");
    missing.title = "missing.ec3";
    player.add(missing);
    player.add(item("third.ec3"));
    player.play();
    REQUIRE(play_out(player, *log));
    REQUIRE(player.history().size() == 1);
    CHECK(player.history()[0].frames == 4 * 1536);
    CHECK(log->heard == 4 * 1536);
    CHECK(log->opens == 1);
    CHECK(player.transport().state() == TransportState::kStopped);
    CHECK(player.queue().current_index() == 1);
    CHECK_FALSE(player.queue().items()[1].playable());
    const auto notes = notes_in(diagnostics);
    INFO(all_of(notes));
    const std::vector<std::string> expected{
        "output opened: local PCM, 48000 Hz, 6 channels (open 1)",
        "item 1 \"first.ec3\" started: E-AC-3, 48000 Hz, 6 channels, 0.128 s",
        "item 2 \"missing.ec3\" cannot be played: no such file: <withheld>\\missing.ec3",
        "playback ends once the output has played out: \"missing.ec3\" cannot be played here, so "
        "playback stops: no such file: <withheld>\\missing.ec3",
        "output closed",
    };
    CHECK(notes == expected);

    // Playing again does not pass over it; moving on is the person's choice.
    const auto again = player.play();
    CHECK(again.action == ac3::hearth::TransportAction::kNone);
    CHECK(again.note.find("missing.ec3") != std::string::npos);
    CHECK(log->opens == 1);
    player.next();
    REQUIRE(play_out(player, *log));
    REQUIRE(player.history().size() == 2);
    CHECK(player.history()[1].title == "third.ec3");

    // An item that will not open as playback starts stops it there and then,
    // where passing over it would have played the next.
    auto starting_log = std::make_shared<FakeDevice::Log>();
    ac3::hearth::DiagnosticLog starting_notes;
    Player starting{std::make_unique<FakeDevice>(starting_log, 8192), library.loader(), *layout,
                    DecoderSettings{}, &starting_notes};
    starting.set_on_failure(ac3::hearth::FailurePolicy::kStop);
    starting.add(item("gone.ec3"));
    starting.add(item("first.ec3"));
    starting.play();
    CHECK(starting.transport().state() == TransportState::kStopped);
    CHECK_FALSE(starting.active());
    CHECK(starting.queue().current_index() == 0);
    CHECK(starting.history().empty());
    CHECK(starting_log->opens == 0);
    const std::vector<std::string> stopped{
        "item 1 \"gone.ec3\" cannot be played: no such file: gone.ec3",
        "item 1 \"gone.ec3\" stopped playback, as an item that fails is set to",
    };
    CHECK(notes_in(starting_notes) == stopped);

    starting.set_on_failure(ac3::hearth::FailurePolicy::kSkip);
    starting.play_item(0);
    CHECK(starting_log->opens == 0);
    starting.next();
    REQUIRE(play_out(starting, *starting_log));
    REQUIRE(starting.history().size() == 1);
    CHECK(starting.history()[0].title == "first.ec3");
}

// The end of the queue. The last item is decoded to its end well before it
// has been heard; until it has, it is still the item playing.

namespace {

// Pumps, with the device's clock running, until the last item has been
// decoded to its end - its units all handed to the device or waiting for
// room - while some of it is still to be heard.
void pump_into_tail(Player& player, FakeDevice::Log& log, std::uint64_t item_frames) {
    for (int step = 0; step < 4; ++step) {
        player.pump();
        advance(log, 480);
    }
    REQUIRE(log.submitted > 0);
    REQUIRE(log.heard < item_frames);
}

}  // namespace

TEST_CASE("player: the last item is still playing until its tail has been heard",
          "[hearth][player]") {
    const std::uint64_t item_frames = 6 * 1536;
    Library library;
    library.files["last.ec3"] = eac3_stream(6);
    library.files["more.ec3"] = eac3_stream(4);
    library.files["44k1.ac3"] = ac3_stream(4, ac3::SampleRate::k44100);
    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log);
    player->queue().add(item("last.ec3"));
    player->play();
    pump_into_tail(*player, *log, item_frames);
    CHECK(player->transport().state() == TransportState::kPlaying);

    SECTION("a pause holds it, and it plays on afterwards") {
        player->pause();
        CHECK(log->paused);
        CHECK(player->transport().state() == TransportState::kPaused);
        const std::uint64_t heard = log->heard;
        const auto at = player->position().heard;
        for (int step = 0; step < 50; ++step) {
            player->pump();
            advance(*log, 480);
        }
        CHECK(log->heard == heard);
        CHECK(player->position().heard == at);
        CHECK(player->transport().state() == TransportState::kPaused);
        player->play();
        CHECK_FALSE(log->paused);
        REQUIRE(play_out(*player, *log));
        CHECK(log->heard == item_frames);
    }

    SECTION("a seek plays it again from there") {
        const std::uint64_t submitted = log->submitted_total;
        player->seek(std::chrono::milliseconds{64});
        CHECK(log->flushes == 1);
        CHECK(player->position().heard == std::chrono::milliseconds{64});
        REQUIRE(play_out(*player, *log));
        // From 64 ms: units 2 to 5.
        CHECK(log->submitted_total == submitted + (4 * 1536));
    }

    SECTION("a seek once everything has gone to the device ends when the new tail is heard") {
        // Everything handed over, and the wait for it begun.
        for (int step = 0; step < 100 && log->submitted_total < item_frames; ++step) {
            player->pump();
            advance(*log, 480);
        }
        REQUIRE(log->submitted_total == item_frames);
        player->pump();
        REQUIRE(log->heard < item_frames);
        // 128 ms is unit 4: two units, heard by 3072 on the new clock.
        player->seek(std::chrono::milliseconds{128});
        std::optional<std::uint64_t> stopped_at;
        for (int step = 0; step < 1000 && !stopped_at; ++step) {
            player->pump();
            if (!log->open) {
                stopped_at = log->clock;
            }
            advance(*log, 480);
        }
        REQUIRE(stopped_at.has_value());
        CHECK(log->heard == 2 * 1536);
        CHECK(*stopped_at < (2 * 1536) + (3 * 480));
    }

    SECTION("a seek to its end, once everything has gone to the device, stops playback at once") {
        for (int step = 0; step < 100 && log->submitted_total < item_frames; ++step) {
            player->pump();
            advance(*log, 480);
        }
        REQUIRE(log->submitted_total == item_frames);
        player->pump();
        REQUIRE(log->heard < item_frames);
        // 192 ms is the end: the last unit is decoded for its overlap and none
        // of it is played, so there is nothing new to wait for - the wait
        // taken before the seek counted frames the flush threw away.
        player->seek(std::chrono::milliseconds{192});
        const auto report = player->pump();
        CHECK(player->transport().state() == TransportState::kStopped);
        CHECK(report.note == "The queue has finished.");
        player->pump();
        CHECK_FALSE(log->open);
        CHECK(log->submitted_total == item_frames);
    }

    SECTION("an item added meanwhile joins it") {
        player->add(item("more.ec3"));
        REQUIRE(play_out(*player, *log));
        CHECK(log->opens == 1);
        const auto& history = player->history();
        REQUIRE(history.size() == 2);
        CHECK(history[1].first_frame == item_frames);
        CHECK(history[1].frames == 4 * 1536);
        CHECK(history[1].output_opens == history[0].output_opens);
        // And heard to its end before the output closed.
        CHECK(log->heard == item_frames + (4 * 1536));
    }

    SECTION("an item added meanwhile that wants another output waits for it to be heard") {
        player->add(item("44k1.ac3"));
        REQUIRE(play_out(*player, *log));
        CHECK(log->rates == std::vector<std::uint32_t>{48000, 44100});
        REQUIRE(log->heard_when_opened.size() == 2);
        CHECK(log->heard_when_opened[1] == item_frames);
        REQUIRE(player->history().size() == 2);
        CHECK(player->history()[1].title == "44k1.ac3");
    }

    SECTION("playback stops once it has been heard, and not before") {
        std::optional<std::string> stopped_note;
        for (int step = 0; step < 1000 && !stopped_note; ++step) {
            const std::uint64_t heard = log->heard;
            const auto report = player->pump();
            if (player->transport().state() == TransportState::kStopped) {
                CHECK(heard == item_frames);
                stopped_note = report.note;
            }
            advance(*log, 480);
        }
        REQUIRE(stopped_note.has_value());
        CHECK(*stopped_note == "The queue has finished.");
        REQUIRE(play_out(*player, *log));
        CHECK(log->heard == item_frames);
    }
}

TEST_CASE("player: a next item that will not open still stops playback after the tail",
          "[hearth][player]") {
    // Asked again at every pump while the tail plays, the transport must still
    // hear of the item that would not open, not of the one after it.
    Library library;
    library.files["first.ec3"] = eac3_stream(6);
    library.files["third.ec3"] = eac3_stream(3);
    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log);
    player->set_on_failure(ac3::hearth::FailurePolicy::kStop);
    player->add(item("first.ec3"));
    player->add(item("gone.ec3"));
    player->add(item("third.ec3"));
    player->play();
    pump_into_tail(*player, *log, 6 * 1536);
    CHECK(player->transport().state() == TransportState::kPlaying);

    SECTION("an edit meanwhile moves the item, and it is still the one stopped at") {
        player->move(2, 0);
        REQUIRE(play_out(*player, *log));
        REQUIRE(player->history().size() == 1);
        CHECK(player->queue().current_index() == 2);
        CHECK(player->queue().items()[2].path == "gone.ec3");
    }

    SECTION("skipping on meanwhile stops at it at once") {
        // It is the next item, so Next starts it, and it will not open.
        player->next();
        CHECK(player->transport().state() == TransportState::kStopped);
        CHECK_FALSE(log->open);
        CHECK(player->queue().current_index() == 1);
        CHECK_FALSE(player->queue().items()[1].playable());
    }

    SECTION("an item put before it meanwhile plays first") {
        player->insert(1, item("third.ec3"));
        REQUIRE(play_out(*player, *log));
        const auto& history = player->history();
        REQUIRE(history.size() == 2);
        CHECK(history[1].title == "third.ec3");
        CHECK(history[1].output_opens == history[0].output_opens);
        CHECK(log->heard == (6 + 3) * 1536);
        // And then it is the one playback stops at.
        CHECK(player->queue().current_index() == 2);
        CHECK(player->queue().items()[2].path == "gone.ec3");
    }

    SECTION("passing over is chosen meanwhile") {
        player->set_on_failure(ac3::hearth::FailurePolicy::kSkip);
        REQUIRE(play_out(*player, *log));
        const auto& history = player->history();
        REQUIRE(history.size() == 2);
        CHECK(history[1].title == "third.ec3");
        CHECK(history[1].first_frame == 6 * 1536);
        CHECK(log->heard == (6 + 3) * 1536);
        CHECK_FALSE(player->queue().items()[1].playable());
    }

    SECTION("a stop forgets it, and the item is tried again") {
        player->stop();
        library.files["gone.ec3"] = eac3_stream(2);
        player->play();
        REQUIRE(play_out(*player, *log));
        const auto& history = player->history();
        REQUIRE(history.size() == 4);
        CHECK(history[2].title == "gone.ec3");
        CHECK(history[3].title == "third.ec3");
    }
}

TEST_CASE("player: an item added during a tail is heard to its end", "[hearth][player]") {
    // Added once the wait for the tail has begun: the join must move the end
    // the wait is for, or the output closes on the added item.
    Library library;
    library.files["last.ec3"] = eac3_stream(6);
    library.files["more.ec3"] = eac3_stream(4);
    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log);
    player->add(item("last.ec3"));
    player->play();
    // Everything handed over, and the wait for it begun.
    for (int step = 0; step < 100 && log->submitted_total < 6 * 1536; ++step) {
        player->pump();
        advance(*log, 480);
    }
    player->pump();
    REQUIRE(log->heard < 6 * 1536);
    player->add(item("more.ec3"));
    REQUIRE(play_out(*player, *log));
    CHECK(log->opens == 1);
    CHECK(log->heard == 10 * 1536);
}

TEST_CASE("player: an item added once the tail has left the device reopens",
          "[hearth][player]") {
    // The device has played everything, and only its output path's delay is
    // left: joining now would follow silence the timeline does not count.
    Library library;
    library.files["last.ec3"] = eac3_stream(6);
    library.files["more.ec3"] = eac3_stream(4);
    auto log = std::make_shared<FakeDevice::Log>();
    log->latency = 4800;
    const auto player = make_player(library, log);
    player->add(item("last.ec3"));
    player->play();
    for (int step = 0; step < 100 && log->heard < 6 * 1536; ++step) {
        player->pump();
        advance(*log, 480);
    }
    REQUIRE(log->heard == 6 * 1536);
    REQUIRE(player->transport().state() == TransportState::kPlaying);
    player->add(item("more.ec3"));
    REQUIRE(play_out(*player, *log));
    CHECK(log->opens == 2);
    REQUIRE(player->history().size() == 2);
    CHECK(player->history()[1].first_frame == 0);
}

TEST_CASE("player: an item queued all along joins even if the device ran dry before",
          "[hearth][player]") {
    // A device that has played everything by the time an item ends means the
    // player fell behind it, not that the next item came late. Each pump here
    // decodes and sends a whole item, which the device plays at once; only the
    // output path's delay is ever left to wait for.
    const std::size_t whole = std::size_t{1} << 20;
    Library library;
    library.files["first.ec3"] = eac3_stream(4);
    library.files["second.ec3"] = eac3_stream(3);
    library.files["44k1-a.ac3"] = ac3_stream(3, ac3::SampleRate::k44100);
    library.files["44k1-b.ac3"] = ac3_stream(2, ac3::SampleRate::k44100);
    auto log = std::make_shared<FakeDevice::Log>();
    log->instant = true;
    log->latency = 480;
    const auto player = make_player(library, log);
    const auto run = [&] {
        for (int step = 0;
             step < 100 && !(player->transport().state() == TransportState::kStopped && !log->open);
             ++step) {
            player->pump(whole);
            advance(*log, 480);
        }
        REQUIRE(player->transport().state() == TransportState::kStopped);
        REQUIRE_FALSE(log->open);
    };

    SECTION("at the item's end") {
        player->add(item("first.ec3"));
        player->add(item("second.ec3"));
        player->play();
        run();
        CHECK(log->opens == 1);
        REQUIRE(player->history().size() == 2);
        CHECK(player->history()[1].first_frame == 4 * 1536);
    }

    SECTION("after a wait that ended in a reopen") {
        player->add(item("first.ec3"));
        player->add(item("44k1-a.ac3"));
        player->add(item("44k1-b.ac3"));
        player->play();
        run();
        CHECK(log->rates == std::vector<std::uint32_t>{48000, 44100});
        REQUIRE(player->history().size() == 3);
        CHECK(player->history()[2].first_frame == 3 * 1536);
    }

    SECTION("after a seek back from a wait") {
        player->add(item("first.ec3"));
        player->play();
        player->pump(whole);
        // Heard, and waiting on the output path's delay.
        REQUIRE(log->heard == 4 * 1536);
        REQUIRE(player->transport().state() == TransportState::kPlaying);
        player->add(item("second.ec3"));
        // 64 ms is unit 2: two units, then the join.
        player->seek(std::chrono::milliseconds{64});
        run();
        CHECK(log->opens == 1);
        REQUIRE(player->history().size() == 2);
        CHECK(player->history()[1].first_frame == 2 * 1536);
        CHECK(log->heard == 5 * 1536);
    }
}

TEST_CASE("player: a reopen decided before a pause waits for the resume", "[hearth][player]") {
    Library library;
    library.files["48k.ec3"] = eac3_stream(4);
    library.files["44k1.ac3"] = ac3_stream(4, ac3::SampleRate::k44100);
    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log);
    player->add(item("48k.ec3"));
    player->add(item("44k1.ac3"));
    player->play();
    // Until the reopen has been decided: the transport has moved on.
    for (int step = 0; step < 200 && player->queue().current_index() == 0; ++step) {
        player->pump();
        advance(*log, 480);
    }
    REQUIRE(player->queue().current_index() == 1);
    REQUIRE(log->opens == 1);
    player->pause();
    for (int step = 0; step < 20; ++step) {
        player->pump();
        advance(*log, 480);
    }
    // Not opened while paused.
    CHECK(log->opens == 1);
    player->play();
    REQUIRE(play_out(*player, *log));
    CHECK(log->rates == std::vector<std::uint32_t>{48000, 44100});
    CHECK(log->heard == 4 * 1536);
}

TEST_CASE("player: an output whose device goes away stops playback, and says so",
          "[hearth][player]") {
    const std::uint64_t item_frames = 6 * 1536;
    Library library;
    library.files["one.ec3"] = eac3_stream(6);
    // A device with room for a fraction of the item, so blocks are still
    // waiting to go in when it goes: what used to keep the player waiting for
    // a clock that had stopped.
    auto log = std::make_shared<FakeDevice::Log>();
    const auto player = make_player(library, log, 2048);
    player->queue().add(item("one.ec3"));
    player->play();

    SECTION("while the item plays") {
        // One pump decodes only part of it: its budget is 4800 frames of the
        // item's 9216.
        player->pump();
        advance(*log, 480);
        REQUIRE(log->submitted > 0);
        REQUIRE(player->transport().state() == TransportState::kPlaying);
    }

    SECTION("while its tail is waited for") {
        // Decoded to its end in one pump, with most of it still here.
        player->pump(2 * item_frames);
        REQUIRE(log->submitted_total < item_frames);
        REQUIRE(player->transport().state() == TransportState::kPlaying);
    }

    SECTION("while paused") {
        player->pump();
        player->pause();
        REQUIRE(log->paused);
        REQUIRE(player->transport().state() == TransportState::kPaused);
    }

    SECTION("between the queue's end and the output's close") {
        // Heard to its end: the transport has stopped, and the output closes
        // at the next pump - so the stop has to be carried out whatever the
        // transport says now.
        for (int step = 0; step < 1000 && player->transport().state() != TransportState::kStopped;
             ++step) {
            player->pump();
            advance(*log, 480);
        }
        REQUIRE(player->transport().state() == TransportState::kStopped);
        REQUIRE(log->open);
    }

    REQUIRE(player->active());
    // Unplugged: the sink stops itself without being closed, as ac3::audio's
    // do. It takes nothing more, has no position, and its clock stands.
    log->open = false;
    const auto report = player->pump();
    CHECK(report.stopped);
    CHECK(player->last_error() == "Playback stopped: the output device went away.");
    CHECK(report.note == player->last_error());
    CHECK(player->transport().state() == TransportState::kStopped);
    // Closed all the same, which is what releases what the sink still holds.
    CHECK(log->closes == 1);
    // Nothing is left to pump, or to wait for.
    CHECK_FALSE(player->active());
    CHECK(player->pump().frames_submitted == 0);
    CHECK(log->closes == 1);

    // The item was not at fault: once the device is back, it plays again.
    CHECK(player->queue().items()[0].playable());
    player->play();
    REQUIRE(play_out(*player, *log));
    CHECK(log->opens == 2);
    CHECK(player->history().back().frames == item_frames);
}