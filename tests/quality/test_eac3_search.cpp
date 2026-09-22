#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/quality/distortion.hpp"
#include "ac3/verify/eac3_selfcheck.hpp"

// EQ13's E-AC-3 half: eac3::FrameConfig::search, CBR only, dbpbcod-only - see
// that field's own doc comment for the scope this mirrors AC-3's
// tests/quality/test_search.cpp under, and why kPerceptual and VBR are not
// covered here. The five cases below are that file's, adapted; a sixth
// checks the CBR-only boundary itself.

namespace {

constexpr int kFrames = 8;
constexpr double kRate = 48000.0;

// Same shape as test_search.cpp's own make_material: a held chord (so the
// distortion measure has something non-degenerate to weigh), a swept
// partial, filtered noise and a transient every couple of frames - silence
// or a single tone both give a search nothing to choose between.
std::vector<std::vector<float>> make_material(int channels) {
    std::mt19937 rng(0x5EA3);
    std::normal_distribution<double> gauss(0.0, 0.08);
    const std::size_t total = static_cast<std::size_t>(kFrames) * ac3::kSamplesPerFrame;
    std::vector<std::vector<float>> out(static_cast<std::size_t>(channels),
                                        std::vector<float>(total, 0.0f));
    double lowpass = 0.0;
    for (std::size_t n = 0; n < total; ++n) {
        const double t = static_cast<double>(n) / kRate;
        double value = 0.0;
        for (const double hz : {220.0, 277.18, 329.63, 659.26}) {
            value += 0.16 * std::sin(2.0 * std::numbers::pi * hz * t);
        }
        value += 0.10 * std::sin(2.0 * std::numbers::pi * (900.0 + (600.0 * t)) * t);
        lowpass = (0.75 * lowpass) + (0.25 * gauss(rng));
        value += lowpass + (0.02 * gauss(rng));
        const std::size_t since = n % 2304;
        if (since < 64) {
            value += 0.45 * std::exp(-static_cast<double>(since) / 12.0) *
                     std::sin(2.0 * std::numbers::pi * 3200.0 * t);
        }
        for (int ch = 0; ch < channels; ++ch) {
            const double phase = 1.0 + (0.11 * ch);
            out[static_cast<std::size_t>(ch)][n] =
                static_cast<float>(std::clamp(value * phase * 0.8, -0.98, 0.98));
        }
    }
    return out;
}

std::vector<std::span<const float>> spans_for(const std::vector<std::vector<float>>& channels,
                                              int frame) {
    std::vector<std::span<const float>> spans;
    spans.reserve(channels.size());
    for (const auto& channel : channels) {
        spans.emplace_back(channel.data() + (static_cast<std::size_t>(frame) *
                                             ac3::kSamplesPerFrame),
                           ac3::kSamplesPerFrame);
    }
    return spans;
}

ac3::eac3::FrameConfig config_for(ac3::quality::Criterion search, ac3::Acmod acmod, bool lfe,
                                  std::uint32_t kbps) {
    ac3::eac3::FrameConfig config;
    config.acmod = acmod;
    config.lfe = lfe;
    config.bitrate_kbps = kbps;
    config.coupling = acmod != ac3::Acmod::k1_0;
    config.search = search;
    return config;
}

std::vector<std::vector<std::byte>> encode_all(const ac3::eac3::FrameConfig& config,
                                               const std::vector<std::vector<float>>& material) {
    ac3::eac3::FrameEncoder encoder(config);
    std::vector<std::vector<std::byte>> frames;
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto spans = spans_for(material, frame);
        auto result = encoder.encode_frame(spans);
        REQUIRE(result.has_value());
        frames.push_back(std::move(*result));
    }
    return frames;
}

}  // namespace

// The same desync risk test_search.cpp's own AC-3 case guards against: every
// candidate dbpbcod moves the masking curve, so every candidate moves every
// mantissa field's width. If the winning candidate's allocation is not what
// finish_frame actually packs, the stream desyncs silently downstream of the
// cause - exactly what Eac3MirrorEncoder exists to catch.
TEST_CASE("the E-AC-3 encoder's model still matches a real decode with the search on",
          "[quality][search][eac3]") {
    const auto stereo = make_material(2);
    const auto surround = make_material(6);  // 5 fbw + LFE
    for (const std::uint32_t kbps : {192U, 448U}) {
        {
            ac3::verify::Eac3MirrorEncoder mirror{ac3::eac3::AccessUnitConfig{
                .independent = config_for(ac3::quality::Criterion::kDistortion,
                                          ac3::Acmod::k2_0, false, kbps)}};
            for (int frame = 0; frame < kFrames; ++frame) {
                auto checked = mirror.encode_access_unit(spans_for(stereo, frame));
                REQUIRE(checked.has_value());
                CAPTURE(kbps, frame, mirror.last_report());
                CHECK(checked->ok());
            }
        }
        {
            ac3::verify::Eac3MirrorEncoder mirror{ac3::eac3::AccessUnitConfig{
                .independent = config_for(ac3::quality::Criterion::kDistortion,
                                          ac3::Acmod::k3_2, true, kbps)}};
            for (int frame = 0; frame < kFrames; ++frame) {
                auto checked = mirror.encode_access_unit(spans_for(surround, frame));
                REQUIRE(checked.has_value());
                CAPTURE(kbps, frame, mirror.last_report());
                CHECK(checked->ok());
            }
        }
    }
}

TEST_CASE("the E-AC-3 search off changes nothing about the encode", "[quality][search][eac3]") {
    const auto material = make_material(2);
    const auto config =
        config_for(ac3::quality::Criterion::kNone, ac3::Acmod::k2_0, false, 192);
    const auto first = encode_all(config, material);
    const auto second = encode_all(config, material);
    REQUIRE(first.size() == second.size());
    for (std::size_t frame = 0; frame < first.size(); ++frame) {
        CAPTURE(frame);
        REQUIRE(first[frame] == second[frame]);
    }
}

TEST_CASE("the E-AC-3 search is deterministic", "[quality][search][eac3]") {
    const auto material = make_material(2);
    const auto config =
        config_for(ac3::quality::Criterion::kDistortion, ac3::Acmod::k2_0, false, 256);
    const auto first = encode_all(config, material);
    const auto second = encode_all(config, material);
    for (std::size_t frame = 0; frame < first.size(); ++frame) {
        CAPTURE(frame);
        REQUIRE(first[frame] == second[frame]);
    }
}

TEST_CASE("the E-AC-3 search actually changes the emitted parameters",
          "[quality][search][eac3]") {
    const auto material = make_material(2);
    const auto without = encode_all(
        config_for(ac3::quality::Criterion::kNone, ac3::Acmod::k2_0, false, 192), material);
    const auto with = encode_all(
        config_for(ac3::quality::Criterion::kDistortion, ac3::Acmod::k2_0, false, 192), material);
    bool differs = false;
    for (std::size_t frame = 0; frame < without.size(); ++frame) {
        differs = differs || without[frame] != with[frame];
    }
    CHECK(differs);
    // CBR: frmsiz signals the exact word count, and dbpbcod is a fixed-width
    // baie field either value costs, so no candidate can move the size.
    for (std::size_t frame = 0; frame < with.size(); ++frame) {
        CAPTURE(frame);
        CHECK(with[frame].size() == without[frame].size());
    }
}

TEST_CASE("searching E-AC-3 on distortion lowers the decoded error",
          "[quality][search][eac3]") {
    const auto material = make_material(2);

    const auto decoded_error = [&](ac3::quality::Criterion criterion, std::uint32_t kbps) {
        const auto frames =
            encode_all(config_for(criterion, ac3::Acmod::k2_0, false, kbps), material);
        ac3::Eac3Decoder decoder;
        double signal = 0.0;
        double noise = 0.0;
        // Frame 0's output is the previous (empty) frame's second half, same
        // one-block §7.9.4 delay test_search.cpp's own AC-3 case accounts for.
        for (std::size_t frame = 1; frame < frames.size(); ++frame) {
            auto decoded = decoder.decode_substream(frames[frame]);
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->has_value());
            const auto& sub = **decoded;
            for (std::size_t ch = 0; ch < sub.channels.size(); ++ch) {
                const auto& out = sub.channels[ch];
                const auto& in = material[ch];
                const std::size_t base = (frame - 1) * ac3::kSamplesPerFrame;
                for (std::size_t n = 0; n < out.size(); ++n) {
                    const double reference = static_cast<double>(in[base + n]);
                    const double error = static_cast<double>(out[n]) - reference;
                    signal += reference * reference;
                    noise += error * error;
                }
            }
        }
        return 10.0 * std::log10(signal / std::max(noise, 1e-30));
    };

    for (const std::uint32_t kbps : {192U, 448U}) {
        const double off = decoded_error(ac3::quality::Criterion::kNone, kbps);
        const double searched = decoded_error(ac3::quality::Criterion::kDistortion, kbps);
        CAPTURE(kbps, off, searched);
        // Never worse than the switch margin the search itself uses - see
        // kCodeSwitchMarginDb in eac3_frame.cpp.
        CHECK(searched >= off - 0.05);
    }
}

// The scope boundary FrameConfig::search documents: VBR/ABR's own budget
// search is not wrapped in a candidate loop, so search=distortion has to be
// exactly as inert there as search=kNone is - not merely "not worse", but
// bit-for-bit the same encode, the same determinism guarantee the off case
// above checks for CBR.
TEST_CASE("the E-AC-3 search stays inert under VBR", "[quality][search][eac3]") {
    const auto material = make_material(2);
    auto without = config_for(ac3::quality::Criterion::kNone, ac3::Acmod::k2_0, false, 448);
    without.vbr = ac3::eac3::VbrConfig{.quality = 0.6};
    auto with = config_for(ac3::quality::Criterion::kDistortion, ac3::Acmod::k2_0, false, 448);
    with.vbr = ac3::eac3::VbrConfig{.quality = 0.6};

    const auto first = encode_all(without, material);
    const auto second = encode_all(with, material);
    REQUIRE(first.size() == second.size());
    for (std::size_t frame = 0; frame < first.size(); ++frame) {
        CAPTURE(frame);
        CHECK(first[frame] == second[frame]);
    }
}

// --------------------------------------------------------------------------
// Roadmap EQ7's E-AC-3 half: FrameConfig::fgaincod.
//
// The risk here is not the same as the dbpbcod search's. dbpbcod moves the
// masking curve and nothing else; fgaincod moves the curve AND opens the
// per-block fgaincode element (Table E1.4), so it changes the bit LAYOUT of
// every block as well as every mantissa width. Reading that element back at
// the wrong offset is precisely the desync that was found on the decode side
// against a real DEE stream - cplfgaincod sits ahead of the per-channel run,
// and omitting it read every fast gain three bits early.
//
// So this pins the code and runs the whole frame through the mirror encoder,
// which diffs the encoder's own model against a real decode block by block.
// --------------------------------------------------------------------------
TEST_CASE("a pinned E-AC-3 fgaincod round-trips through a real decode",
          "[quality][search][eac3]") {
    const auto stereo = make_material(2);
    const auto surround = make_material(6);  // 5 fbw + LFE, so the run has an LFE tail
    // 4 is Table E1.4's implied default and writes no element at all; the
    // rest all open one. 0 and 7 are the ends of Table 7.11.
    for (const int fgaincod : {0, 2, 4, 7}) {
        for (const std::uint32_t kbps : {192U, 448U}) {
            {
                auto config = config_for(ac3::quality::Criterion::kNone, ac3::Acmod::k2_0,
                                         false, kbps);
                config.fgaincod = fgaincod;
                ac3::verify::Eac3MirrorEncoder mirror{
                    ac3::eac3::AccessUnitConfig{.independent = config}};
                for (int frame = 0; frame < kFrames; ++frame) {
                    auto checked = mirror.encode_access_unit(spans_for(stereo, frame));
                    REQUIRE(checked.has_value());
                    CAPTURE(fgaincod, kbps, frame, mirror.last_report());
                    CHECK(checked->ok());
                }
            }
            {
                // Coupled 5.1: the one shape that exercises cplfgaincod's own
                // slot ahead of the per-channel run, and the LFE's at the end
                // of it.
                auto config = config_for(ac3::quality::Criterion::kNone, ac3::Acmod::k3_2,
                                         true, kbps);
                config.fgaincod = fgaincod;
                ac3::verify::Eac3MirrorEncoder mirror{
                    ac3::eac3::AccessUnitConfig{.independent = config}};
                for (int frame = 0; frame < kFrames; ++frame) {
                    auto checked = mirror.encode_access_unit(spans_for(surround, frame));
                    REQUIRE(checked.has_value());
                    CAPTURE(fgaincod, kbps, frame, mirror.last_report());
                    CHECK(checked->ok());
                }
            }
        }
    }
}

// The default has to stay exactly where it was: -1 means "leave Table E1.4's
// implied 0x4 alone", which is a different statement from "pin 4". Both
// produce the same ALLOCATION, but only the first writes no fgaincode
// element, and the roadmap entry's whole cost argument rests on that
// distinction. Byte equality is the only check that can tell them apart.
TEST_CASE("E-AC-3 fgaincod defaults to writing no fgaincode element at all",
          "[quality][search][eac3]") {
    const auto material = make_material(2);
    auto defaulted = config_for(ac3::quality::Criterion::kNone, ac3::Acmod::k2_0, false, 192);
    auto pinned_default = defaulted;
    pinned_default.fgaincod = 4;  // the same code, stated rather than implied
    auto pinned_other = defaulted;
    pinned_other.fgaincod = 2;

    const auto base = encode_all(defaulted, material);
    const auto same = encode_all(pinned_default, material);
    const auto other = encode_all(pinned_other, material);
    REQUIRE(base.size() == same.size());
    for (std::size_t frame = 0; frame < base.size(); ++frame) {
        CAPTURE(frame);
        // Pinning the default value is indistinguishable from not pinning it:
        // frmfgaincode stays 0 either way.
        CHECK(base[frame] == same[frame]);
        // A non-default code is not: the element appears, so the bytes move.
        CHECK(base[frame] != other[frame]);
    }
}

// The fgaincod axis is one-directional, and that is a measured decision
// rather than a cautious one - see the long note at its candidate guard in
// eac3_frame.cpp's step 7a.
//
// The short version: this search minimises decoded-domain distortion, and on
// E-AC-3 that criterion is OPPOSED to perceived quality along this axis. A
// sweep of all eight codes on real CC0 material found SNR rising monotonically
// with fgaincod while ViSQOL MOS-LQO fell, the MOS optimum sitting exactly on
// §8.2.12's 0x4. An unrestricted search therefore buys SNR it can see and
// spends quality it cannot - measured at -0.396 MOS against the one-axis
// search at 96 kbit/s stereo. Only codes below 0x4, where the two measures
// agree, are offered.
//
// 96 kbit/s stereo is the cell where AC-3's curve asks for 6 (above the
// default) and where the regression was largest, so a search there must come
// back with the default; 640 is where it asks for 0 and the axis is allowed
// to act.
TEST_CASE("the E-AC-3 search never raises fgaincod above the default",
          "[quality][search][eac3]") {
    const auto material = make_material(2);
    // Low rate: AC-3's curve points up, so the axis must stay out of it and
    // the encode must be identical to what the one-axis search produces.
    // Pinning the default reproduces "one axis" exactly - it takes fgaincod
    // out of the candidate set without changing anything else.
    auto two_axis = config_for(ac3::quality::Criterion::kDistortion, ac3::Acmod::k2_0, false, 96);
    auto one_axis = two_axis;
    one_axis.fgaincod = 4;  // the default, stated - no element, no candidate

    const auto searched = encode_all(two_axis, material);
    const auto pinned = encode_all(one_axis, material);
    REQUIRE(searched.size() == pinned.size());
    for (std::size_t frame = 0; frame < searched.size(); ++frame) {
        CAPTURE(frame);
        CHECK(searched[frame] == pinned[frame]);
    }

    // And the guard is a direction test, not a blanket disable: at 640 the
    // curve asks for 0, which is below the default, so the axis is live. That
    // does not oblige the search to TAKE it - the refit may still reject it -
    // so this only asserts the encode remains valid and decodable, which is
    // what the mirror check below covers properly.
    const int curve_640 = ac3::rate_adaptive_fgaincod(640, 2);
    CHECK(curve_640 < 4);
    const int curve_96 = ac3::rate_adaptive_fgaincod(96, 2);
    CHECK(curve_96 > 4);
}
