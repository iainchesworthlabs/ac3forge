#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "stream_playback.hpp"

// apps/common/stream_playback.hpp: the decoder choice and the end-of-stream
// flush that ac3cli's 'monitor' and 'spatial' play a stream with. Both
// commands open a render device before their first unit plays, so
// tests/cli/test_cli_live.cpp can only show what they do on a machine that has
// one. These cases hold the two pieces those loops are built from on every CI
// leg, with real encoders and real decoders.
//
// Each held-back stream below is encoded one access unit longer than it is
// kept. Decoding the longer stream releases the shorter one's last unit
// through decode_access_unit, so that unit is what held_back_unit has to
// rebuild from flush(), sample for sample: the unit that releases it carries
// no transient, and §3.7's correction only reaches back from a frame that
// signals one.

namespace {

using ac3::Acmod;
using ac3::DownmixTarget;
namespace cm = ac3::eac3::chanmap;

constexpr int kUnits = 5;
constexpr auto kFrame = static_cast<std::size_t>(ac3::kSamplesPerFrame);
// Late in the frame, where block switching - and with it transient pre-noise
// processing - fires; the decoder tests' own onset.
constexpr std::size_t kOnsetSample = 960;

// The 7.1 tones test_eac3_decoder.cpp's legacy-core test uses: the
// dependent's Ls/Rs are not the bed's, so a unit that kept the bed's there
// shows it.
constexpr std::array<double, 6> kBedTones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
constexpr std::array<double, 4> kRearTones = {500.0, 1600.0, 400.0, 1800.0};

// One access unit of PCM: silence until kOnsetSample of `onset_unit`, then
// each channel's own tone, steady from there on so no later unit signals a
// correction. A cosine, so the onset is a step well clear of §8.2.2's silence
// gate.
std::vector<std::vector<float>> unit_pcm(std::span<const double> tones, int unit,
                                         int onset_unit) {
    const auto onset = static_cast<std::size_t>(onset_unit) * kFrame + kOnsetSample;
    std::vector<std::vector<float>> pcm(tones.size(), std::vector<float>(kFrame, 0.0F));
    for (std::size_t ch = 0; ch < tones.size(); ++ch) {
        for (std::size_t i = 0; i < kFrame; ++i) {
            const auto n = static_cast<std::size_t>(unit) * kFrame + i;
            if (n < onset) {
                continue;
            }
            const double t = static_cast<double>(n - onset) / 48000.0;
            pcm[ch][i] =
                static_cast<float>(0.4 * std::cos(2.0 * std::numbers::pi * tones[ch] * t));
        }
    }
    return pcm;
}

std::vector<std::span<const float>> views(const std::vector<std::vector<float>>& pcm) {
    return {pcm.begin(), pcm.end()};
}

// `units` access units whose last one is still held back when the stream
// ends, and the same units with one more behind them, which releases it.
struct HeldAndReleased {
    std::vector<std::byte> held;
    std::vector<std::byte> released;
};

void append(HeldAndReleased& streams, int unit, int units, std::span<const std::byte> bytes) {
    streams.released.insert(streams.released.end(), bytes.begin(), bytes.end());
    if (unit < units) {
        streams.held.insert(streams.held.end(), bytes.begin(), bytes.end());
    }
}

// §E2.3.1.2's legacy core: an AC-3 5.1 bed with a 7.1 rear dependent behind
// it. Only the dependent can hold its unit back - AC-3 has no transproce.
HeldAndReleased legacy_core_streams(int units, int onset_unit) {
    ac3::FrameEncoder core{{.bitrate_kbps = 448, .acmod = Acmod::k3_2, .lfe = true}};
    ac3::eac3::FrameEncoder rear{{.bitrate_kbps = 320,
                                  .acmod = Acmod::k2_2,
                                  .strmtyp = ac3::eac3::StreamType::kDependent,
                                  .substreamid = 0,
                                  .chanmap = cm::k71Rear,
                                  .last_dependent = true,
                                  .transient_prenoise = true}};
    HeldAndReleased streams;
    for (int unit = 0; unit <= units; ++unit) {
        const auto bed = unit_pcm(kBedTones, unit, onset_unit);
        const auto core_frame = core.encode_frame(views(bed));
        REQUIRE(core_frame.has_value());
        append(streams, unit, units, *core_frame);
        const auto dep = unit_pcm(kRearTones, unit, onset_unit);
        const auto dep_frame = rear.encode_frame(views(dep));
        REQUIRE(dep_frame.has_value());
        append(streams, unit, units, *dep_frame);
    }
    return streams;
}

// A genuine E-AC-3 5.1 bed that holds its units back, with the same rear
// dependent beside it releasing every call.
HeldAndReleased bed_and_dependent_streams() {
    ac3::eac3::AccessUnitEncoder encoder{
        {.independent = {.bitrate_kbps = 448,
                         .acmod = Acmod::k3_2,
                         .lfe = true,
                         .transient_prenoise = true},
         .dependents = {{.bitrate_kbps = 320, .acmod = Acmod::k2_2, .chanmap = cm::k71Rear}}}};
    HeldAndReleased streams;
    for (int unit = 0; unit <= kUnits; ++unit) {
        auto pcm = unit_pcm(kBedTones, unit, 2);
        auto rear = unit_pcm(kRearTones, unit, 2);
        pcm.insert(pcm.end(), rear.begin(), rear.end());
        const auto encoded = encoder.encode_access_unit(views(pcm));
        REQUIRE(encoded.has_value());
        append(streams, unit, kUnits, encoded->bytes);
    }
    return streams;
}

// One E-AC-3 substream per unit, holding back. 1+1 has to say Ch2's own
// dialnorm.
HeldAndReleased single_substream_streams(Acmod acmod, bool lfe, std::span<const double> tones) {
    ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 448,
         .acmod = acmod,
         .lfe = lfe,
         .dialnorm2 = acmod == Acmod::kDualMono ? std::optional<int>{31} : std::nullopt,
         .transient_prenoise = true}};
    REQUIRE(static_cast<std::size_t>(encoder.channel_count()) == tones.size());
    HeldAndReleased streams;
    for (int unit = 0; unit <= kUnits; ++unit) {
        const auto pcm = unit_pcm(tones, unit, 2);
        const auto frame = encoder.encode_frame(views(pcm));
        REQUIRE(frame.has_value());
        append(streams, unit, kUnits, *frame);
    }
    return streams;
}

// What 'monitor' does with a stream it reads as access units: every unit
// decode_access_unit hands back, then - with `flush` - the unit
// held_back_unit rebuilds from flush(), laid out against the first unit's
// layout.
std::vector<ac3::DecodedAccessUnit> play(std::span<const std::byte> stream,
                                         const ac3::DecoderConfig& config, bool flush) {
    const auto units = ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    ac3::Eac3Decoder decoder{config};
    std::vector<ac3::DecodedAccessUnit> played;
    std::optional<cm::Layout> programme;
    for (const auto& unit : *units) {
        auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            continue;
        }
        if (!programme.has_value()) {
            programme = (*decoded)->layout;
        }
        played.push_back(std::move(**decoded));
    }
    if (flush) {
        auto held = ac3::apps::held_back_unit(decoder.flush(), programme,
                                              config.output.target != DownmixTarget::kAsCoded);
        if (held.has_value()) {
            played.push_back(std::move(*held));
        }
    }
    return played;
}

std::size_t samples_played(const std::vector<ac3::DecodedAccessUnit>& played) {
    std::size_t total = 0;
    for (const auto& unit : played) {
        total += unit.channels.empty() ? 0 : unit.channels.front().size();
    }
    return total;
}

bool same_layout(const cm::Layout& a, const cm::Layout& b) {
    return a.count == b.count && std::equal(a.begin(), a.end(), b.begin());
}

// The power of one frequency in `x`, whatever its phase.
double tone_power(std::span<const float> x, double hz) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / 48000.0;
        re += static_cast<double>(x[i]) * std::cos(phase);
        im += static_cast<double>(x[i]) * std::sin(phase);
    }
    return re * re + im * im;
}

// The held stream played with the flush against the released one played
// without it: the same units, the last rebuilt by held_back_unit in one and
// assembled by decode_access_unit in the other.
void check_held_unit_matches_release(const HeldAndReleased& streams,
                                     const ac3::DecoderConfig& config) {
    const auto played = play(streams.held, config, true);
    const auto reference = play(streams.released, config, false);
    REQUIRE(played.size() == static_cast<std::size_t>(kUnits));
    REQUIRE(reference.size() == static_cast<std::size_t>(kUnits));
    CHECK(samples_played(played) == static_cast<std::size_t>(kUnits) * kFrame);
    const auto& held = played.back();
    const auto& released = reference.back();
    CHECK(held.sample_rate == released.sample_rate);
    CHECK(held.acmod == released.acmod);
    CHECK(same_layout(held.layout, released.layout));
    REQUIRE(held.channels.size() == released.channels.size());
    // A named bool: a failing comparison of two frames of PCM would have
    // Catch2 print every sample of both.
    const bool same_audio = held.channels == released.channels;
    CHECK(same_audio);
}

}  // namespace

TEST_CASE("reads_as_access_units sends a legacy core to the access-unit decoder",
          "[decoder][eac3][monitor]") {
    ac3::FrameEncoder encoder{{.bitrate_kbps = 448, .acmod = Acmod::k3_2, .lfe = true}};
    std::vector<std::byte> plain;
    for (int unit = 0; unit < 3; ++unit) {
        const auto pcm = unit_pcm(kBedTones, unit, 0);
        const auto frame = encoder.encode_frame(views(pcm));
        REQUIRE(frame.has_value());
        plain.insert(plain.end(), frame->begin(), frame->end());
    }
    const auto legacy = legacy_core_streams(3, 1).held;
    const auto eac3 = single_substream_streams(Acmod::k2_0, false, std::array{1000.0, 1500.0}).held;

    // The first frame of a legacy-core stream says AC-3 exactly as a plain
    // AC-3 stream's does, which is what 'monitor' used to dispatch on.
    REQUIRE(ac3::stream_bsid(legacy).value_or(-1) <= 8);
    REQUIRE(ac3::stream_bsid(plain).value_or(-1) <= 8);

    CHECK_FALSE(ac3::apps::reads_as_access_units(plain));
    CHECK(ac3::apps::reads_as_access_units(legacy));
    CHECK(ac3::apps::reads_as_access_units(eac3));
    CHECK_FALSE(ac3::apps::reads_as_access_units(std::span(plain).first(3)));

    // FrameDecoder refuses the legacy-core stream at its first dependent -
    // the failure 'monitor' printed for one - and the access-unit path reads
    // every unit of it.
    const auto frames = ac3::split_frames(legacy);
    REQUIRE(frames.has_value());
    REQUIRE(frames->size() == 6);
    ac3::FrameDecoder frame_decoder;
    REQUIRE(frame_decoder.decode_frame((*frames)[0]).has_value());
    CHECK_FALSE(frame_decoder.decode_frame((*frames)[1]).has_value());
    const auto played = play(legacy, {}, true);
    REQUIRE(played.size() == 3);
    CHECK(played.front().channels.size() == 8);
}

TEST_CASE("held_back_unit is empty when nothing was held back", "[decoder][eac3][monitor]") {
    CHECK_FALSE(ac3::apps::held_back_unit({}, std::nullopt, false).has_value());

    // A stream that never used the tool leaves flush() nothing to return.
    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = Acmod::k2_0}};
    ac3::Eac3Decoder decoder;
    for (int unit = 0; unit < 3; ++unit) {
        const auto pcm = unit_pcm(std::array{1000.0, 1500.0}, unit, 0);
        const auto frame = encoder.encode_frame(views(pcm));
        REQUIRE(frame.has_value());
        const auto decoded = decoder.decode_access_unit(*frame);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
    }
    CHECK_FALSE(ac3::apps::held_back_unit(decoder.flush(), std::nullopt, false).has_value());

    // A dependent with no bed beside it has nothing to extend.
    ac3::DecodedSubstream orphan;
    orphan.strmtyp = ac3::eac3::StreamType::kDependent;
    orphan.acmod = Acmod::k2_2;
    orphan.chanmap = cm::k71Rear;
    orphan.channels.assign(4, std::vector<float>(kFrame, 0.25F));
    std::vector<ac3::DecodedSubstream> flushed;
    flushed.push_back(std::move(orphan));
    CHECK_FALSE(ac3::apps::held_back_unit(std::move(flushed), std::nullopt, false).has_value());
}

TEST_CASE("a single substream's held-back last unit plays, folded or not",
          "[decoder][eac3][monitor]") {
    const auto streams =
        single_substream_streams(Acmod::k3_2, true, std::span<const double>(kBedTones));

    // Without the flush the stream comes out a unit short: the last unit is
    // still inside the decoder.
    CHECK(play(streams.held, {}, false).size() == static_cast<std::size_t>(kUnits - 1));

    for (const auto target : {DownmixTarget::kAsCoded, DownmixTarget::kLoRo, DownmixTarget::kMono}) {
        INFO("target " << static_cast<int>(target));
        check_held_unit_matches_release(streams, {.output = {.target = target}});
    }
}

TEST_CASE("a legacy core's held-back unit lays its dependent over the core",
          "[decoder][eac3][monitor]") {
    const auto streams = legacy_core_streams(kUnits, 2);

    // The premise: the core is never held, so flush() returns the held
    // dependent first and the core that was waiting for it after.
    {
        const auto units = ac3::split_access_units(streams.held);
        REQUIRE(units.has_value());
        ac3::Eac3Decoder decoder;
        for (const auto& unit : *units) {
            REQUIRE(decoder.decode_access_unit(unit).has_value());
        }
        const auto flushed = decoder.flush();
        REQUIRE(flushed.size() == 2);
        CHECK(flushed[0].strmtyp == ac3::eac3::StreamType::kDependent);
        CHECK(flushed[1].bsid <= 8);
    }

    check_held_unit_matches_release(streams, {});

    // The Ls slot carries the dependent's tone rather than the core's, the
    // §E3.8.2 overwrite the order above would undo if taken as given.
    const auto played = play(streams.held, {}, true);
    REQUIRE(played.size() == static_cast<std::size_t>(kUnits));
    const auto& held = played.back();
    const int ls = held.layout.index_of(cm::Location::kLeftSurround);
    REQUIRE(ls >= 0);
    const auto& channel = held.channels[static_cast<std::size_t>(ls)];
    CHECK(tone_power(channel, kRearTones[0]) > 100.0 * tone_power(channel, kBedTones[3]));
}

TEST_CASE("a held-back bed takes the dependent that was waiting for it",
          "[decoder][eac3][monitor]") {
    check_held_unit_matches_release(bed_and_dependent_streams(), {});
}

TEST_CASE("under a fold, a legacy core's held-back unit is the core's own fold",
          "[decoder][eac3][monitor]") {
    const auto streams = legacy_core_streams(kUnits, 2);
    const auto frames = ac3::split_frames(streams.held);
    REQUIRE(frames.has_value());

    for (const auto target : {DownmixTarget::kLoRo, DownmixTarget::kMono}) {
        INFO("target " << static_cast<int>(target));
        const ac3::DecoderConfig config{.output = {.target = target}};
        const std::size_t width = target == DownmixTarget::kMono ? 1 : 2;

        // Every unit folds through Eac3Decoder, the core as much as the rest:
        // the esp-idf player's reason for giving a lone AC-3 syncframe to
        // FrameDecoder under a fold was fixed in the library by #690, so a
        // player folding a legacy-core stream needs no such split.
        const auto played = play(streams.held, config, true);
        REQUIRE(played.size() == static_cast<std::size_t>(kUnits));
        for (const auto& unit : played) {
            CHECK(unit.channels.size() == width);
        }
        CHECK(samples_played(played) == static_cast<std::size_t>(kUnits) * kFrame);

        // flush() folded the core and the dependent each on its own, and the
        // unit is the core's fold: what FrameDecoder folds the same core
        // frames to.
        ac3::FrameDecoder core_decoder{config};
        std::vector<std::vector<float>> core_fold;
        for (const auto& frame : *frames) {
            if (ac3::stream_bsid(frame).value_or(16) > 8) {
                continue;
            }
            auto decoded = core_decoder.decode_frame(frame);
            REQUIRE(decoded.has_value());
            core_fold = std::move(decoded->channels);
        }
        const bool same_audio = played.back().channels == core_fold;
        CHECK(same_audio);
    }
}

TEST_CASE("a stream held back before any unit came out still plays that unit",
          "[decoder][eac3][monitor]") {
    // One unit, whose dependent holds it back from the start: nothing comes
    // out of the loop, so the layout comes from the flushed substreams.
    const auto streams = legacy_core_streams(1, 0);
    CHECK(play(streams.held, {}, false).empty());
    const auto units = ac3::split_access_units(streams.held);
    REQUIRE(units.has_value());
    ac3::Eac3Decoder decoder;
    REQUIRE(units->size() == 1);
    const auto decoded = decoder.decode_access_unit(units->front());
    REQUIRE(decoded.has_value());
    REQUIRE_FALSE(decoded->has_value());
    const auto held = ac3::apps::held_back_unit(decoder.flush(), std::nullopt, false);
    REQUIRE(held.has_value());

    const auto reference = play(streams.released, {}, false);
    REQUIRE(reference.size() == 1);
    CHECK(held->layout.count == 8);
    CHECK(same_layout(held->layout, reference.front().layout));
    const bool same_audio = held->channels == reference.front().channels;
    CHECK(same_audio);
}

TEST_CASE("a dual mono held-back unit stays in coded order", "[decoder][eac3][monitor]") {
    const auto streams =
        single_substream_streams(Acmod::kDualMono, false, std::array{1000.0, 1500.0});
    check_held_unit_matches_release(streams, {});
    const auto played = play(streams.held, {}, true);
    REQUIRE_FALSE(played.empty());
    CHECK(played.back().layout.count == 0);
    CHECK(played.back().channels.size() == 2);
}
