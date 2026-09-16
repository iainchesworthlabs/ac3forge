#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "ac3/render/layout.hpp"
#include "bitstream_sink.hpp"
#include "diagnostic_log.hpp"
#include "engine_thread.hpp"
#include "pcm_sink.hpp"
#include "player.hpp"

// ac3::hearth::Player's bitstream output (apps/hearth/engine/player.cpp),
// against a fake IEC 61937 link and a fake PCM device, each with a clock the
// test runs.
//
// What passthrough adds to the player: an item the output decision
// bitstreams is sent as bursts packed from its own access units - checked
// here byte for byte against wrap_frame() and Eac3BurstPacker - on a
// timeline that the joins, the seeks, the meters and the unit reports share
// with a decoded item's. The decode still runs, for the meters, and the
// decision is asked again before every join.

namespace {

using ac3::audio::BitstreamFormat;
using ac3::hearth::BitstreamSink;
using ac3::hearth::DecoderSettings;
using ac3::hearth::DiagnosticLog;
using ac3::hearth::ItemFacts;
using ac3::hearth::ItemLoader;
using ac3::hearth::LoadedItem;
using ac3::hearth::MeterSnapshot;
using ac3::hearth::OpenOutputFormat;
using ac3::hearth::OutputChoice;
using ac3::hearth::OutputMode;
using ac3::hearth::PcmSink;
using ac3::hearth::Player;
using ac3::hearth::PlayerOutputs;
using ac3::hearth::QueueItem;
using ac3::hearth::TransportState;
using ac3::hearth::UnitReport;

using Bytes = std::vector<std::byte>;
using Units = std::vector<Bytes>;

// What either fake records, and its clock: content frames since the last
// open or flush, as the real sinks count them.
struct Clock {
    std::uint32_t opens = 0;
    std::uint32_t closes = 0;
    std::uint32_t flushes = 0;
    std::uint64_t submitted = 0;
    std::uint64_t heard = 0;
    std::uint64_t clock = 0;
    // At each close, what had been submitted and not yet heard.
    std::vector<std::uint64_t> unheard_at_close{};
    bool open = false;
    bool paused = false;

    void restart() {
        submitted = 0;
        heard = 0;
        clock = 0;
    }

    // Time passes: what is held is heard first, and the rest is silence.
    void advance(std::uint64_t frames) {
        if (!open || paused) {
            return;
        }
        heard += std::min(submitted - heard, frames);
        clock += frames;
    }

    [[nodiscard]] ac3::audio::MonitorPosition position() const {
        return ac3::audio::MonitorPosition{
            .frames_played = clock, .frames_queued = submitted - heard, .latency_frames = 0};
    }
};

class FakeLink final : public BitstreamSink {
public:
    struct Log : Clock {
        std::vector<BitstreamFormat> formats{};
        std::vector<std::uint32_t> rates{};
        std::vector<std::string> endpoints{};
        // Every burst, in order, over the link's whole life, and how many
        // there were at each flush.
        std::vector<Bytes> bursts{};
        std::vector<std::size_t> bursts_at_flush{};
        std::size_t capacity_bursts = 6;
        std::optional<BitstreamFormat> format{};
        bool wrong_size = false;
    };

    explicit FakeLink(std::shared_ptr<Log> log) : log_(std::move(log)) {}

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        ++log_->opens;
        log_->formats.push_back(format.format);
        log_->rates.push_back(format.sample_rate);
        log_->endpoints.push_back(format.endpoint_id);
        log_->format = format.format;
        log_->open = true;
        log_->paused = false;
        log_->restart();
        return OpenOutputFormat{.sample_rate = format.sample_rate,
                                .channels = 2,
                                .mode = OutputMode::kBitstream,
                                .stream = format.format};
    }

    void close() override {
        ++log_->closes;
        log_->unheard_at_close.push_back(log_->submitted - log_->heard);
        log_->open = false;
    }

    [[nodiscard]] bool is_open() const override { return log_->open; }

    bool submit(std::span<const std::byte> burst) override {
        const std::uint64_t held = (log_->submitted - log_->heard) / ac3::kSamplesPerFrame;
        if (!log_->open || held + 1 > log_->capacity_bursts) {
            return false;
        }
        const std::size_t expected = log_->format == BitstreamFormat::kEac3
                                         ? ac3::iec61937::kEac3BurstBytes
                                         : ac3::iec61937::kBurstBytes;
        if (burst.size() != expected) {
            // Failed once, and refused: a REQUIRE would throw through the
            // player, and the player offers the same burst at every pump.
            if (!log_->wrong_size) {
                log_->wrong_size = true;
                FAIL_CHECK("a burst of " << burst.size() << " bytes, not " << expected);
            }
            return false;
        }
        log_->bursts.emplace_back(burst.begin(), burst.end());
        log_->submitted += ac3::kSamplesPerFrame;
        return true;
    }

    [[nodiscard]] std::optional<ac3::audio::MonitorPosition> position() const override {
        if (!log_->open) {
            return std::nullopt;
        }
        return log_->position();
    }

    void flush() override {
        ++log_->flushes;
        log_->bursts_at_flush.push_back(log_->bursts.size());
        log_->restart();
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
    std::shared_ptr<Log> log_;
};

class FakePcm final : public PcmSink {
public:
    struct Log : Clock {
        std::vector<std::uint32_t> rates{};
        bool wrong_width = false;
    };

    explicit FakePcm(std::shared_ptr<Log> log) : log_(std::move(log)) {}

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        ++log_->opens;
        log_->rates.push_back(format.sample_rate);
        log_->open = true;
        log_->paused = false;
        log_->restart();
        width_ = static_cast<std::uint16_t>(format.layout.slots());
        return OpenOutputFormat{.sample_rate = format.sample_rate,
                                .channels = width_,
                                .mode = OutputMode::kLocalPcm};
    }

    void close() override {
        ++log_->closes;
        log_->unheard_at_close.push_back(log_->submitted - log_->heard);
        log_->open = false;
    }

    [[nodiscard]] bool is_open() const override { return log_->open; }

    bool submit(std::span<const std::span<const float>> slots, std::size_t frames) override {
        if (!log_->open || log_->submitted - log_->heard + frames > 8192) {
            return false;
        }
        if (slots.size() != width_) {
            // As the link's size check.
            if (!log_->wrong_width) {
                log_->wrong_width = true;
                FAIL_CHECK(slots.size() << " slots, not " << width_);
            }
            return false;
        }
        log_->submitted += frames;
        return true;
    }

    [[nodiscard]] std::optional<ac3::audio::MonitorPosition> position() const override {
        if (!log_->open) {
            return std::nullopt;
        }
        return log_->position();
    }

    void flush() override {
        ++log_->flushes;
        log_->restart();
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
    std::shared_ptr<Log> log_;
    std::uint16_t width_ = 0;
};

// `count` AC-3 frames of a quiet tone, each its own unit.
Units ac3_units(int count, ac3::SampleRate rate = ac3::SampleRate::k48000) {
    ac3::EncoderConfig config;
    config.sample_rate = rate;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    ac3::FrameEncoder encoder{config};
    Units out;
    for (int f = 0; f < count; ++f) {
        std::vector<float> samples(ac3::kSamplesPerFrame);
        for (std::size_t n = 0; n < samples.size(); ++n) {
            samples[n] = 0.1F * static_cast<float>((static_cast<int>(n) + f * 37) % 50 - 25) / 25.0F;
        }
        const std::vector<std::span<const float>> views(2, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

// `count` E-AC-3 units of 2^numblkscod-ish blocks (1, 2, 3 or 6).
Units eac3_units(int count, int numblkscod = 3) {
    ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    config.numblkscod = numblkscod;
    ac3::eac3::FrameEncoder encoder{config};
    const auto samples_per_unit = static_cast<std::size_t>(encoder.samples_per_frame());
    Units out;
    for (int f = 0; f < count; ++f) {
        std::vector<float> samples(samples_per_unit);
        for (std::size_t n = 0; n < samples.size(); ++n) {
            samples[n] = 0.1F * static_cast<float>((static_cast<int>(n) + f * 29) % 40 - 20) / 20.0F;
        }
        const std::vector<std::span<const float>> views(2, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

// AC-3 frames each encoded on its own with dialnorm 10 + its index, so a
// unit report says which unit it is.
Units numbered_ac3_units(int count) {
    Units out;
    for (int f = 0; f < count; ++f) {
        ac3::EncoderConfig config;
        config.bitrate_kbps = 192;
        config.acmod = ac3::Acmod::k2_0;
        config.dialnorm = 10 + f;
        ac3::FrameEncoder encoder{config};
        const std::vector<float> samples(ac3::kSamplesPerFrame, 0.05F);
        const std::vector<std::span<const float>> views(2, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

// The same for E-AC-3 units of `numblkscod`'s length.
Units numbered_eac3_units(int count, int numblkscod) {
    Units out;
    for (int f = 0; f < count; ++f) {
        ac3::eac3::FrameConfig config;
        config.bitrate_kbps = 192;
        config.acmod = ac3::Acmod::k2_0;
        config.numblkscod = numblkscod;
        config.dialnorm = 10 + f;
        ac3::eac3::FrameEncoder encoder{config};
        const std::vector<float> samples(static_cast<std::size_t>(encoder.samples_per_frame()),
                                         0.05F);
        const std::vector<std::span<const float>> views(2, samples);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        out.push_back(std::move(*frame));
    }
    return out;
}

// Two programmes in one E-AC-3 stream, a unit of each in turn: independent
// substreams 0 and 1.
Bytes two_programmes(int count) {
    ac3::eac3::FrameConfig config;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    config.substreamid = 0;
    ac3::eac3::FrameEncoder first{config};
    config.substreamid = 1;
    ac3::eac3::FrameEncoder second{config};
    const std::vector<float> samples(ac3::kSamplesPerFrame, 0.05F);
    const std::vector<std::span<const float>> views(2, samples);
    Bytes out;
    for (int f = 0; f < count; ++f) {
        for (ac3::eac3::FrameEncoder* encoder : {&first, &second}) {
            auto frame = encoder->encode_frame(views);
            REQUIRE(frame.has_value());
            out.insert(out.end(), frame->begin(), frame->end());
        }
    }
    return out;
}

Bytes joined(const Units& units) {
    Bytes out;
    for (const Bytes& unit : units) {
        out.insert(out.end(), unit.begin(), unit.end());
    }
    return out;
}

// What the link should carry for these units, one after another.
std::vector<Bytes> expected_bursts(const Units& units, BitstreamFormat format) {
    std::vector<Bytes> out;
    ac3::iec61937::Eac3BurstPacker packer;
    for (const Bytes& unit : units) {
        if (format == BitstreamFormat::kAc3) {
            auto burst = ac3::iec61937::wrap_frame(unit);
            REQUIRE(burst.has_value());
            out.push_back(std::move(*burst));
        } else {
            auto burst = packer.push(unit);
            REQUIRE(burst.has_value());
            if (burst->has_value()) {
                out.push_back(std::move(**burst));
            }
        }
    }
    return out;
}

// Items in memory, as raw elementary streams, with an optional part to play.
struct Library {
    struct File {
        Bytes bytes;
        std::uint64_t skip = 0;
        std::optional<std::uint64_t> play{};
    };
    std::map<std::string, File> files;

    [[nodiscard]] ItemLoader loader() const {
        return [this](const std::string& path) -> std::expected<LoadedItem, std::string> {
            const auto found = files.find(path);
            if (found == files.end()) {
                return std::unexpected("no such file: " + path);
            }
            return LoadedItem{.bytes = found->second.bytes,
                              .skip_samples = found->second.skip,
                              .play_samples = found->second.play,
                              .note = {}};
        };
    }
};

QueueItem item(const std::string& path) {
    QueueItem entry;
    entry.path = path;
    entry.title = path;
    return entry;
}

// The decision a test sets: bitstream a stream at 48 kHz to "hdmi", unless
// told not to for one format; decode anything else.
struct Policy {
    bool bitstream_ac3 = true;
    bool bitstream_eac3 = true;
    std::string endpoint = "hdmi";
    // When set, every decision is this.
    std::optional<OutputChoice> fixed{};
    int calls = 0;
    // What the player said it held open at each decision.
    std::vector<ac3::hearth::HeldOutput> held{};
    // The facts of each item decided.
    std::vector<ItemFacts> asked{};

    [[nodiscard]] OutputChoice choose(const ItemFacts& facts,
                                      const ac3::hearth::HeldOutput& holding = {}) {
        ++calls;
        held.push_back(holding);
        asked.push_back(facts);
        if (fixed) {
            return *fixed;
        }
        const bool wanted = facts.stream == BitstreamFormat::kAc3    ? bitstream_ac3
                            : facts.stream == BitstreamFormat::kEac3 ? bitstream_eac3
                                                                     : false;
        if (wanted && facts.sample_rate == 48000) {
            return OutputChoice{.mode = OutputMode::kBitstream,
                                .endpoint_id = endpoint,
                                .endpoint_name = "HDMI",
                                .reason = "Bitstreaming to \"HDMI\" over IEC 61937, untouched."};
        }
        return OutputChoice{.mode = OutputMode::kLocalPcm,
                            .endpoint_id = "speakers",
                            .endpoint_name = "Speakers",
                            .reason = "Decoding here and playing PCM to \"Speakers\"."};
    }
};

struct Rig {
    std::shared_ptr<FakeLink::Log> link = std::make_shared<FakeLink::Log>();
    std::shared_ptr<FakePcm::Log> pcm = std::make_shared<FakePcm::Log>();
    std::shared_ptr<Policy> policy = std::make_shared<Policy>();
    DiagnosticLog diagnostics{256};
    std::unique_ptr<Player> player;

    explicit Rig(const Library& library, bool with_link = true, const DecoderSettings& settings = {}) {
        const auto layout = ac3::render::OutputLayout::parse("2.0");
        REQUIRE(layout.has_value());
        PlayerOutputs outputs;
        outputs.pcm = std::make_unique<FakePcm>(pcm);
        if (with_link) {
            outputs.bitstream = std::make_unique<FakeLink>(link);
        }
        outputs.choose = [policy = policy](const ItemFacts& facts,
                                           const ac3::hearth::HeldOutput& held) {
            return policy->choose(facts, held);
        };
        player = std::make_unique<Player>(std::move(outputs), library.loader(), *layout, settings,
                                          &diagnostics);
    }

    void advance(std::uint64_t frames) {
        link->advance(frames);
        pcm->advance(frames);
    }

    // Plays until the queue has finished and every output has closed.
    bool play_out(std::uint64_t period = 480) {
        for (int step = 0; step < 100000; ++step) {
            player->pump();
            advance(period);
            if (player->transport().state() == TransportState::kStopped && !link->open &&
                !pcm->open) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::vector<std::string> notes() const {
        std::vector<std::string> out;
        for (const std::string& line : diagnostics.lines()) {
            // Past the "+ssss.mmm " stamp.
            out.push_back(line.substr(line.find(' ') + 1));
        }
        return out;
    }
};

bool has_note(const std::vector<std::string>& notes, const std::string& line) {
    return std::ranges::find(notes, line) != notes.end();
}

}  // namespace

TEST_CASE("bitstream: an AC-3 queue goes out a burst a frame, and joins",
          "[hearth][player][bitstream]") {
    const Units a = ac3_units(5);
    const Units b = ac3_units(4);
    Library library;
    library.files["a.ac3"] = {.bytes = joined(a)};
    // The second item's edit list skips into its first frame, which is sent
    // whole all the same.
    library.files["b.ac3"] = {.bytes = joined(b), .skip = 100};
    Rig rig{library};
    rig.player->queue().add(item("a.ac3"));
    rig.player->queue().add(item("b.ac3"));

    rig.player->play();
    CHECK(rig.player->transport().open_format().mode == OutputMode::kBitstream);
    CHECK(rig.player->transport().open_format().stream == BitstreamFormat::kAc3);
    CHECK(rig.player->output_choice().endpoint_id == "hdmi");
    REQUIRE(rig.play_out());

    // One link, opened at the item's rate for the chosen endpoint, and never
    // the PCM device.
    CHECK(rig.link->opens == 1);
    CHECK(rig.link->formats == std::vector<BitstreamFormat>{BitstreamFormat::kAc3});
    CHECK(rig.link->rates == std::vector<std::uint32_t>{48000});
    CHECK(rig.link->endpoints == std::vector<std::string>{"hdmi"});
    CHECK(rig.pcm->opens == 0);
    // Every frame of both items, wrapped, in order, and all of it heard
    // before the link closed.
    Units both = a;
    both.insert(both.end(), b.begin(), b.end());
    CHECK(rig.link->bursts == expected_bursts(both, BitstreamFormat::kAc3));
    CHECK(rig.link->unheard_at_close == std::vector<std::uint64_t>{0});

    const auto& history = rig.player->history();
    REQUIRE(history.size() == 2);
    CHECK(history[0].first_frame == 0);
    CHECK(history[0].frames == 5 * ac3::kSamplesPerFrame);
    CHECK(history[0].expected_frames == 5 * ac3::kSamplesPerFrame);
    CHECK(history[1].first_frame == 5 * ac3::kSamplesPerFrame);
    CHECK(history[1].frames == 4 * ac3::kSamplesPerFrame);
    CHECK(history[1].expected_frames == 4 * ac3::kSamplesPerFrame);
    CHECK(history[1].output_opens == history[0].output_opens);
    // Asked once for each item: at the start, and before the join.
    CHECK(rig.policy->calls == 2);

    const auto notes = rig.notes();
    CHECK(has_note(notes, "output chosen: Bitstreaming to \"HDMI\" over IEC 61937, untouched."));
    CHECK(has_note(notes, "output opened: bitstream (AC-3), 48000 Hz (open 1)"));
    CHECK(has_note(notes, "item 2 \"b.ac3\" joined the open output: AC-3, 48000 Hz, 2 channels, "
                          "0.128 s"));
}

TEST_CASE("bitstream: E-AC-3 units of three blocks are packed two to a burst, across a join",
          "[hearth][player][bitstream]") {
    const Units a = eac3_units(5, /*numblkscod=*/2);
    const Units b = eac3_units(3, /*numblkscod=*/2);
    const std::uint64_t unit_samples = 3 * ac3::kSamplesPerBlock;
    Library library;
    library.files["a.ec3"] = {.bytes = joined(a)};
    library.files["b.ec3"] = {.bytes = joined(b)};
    Rig rig{library};
    rig.player->queue().add(item("a.ec3"));
    rig.player->queue().add(item("b.ec3"));

    rig.player->play();
    CHECK(rig.player->transport().open_format().stream == BitstreamFormat::kEac3);
    rig.player->pump();
    REQUIRE(rig.player->history().size() == 2);

    // The second item is heard from where the first one's last unit ends,
    // part-way through the burst they share, not from where the whole
    // bursts before it end.
    const std::uint64_t join = 5 * unit_samples;
    rig.advance(join - 1);
    CHECK(rig.player->position().item == 0);
    rig.advance(unit_samples + 1);
    CHECK(rig.player->position().item == 1);
    CHECK(rig.player->position().heard == std::chrono::milliseconds{16});
    REQUIRE(rig.play_out());

    // Eight units make four bursts, the third holding the last of the first
    // item and the first of the second.
    Units both = a;
    both.insert(both.end(), b.begin(), b.end());
    const auto expected = expected_bursts(both, BitstreamFormat::kEac3);
    REQUIRE(expected.size() == 4);
    CHECK(rig.link->bursts == expected);
    CHECK(rig.link->opens == 1);
    CHECK(rig.link->unheard_at_close == std::vector<std::uint64_t>{0});

    const auto& history = rig.player->history();
    REQUIRE(history.size() == 2);
    CHECK(history[0].frames == 5 * unit_samples);
    CHECK(history[1].first_frame == 5 * unit_samples);
    CHECK(history[1].frames == 3 * unit_samples);
}

TEST_CASE("bitstream: a unit's report is released when its own frames are heard, in a shared burst",
          "[hearth][player][bitstream]") {
    // Three-block units, two to a burst, each saying which it is. The first
    // unit of a pair is decoded before its burst is whole, so the end of the
    // bursts queued so far is not where it starts.
    const std::uint64_t unit_samples = 3 * ac3::kSamplesPerBlock;
    Library library;
    library.files["a.ec3"] = {.bytes = joined(numbered_eac3_units(8, /*numblkscod=*/2))};
    Rig rig{library};
    rig.player->queue().add(item("a.ec3"));
    rig.player->play();
    rig.player->pump();

    UnitReport report;
    rig.advance(1);
    for (int unit = 0; unit < 8; ++unit) {
        INFO("unit " << unit);
        REQUIRE(rig.player->unit_report(report));
        CHECK(report.dialnorm == 10 + unit);
        // Up to the unit's last frame, nothing newer.
        rig.advance(unit_samples - 1);
        CHECK_FALSE(rig.player->unit_report(report));
        rig.advance(1);
    }
    REQUIRE(rig.play_out());
}

TEST_CASE("bitstream: a different stream or mode reopens, once what was sent has been heard",
          "[hearth][player][bitstream]") {
    // E-AC-3 bitstreamed, then AC-3 bitstreamed - a link at another speed -
    // then a 44.1 kHz item the policy decodes.
    Library library;
    library.files["a.ec3"] = {.bytes = joined(eac3_units(4))};
    library.files["b.ac3"] = {.bytes = joined(ac3_units(3))};
    library.files["c.ac3"] = {.bytes = joined(ac3_units(3, ac3::SampleRate::k44100))};
    Rig rig{library};
    rig.player->queue().add(item("a.ec3"));
    rig.player->queue().add(item("b.ac3"));
    rig.player->queue().add(item("c.ac3"));

    rig.player->play();
    REQUIRE(rig.play_out());

    CHECK(rig.link->opens == 2);
    CHECK(rig.link->formats ==
          std::vector<BitstreamFormat>{BitstreamFormat::kEac3, BitstreamFormat::kAc3});
    CHECK(rig.pcm->opens == 1);
    CHECK(rig.pcm->rates == std::vector<std::uint32_t>{44100});
    // Each output was heard out before the next one opened.
    CHECK(rig.link->unheard_at_close == std::vector<std::uint64_t>{0, 0});
    CHECK(rig.pcm->unheard_at_close == std::vector<std::uint64_t>{0});
    CHECK(rig.player->transport().open_format().mode == OutputMode::kNone);

    const auto& history = rig.player->history();
    REQUIRE(history.size() == 3);
    CHECK(history[1].output_opens == history[0].output_opens + 1);
    CHECK(history[2].output_opens == history[1].output_opens + 1);
    CHECK(history[1].frames == 3 * ac3::kSamplesPerFrame);

    const auto notes = rig.notes();
    CHECK(has_note(notes, "output opened: bitstream (E-AC-3), 48000 Hz (open 1)"));
    CHECK(has_note(notes, "output opened: bitstream (AC-3), 48000 Hz (open 2)"));
    CHECK(has_note(notes, "output opened: local PCM, 44100 Hz, 2 channels (open 3)"));
    CHECK(has_note(notes, "item 2 \"b.ac3\" is next, once the output has played out and "
                          "reopened: \"b.ac3\" is AC-3 and the output is carrying E-AC-3, so it "
                          "reopens - there is a gap."));
    CHECK(has_note(notes, "item 3 \"c.ac3\" is next, once the output has played out and "
                          "reopened: \"c.ac3\" plays as local PCM, and the output is open for "
                          "bitstream, so it reopens - there is a gap."));
}

TEST_CASE("bitstream: a seek flushes the link and sends from the unit the position is in",
          "[hearth][player][bitstream]") {
    // Long enough that the item is still being decoded when the seek comes:
    // a seek once the last unit has gone in is the player's to follow up.
    const Units a = ac3_units(30);
    Library library;
    library.files["a.ac3"] = {.bytes = joined(a)};
    Rig rig{library};
    rig.player->queue().add(item("a.ac3"));
    rig.player->play();
    for (int i = 0; i < 2; ++i) {
        rig.player->pump();
        rig.advance(480);
    }
    REQUIRE_FALSE(rig.link->bursts.empty());

    // 100 ms is sample 4800, in the fourth frame (4608 to 6144): the third is
    // decoded to prime the decoder, and not sent.
    rig.player->seek(std::chrono::milliseconds{100});
    REQUIRE(rig.link->flushes == 1);
    const std::size_t from = rig.link->bursts_at_flush.front();
    CHECK(rig.player->position().heard == std::chrono::milliseconds{96});
    REQUIRE(rig.play_out());

    const std::vector<Bytes> sent(std::next(rig.link->bursts.begin(), static_cast<std::ptrdiff_t>(from)),
                                  rig.link->bursts.end());
    CHECK(sent == expected_bursts(Units(std::next(a.begin(), 3), a.end()), BitstreamFormat::kAc3));
}

TEST_CASE("bitstream: the position and the meters follow the link's clock, and pause reaches it",
          "[hearth][player][bitstream]") {
    // Long enough to be still playing, rather than playing out, at the pause.
    Library library;
    library.files["a.ac3"] = {.bytes = joined(ac3_units(30))};
    Rig rig{library};
    rig.player->queue().add(item("a.ac3"));
    rig.player->play();

    // Nothing heard yet: nothing to release.
    rig.player->pump();
    MeterSnapshot meters;
    UnitReport report;
    CHECK_FALSE(rig.player->meters(meters));
    CHECK_FALSE(rig.player->unit_report(report));
    CHECK(rig.player->position().heard == std::chrono::milliseconds{0});

    // The meters take a snapshot every 2400 frames, stamped with the end of
    // the 256-frame block the interval ends in, so the first is heard at
    // frame 2560 of the link: the decode's own frames, not the bursts
    // already queued ahead of them.
    rig.advance(2559);
    CHECK_FALSE(rig.player->meters(meters));
    rig.advance(1);
    CHECK(rig.player->meters(meters));
    CHECK(meters.output_frame == 2560);

    // Two frames heard: 64 ms, and a unit report to show.
    rig.advance((2 * ac3::kSamplesPerFrame) - 2560);
    rig.player->pump();
    CHECK(rig.player->position().heard == std::chrono::milliseconds{64});
    CHECK(rig.player->position().duration == std::chrono::milliseconds{960});
    CHECK(rig.player->unit_report(report));
    CHECK(report.acmod == ac3::Acmod::k2_0);

    // The decoder settings reach the meters only, and the status says so.
    CHECK_FALSE(rig.player->settings_note().empty());

    rig.player->pause();
    CHECK(rig.link->paused);
    rig.advance(ac3::kSamplesPerFrame);
    CHECK(rig.player->position().heard == std::chrono::milliseconds{64});
    rig.player->play();
    CHECK_FALSE(rig.link->paused);
    REQUIRE(rig.play_out());
    CHECK(rig.player->settings_note().empty());
}

TEST_CASE("bitstream: an edit list's priming or padding inside a unit is sent whole",
          "[hearth][player][bitstream]") {
    const Units a = ac3_units(4);
    Library library;
    // 100 samples of priming, and an end 100 samples short of the third
    // frame's: a receiver decodes whole frames, so the first three go.
    library.files["a.ac3"] = {.bytes = joined(a), .skip = 100, .play = 3 * 1536 - 200};
    Rig rig{library};
    rig.player->queue().add(item("a.ac3"));
    rig.player->play();
    REQUIRE(rig.play_out());

    CHECK(rig.link->bursts ==
          expected_bursts(Units(a.begin(), std::next(a.begin(), 3)), BitstreamFormat::kAc3));
    const auto& history = rig.player->history();
    REQUIRE(history.size() == 1);
    CHECK(history[0].frames == 3 * ac3::kSamplesPerFrame);
    CHECK(history[0].expected_frames == 3 * ac3::kSamplesPerFrame);
}

TEST_CASE("bitstream: with no link, or no usable output, playback stops and says why",
          "[hearth][player][bitstream]") {
    Library library;
    library.files["a.ac3"] = {.bytes = joined(ac3_units(2))};

    SECTION("a player with no passthrough output") {
        Rig rig{library, /*with_link=*/false};
        rig.player->queue().add(item("a.ac3"));
        rig.player->play();
        CHECK(rig.player->transport().state() == TransportState::kStopped);
        CHECK(rig.player->last_error() == "This player has no passthrough output.");
        // The output was at fault, not the item.
        CHECK(rig.player->queue().items()[0].playable());
        CHECK(rig.pcm->opens == 0);
    }

    SECTION("a decision with nothing to play to") {
        Rig rig{library};
        rig.policy->fixed = OutputChoice{.mode = OutputMode::kNone,
                                         .endpoint_id = {},
                                         .endpoint_name = {},
                                         .reason = "This machine reports no output at all."};
        rig.player->queue().add(item("a.ac3"));
        rig.player->play();
        CHECK(rig.player->transport().state() == TransportState::kStopped);
        CHECK(rig.player->last_error() == "This machine reports no output at all.");
        CHECK(rig.player->queue().items()[0].playable());
        CHECK(rig.link->opens == 0);
        CHECK(rig.pcm->opens == 0);
        CHECK(has_note(rig.notes(), "item 1 \"a.ac3\" could not start: This machine reports no "
                                    "output at all."));
    }
}

TEST_CASE("bitstream: when the outputs change, the playing item follows from where it was heard",
          "[hearth][player][bitstream]") {
    Library library;
    library.files["a.ac3"] = {.bytes = joined(ac3_units(30))};
    Rig rig{library};
    rig.player->queue().add(item("a.ac3"));
    rig.player->play();
    rig.player->pump();
    rig.advance(2 * ac3::kSamplesPerFrame);
    rig.player->pump();
    REQUIRE(rig.player->position().heard == std::chrono::milliseconds{64});

    // Nothing has changed: nothing moves.
    CHECK(rig.player->refollow().empty());
    CHECK(rig.link->opens == 1);

    // The receiver no longer takes AC-3: the item is decoded instead, from
    // the frame being heard.
    rig.policy->bitstream_ac3 = false;
    const std::string changed = rig.player->refollow();
    CHECK(changed == "The output changed: Decoding here and playing PCM to \"Speakers\".");
    CHECK(rig.link->closes == 1);
    CHECK(rig.pcm->opens == 1);
    CHECK(rig.player->transport().open_format().mode == OutputMode::kLocalPcm);
    CHECK(rig.player->position().heard == std::chrono::milliseconds{64});
    CHECK(rig.player->transport().state() == TransportState::kPlaying);
    REQUIRE(rig.player->history().size() == 2);
    CHECK(rig.player->history()[1].queue_index == 0);

    // Paused, it moves back paused.
    rig.player->pump();
    rig.player->pause();
    rig.policy->bitstream_ac3 = true;
    CHECK_FALSE(rig.player->refollow().empty());
    CHECK(rig.link->opens == 2);
    CHECK(rig.link->paused);
    CHECK(rig.pcm->closes == 1);
    CHECK(rig.player->transport().state() == TransportState::kPaused);

    rig.player->play();
    CHECK_FALSE(rig.link->paused);

    // The same mode to another endpoint is a move too.
    rig.player->pump();
    rig.policy->endpoint = "hdmi2";
    CHECK_FALSE(rig.player->refollow().empty());
    CHECK(rig.link->endpoints == std::vector<std::string>{"hdmi", "hdmi", "hdmi2"});
    REQUIRE(rig.play_out());
    CHECK(has_note(rig.notes(), "output changed: Decoding here and playing PCM to \"Speakers\"."));
}

TEST_CASE("bitstream: a join the packer cannot make whole bursts of reopens instead",
          "[hearth][player][bitstream]") {
    const std::uint64_t three_blocks = 3 * ac3::kSamplesPerBlock;

    SECTION("units of another length after a burst left open") {
        // Five three-block units leave one waiting for a partner. Six-block
        // units cannot finish that burst, so the link is played out and
        // opened again, and the waiting unit is never sent.
        const Units a = eac3_units(5, /*numblkscod=*/2);
        const Units b = eac3_units(2, /*numblkscod=*/3);
        Library library;
        library.files["a.ec3"] = {.bytes = joined(a)};
        library.files["b.ec3"] = {.bytes = joined(b)};
        Rig rig{library};
        rig.player->queue().add(item("a.ec3"));
        rig.player->queue().add(item("b.ec3"));
        rig.player->play();
        REQUIRE(rig.play_out());

        CHECK(rig.link->opens == 2);
        CHECK(rig.link->unheard_at_close == std::vector<std::uint64_t>{0, 0});
        auto expected = expected_bursts(Units(a.begin(), std::next(a.begin(), 4)),
                                        BitstreamFormat::kEac3);
        const auto second = expected_bursts(b, BitstreamFormat::kEac3);
        expected.insert(expected.end(), second.begin(), second.end());
        CHECK(rig.link->bursts == expected);

        const auto& history = rig.player->history();
        REQUIRE(history.size() == 2);
        // The unit that never made a burst was not played.
        CHECK(history[0].frames == 4 * three_blocks);
        CHECK(history[0].expected_frames == 5 * three_blocks);
        CHECK(history[1].frames == 2 * ac3::kSamplesPerFrame);
        CHECK(history[1].output_opens == history[0].output_opens + 1);
        CHECK(has_note(rig.notes(),
                       "item 2 \"b.ec3\" is next, once the output has played out and reopened: "
                       "\"b.ec3\" has units of another length, which cannot finish the burst the "
                       "item before left open, so the output reopens - there is a gap."));
    }

    SECTION("units that do finish it join") {
        // One-block units make up the three blocks the burst is short of.
        const Units a = eac3_units(5, /*numblkscod=*/2);
        const Units c = eac3_units(9, /*numblkscod=*/0);
        Library library;
        library.files["a.ec3"] = {.bytes = joined(a)};
        library.files["c.ec3"] = {.bytes = joined(c)};
        Rig rig{library};
        rig.player->queue().add(item("a.ec3"));
        rig.player->queue().add(item("c.ec3"));
        rig.player->play();
        REQUIRE(rig.play_out());

        CHECK(rig.link->opens == 1);
        Units both = a;
        both.insert(both.end(), c.begin(), c.end());
        CHECK(rig.link->bursts == expected_bursts(both, BitstreamFormat::kEac3));
        const auto& history = rig.player->history();
        REQUIRE(history.size() == 2);
        CHECK(history[0].frames == 5 * three_blocks);
        CHECK(history[1].first_frame == 5 * three_blocks);
        CHECK(history[1].frames == 9 * ac3::kSamplesPerBlock);
    }
}

TEST_CASE("bitstream: a join to another endpoint reopens there", "[hearth][player][bitstream]") {
    Library library;
    library.files["a.ac3"] = {.bytes = joined(ac3_units(3))};
    library.files["b.ac3"] = {.bytes = joined(ac3_units(3))};
    Rig rig{library};
    rig.player->queue().add(item("a.ac3"));
    rig.player->queue().add(item("b.ac3"));
    rig.player->play();
    // The decision for what comes next names another endpoint.
    rig.policy->endpoint = "hdmi2";
    REQUIRE(rig.play_out());

    CHECK(rig.link->endpoints == std::vector<std::string>{"hdmi", "hdmi2"});
    CHECK(rig.link->unheard_at_close == std::vector<std::uint64_t>{0, 0});
    CHECK(has_note(rig.notes(),
                   "item 2 \"b.ac3\" is next, once the output has played out and reopened: "
                   "\"b.ac3\" plays on \"HDMI\", so the output reopens there - there is a gap."));
}

TEST_CASE("bitstream: an output change during a join waits until the join has been heard",
          "[hearth][player][bitstream]") {
    Library library;
    library.files["a.ac3"] = {.bytes = joined(ac3_units(4))};
    library.files["b.ac3"] = {.bytes = joined(ac3_units(30))};
    Rig rig{library};
    rig.player->queue().add(item("a.ac3"));
    rig.player->queue().add(item("b.ac3"));
    rig.player->play();
    rig.player->pump();
    // b has joined; a is what is being heard.
    REQUIRE(rig.player->history().size() == 2);
    REQUIRE(rig.player->position().item == 0);

    rig.policy->bitstream_ac3 = false;
    CHECK(rig.player->refollow().empty());
    CHECK(rig.link->closes == 0);
    CHECK(rig.pcm->opens == 0);

    // a heard out, and b's first frame too: the move comes now, from there.
    rig.advance((4 * ac3::kSamplesPerFrame) + ac3::kSamplesPerFrame);
    const auto report = rig.player->pump();
    CHECK(report.note == "The output changed: Decoding here and playing PCM to \"Speakers\".");
    CHECK(rig.link->closes == 1);
    CHECK(rig.pcm->opens == 1);
    CHECK(rig.player->position().item == 1);
    CHECK(rig.player->position().heard == std::chrono::milliseconds{32});
    // Asked once for each item, and twice when the move came - whether to
    // move, and then for the output opened - but not while it had to wait.
    CHECK(rig.policy->calls == 4);
}

TEST_CASE("bitstream: the meters and reports keep to the link after a unit that will not decode",
          "[hearth][player][bitstream]") {
    Units a = numbered_ac3_units(12);
    // The fourth frame's CRC no longer holds; with concealment off, it does
    // not decode. It is still sent: the receiver judges its own input.
    a[3][a[3].size() / 2] ^= std::byte{0x5A};
    Library library;
    library.files["a.ac3"] = {.bytes = joined(a)};
    DecoderSettings settings;
    settings.concealment = ac3::ConcealmentPolicy::kNone;
    Rig rig{library, /*with_link=*/true, settings};
    rig.player->queue().add(item("a.ac3"));
    rig.player->play();
    for (int i = 0; i < 5; ++i) {
        rig.player->pump();
        rig.advance(ac3::kSamplesPerFrame);
    }
    rig.player->pump();
    rig.advance(1);
    CHECK(rig.link->bursts.size() >= 6);
    CHECK(rig.link->bursts[3] == expected_bursts(Units{a[3]}, BitstreamFormat::kAc3).front());

    // Five frames and one sample heard: the sixth unit's report is the
    // newest, whatever became of the fourth.
    UnitReport report;
    REQUIRE(rig.player->unit_report(report));
    CHECK(report.dialnorm == 15);
    const auto notes = rig.notes();
    CHECK(std::ranges::any_of(notes, [](const std::string& line) {
        return line.starts_with("item 1 \"a.ac3\" has a unit that could not be decoded: ");
    }));
}

TEST_CASE("bitstream: another programme of a stream is decoded, and the reason says why",
          "[hearth][player][bitstream]") {
    Library library;
    library.files["two.ec3"] = {.bytes = two_programmes(4)};
    DecoderSettings settings;
    settings.programme = 1;
    Rig rig{library, /*with_link=*/true, settings};
    rig.player->queue().add(item("two.ec3"));
    rig.player->play();

    // Asked as if the stream could not be sent at all.
    REQUIRE_FALSE(rig.policy->asked.empty());
    CHECK_FALSE(rig.policy->asked.front().stream.has_value());
    CHECK(rig.player->transport().open_format().mode == OutputMode::kLocalPcm);
    CHECK(rig.player->output_choice().reason ==
          "Decoding here and playing PCM to \"Speakers\". Programme 1 is chosen, and a receiver "
          "plays only a stream's first.");
    CHECK(rig.link->opens == 0);
    REQUIRE(rig.play_out());
}

TEST_CASE("bitstream: a decision the player has no sink for stops playback",
          "[hearth][player][bitstream]") {
    Library library;
    library.files["a.ac3"] = {.bytes = joined(ac3_units(2))};
    auto policy = std::make_shared<Policy>();
    policy->bitstream_ac3 = false;
    PlayerOutputs outputs;
    outputs.bitstream = std::make_unique<FakeLink>(std::make_shared<FakeLink::Log>());
    outputs.choose = [policy](const ItemFacts& facts, const ac3::hearth::HeldOutput& held) {
        return policy->choose(facts, held);
    };
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    Player player{std::move(outputs), library.loader(), *layout};
    player.queue().add(item("a.ac3"));
    player.play();
    CHECK(player.transport().state() == TransportState::kStopped);
    CHECK(player.last_error() == "This player has no local output.");
    CHECK(player.queue().items()[0].playable());
    CHECK_FALSE(player.active());
}

TEST_CASE("bitstream: an item that cannot follow a change while paused leaves its successor paused",
          "[hearth][player][bitstream]") {
    Library library;
    library.files["a.ac3"] = {.bytes = joined(ac3_units(30))};
    library.files["b.ac3"] = {.bytes = joined(ac3_units(30))};
    Rig rig{library};
    rig.player->queue().add(item("a.ac3"));
    rig.player->queue().add(item("b.ac3"));
    rig.player->play();
    rig.player->pump();
    rig.player->pause();

    // a's file has gone when the output changes, so a cannot be opened on
    // the new output, and the queue moves on to b - which waits.
    library.files.erase("a.ac3");
    rig.policy->bitstream_ac3 = false;
    const std::string changed = rig.player->refollow();
    CHECK(changed == "The output changed, and the item could not follow: no such file: a.ac3");
    CHECK_FALSE(rig.player->queue().items()[0].playable());
    CHECK(rig.player->queue().current_index() == 1);
    CHECK(rig.player->transport().state() == TransportState::kPaused);
    CHECK(rig.pcm->opens == 1);
    CHECK(rig.pcm->paused);

    rig.player->play();
    CHECK_FALSE(rig.pcm->paused);
    REQUIRE(rig.play_out());
    CHECK(rig.player->history().back().queue_index == 1);
}

TEST_CASE("bitstream: an engine decides from its endpoints and follows their changes",
          "[hearth][player][bitstream][concurrency]") {
    Library library;
    library.files["a.ac3"] = {.bytes = joined(ac3_units(30))};
    auto link = std::make_shared<FakeLink::Log>();
    auto pcm = std::make_shared<FakePcm::Log>();
    auto takes_ac3 = std::make_shared<std::atomic_bool>(true);
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());

    {
        ac3::hearth::EngineOutputs outputs;
        outputs.pcm = std::make_unique<FakePcm>(pcm);
        outputs.bitstream = std::make_unique<FakeLink>(link);
        // The receiver's answer arrives in its descriptor: the probe of an
        // output the player holds cannot see past the player's own link.
        outputs.endpoints = [takes_ac3](std::uint32_t) {
            ac3::hearth::EndpointReading reading;
            reading.device.id = "hdmi";
            reading.device.name = "HDMI";
            reading.device.supports_ac3_passthrough = true;
            reading.device.channels = 2;
            ac3::audio::SinkAudioCapabilities sink;
            sink.pcm = true;
            sink.ac3 = takes_ac3->load();
            reading.descriptor = sink;
            return std::vector<ac3::hearth::EndpointReading>{reading};
        };
        ac3::hearth::Engine engine{std::move(outputs), library.loader(), *layout};
        engine.add({item("a.ac3")});
        engine.play();
        engine.sync();
        auto status = engine.status();
        CHECK(status.output.mode == OutputMode::kBitstream);
        CHECK(status.output.stream == BitstreamFormat::kAc3);
        CHECK(status.output_reason.find("Bitstreaming AC-3 to \"HDMI\"") != std::string::npos);

        // The receiver changes: nothing happens until the engine is told.
        takes_ac3->store(false);
        engine.refresh_outputs();
        engine.sync();
        status = engine.status();
        CHECK(status.output.mode == OutputMode::kLocalPcm);
        CHECK(status.note.find("The output changed: Decoding here") == 0);

        // A pinned decode, then automatic again with the receiver back. The
        // receiver comes back only once the pin has been decided, so only
        // the refresh can tell the engine.
        engine.set_output_preferences(ac3::hearth::OutputPreferences{
            .pinned = OutputMode::kLocalPcm, .endpoint_id = {}, .follow_sink = true});
        engine.sync();
        takes_ac3->store(true);
        engine.refresh_outputs();
        engine.sync();
        status = engine.status();
        CHECK(status.output_preferences.pinned == OutputMode::kLocalPcm);
        CHECK(status.output.mode == OutputMode::kLocalPcm);
        engine.set_output_preferences(ac3::hearth::OutputPreferences{});
        engine.sync();
        CHECK(engine.status().output.mode == OutputMode::kBitstream);
    }
    // Read once the engine thread has gone.
    CHECK(link->opens == 2);
    CHECK(link->endpoints == std::vector<std::string>{"hdmi", "hdmi"});
    CHECK(pcm->opens == 1);

    // An engine with no passthrough output decodes what an endpoint would
    // take as a bitstream.
    {
        ac3::hearth::EngineOutputs outputs;
        outputs.pcm = std::make_unique<FakePcm>(std::make_shared<FakePcm::Log>());
        outputs.endpoints = [](std::uint32_t) {
            ac3::hearth::EndpointReading reading;
            reading.device.id = "hdmi";
            reading.device.name = "HDMI";
            reading.device.supports_ac3_passthrough = true;
            reading.device.channels = 2;
            return std::vector<ac3::hearth::EndpointReading>{reading};
        };
        ac3::hearth::Engine decoding{std::move(outputs), library.loader(), *layout};
        decoding.add({item("a.ac3")});
        decoding.play();
        decoding.sync();
        CHECK(decoding.status().output.mode == OutputMode::kLocalPcm);
    }

    // An engine given its outputs' decisions takes no choices of its own.
    auto policy = std::make_shared<Policy>();
    PlayerOutputs outputs;
    outputs.pcm = std::make_unique<FakePcm>(std::make_shared<FakePcm::Log>());
    outputs.choose = [policy](const ItemFacts& facts, const ac3::hearth::HeldOutput& held) {
        return policy->choose(facts, held);
    };
    ac3::hearth::Engine owned{std::move(outputs), library.loader(), *layout};
    owned.set_output_preferences(ac3::hearth::OutputPreferences{
        .pinned = OutputMode::kLocalPcm, .endpoint_id = {}, .follow_sink = true});
    owned.sync();
    CHECK(owned.status().note == "This engine's outputs are chosen by its owner, not here.");
    CHECK_FALSE(owned.status().output_preferences.pinned.has_value());
}

TEST_CASE("bitstream: the engine publishes the stream, the reason and the settings note",
          "[hearth][player][bitstream][concurrency]") {
    Library library;
    library.files["a.ec3"] = {.bytes = joined(eac3_units(4))};
    auto link = std::make_shared<FakeLink::Log>();
    auto pcm = std::make_shared<FakePcm::Log>();
    auto policy = std::make_shared<Policy>();
    PlayerOutputs outputs;
    outputs.pcm = std::make_unique<FakePcm>(pcm);
    outputs.bitstream = std::make_unique<FakeLink>(link);
    outputs.choose = [policy](const ItemFacts& facts, const ac3::hearth::HeldOutput& held) {
        return policy->choose(facts, held);
    };
    const auto layout = ac3::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());

    ac3::hearth::Engine engine{std::move(outputs), library.loader(), *layout};
    engine.add({item("a.ec3")});
    engine.play();
    engine.sync();
    const auto status = engine.status();
    CHECK(status.output.mode == OutputMode::kBitstream);
    CHECK(status.output.stream == BitstreamFormat::kEac3);
    CHECK(status.output_reason == "Bitstreaming to \"HDMI\" over IEC 61937, untouched.");
    CHECK_FALSE(status.settings_note.empty());
}
