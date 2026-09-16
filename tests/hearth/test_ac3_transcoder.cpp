#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/metadata_edit.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3/meta/mixing.hpp"
#include "ac3/render/layout.hpp"
#include "ac3_transcoder.hpp"
#include "decoder_settings.hpp"
#include "stream_decoder.hpp"

// ac3::hearth::Ac3Transcoder (apps/hearth/engine/ac3_transcoder.cpp): six
// slots of 5.1 in, AC-3 syncframes out, with the source's metadata written
// into each and every sample taken coming back out after the encoder's
// 256-sample shift.
//
// The input here is synthesised straight onto the six slots rather than
// decoded from an E-AC-3 stream, so what is measured is this generation's
// coding alone. Each slot carries its own frequency, so a channel sent to the
// wrong place fails the comparison instead of passing it.

namespace {

using ac3::hearth::Ac3Transcoder;
using Planar = std::array<std::vector<float>, Ac3Transcoder::kChannels>;

constexpr std::array<double, Ac3Transcoder::kChannels> kHz{440.0, 660.0, 880.0,
                                                           550.0, 770.0, 50.0};

// `frames` samples of each slot's tone, from sample `from` of it.
Planar tones(std::size_t frames, std::size_t from = 0, double level = 0.25) {
    Planar out;
    for (std::size_t slot = 0; slot < out.size(); ++slot) {
        out[slot].resize(frames);
        for (std::size_t n = 0; n < frames; ++n) {
            out[slot][n] = static_cast<float>(
                level * std::sin(2.0 * std::numbers::pi * kHz[slot] *
                                 static_cast<double>(n + from) / 48000.0));
        }
    }
    return out;
}

// What came out: every frame, and the spans each carried.
struct Output {
    std::vector<std::vector<std::byte>> frames;
    std::vector<std::vector<Ac3Transcoder::Span>> spans;

    [[nodiscard]] Ac3Transcoder::FrameFn sink() {
        return [this](std::span<const std::byte> frame, std::span<const Ac3Transcoder::Span> of) {
            frames.emplace_back(frame.begin(), frame.end());
            spans.emplace_back(of.begin(), of.end());
        };
    }
};

// Hands `input` over in blocks of `block`, for `record`.
void take(Ac3Transcoder& transcoder, const Planar& input, std::size_t record,
          std::size_t block = ac3::kSamplesPerBlock) {
    for (std::size_t at = 0; at < input[0].size(); at += block) {
        const std::size_t n = std::min(block, input[0].size() - at);
        std::array<std::span<const float>, Ac3Transcoder::kChannels> slots{};
        for (std::size_t slot = 0; slot < slots.size(); ++slot) {
            slots[slot] = std::span<const float>(input[slot]).subspan(at, n);
        }
        transcoder.take(slots, n, record);
    }
}

// The frames decoded as a receiver with no settings of its own would.
Planar decode(const std::vector<std::vector<std::byte>>& frames) {
    const auto layout = ac3::render::OutputLayout::named("5.1");
    REQUIRE(layout.has_value());
    ac3::hearth::StreamDecoder decoder{*layout, 48000, ac3::hearth::transcode_settings({})};
    Planar out;
    const auto deliver = [&out](std::span<const std::span<const float>> slots, std::size_t n) {
        REQUIRE(slots.size() == out.size());
        for (std::size_t slot = 0; slot < slots.size(); ++slot) {
            const std::span<const float> part = slots[slot].first(n);
            out[slot].insert(out[slot].end(), part.begin(), part.end());
        }
    };
    for (const auto& frame : frames) {
        REQUIRE(decoder.decode(frame, deliver).has_value());
    }
    decoder.finish(deliver);
    return out;
}

// How far `decoded`, shifted back by the encoder's delay, is from `input`, in
// dB of signal to error.
double snr_db(const std::vector<float>& input, const std::vector<float>& decoded) {
    const auto shift = static_cast<std::size_t>(Ac3Transcoder::kDelay);
    REQUIRE(decoded.size() >= input.size() + shift);
    double signal = 0.0;
    double error = 0.0;
    for (std::size_t n = 0; n < input.size(); ++n) {
        const auto want = static_cast<double>(input[n]);
        const auto got = static_cast<double>(decoded[n + shift]);
        signal += want * want;
        error += (got - want) * (got - want);
    }
    return 10.0 * std::log10(signal / std::max(error, 1e-30));
}

ac3::hearth::UnitReport report(int dialnorm, std::optional<std::uint8_t> compr,
                               std::optional<int> bsmod, ac3::Acmod acmod = ac3::Acmod::k3_2) {
    ac3::hearth::UnitReport out;
    out.acmod = acmod;
    out.lfe = true;
    out.dialnorm = dialnorm;
    out.compr = compr;
    out.bsmod = bsmod;
    return out;
}

ac3::io::FrameMetadata metadata(const std::vector<std::byte>& frame) {
    const auto read = ac3::io::read_frame_metadata(frame);
    REQUIRE(read.has_value());
    return *read;
}

}  // namespace

TEST_CASE("transcoder: every slot comes back through AC-3, in its place, 256 samples late",
          "[hearth][transcode]") {
    const Planar input = tones(20 * ac3::kSamplesPerFrame);
    Ac3Transcoder transcoder{48000, {}};
    Output output;
    take(transcoder, input, 0);
    REQUIRE(transcoder.encode_ready(output.sink()).has_value());
    CHECK(output.frames.size() == 20);
    CHECK(transcoder.buffered() == 0);
    REQUIRE(transcoder.finish(output.sink()).has_value());
    // The last 256 samples come out in one more frame.
    CHECK(output.frames.size() == 21);

    const Planar decoded = decode(output.frames);
    for (std::size_t slot = 0; slot < input.size(); ++slot) {
        CAPTURE(slot);
        CHECK(snr_db(input[slot], decoded[slot]) > 50.0);
    }
    // Two slots swapped would not pass.
    CHECK(snr_db(input[0], decoded[2]) < 0.0);

    // 3/2 with LFE, at the rate the command line uses.
    const auto first = metadata(output.frames.front());
    CHECK(first.acmod == ac3::Acmod::k3_2);
    CHECK(first.lfe);
    CHECK(first.bytes == 448 * 1000 / 8 * ac3::kSamplesPerFrame / 48000);
}

TEST_CASE("transcoder: blocks of any size make the same frames", "[hearth][transcode]") {
    const Planar input = tones(4 * ac3::kSamplesPerFrame);
    Output blocks;
    Output ragged;
    Ac3Transcoder a{48000, {}};
    Ac3Transcoder b{48000, {}};
    take(a, input, 0);
    take(b, input, 0, 700);
    REQUIRE(a.finish(blocks.sink()).has_value());
    REQUIRE(b.finish(ragged.sink()).has_value());
    CHECK(blocks.frames == ragged.frames);
}

TEST_CASE("transcoder: the source's dialnorm, compr and service reach each frame",
          "[hearth][transcode]") {
    // Units a frame long. A frame's gain reaches back over the last block of
    // the unit before, so its word is the more attenuating of the two units'
    // - the first four send one, and all boost: +24, +15.6, +18 and +12 dB.
    const std::array<ac3::hearth::UnitReport, 5> units{
        report(20, std::uint8_t{0x40}, 2),
        report(20, std::uint8_t{0x28}, std::nullopt),
        // A reserved dialnorm is 31; code 7 below 2/0 is a voice-over, which
        // 3/2 would call karaoke, so it is not carried.
        report(0, std::uint8_t{0x30}, 7, ac3::Acmod::k1_0),
        // From 2/0 up it is karaoke already.
        report(24, std::uint8_t{0x20}, 7, ac3::Acmod::k2_0),
        // No word: the frame's own counts too, and at this level it is no
        // boost at all.
        report(24, std::nullopt, std::nullopt),
    };
    Ac3Transcoder transcoder{48000, {}};
    Output output;
    for (std::size_t unit = 0; unit < units.size(); ++unit) {
        take(transcoder, tones(ac3::kSamplesPerFrame, unit * ac3::kSamplesPerFrame), 0);
        transcoder.describe_source(units[unit], ac3::kSamplesPerFrame, 0);
        REQUIRE(transcoder.encode_ready(output.sink()).has_value());
    }
    REQUIRE(output.frames.size() == 5);

    const auto a = metadata(output.frames[0]);
    CHECK(a.dialnorm == 20);
    CHECK(a.compr == std::uint8_t{0x40});
    CHECK(a.bsmod == 2);

    // No service said: the item's last stands.
    const auto b = metadata(output.frames[1]);
    CHECK(b.dialnorm == 20);
    CHECK(b.compr == std::uint8_t{0x28});
    CHECK(b.bsmod == 2);

    // The unit before holds the word down, not this frame's own unit.
    const auto c = metadata(output.frames[2]);
    CHECK(c.dialnorm == 31);
    CHECK(c.compr == std::uint8_t{0x28});
    CHECK(c.bsmod == 2);

    const auto d = metadata(output.frames[3]);
    CHECK(d.dialnorm == 24);
    CHECK(d.compr == std::uint8_t{0x20});
    CHECK(d.bsmod == 7);

    const auto e = metadata(output.frames[4]);
    CHECK(e.dialnorm == 24);
    REQUIRE(e.compr.has_value());
    CHECK(ac3::meta::compr_gain(*e.compr) <= 1.0);
    CHECK(e.bsmod == 7);
}

TEST_CASE("transcoder: a frame with no compr word gets one for its own dialnorm",
          "[hearth][transcode]") {
    // Loud enough that the ceiling holds the word down at dialnorm 31 but not
    // at 20, where dialogue sits 11 dB higher already - and then quiet, with
    // the loud frame's tail still in reach of the quiet one's word.
    Planar input = tones(3 * ac3::kSamplesPerFrame, 0, 0.7);
    const Planar quiet = tones(ac3::kSamplesPerFrame, 3 * ac3::kSamplesPerFrame, 0.01);
    for (std::size_t slot = 0; slot < input.size(); ++slot) {
        input[slot].insert(input[slot].end(), quiet[slot].begin(), quiet[slot].end());
    }
    const auto run = [&input](const std::array<int, 4>& dialnorms) {
        Ac3Transcoder transcoder{48000, {}};
        Output output;
        for (std::size_t unit = 0; unit < dialnorms.size(); ++unit) {
            Planar part;
            for (std::size_t slot = 0; slot < part.size(); ++slot) {
                const auto from = std::next(input[slot].begin(),
                                            static_cast<std::ptrdiff_t>(unit * ac3::kSamplesPerFrame));
                part[slot].assign(from, std::next(from, ac3::kSamplesPerFrame));
            }
            take(transcoder, part, 0);
            transcoder.describe_source(report(dialnorms[unit], std::nullopt, std::nullopt),
                                       ac3::kSamplesPerFrame, 0);
            REQUIRE(transcoder.encode_ready(output.sink()).has_value());
        }
        std::vector<std::uint8_t> words;
        for (const auto& frame : output.frames) {
            const auto meta = metadata(frame);
            REQUIRE(meta.compr.has_value());
            words.push_back(*meta.compr);
        }
        return words;
    };
    const auto at_20 = run({20, 20, 20, 20});
    const auto at_31 = run({31, 31, 31, 31});
    const auto changed = run({20, 20, 31, 31});
    REQUIRE(at_31.size() == 4);
    CHECK(ac3::meta::compr_gain(at_31[2]) < ac3::meta::compr_gain(at_20[2]));
    // Where dialnorm moves to 31, the word is 31's.
    CHECK(changed[0] == at_20[0]);
    CHECK(changed[2] == at_31[2]);
    CHECK(changed[3] == at_31[3]);

    // And it is the word an encoder given that dialnorm writes itself.
    ac3::EncoderConfig config;
    config.bitrate_kbps = Ac3Transcoder::kBitrateKbps;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = true;
    config.dialnorm = 31;
    config.heavy.emplace();
    ac3::FrameEncoder encoder{config};
    for (std::size_t frame = 0; frame < 4; ++frame) {
        CAPTURE(frame);
        std::array<std::span<const float>, Ac3Transcoder::kChannels> views{};
        for (std::size_t slot = 0; slot < views.size(); ++slot) {
            views[slot] = std::span<const float>(input[slot])
                              .subspan(frame * ac3::kSamplesPerFrame, ac3::kSamplesPerFrame);
        }
        const auto encoded = encoder.encode_frame(views);
        REQUIRE(encoded.has_value());
        CHECK(metadata(*encoded).compr == std::optional<std::uint8_t>{at_31[frame]});
    }
}

TEST_CASE("transcoder: a frame across a join takes the item that fills most of it",
          "[hearth][transcode]") {
    // The second frame decodes to samples 1280 to 2816 of what was taken. The
    // first item fills more of that when it runs past 2048, and less when it
    // stops short of it.
    const auto second_frame = [](std::size_t first_item) {
        Ac3Transcoder transcoder{48000, {}};
        Output output;
        take(transcoder, tones(first_item), 0);
        transcoder.describe_source(report(20, std::nullopt, 2), first_item, 0);
        const std::size_t rest = (3 * ac3::kSamplesPerFrame) - first_item;
        take(transcoder, tones(rest, first_item), 1);
        // The second item sends no service; the first's is not its.
        transcoder.describe_source(report(24, std::nullopt, std::nullopt), rest, 1);
        REQUIRE(transcoder.encode_ready(output.sink()).has_value());
        REQUIRE(output.frames.size() == 3);
        return metadata(output.frames[1]);
    };
    const auto mostly_first = second_frame(2100);
    CHECK(mostly_first.dialnorm == 20);
    CHECK(mostly_first.bsmod == 2);
    const auto mostly_second = second_frame(2000);
    CHECK(mostly_second.dialnorm == 24);
    CHECK(mostly_second.bsmod == 0);
}

TEST_CASE("transcoder: dual mono heard as its second channel is levelled as that channel",
          "[hearth][transcode]") {
    auto dual = report(31, std::uint8_t{0x40}, std::nullopt, ac3::Acmod::kDualMono);
    dual.dialnorm2 = 20;
    dual.compr2 = std::uint8_t{0x30};
    const auto sent = [&dual](ac3::hearth::DualMonoChoice choice) {
        Ac3Transcoder transcoder{48000, {}};
        Output output;
        take(transcoder, tones(ac3::kSamplesPerFrame), 0);
        transcoder.describe_source(dual, ac3::kSamplesPerFrame, 0, choice);
        REQUIRE(transcoder.encode_ready(output.sink()).has_value());
        REQUIRE(output.frames.size() == 1);
        return metadata(output.frames[0]);
    };
    const auto second = sent(ac3::hearth::DualMonoChoice::kSecond);
    CHECK(second.dialnorm == 20);
    CHECK(second.compr == std::uint8_t{0x30});
    for (const auto choice : {ac3::hearth::DualMonoChoice::kBoth, ac3::hearth::DualMonoChoice::kFirst}) {
        const auto first = sent(choice);
        CHECK(first.dialnorm == 31);
        CHECK(first.compr == std::uint8_t{0x40});
    }
}

TEST_CASE("transcoder: the fold levels are an item's own, or its preferred pair's nearest",
          "[hearth][transcode]") {
    using ac3::meta::CentreMixLevel;
    using ac3::meta::MixLevel;
    using ac3::meta::SurroundMixLevel;
    using Fold = Ac3Transcoder::FoldLevels;
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0F);

    // AC-3 carries AC-3's own.
    ac3::EncoderConfig ac3_config;
    ac3_config.acmod = ac3::Acmod::k3_2;
    ac3_config.lfe = true;
    ac3_config.cmixlev = CentreMixLevel::kMinus3dB;
    ac3_config.surmixlev = SurroundMixLevel::kSilent;
    ac3::FrameEncoder ac3_encoder{ac3_config};
    const std::vector<std::span<const float>> six(6, silence);
    const auto ac3_frame = ac3_encoder.encode_frame(six);
    REQUIRE(ac3_frame.has_value());
    CHECK(Ac3Transcoder::fold_levels(*ac3_frame) ==
          Fold{.centre = CentreMixLevel::kMinus3dB, .surround = SurroundMixLevel::kSilent});

    const auto eac3_fold = [&silence](std::optional<ac3::meta::MixMetadata> mixing) {
        ac3::eac3::FrameConfig config;
        config.acmod = ac3::Acmod::k3_2;
        config.lfe = true;
        config.mixing = mixing;
        ac3::eac3::FrameEncoder encoder{config};
        const std::vector<std::span<const float>> views(6, silence);
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        return Ac3Transcoder::fold_levels(*frame);
    };
    // Lt/Rt preferred: that pair.
    ac3::meta::MixMetadata ltrt;
    ltrt.dmixmod = ac3::meta::DownmixMode::kLtRt;
    ltrt.ltrtcmixlev = MixLevel::kMinus6dB;
    ltrt.ltrtsurmixlev = MixLevel::kMinus3dB;
    ltrt.lorocmixlev = MixLevel::kMinus3dB;
    ltrt.lorosurmixlev = MixLevel::kMinus6dB;
    CHECK(eac3_fold(ltrt) ==
          Fold{.centre = CentreMixLevel::kMinus6dB, .surround = SurroundMixLevel::kMinus3dB});
    // Otherwise Lo/Ro, each to the nearest level AC-3 has.
    ac3::meta::MixMetadata loro;
    loro.dmixmod = ac3::meta::DownmixMode::kLoRo;
    loro.lorocmixlev = MixLevel::kMinus1_5dB;
    loro.lorosurmixlev = MixLevel::kMinus4_5dB;
    CHECK(eac3_fold(loro) ==
          Fold{.centre = CentreMixLevel::kMinus3dB, .surround = SurroundMixLevel::kMinus6dB});
    // Nothing said: the defaults.
    CHECK(eac3_fold(std::nullopt) == Fold{});
    const std::vector<std::byte> noise(64, std::byte{0x5A});
    CHECK(Ac3Transcoder::fold_levels(noise) == Fold{});

    // And every frame carries the transcoder's.
    Ac3Transcoder transcoder{
        48000, Fold{.centre = CentreMixLevel::kMinus6dB, .surround = SurroundMixLevel::kSilent}};
    Output output;
    take(transcoder, tones(2 * ac3::kSamplesPerFrame), 0);
    REQUIRE(transcoder.finish(output.sink()).has_value());
    for (const auto& frame : output.frames) {
        const auto meta = metadata(frame);
        CHECK(meta.cmixlev == CentreMixLevel::kMinus6dB);
        CHECK(meta.surmixlev == SurroundMixLevel::kSilent);
    }
}

TEST_CASE("transcoder: finish pads with each channel's last sample until everything is out",
          "[hearth][transcode]") {
    SECTION("a part-frame whose end the encoder still holds, then one more") {
        // A steady level, so padding that dropped to silence would show. The
        // part-frame's samples run into its last 256, which come out only in
        // the frame after.
        Planar input;
        for (auto& slot : input) {
            slot.assign((2 * ac3::kSamplesPerFrame) + 1400, 0.25F);
        }
        Ac3Transcoder transcoder{48000, {}};
        Output output;
        take(transcoder, input, 3);
        REQUIRE(transcoder.encode_ready(output.sink()).has_value());
        CHECK(output.frames.size() == 2);
        CHECK(transcoder.buffered() == 1400);
        REQUIRE(transcoder.finish(output.sink()).has_value());
        REQUIRE(output.frames.size() == 4);
        CHECK(transcoder.buffered() == 0);

        using Span = Ac3Transcoder::Span;
        const auto spans = [&output](std::size_t frame) {
            std::vector<std::pair<std::size_t, std::uint64_t>> out;
            for (const Span& span : output.spans[frame]) {
                out.emplace_back(span.record, span.frames);
            }
            return out;
        };
        using Pairs = std::vector<std::pair<std::size_t, std::uint64_t>>;
        CHECK(spans(0) == Pairs{{3, ac3::kSamplesPerFrame}});
        CHECK(spans(1) == Pairs{{3, ac3::kSamplesPerFrame}});
        CHECK(spans(2) == Pairs{{3, 1400}});
        CHECK(spans(3).empty());

        // Past the end, the level holds rather than dropping away.
        const Planar decoded = decode(output.frames);
        const std::size_t end = input[0].size() + Ac3Transcoder::kDelay;
        REQUIRE(decoded[0].size() >= end + 1000);
        for (std::size_t n = end + 200; n < end + 1000; n += 100) {
            CAPTURE(n);
            CHECK(std::abs(decoded[0][n] - 0.25F) < 0.05F);
        }
    }

    SECTION("a short part-frame comes out in its own frame") {
        Ac3Transcoder transcoder{48000, {}};
        Output output;
        take(transcoder, tones((2 * ac3::kSamplesPerFrame) + 100), 0);
        REQUIRE(transcoder.finish(output.sink()).has_value());
        CHECK(output.frames.size() == 3);
    }

    SECTION("whole frames still need one more") {
        Ac3Transcoder transcoder{48000, {}};
        Output output;
        take(transcoder, tones(2 * ac3::kSamplesPerFrame), 0);
        REQUIRE(transcoder.finish(output.sink()).has_value());
        CHECK(output.frames.size() == 3);
    }

    SECTION("nothing taken, nothing sent") {
        Ac3Transcoder transcoder{48000, {}};
        Output output;
        REQUIRE(transcoder.finish(output.sink()).has_value());
        CHECK(output.frames.empty());
    }
}

TEST_CASE("transcoder: a frame's spans follow each record across it", "[hearth][transcode]") {
    Ac3Transcoder transcoder{48000, {}};
    Output output;
    for (std::size_t record = 0; record < 3; ++record) {
        take(transcoder, tones(1000, record * 1000), record, 300);
    }
    REQUIRE(transcoder.encode_ready(output.sink()).has_value());
    REQUIRE(transcoder.finish(output.sink()).has_value());
    REQUIRE(output.frames.size() == 3);

    const auto frames_of = [&output](std::size_t frame, std::size_t record) {
        std::uint64_t total = 0;
        for (const auto& span : output.spans[frame]) {
            if (span.record == record) {
                total += span.frames;
            }
        }
        return total;
    };
    CHECK(frames_of(0, 0) == 1000);
    CHECK(frames_of(0, 1) == 536);
    CHECK(frames_of(1, 1) == 464);
    CHECK(frames_of(1, 2) == 1000);
    CHECK(output.spans[2].empty());
}

TEST_CASE("transcoder: AC-3's rates, and a reset", "[hearth][transcode]") {
    CHECK(Ac3Transcoder::carries(48000));
    CHECK(Ac3Transcoder::carries(44100));
    CHECK(Ac3Transcoder::carries(32000));
    CHECK_FALSE(Ac3Transcoder::carries(24000));
    CHECK_FALSE(Ac3Transcoder::carries(96000));

    // A rate AC-3 does not have is refused when the first frame is made.
    Ac3Transcoder half{24000, {}};
    Output refused;
    take(half, tones(ac3::kSamplesPerFrame), 0);
    const auto made = half.encode_ready(refused.sink());
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error() == "AC-3 has no 24000 Hz.");
    CHECK(refused.frames.empty());

    // A reset drops what was taken; the frames after it are a fresh stream,
    // the same as a new transcoder's.
    const Planar input = tones(3 * ac3::kSamplesPerFrame);
    Ac3Transcoder reused{44100, {}};
    Output before;
    take(reused, tones(1000, 5000), 0);
    reused.describe_source(report(12, std::uint8_t{0x22}, 1), 1000, 0);
    reused.reset();
    CHECK(reused.buffered() == 0);
    take(reused, input, 0);
    REQUIRE(reused.finish(before.sink()).has_value());

    Ac3Transcoder fresh{44100, {}};
    Output after;
    take(fresh, input, 0);
    REQUIRE(fresh.finish(after.sink()).has_value());
    CHECK(before.frames == after.frames);
    CHECK(metadata(after.frames.front()).sample_rate == ac3::SampleRate::k44100);
}
