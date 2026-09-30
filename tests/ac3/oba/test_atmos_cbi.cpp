#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/oba/atmos.hpp"
#include "iclforge/objects/oamd.hpp"

// AtmosEncoder's channel-based-immersive (CBI) bed path: program.bed != 0,
// dynamic_objects == 0, coded through OAMD+JOC exactly like a dynamic-object
// programme rather than a completely separate feature - see
// docs/concepts/atmos-joc.md's "A programme need not be objects" note, and
// bed51 (AtmosConfig::emit_object_metadata off), which is the OTHER thing
// named "bed" in this codebase and means something unrelated: no object
// container at all.
//
// Builds a synthetic 5.1.4 bed with a distinct tone per channel - the same
// fixture-construction idea test_dee_joc_fixture.cpp uses for a real DEE
// stream, and the same channel order (confirmed against a real DEE-produced
// 5.1.4 stream - see tools/generators/gen_object_fixture.py) - then
// round-trips it through Eac3Decoder and identifies each reconstructed
// channel by its own tone, independent of anything build_payload/
// joc::build_payload assert internally about what they just wrote.

namespace {

constexpr int kFrame = iclforge::kSamplesPerFrame;

std::vector<float> tone(double hz, double amplitude, std::uint64_t start) {
    std::vector<float> out(kFrame);
    for (int n = 0; n < kFrame; ++n) {
        const double t = static_cast<double>(start + static_cast<std::uint64_t>(n)) / 48000.0;
        out[static_cast<std::size_t>(n)] =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * t));
    }
    return out;
}

// Goertzel-style projection onto one frequency, the same technique
// test_dee_joc_fixture.cpp uses: enough to say which of a handful of
// well-separated tones dominates a signal.
double tone_magnitude(std::span<const float> signal, double frequency, double sample_rate) {
    double real = 0.0;
    double imag = 0.0;
    for (std::size_t n = 0; n < signal.size(); ++n) {
        const double phase =
            2.0 * std::numbers::pi * frequency * static_cast<double>(n) / sample_rate;
        real += static_cast<double>(signal[n]) * std::cos(phase);
        imag += static_cast<double>(signal[n]) * std::sin(phase);
    }
    return std::hypot(real, imag) / static_cast<double>(signal.size());
}

// DEE's own cbi_wav channel order for 5.1.4 (tools/generators/gen_object_fixture.py,
// measured against a real Dolby Encoding Engine stream) - and exactly
// iclforge::oba::bed_labels()'s own order for this bed, which is what
// AtmosEncoder::encode_bed_frame documents its `channels` argument to expect.
struct BedChannel {
    const char* label;
    double frequency;
};
constexpr std::array<BedChannel, 10> kInput = {{
    {"L", 220.0}, {"R", 277.2}, {"C", 330.0}, {"LFE", 55.0}, {"Ls", 554.4},
    {"Rs", 660.0}, {"Tfl", 740.0}, {"Tfr", 831.6}, {"Tbl", 880.0}, {"Tbr", 1108.8},
}};

constexpr std::uint16_t kBed514 = iclforge::oba::bed::kLR | iclforge::oba::bed::kC |
                                  iclforge::oba::bed::kLfe | iclforge::oba::bed::kLsRs |
                                  iclforge::oba::bed::kTflTfr | iclforge::oba::bed::kTblTbr;

}  // namespace

TEST_CASE("AtmosEncoder's BedProgram constructor writes a real bed programme, not objects",
          "[atmos][cbi]") {
    iclforge::oba::AtmosEncoder encoder{{.bitrate_kbps = 448},
                                        iclforge::oba::BedProgram{.bed = kBed514}};
    CHECK_FALSE(encoder.program().dynamic_only);
    CHECK(encoder.program().bed == kBed514);
    CHECK(encoder.program().dynamic_objects == 0);
    CHECK(encoder.dynamic_object_count() == 0);
    CHECK(iclforge::oba::object_count(encoder.program()) == 10);
    CHECK(iclforge::oba::joc_object_count(encoder.program()) == 9);  // 10 bed channels less the LFE
}

TEST_CASE("Eac3Decoder recovers program.bed != 0 with 0 dynamic objects from a CBI encode",
          "[atmos][cbi][decoder]") {
    iclforge::oba::AtmosEncoder encoder{{.bitrate_kbps = 448},
                                        iclforge::oba::BedProgram{.bed = kBed514}};

    iclforge::eac3::AccessUnit unit;
    std::vector<std::vector<float>> essences;
    std::vector<std::span<const float>> views(kInput.size());
    for (int frame = 0; frame < 3; ++frame) {
        const auto start = static_cast<std::uint64_t>(frame) * kFrame;
        essences.clear();
        for (const auto& channel : kInput) {
            essences.push_back(tone(channel.frequency, 0.3, start));
        }
        for (std::size_t i = 0; i < views.size(); ++i) {
            views[i] = essences[i];
        }
        auto encoded = encoder.encode_bed_frame(views);
        REQUIRE(encoded.has_value());
        unit = *encoded;
    }
    REQUIRE(unit.substream_count() == 1);

    iclforge::Eac3Decoder decoder;
    const auto decoded = decoder.decode_substream(unit.substream(0));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());
    REQUIRE((*decoded)->object_metadata.has_value());

    const auto& metadata = *(*decoded)->object_metadata;
    CHECK_FALSE(metadata.program.dynamic_only);
    CHECK(metadata.program.bed == kBed514);
    CHECK(metadata.program.dynamic_objects == 0);
    CHECK(iclforge::oba::object_count(metadata.program) == 10);
    CHECK(metadata.objects.empty());  // no dynamic objects to describe

    // The LFE feeds the bed's own LFE channel directly (see
    // AtmosEncoder::encode_bed_frame's own comment) rather than through JOC -
    // AC-3 coded order is L, C, R, Ls, Rs, LFE (AtmosEncoder::bed()'s own
    // comment), so index 5 is where its 55 Hz tone should dominate.
    REQUIRE((*decoded)->channels.size() == 6);
    double best_magnitude = -1.0;
    std::size_t best_channel = 0;
    for (std::size_t channel = 0; channel < 6; ++channel) {
        const double magnitude = tone_magnitude((*decoded)->channels[channel], 55.0, 48000.0);
        if (magnitude > best_magnitude) {
            best_magnitude = magnitude;
            best_channel = channel;
        }
    }
    CHECK(best_channel == 5);
}

TEST_CASE("every JOC object a CBI encode reconstructs carries its own bed channel's tone",
          "[atmos][cbi][decoder][joc]") {
    iclforge::oba::AtmosEncoder encoder{{.bitrate_kbps = 448},
                                        iclforge::oba::BedProgram{.bed = kBed514}};

    iclforge::Eac3Decoder decoder;
    std::vector<std::vector<float>> accumulated;
    std::vector<std::vector<float>> essences;
    std::vector<std::span<const float>> views(kInput.size());
    // Several frames: the first has no previous matrix to interpolate from
    // (§6.6.5), so its own reconstruction is skipped rather than accumulated.
    constexpr int kFrames = 8;
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto start = static_cast<std::uint64_t>(frame) * kFrame;
        essences.clear();
        for (const auto& channel : kInput) {
            essences.push_back(tone(channel.frequency, 0.3, start));
        }
        for (std::size_t i = 0; i < views.size(); ++i) {
            views[i] = essences[i];
        }
        auto encoded = encoder.encode_bed_frame(views);
        REQUIRE(encoded.has_value());
        REQUIRE(encoded->substream_count() == 1);

        const auto decoded = decoder.decode_substream(encoded->substream(0));
        REQUIRE(decoded.has_value());
        if (!decoded->has_value() || (*decoded)->object_audio.empty()) {
            continue;
        }
        if (frame < 3) {
            continue;  // skip the ramp-in
        }
        const auto& audio = (*decoded)->object_audio;
        if (accumulated.empty()) {
            accumulated.assign(audio.size(), {});
        }
        for (std::size_t i = 0; i < audio.size(); ++i) {
            accumulated[i].insert(accumulated[i].end(), audio[i].begin(), audio[i].end());
        }
    }
    // 9 bed channels less the LFE §6.3.2.2 bypasses.
    REQUIRE(accumulated.size() == 9);

    // kInput minus the LFE (index 3), in joc_object_indices() order - the
    // bed's own labels, front to back, LFE dropped.
    constexpr std::array<const char*, 9> kExpected = {"L",  "R",   "C",   "Ls",  "Rs",
                                                       "Tfl", "Tfr", "Tbl", "Tbr"};
    for (std::size_t object = 0; object < accumulated.size(); ++object) {
        REQUIRE_FALSE(accumulated[object].empty());
        std::size_t best = 0;
        double best_magnitude = -1.0;
        for (std::size_t channel = 0; channel < kInput.size(); ++channel) {
            const double magnitude =
                tone_magnitude(accumulated[object], kInput[channel].frequency, 48000.0);
            if (magnitude > best_magnitude) {
                best_magnitude = magnitude;
                best = channel;
            }
        }
        INFO("object " << object << " strongest tone is " << kInput[best].label << ", expected "
                       << kExpected[object]);
        CHECK(std::string{kInput[best].label} == std::string{kExpected[object]});
    }
}

TEST_CASE("a CBI encode with a 9.1.6 layout writes the wider bed and no dynamic objects",
          "[atmos][cbi]") {
    // Coverage for a layout beyond the DEE-verified 5.1.4 one - see
    // docs/concepts/atmos-joc.md for which layouts are checked against a real
    // DEE stream and which are extended from Table 12's own channel order.
    constexpr std::uint16_t kBed916 =
        iclforge::oba::bed::kLR | iclforge::oba::bed::kC | iclforge::oba::bed::kLfe |
        iclforge::oba::bed::kLsRs | iclforge::oba::bed::kLbRb | iclforge::oba::bed::kLwRw |
        iclforge::oba::bed::kTflTfr | iclforge::oba::bed::kTslTsr | iclforge::oba::bed::kTblTbr;
    REQUIRE(iclforge::oba::bed::channel_count(kBed916) == 16);

    iclforge::oba::AtmosEncoder encoder{{.bitrate_kbps = 768},
                                        iclforge::oba::BedProgram{.bed = kBed916}};
    CHECK(iclforge::oba::object_count(encoder.program()) == 16);
    CHECK(iclforge::oba::joc_object_count(encoder.program()) == 15);

    std::vector<std::vector<float>> essences(16);
    std::vector<std::span<const float>> views(16);
    iclforge::eac3::AccessUnit unit;
    for (int frame = 0; frame < 2; ++frame) {
        const auto start = static_cast<std::uint64_t>(frame) * kFrame;
        for (std::size_t i = 0; i < 16; ++i) {
            essences[i] = tone(200.0 + static_cast<double>(i) * 61.0, 0.2, start);
            views[i] = essences[i];
        }
        auto encoded = encoder.encode_bed_frame(views);
        REQUIRE(encoded.has_value());
        unit = *encoded;
    }

    iclforge::Eac3Decoder decoder;
    const auto decoded = decoder.decode_substream(unit.substream(0));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());
    REQUIRE((*decoded)->object_metadata.has_value());
    const auto& metadata = *(*decoded)->object_metadata;
    CHECK_FALSE(metadata.program.dynamic_only);
    CHECK(metadata.program.bed == kBed916);
    CHECK(metadata.program.dynamic_objects == 0);
    CHECK((*decoded)->object_audio.size() == 15);
}
