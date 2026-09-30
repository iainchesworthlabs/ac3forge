#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"

// Reconstruction-level coverage for the three 7-channel JOC downmix
// configurations (Table 47 idx 1/2/4), which decode_substream_core alone
// cannot reconstruct - their extra channel pair lives in an Annex E
// dependent substream, resolved by decode_access_unit_core once the
// programme's channels are unioned (see DecodedSubstream::joc_pending_bytes,
// decoder.hpp).
//
// No real third-party stream exercises these configs: this project's own
// AtmosEncoder only ever writes kDmxConfig5X (joc.hpp's own comment), and
// the one real Dolby-Encoding-Engine fixture in this repo
// (tests/oba/test_dee_joc_fixture.cpp) uses kDmxConfig5XPhaseShift even from
// a full 7.1.4 source. So this hand-builds a synthetic stream, the same
// methodology tests/decoder/test_eac3_decoder.cpp's "an AC-3 core plus an
// E-AC-3 dependent decodes to 7.1" and test_dee_joc_fixture.cpp each already
// use for one piece of this: a distinguishable tone per physical channel,
// and per-object dominant-tone identification after reconstruction.
//
// Each test's JOC matrix is a straight per-channel routing - object i reads
// entirely from downmix channel i - so a correctly reconstructed object is
// dominated by whichever physical channel eac3_decoder.cpp's location
// lookup placed at that position. This is necessarily a self-consistency
// check (this file is both the encoder and the only exerciser of the
// decoder side here, since no independent third-party bitstream exists for
// these configs) - see joc.hpp's own comment on the residual, spec-textual-
// but-not-independently-confirmed uncertainty this shares: which physical
// channel occupies downmix position 5/6 for a config this project has never
// received a real bitstream for.

namespace {

using Location = ac3::eac3::chanmap::Location;

struct PositionTone {
    Location location;
    double frequency;
};

// Goertzel-style projection onto one frequency: the magnitude of the
// signal's correlation with a complex exponential, normalised by length.
// Same technique test_dee_joc_fixture.cpp uses for the same reason - it
// says which of a handful of well-separated tones dominates without a full
// spectrum.
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

// A whole-matrix (non-sparse) JOC payload where object i reads entirely
// from downmix channel i, every parameter band. Reuses joc::build_payload
// directly (recently widened to accept any Table 47 configuration rather
// than only kDmxConfig5X - see its own comment) rather than hand-rolling
// the Huffman-coded bitstream by hand: the payload this test decodes is
// exactly what the codec's own encode/decode symmetry already trusts
// elsewhere, not a second, independently-fallible implementation of it.
std::vector<std::byte> identity_routing_joc(int dmx_config_idx, int channels) {
    ac3::oba::joc::FrameParameters params;
    params.objects = channels;
    params.channels = channels;
    params.dmx_config_idx = dmx_config_idx;
    params.matrix.assign(params.coefficient_count(), 0.0);
    for (int object = 0; object < channels; ++object) {
        for (int band = 0; band < params.bands(); ++band) {
            params.at(object, object, band) = 1.0;
        }
    }
    return ac3::oba::joc::build_payload(params);
}

std::vector<std::byte> emdf_container(int dmx_config_idx, int channels) {
    // A dynamic-object-only program rather than a bed: the only thing this
    // needs from OAMD is oba::joc_object_indices() returning `channels`
    // entries so decode_access_unit_core's own count check passes - what
    // the objects' own positions say is irrelevant to reconstructing their
    // audio.
    const ac3::oba::Program program{.dynamic_only = true, .dynamic_objects = channels};
    const std::vector<ac3::oba::DynamicObject> objects(static_cast<std::size_t>(channels));
    const auto oamd_payload = ac3::oba::build_payload(program, objects);
    const auto joc_payload = identity_routing_joc(dmx_config_idx, channels);
    const std::vector<ac3::emdf::Payload> payloads = {
        {.id = ac3::emdf::kPayloadIdOamd, .bytes = oamd_payload},
        {.id = ac3::emdf::kPayloadIdJoc, .bytes = joc_payload}};
    return ac3::emdf::build_container(payloads);
}

// Builds a stream (E-AC-3 independent 5.1 carrying the EMDF container, plus
// one dependent widening it to `channels`), decodes it, and returns each
// JOC object's accumulated audio - skipping the first two access units'
// worth so the kMdctBand transform's own history has filled from real
// content rather than the zero-padding every stream's first block starts
// from (joc.cpp's own gather_and_window comment).
std::vector<std::vector<float>> reconstruct_wide_bed(const std::array<PositionTone, 6>& bed_tones,
                                                     std::uint16_t dependent_chanmap,
                                                     std::span<const PositionTone> dependent_tones,
                                                     int dmx_config_idx, int channels) {
    const auto container = emdf_container(dmx_config_idx, channels);

    ac3::eac3::FrameEncoder bed{{.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    // The dependent's own acmod codes exactly channel_count(dependent_chanmap)
    // channels (k71Rear=4, Ls/Rs/Lrs/Rrs; k512Height=2, Vhl/Vhr only) - the
    // chanmap says which locations those coded channels occupy, not how many
    // of them there are.
    ac3::eac3::FrameEncoder dependent{
        {.bitrate_kbps = 192,
         .acmod = dependent_tones.size() == 4 ? ac3::Acmod::k2_2 : ac3::Acmod::k2_0,
         .strmtyp = ac3::eac3::StreamType::kDependent,
         .substreamid = 0,
         .chanmap = dependent_chanmap,
         .last_dependent = true}};

    constexpr int kFrames = 8;
    constexpr double kAmplitude = 0.5;
    std::vector<std::byte> stream;
    std::uint64_t n0 = 0;
    for (int f = 0; f < kFrames; ++f) {
        std::vector<std::vector<float>> bed_block(6, std::vector<float>(ac3::kSamplesPerFrame));
        std::vector<std::vector<float>> dep_block(dependent_tones.size(),
                                                   std::vector<float>(ac3::kSamplesPerFrame));
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const double t = static_cast<double>(n0 + static_cast<std::uint64_t>(i)) / 48000.0;
            for (std::size_t ch = 0; ch < 6; ++ch) {
                bed_block[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                    kAmplitude * std::sin(2.0 * std::numbers::pi * bed_tones[ch].frequency * t));
            }
            for (std::size_t ch = 0; ch < dependent_tones.size(); ++ch) {
                dep_block[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                    kAmplitude *
                    std::sin(2.0 * std::numbers::pi * dependent_tones[ch].frequency * t));
            }
        }
        n0 += static_cast<std::uint64_t>(ac3::kSamplesPerFrame);

        const std::vector<std::span<const float>> bed_views(bed_block.begin(), bed_block.end());
        const auto bed_frame = bed.encode_frame(bed_views, container);
        REQUIRE(bed_frame.has_value());
        stream.insert(stream.end(), bed_frame->begin(), bed_frame->end());

        const std::vector<std::span<const float>> dep_views(dep_block.begin(), dep_block.end());
        const auto dep_frame = dependent.encode_frame(dep_views);
        REQUIRE(dep_frame.has_value());
        stream.insert(stream.end(), dep_frame->begin(), dep_frame->end());
    }

    const auto units = ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == static_cast<std::size_t>(kFrames));

    ac3::Eac3Decoder decoder;
    std::vector<std::vector<float>> accumulated(static_cast<std::size_t>(channels));
    int frame_index = 0;
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        ++frame_index;
        if (frame_index <= 2) {
            continue;
        }
        const auto& access_unit = **decoded;
        REQUIRE(access_unit.object_audio.size() == static_cast<std::size_t>(channels));
        for (int obj = 0; obj < channels; ++obj) {
            const auto& audio = access_unit.object_audio[static_cast<std::size_t>(obj)];
            accumulated[static_cast<std::size_t>(obj)].insert(
                accumulated[static_cast<std::size_t>(obj)].end(), audio.begin(), audio.end());
        }
    }
    return accumulated;
}

// A spectral, delay-insensitive stand-in for per-object SNR: the target
// tone's own magnitude against the strongest OTHER candidate tone found in
// the same object, in dB. Deliberately not a sample-domain SNR against a
// synthesised reference - that needs the reconstruction's exact algorithmic
// delay (reconstruction_delay(Domain::kMdctBand), joc.hpp) folded into a
// hand-built reference signal, which a bookkeeping mistake in this test
// could get wrong in either direction; a magnitude ratio measures the same
// "did the right content end up in the right object" question without that
// fragility, at the cost of not being sample-accurate.
void check_object_dominates(const std::vector<std::vector<float>>& accumulated,
                            std::span<const double> expected_hz, std::size_t object) {
    REQUIRE(accumulated[object].size() >= 8000);
    const double own = tone_magnitude(accumulated[object], expected_hz[object], 48000.0);
    double best_other = 0.0;
    for (std::size_t other = 0; other < expected_hz.size(); ++other) {
        if (other == object) {
            continue;
        }
        best_other =
            std::max(best_other, tone_magnitude(accumulated[object], expected_hz[other], 48000.0));
    }
    const double snr_db = best_other > 0.0 ? 20.0 * std::log10(own / best_other) : 200.0;
    INFO("object " << object << " own=" << own << " best_other=" << best_other
                   << " snr_db=" << snr_db);
    CHECK(snr_db > 20.0);
}

}  // namespace

TEST_CASE("JOC reconstructs a kDmxConfig7X (Lb/Rb) downmix from the Annex E dependent",
          "[oba][joc]") {
    namespace cm = ac3::eac3::chanmap;
    // AC-3 Table 5.8 coded order for acmod k3_2 + lfe: L, C, R, Ls, Rs, LFE.
    // Ls/Rs are deliberately given tones the dependent below will REPLACE
    // (not add to) - proving the override happens, not just that a channel
    // this test never touches happens to read right.
    const std::array<PositionTone, 6> bed_tones = {{{Location::kLeft, 919.0},
                                                     {Location::kCentre, 647.0},
                                                     {Location::kRight, 1181.0},
                                                     {Location::kLeftSurround, 353.0},
                                                     {Location::kRightSurround, 2311.0},
                                                     {Location::kLfe, 55.0}}};
    // k71Rear's own coded order (eac3_tables.hpp's static_asserts): Ls, Rs,
    // Lrs, Rrs - the dependent's Ls/Rs REPLACE the bed's own, matching real
    // §E3.8.2 semantics and test_eac3_decoder.cpp's own "AC-3 core plus
    // E-AC-3 dependent decodes to 7.1" test.
    const std::array<PositionTone, 4> dependent_tones = {{{Location::kLeftSurround, 1523.0},
                                                           {Location::kRightSurround, 1847.0},
                                                           {Location::kLrs, 463.0},
                                                           {Location::kRrs, 2089.0}}};
    const auto accumulated =
        reconstruct_wide_bed(bed_tones, cm::k71Rear, dependent_tones,
                             ac3::oba::joc::kDmxConfig7X, ac3::oba::joc::kNumChannels5X + 2);

    // JOC channel order (Table 53), not AC-3's coded order: L, R, C, Ls, Rs,
    // Lb(=Lrs), Rb(=Rrs) - position i is object i's own "home" channel, per
    // identity_routing_joc().
    const std::array<double, 7> expected = {919.0, 1181.0, 647.0, 1523.0, 1847.0, 463.0, 2089.0};
    REQUIRE(accumulated.size() == 7);
    for (std::size_t object = 0; object < 7; ++object) {
        check_object_dominates(accumulated, expected, object);
    }
}

TEST_CASE(
    "JOC reconstructs a kDmxConfig5XPlus2PhaseShift (Tfl/Tfr) downmix from the Annex E dependent",
    "[oba][joc]") {
    namespace cm = ac3::eac3::chanmap;
    // k512Height is purely additive (kVhlVhrBit alone) - it does not touch
    // the bed's own Ls/Rs, unlike k71Rear above, so every bed tone here
    // survives the union unmodified.
    const std::array<PositionTone, 6> bed_tones = {{{Location::kLeft, 919.0},
                                                     {Location::kCentre, 647.0},
                                                     {Location::kRight, 1181.0},
                                                     {Location::kLeftSurround, 1523.0},
                                                     {Location::kRightSurround, 1847.0},
                                                     {Location::kLfe, 55.0}}};
    const std::array<PositionTone, 2> dependent_tones = {
        {{Location::kVhl, 463.0}, {Location::kVhr, 2089.0}}};
    const auto accumulated = reconstruct_wide_bed(bed_tones, cm::k512Height, dependent_tones,
                                                  ac3::oba::joc::kDmxConfig5XPlus2PhaseShift,
                                                  ac3::oba::joc::kNumChannels5X + 2);

    const std::array<double, 7> expected = {919.0, 1181.0, 647.0, 1523.0, 1847.0, 463.0, 2089.0};
    REQUIRE(accumulated.size() == 7);
    for (std::size_t object = 0; object < 7; ++object) {
        check_object_dominates(accumulated, expected, object);
    }
}
