#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/core/bitreader.hpp"
#include "ac3/core/bitwriter.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/oba/joc.hpp"
#include "ac3/oba/oamd.hpp"

namespace {

// The decoder side of §6.6.3 Pseudocode 4, walking the normative trees. The
// encoder was generated from those same trees, so this is not a round trip
// against itself: the generator inverted them and this walks them forwards. A
// disagreement means the inversion is wrong.
struct HuffTree {
    std::span<const std::array<int, 2>> nodes;
};

// Rebuilt from the encode tables rather than duplicating the trees: a prefix
// code is uniquely determined by its (code, length) pairs, so decoding by
// longest-match over them is equivalent to walking the tree.
// §5.5.1 variable_bits_max(n, max_num_groups), as the decoder. Same shape as
// EMDF's variable_bits with a ceiling on the group count.
std::uint32_t read_variable_bits_max(ac3::BitReader& r, int group_bits, int max_groups) {
    std::uint32_t value = 0;
    for (int group = 1;; ++group) {
        value += r.read(group_bits);
        const bool read_more = r.read_bit() != 0;
        if (!read_more || group >= max_groups) {
            return value;
        }
        value <<= group_bits;
        value += 1u << group_bits;
    }
}

int huff_decode(std::span<const ac3::oba::joc::HuffCode> table, ac3::BitReader& r) {
    std::uint32_t accumulated = 0;
    for (int bits = 1; bits <= 32; ++bits) {
        accumulated = (accumulated << 1) | r.read_bit();
        for (std::size_t value = 0; value < table.size(); ++value) {
            if (table[value].bits == bits && table[value].code == accumulated) {
                return static_cast<int>(value);
            }
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("JOC quantization round-trips through the spec's own scale", "[oba][joc]") {
    // §6.6.4's note pins the reachable range exactly, which is the cheapest
    // check that the 820/4096 scale and the nquant/2 origin are both right.
    CHECK_THAT(ac3::oba::joc::dequantize(0, false),
               Catch::Matchers::WithinAbs(-9.609, 0.001));
    CHECK_THAT(ac3::oba::joc::dequantize(95, false),
               Catch::Matchers::WithinAbs(9.410, 0.001));
    CHECK_THAT(ac3::oba::joc::dequantize(0, true),
               Catch::Matchers::WithinAbs(-9.609, 0.001));
    CHECK_THAT(ac3::oba::joc::dequantize(191, true),
               Catch::Matchers::WithinAbs(9.509, 0.001));

    // Zero gain is a code, not an approximation - it is the origin.
    CHECK(ac3::oba::joc::quantize(0.0, false) == 48);
    CHECK(ac3::oba::joc::quantize(0.0, true) == 96);
    CHECK(ac3::oba::joc::dequantize(48, false) == 0.0);

    // Fine quantization must actually halve the step.
    const double coarse_step = ac3::oba::joc::dequantize(49, false);
    const double fine_step = ac3::oba::joc::dequantize(97, true);
    CHECK_THAT(coarse_step, Catch::Matchers::WithinAbs(2.0 * fine_step, 1e-12));

    for (const bool fine : {false, true}) {
        for (const double value : {-9.0, -1.0, -0.2, 0.0, 0.5, 1.0, 3.3, 9.0}) {
            const double back = ac3::oba::joc::dequantize(ac3::oba::joc::quantize(value, fine), fine);
            CHECK_THAT(back, Catch::Matchers::WithinAbs(value, fine ? 0.051 : 0.101));
        }
    }
}

TEST_CASE("Table 54 matches the standard's worked example", "[oba][joc]") {
    // §6.6.5: "If joc_num_bands = 15 and the input to sb_to_pb(subband) is the
    // subband value 24, sb_to_pb(24) returns the value 13."
    STATIC_CHECK(ac3::oba::joc::kNumBands[6] == 15);
    STATIC_CHECK(ac3::oba::joc::kSubbandToBand[6][24] == 13);
    // Every mapping has to reach its last band, or the top of the spectrum
    // would be coded with parameters nothing ever reads.
    for (std::size_t idx = 0; idx < ac3::oba::joc::kNumBands.size(); ++idx) {
        CHECK(ac3::oba::joc::kSubbandToBand[idx][0] == 0);
        CHECK(ac3::oba::joc::kSubbandToBand[idx][63] == ac3::oba::joc::kNumBands[idx] - 1);
    }
}

TEST_CASE("JOC payload decodes back to the matrix it was given", "[oba][joc]") {
    ac3::oba::joc::FrameParameters params{.objects = 4, .num_bands_idx = 4, .seq_count = 7};
    params.matrix.resize(params.coefficient_count());
    // A matrix with structure rather than noise: each object leans on a
    // different channel, and the lean varies across bands. Constant values
    // would make every differential zero and hide a broken predictor.
    for (int object = 0; object < params.objects; ++object) {
        for (int channel = 0; channel < params.channels; ++channel) {
            for (int band = 0; band < params.bands(); ++band) {
                params.at(object, channel, band) =
                    (object == channel ? 1.0 : -0.3) + 0.1 * band - 0.05 * channel;
            }
        }
    }

    const auto payload = ac3::oba::joc::build_payload(params);
    ac3::BitReader r{payload};

    // --- joc_header ---
    CHECK(r.read(3) == 0);  // joc_dmx_config_idx: 5.X
    CHECK(r.read(6) == 3);  // joc_num_objects_bits = objects - 1
    CHECK(r.read(3) == 0);  // joc_ext_config_idx

    // --- joc_info ---
    CHECK(r.read(3) == 0);   // joc_clipgain_x_bits
    CHECK(r.read(5) == 0);   // joc_clipgain_y_bits
    CHECK(r.read(10) == 7);  // joc_seq_count_bits
    for (int object = 0; object < params.objects; ++object) {
        CHECK(r.read(1) == 1);  // b_joc_obj_present
        CHECK(r.read(3) == 4);  // joc_num_bands_idx
        CHECK(r.read(1) == 0);  // b_joc_sparse
        CHECK(r.read(1) == 0);  // joc_num_quant_idx
        CHECK(r.read(1) == 0);  // joc_slope_idx
        CHECK(r.read(1) == 0);  // joc_num_dpoints_bits
    }

    // --- joc_data, undone exactly as §6.6.2 Pseudocode 3 specifies ---
    constexpr int kNquant = 96;
    const std::span<const ac3::oba::joc::HuffCode> table{ac3::oba::joc::kMtxCoarse};
    for (int object = 0; object < params.objects; ++object) {
        for (int channel = 0; channel < params.channels; ++channel) {
            int previous = kNquant / 2;  // the offset Pseudocode 3 starts from
            for (int band = 0; band < params.bands(); ++band) {
                const int difference = huff_decode(table, r);
                REQUIRE(difference >= 0);
                const int code = (previous + difference) % kNquant;
                previous = code;
                CHECK_THAT(ac3::oba::joc::dequantize(code, false),
                           Catch::Matchers::WithinAbs(
                               params.at(object, channel, band), 0.101));
            }
        }
    }
    CHECK_FALSE(r.overflowed());
    // Only padding may remain, and §6.2.1 caps it at seven bits.
    CHECK(payload.size() * 8 - r.bit_position() < 8);
}

TEST_CASE("reconstruct is a delayed identity when the matrix is a pure passthrough",
         "[oba][joc]") {
    // A degenerate but exact check on the transform pair itself, decoupled
    // from any panning/mixing math: M[0][0][*] = 1, every other entry 0,
    // should hand channel 0 straight back through - modulo the algorithmic
    // delay of whichever pair the domain runs. Both are checked, because
    // both ship: the MDCT pair's own 256 samples (the same one-block delay
    // tests/decoder/test_eac3_decoder.cpp's snr_db helper documents), and
    // the filterbank's 576 (its 640-tap window less one 64-sample hop).
    // oba::joc::reconstruction_delay() is the single place either number is
    // written down, so a test that used the wrong one could not silently
    // pass by measuring a shifted signal against itself.
    for (const auto domain : {ac3::oba::joc::Domain::kMdctBand, ac3::oba::joc::Domain::kQmf}) {
        CAPTURE(domain == ac3::oba::joc::Domain::kQmf);
        ac3::oba::joc::FrameParameters params{.objects = 1, .num_bands_idx = 4};
        params.matrix.assign(params.coefficient_count(), 0.0);
        for (int band = 0; band < params.bands(); ++band) {
            params.at(0, 0, band) = 1.0;
        }

        std::vector<std::vector<float>> bed(5, std::vector<float>(ac3::kSamplesPerFrame, 0.0f));
        for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
            bed[0][static_cast<std::size_t>(n)] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(n) / 48000.0));
        }

        ac3::oba::joc::ReconstructionState state;
        const std::vector<std::span<const float>> bed_views(bed.begin(), bed.end());
        std::vector<std::vector<float>> out;
        for (int frame = 0; frame < 3; ++frame) {
            out = ac3::oba::joc::reconstruct(bed_views, params, state, /*fast_mdct=*/false,
                                       /*fast_imdct=*/false, domain);
        }
        REQUIRE(out.size() == 1);

        const int delay = ac3::oba::joc::reconstruction_delay(domain);
        double signal = 0.0;
        double error = 0.0;
        for (int n = delay; n < ac3::kSamplesPerFrame; ++n) {
            const double s = static_cast<double>(bed[0][static_cast<std::size_t>(n - delay)]);
            const double r = static_cast<double>(out[0][static_cast<std::size_t>(n)]);
            signal += s * s;
            error += (s - r) * (s - r);
        }
        CHECK(10.0 * std::log10(signal / std::max(error, 1e-30)) > 100.0);
    }
}

TEST_CASE("JOC parse_payload decodes back to the matrix it was given", "[oba][joc]") {
    for (const bool fine : {false, true}) {
        CAPTURE(fine);
        ac3::oba::joc::FrameParameters params{
            .objects = 4, .num_bands_idx = 4, .fine_quant = fine, .seq_count = 7};
        params.matrix.resize(params.coefficient_count());
        for (int object = 0; object < params.objects; ++object) {
            for (int channel = 0; channel < params.channels; ++channel) {
                for (int band = 0; band < params.bands(); ++band) {
                    params.at(object, channel, band) =
                        (object == channel ? 1.0 : -0.3) + 0.1 * band - 0.05 * channel;
                }
            }
        }

        const auto payload = ac3::oba::joc::build_payload(params);
        const auto decoded = ac3::oba::joc::parse_payload(payload);
        REQUIRE(decoded.has_value());
        CHECK(decoded->objects == params.objects);
        CHECK(decoded->channels == params.channels);
        CHECK(decoded->num_bands_idx == params.num_bands_idx);
        CHECK(decoded->fine_quant == params.fine_quant);
        CHECK(decoded->seq_count == params.seq_count);
        REQUIRE(decoded->matrix.size() == params.matrix.size());
        for (int object = 0; object < params.objects; ++object) {
            for (int channel = 0; channel < params.channels; ++channel) {
                for (int band = 0; band < params.bands(); ++band) {
                    CAPTURE(object, channel, band);
                    CHECK_THAT(decoded->at(object, channel, band),
                              Catch::Matchers::WithinAbs(params.at(object, channel, band),
                                                          fine ? 0.051 : 0.101));
                }
            }
        }
    }
}

TEST_CASE("JOC parse_payload covers every band count and the object-count boundary", "[oba][joc]") {
    for (const int num_bands_idx : {0, 1, 2, 3, 4, 5, 6, 7}) {
        CAPTURE(num_bands_idx);
        for (const int objects : {1, ac3::oba::joc::kMaxObjects}) {
            CAPTURE(objects);
            ac3::oba::joc::FrameParameters params{.objects = objects, .num_bands_idx = num_bands_idx};
            params.matrix.assign(params.coefficient_count(), 0.0);
            for (int object = 0; object < objects; ++object) {
                for (int channel = 0; channel < params.channels; ++channel) {
                    for (int band = 0; band < params.bands(); ++band) {
                        params.at(object, channel, band) = 0.2 * static_cast<double>(band + 1);
                    }
                }
            }
            const auto payload = ac3::oba::joc::build_payload(params);
            const auto decoded = ac3::oba::joc::parse_payload(payload);
            REQUIRE(decoded.has_value());
            CHECK(decoded->objects == objects);
            CHECK(decoded->bands() == params.bands());
        }
    }
}

TEST_CASE("JOC parse_payload rejects what it cannot cleanly interpret", "[oba][joc]") {
    ac3::oba::joc::FrameParameters params{.objects = 2, .num_bands_idx = 3};
    params.matrix.assign(params.coefficient_count(), 0.5);
    const auto payload = ac3::oba::joc::build_payload(params);
    REQUIRE(ac3::oba::joc::parse_payload(payload).has_value());

    SECTION("truncated payload") {
        for (const std::size_t cut : {std::size_t{1}, payload.size() / 2, payload.size() - 1}) {
            CAPTURE(cut);
            const std::vector<std::byte> truncated(payload.begin(),
                                                   payload.begin() + static_cast<std::ptrdiff_t>(cut));
            CHECK_FALSE(ac3::oba::joc::parse_payload(truncated).has_value());
        }
    }

    SECTION("a non-5.X downmix config") {
        // joc_dmx_config_idx occupies the payload's top 3 bits; forcing them
        // to a nonzero value is the cheapest way to name a 7.X config this
        // parser does not implement.
        auto corrupt = payload;
        corrupt[0] |= std::byte{0b001'00000};
        CHECK_FALSE(ac3::oba::joc::parse_payload(corrupt).has_value());
    }

    SECTION("an empty payload") {
        CHECK_FALSE(ac3::oba::joc::parse_payload({}).has_value());
    }
}

TEST_CASE("JOC codes an unchanged band in a single bit", "[oba][joc]") {
    // Value 0 has a one-bit codeword in every generic table, which is the
    // whole reason the matrix is differentially coded along the bands: a
    // coefficient that does not move across the spectrum is nearly free.
    ac3::oba::joc::FrameParameters flat{.objects = 1, .num_bands_idx = 7};  // 23 bands
    flat.matrix.assign(flat.coefficient_count(), 0.0);
    const auto payload = ac3::oba::joc::build_payload(flat);
    // joc_header 12 + joc_info's fixed 18 + 8 per object (presence, bands,
    // sparse, quant, slope, data points) = 38 bits, then 5 channels x 23 bands
    // of zero-difference codewords at one bit each.
    CHECK(payload.size() == (38 + 5 * 23 + 7) / 8);
}

TEST_CASE("OAMD describes a dynamic-object program and its LFE", "[oba][oamd]") {
    // The shape Dolby's own DD+ JOC reference streams use: object_count 16,
    // b_dyn_object_only_program 1, b_lfe_present 1, and joc_num_objects 15 -
    // one fewer, because the LFE is bypassed rather than matrixed.
    const ac3::oba::Program program{
        .dynamic_only = true, .lfe = true, .dynamic_objects = 3};
    CHECK(ac3::oba::object_count(program) == 4);
    CHECK(ac3::oba::joc_object_count(program) == 3);

    const std::array<ac3::oba::DynamicObject, 3> objects{{
        {.position = {.x = 0.0, .y = 0.0, .z = 0.0}, .gain_db = 0.0},
        {.position = {.x = 1.0, .y = 1.0, .z = 1.0}, .gain_db = -6.0},
        {.position = {.x = 0.5, .y = 0.5, .z = -1.0}, .gain_db = 3.0},
    }};
    const auto payload = ac3::oba::build_payload(program, objects);
    ac3::BitReader r{payload};

    CHECK(r.read(2) == 0);  // oa_md_version_bits
    CHECK(r.read(5) == 3);  // object_count_bits = object_count - 1

    // --- program_assignment ---
    // The whole branch is two bits: object_count above already covers the
    // program, so the dynamic-object count is what is left after the LFE.
    CHECK(r.read(1) == 1);  // b_dyn_object_only_program
    CHECK(r.read(1) == 1);  // b_lfe_present

    CHECK(r.read(1) == 0);  // b_alternate_object_data_present
    CHECK(r.read(4) == 1);  // oa_element_count_bits

    // --- oa_element_md ---
    CHECK(r.read(4) == 1);  // oa_element_id_idx: object_element
    const auto size_bits = read_variable_bits_max(r, 4, 4);
    CHECK(r.read(1) == 0);  // b_discard_unknown_element
    const std::size_t element_start = r.bit_position();

    // --- object_element ---
    CHECK(r.read(2) == 0);     // sample_offset_code
    CHECK(r.read(3) == 0);     // num_obj_info_blocks_bits => one block
    CHECK(r.read(6) == 0);     // block_offset_factor_bits
    CHECK(r.read(2) == 0b10);  // ramp_duration_code: 1 536 samples, one frame
    CHECK(r.read(1) == 1);     // b_reserved_data_not_present

    // Object 0 is the bed's LFE: basic info only, no render info, because
    // §5.5.9 forces object_render_info_status_idx to 0 for a bed object.
    CHECK(r.read(1) == 0);     // b_object_not_active
    CHECK(r.read(2) == 0b00);  // object_gain_idx: 0 dB
    CHECK(r.read(1) == 1);     // b_default_object_priority
    CHECK(r.read(1) == 0);     // b_additional_table_data_exists

    const std::array<std::uint32_t, 3> expect_x{0, 62, 31};
    const std::array<std::uint32_t, 3> expect_y{0, 62, 31};
    const std::array<std::uint32_t, 3> expect_z_sign{1, 1, 0};
    const std::array<std::uint32_t, 3> expect_z{0, 15, 15};
    // Table 19: +3 dB is code 15-3 = 12; -6 dB is code 14-(-6) = 20.
    const std::array<std::uint32_t, 3> expect_gain_idx{0b00, 0b10, 0b10};
    const std::array<std::uint32_t, 3> expect_gain_bits{0, 20, 12};

    for (std::size_t object = 0; object < objects.size(); ++object) {
        CAPTURE(object);
        CHECK(r.read(1) == 0);  // b_object_not_active
        CHECK(r.read(2) == expect_gain_idx[object]);
        if (expect_gain_idx[object] == 0b10) {
            CHECK(r.read(6) == expect_gain_bits[object]);
        }
        CHECK(r.read(1) == 1);  // b_default_object_priority

        CHECK(r.read(6) == expect_x[object]);
        CHECK(r.read(6) == expect_y[object]);
        CHECK(r.read(1) == expect_z_sign[object]);
        CHECK(r.read(4) == expect_z[object]);
        CHECK(r.read(1) == 0);     // b_object_distance_specified
        CHECK(r.read(3) == 0);     // zone_constraints_idx
        CHECK(r.read(1) == 1);     // b_enable_elevation
        CHECK(r.read(2) == 0b00);  // object_size_idx: a point source
        CHECK(r.read(1) == 0);     // b_object_use_screen_ref
        CHECK(r.read(1) == 0);     // b_object_snap
        CHECK(r.read(1) == 0);     // b_additional_table_data_exists
    }
    CHECK_FALSE(r.overflowed());

    // §5.6.4.3: oa_element_size covers b_discard_unknown_element, the element
    // and its padding. The reader is at the end of the element now, so the
    // measured content is exactly known and the declared size must be the
    // fewest whole bytes that holds it - stated as an equality, because "the
    // region covers the content" alone is satisfied by a size that is one byte
    // too big and only fails when the content happens to fill its last byte.
    const std::size_t content_bits = r.bit_position() - (element_start - 1);
    CHECK((size_bits + 1) * 8 == (content_bits + 7) / 8 * 8);

    // That boundary is a byte measured from the ELEMENT's start, which is not
    // the payload's - so §5.5.2's own trailing padding still has bits to add.
    const std::size_t element_end = element_start - 1 + (size_bits + 1) * 8;
    CHECK(element_end <= payload.size() * 8);
    CHECK(payload.size() * 8 - element_end < 8);
}

TEST_CASE("oa_element_size holds whatever the object count makes it", "[oba][oamd]") {
    // The element's length moves by 31 bits per object, so it lands on every
    // residue mod 8 as the count climbs - including the one where the content
    // exactly fills its last byte, which is the only count that catches a size
    // computed from the element without the flag bit that precedes it.
    for (int count = 1; count <= 8; ++count) {
        CAPTURE(count);
        const ac3::oba::Program program{
            .dynamic_only = true, .lfe = true, .dynamic_objects = count};
        const std::vector<ac3::oba::DynamicObject> objects(
            static_cast<std::size_t>(count));
        const auto payload = ac3::oba::build_payload(program, objects);

        ac3::BitReader r{payload};
        r.skip(2 + 5);  // version, object_count
        r.skip(1 + 1);  // b_dyn_object_only_program, b_lfe_present
        r.skip(1 + 4);  // alternate data, oa_element_count
        r.skip(4);                  // oa_element_id_idx
        const auto size_bits = read_variable_bits_max(r, 4, 4);
        const std::size_t flag_at = r.bit_position();

        r.skip(1);                  // b_discard_unknown_element
        r.skip(2 + 3 + 6 + 2 + 1);  // md_update_info, block_update_info, reserved
        r.skip(1 + 2 + 1 + 1);      // the bed's LFE object_info_block
        for (int object = 0; object < count; ++object) {
            r.skip(1 + 2 + 1);                     // not_active, gain (0 dB), priority
            r.skip(6 + 6 + 1 + 4 + 1);             // position and distance
            r.skip(3 + 1 + 2 + 1 + 1);             // zone, size, screen ref, snap
            r.skip(1);                             // b_additional_table_data_exists
        }
        REQUIRE_FALSE(r.overflowed());

        const std::size_t content_bits = r.bit_position() - flag_at;
        CHECK((size_bits + 1) * 8 == (content_bits + 7) / 8 * 8);
    }
}

TEST_CASE("OAMD carries a full 5.1 bed when asked", "[oba][oamd]") {
    const ac3::oba::Program program{
        .dynamic_only = false, .bed = ac3::oba::bed::k51, .dynamic_objects = 0};
    CHECK(ac3::oba::object_count(program) == 6);
    CHECK(ac3::oba::joc_object_count(program) == 5);

    const auto payload = ac3::oba::build_payload(program, {});
    ac3::BitReader r{payload};
    CHECK(r.read(2) == 0);       // oa_md_version_bits
    CHECK(r.read(5) == 5);       // object_count_bits
    CHECK(r.read(1) == 0);  // b_dyn_object_only_program
    // content_description written index 0 first, so the bed flag (element 3)
    // is the LAST of the four bits, not the first.
    CHECK(r.read(4) == 0b0001);  // a bed, no dynamic objects
    CHECK(r.read(1) == 0);       // b_bed_chan_distribute
    CHECK(r.read(1) == 0);       // b_multiple_bed_instances_present
    CHECK(r.read(1) == 0);       // b_lfe_only
    CHECK(r.read(1) == 1);       // b_standard_chan_assign
    // Table 12 with index 0 first: L/R (9), C (8), LFE (7), Ls/Rs (6) become
    // the four LEAST significant bits of the 10. Same order Dolby's own
    // encoder writes for a 7.1.4 bed.
    CHECK(r.read(10) == 0b0000001111);
}

TEST_CASE("OAMD payload decodes back to the program and objects it described", "[oba][oamd]") {
    const ac3::oba::Program program{
        .dynamic_only = true, .lfe = true, .dynamic_objects = 3};
    const std::array<ac3::oba::DynamicObject, 3> objects{{
        {.position = {.x = 0.0, .y = 0.0, .z = 0.0}, .gain_db = 0.0},
        {.position = {.x = 1.0, .y = 1.0, .z = 1.0}, .gain_db = -6.0},
        {.position = {.x = 0.5, .y = 0.5, .z = -1.0}, .gain_db = 3.0},
    }};
    const auto payload = ac3::oba::build_payload(program, objects);

    const auto decoded = ac3::oba::parse_payload(payload);
    REQUIRE(decoded.has_value());
    CHECK(decoded->program.dynamic_only == program.dynamic_only);
    CHECK(decoded->program.lfe == program.lfe);
    CHECK(decoded->program.dynamic_objects == program.dynamic_objects);
    REQUIRE(decoded->objects.size() == objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        CAPTURE(i);
        // Positions/z round-trip exactly here because every input value sits
        // exactly on the quantizer's grid (0, 0.5, 1 over 62nds; -1, 0, 1
        // over 15ths) - the same reason test_oba's encode-side test above can
        // assert exact codes rather than tolerances.
        CHECK(decoded->objects[i].position.x == objects[i].position.x);
        CHECK(decoded->objects[i].position.y == objects[i].position.y);
        CHECK(decoded->objects[i].position.z == objects[i].position.z);
        CHECK(decoded->objects[i].gain_db == objects[i].gain_db);
    }
}

TEST_CASE("OAMD payload decodes a full 5.1 bed with no dynamic objects", "[oba][oamd]") {
    const ac3::oba::Program program{
        .dynamic_only = false, .bed = ac3::oba::bed::k51, .dynamic_objects = 0};
    const auto payload = ac3::oba::build_payload(program, {});

    const auto decoded = ac3::oba::parse_payload(payload);
    REQUIRE(decoded.has_value());
    CHECK_FALSE(decoded->program.dynamic_only);
    CHECK(decoded->program.bed == ac3::oba::bed::k51);
    CHECK(decoded->program.dynamic_objects == 0);
    CHECK(decoded->objects.empty());
}

TEST_CASE("OAMD payload decodes a bed plus dynamic objects together", "[oba][oamd]") {
    const ac3::oba::Program program{
        .dynamic_only = false, .bed = ac3::oba::bed::kLfe, .dynamic_objects = 2};
    const std::array<ac3::oba::DynamicObject, 2> objects{{
        {.position = {.x = 0.25, .y = 0.75, .z = 0.5}, .gain_db = -20.0},
        {.position = {.x = 1.0, .y = 0.0, .z = -0.5}, .gain_db = 10.0},
    }};
    const auto payload = ac3::oba::build_payload(program, objects);

    const auto decoded = ac3::oba::parse_payload(payload);
    REQUIRE(decoded.has_value());
    CHECK_FALSE(decoded->program.dynamic_only);
    CHECK(decoded->program.bed == ac3::oba::bed::kLfe);
    REQUIRE(decoded->objects.size() == 2);
    CHECK(decoded->objects[0].gain_db == -20.0);
    CHECK(decoded->objects[1].gain_db == 10.0);
}

TEST_CASE("OAMD gain decodes at its boundary and mid-range values", "[oba][oamd]") {
    // Table 19's two ends (+15, -49) plus a value from each range, and the
    // unreachable-through-object_gain_bits 0 dB that forces the other index.
    for (const double gain : {0.0, 15.0, -49.0, 7.0, -12.0, 1.0, -1.0}) {
        CAPTURE(gain);
        const ac3::oba::Program program{
            .dynamic_only = true, .lfe = false, .dynamic_objects = 1};
        const std::array<ac3::oba::DynamicObject, 1> objects{
            {{.position = {.x = 0.5, .y = 0.5, .z = 0.0}, .gain_db = gain}}};
        const auto payload = ac3::oba::build_payload(program, objects);
        const auto decoded = ac3::oba::parse_payload(payload);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->objects.size() == 1);
        CHECK(decoded->objects[0].gain_db == gain);
    }
}

TEST_CASE("OAMD parse_payload rejects what it cannot cleanly interpret", "[oba][oamd]") {
    const ac3::oba::Program program{
        .dynamic_only = true, .lfe = true, .dynamic_objects = 2};
    const std::array<ac3::oba::DynamicObject, 2> objects{{
        {.position = {.x = 0.2, .y = 0.3, .z = 0.1}, .gain_db = 0.0},
        {.position = {.x = 0.8, .y = 0.7, .z = -0.2}, .gain_db = -4.0},
    }};
    const auto payload = ac3::oba::build_payload(program, objects);
    REQUIRE(ac3::oba::parse_payload(payload).has_value());  // the payload under test genuinely parses

    SECTION("truncated payload") {
        for (std::size_t cut : {std::size_t{1}, payload.size() / 2, payload.size() - 1}) {
            CAPTURE(cut);
            const std::vector<std::byte> truncated(payload.begin(),
                                                   payload.begin() + static_cast<std::ptrdiff_t>(cut));
            CHECK_FALSE(ac3::oba::parse_payload(truncated).has_value());
        }
    }

    SECTION("object_count contradicts the program it describes") {
        // Flip one bit of object_count_bits (bits 2..6 of byte 0): a
        // consistency check this parser makes that a byte-for-byte inverse
        // of build_payload alone would never exercise.
        auto corrupt = payload;
        corrupt[0] ^= std::byte{0b0000'0100};
        CHECK_FALSE(ac3::oba::parse_payload(corrupt).has_value());
    }

    SECTION("an empty payload") {
        CHECK_FALSE(ac3::oba::parse_payload({}).has_value());
    }
}

// --- roadmap DC6/DC7: the syntax the parsers used to refuse ----------------

TEST_CASE("OAMD round-trips object size, snap and zone constraints", "[oba][oamd]") {
    // Table 17's three shapes, plus the two rendering flags §5.6.1.5/§5.6.1.6
    // put beside them. Every size value here sits on the 31-step grid so the
    // comparison can be exact rather than a tolerance.
    const ac3::oba::Program program{.dynamic_only = true, .lfe = false, .dynamic_objects = 3};
    const std::array<ac3::oba::DynamicObject, 3> objects{{
        {.position = {.x = 0.5, .y = 0.5, .z = 0.0},
         .size = {},  // object_size_idx 0b00, a point source
         .zone = ac3::oba::ZoneConstraint::kNone,
         .enable_elevation = true,
         .snap = false},
        {.position = {.x = 0.0, .y = 1.0, .z = 1.0},
         // isotropic: one object_size_bits for all three axes
         .size = {.width = 16.0 / 31.0, .depth = 16.0 / 31.0, .height = 16.0 / 31.0},
         .zone = ac3::oba::ZoneConstraint::kScreenOnly,
         .enable_elevation = false,
         .snap = true},
        {.position = {.x = 1.0, .y = 0.0, .z = -1.0},
         // three separate axes: object_width/depth/height_bits
         .size = {.width = 5.0 / 31.0, .depth = 20.0 / 31.0, .height = 31.0 / 31.0},
         .zone = ac3::oba::ZoneConstraint::kSurroundOnly,
         .enable_elevation = true,
         .snap = true},
    }};

    const auto payload = ac3::oba::build_payload(program, objects);
    const auto decoded = ac3::oba::parse_payload(payload);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->objects.size() == objects.size());
    for (std::size_t i = 0; i < objects.size(); ++i) {
        CAPTURE(i);
        CHECK(decoded->objects[i].size.width == objects[i].size.width);
        CHECK(decoded->objects[i].size.depth == objects[i].size.depth);
        CHECK(decoded->objects[i].size.height == objects[i].size.height);
        CHECK(decoded->objects[i].snap == objects[i].snap);
        CHECK(decoded->objects[i].zone == objects[i].zone);
        CHECK(decoded->objects[i].enable_elevation == objects[i].enable_elevation);
    }
}

TEST_CASE("OAMD round-trips a non-default object priority", "[oba][oamd]") {
    // §5.6.1.3.2: the 5-bit field spans [0; 1) in 32nds, and 1,0 is reachable
    // only through b_default_object_priority - so 1,0 and 31/32 have to come
    // back distinguishable.
    const ac3::oba::Program program{.dynamic_only = true, .lfe = false, .dynamic_objects = 3};
    const std::array<ac3::oba::DynamicObject, 3> objects{{
        {.priority = 1.0},
        {.priority = 31.0 / 32.0},
        {.priority = 0.0},
    }};
    const auto payload = ac3::oba::build_payload(program, objects);
    const auto decoded = ac3::oba::parse_payload(payload);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->objects.size() == 3);
    CHECK(decoded->objects[0].priority == 1.0);
    CHECK(decoded->objects[1].priority == 31.0 / 32.0);
    CHECK(decoded->objects[2].priority == 0.0);
}

TEST_CASE("OAMD reads a program shape build_payload never writes", "[oba][oamd]") {
    // Hand-built rather than round-tripped, because the point is syntax the
    // writer has no way to produce: two md_update blocks, a second one coding
    // its object differentially, an explicit priority, a distance, a screen
    // reference and Table 18's "reuse the previous object's gain".
    ac3::BitWriter w;
    // §5.6's flag arrays go out index 0 first, so element n lands at bit
    // (width - 1 - n) - the same transform oamd.cpp's flags_msb_first makes.
    const auto flags_index0_first = [](ac3::BitWriter& into, std::uint32_t flags, int width) {
        std::uint32_t out = 0;
        for (int i = 0; i < width; ++i) {
            if ((flags & (1u << i)) != 0) {
                out |= 1u << (width - 1 - i);
            }
        }
        into.put(out, width);
    };

    w.put(0, 2);  // oa_md_version_bits
    w.put(1, 5);  // object_count_bits => 2 objects
    w.put(1, 1);  // b_dyn_object_only_program
    w.put(0, 1);  // b_lfe_present => both objects are dynamic
    w.put(0, 1);  // b_alternate_object_data_present
    w.put(1, 4);  // oa_element_count_bits

    // The object_element is measured after the fact, so it is built into its
    // own writer first and its size then prefixed - exactly what
    // build_payload's own probe pass does.
    ac3::BitWriter e;
    e.put(0b10, 2);  // sample_offset_code: an explicit sample_offset_bits
    e.put(9, 5);     // sample_offset = 9 samples
    e.put(1, 3);     // num_obj_info_blocks_bits => 2 blocks
    e.put(0, 6);     // block[0] block_offset_factor
    e.put(0b10, 2);  // block[0] ramp_duration_code => 1536
    e.put(24, 6);    // block[1] block_offset_factor
    e.put(0b01, 2);  // block[1] ramp_duration_code => 512
    e.put(1, 1);     // b_reserved_data_not_present

    // --- object 0 -----------------------------------------------------------
    // block 0: status indices are implied, so this is the ordinary shape.
    e.put(0, 1);     // b_object_not_active
    e.put(0b10, 2);  // object_gain_idx: explicit
    e.put(9, 6);     // object_gain_bits 9 => +6 dB (Table 19's first range)
    e.put(0, 1);     // b_default_object_priority == 0
    e.put(8, 5);     // object_priority_bits => 8/32
    e.put(31, 6);    // pos3D_X_bits
    e.put(31, 6);    // pos3D_Y_bits
    e.put(1, 1);     // pos3D_Z_sign_bits: positive
    e.put(15, 4);    // pos3D_Z_bits => +1
    e.put(1, 1);     // b_object_distance_specified
    e.put(0, 1);     // b_object_at_infinity == 0
    e.put(3, 4);     // distance_factor_idx 3 => 2,0
    e.put(0, 3);     // zone_constraints_idx
    e.put(1, 1);     // b_enable_elevation
    e.put(0b00, 2);  // object_size_idx: point source
    e.put(1, 1);     // b_object_use_screen_ref
    e.put(5, 3);     // screen_factor_bits => (5 + 1) / 8
    e.put(3, 2);     // depth_factor_idx 3 => 2,0
    e.put(0, 1);     // b_object_snap
    e.put(0, 1);     // b_additional_table_data_exists
    // block 1: explicit status indices, a differential position, and a
    // "mixed" render info that names only the position group.
    e.put(0, 1);     // b_object_not_active
    e.put(0b10, 2);  // object_basic_info_status_idx: full REUSE, nothing coded
    e.put(0b11, 2);  // object_render_info_status_idx: mixed
    flags_index0_first(e, 1u << 3, 4);  // obj_render_info: position only (Table 31 index 3)
    e.put(1, 1);                     // b_differential_position_specified
    e.put(0b111, 3);                 // diff_pos3D_X_bits = -1
    e.put(0b000, 3);                 // diff_pos3D_Y_bits = 0
    e.put(0b110, 3);                 // diff_pos3D_Z_bits = -2
    e.put(0, 1);                     // b_object_distance_specified
    e.put(0, 1);                     // b_object_snap
    e.put(0, 1);                     // b_additional_table_data_exists

    // --- object 1 -----------------------------------------------------------
    e.put(0, 1);     // b_object_not_active
    e.put(0b11, 2);  // object_gain_idx: the previous object's gain in this block
    e.put(1, 1);     // b_default_object_priority
    e.put(0, 6);     // pos3D_X_bits
    e.put(0, 6);     // pos3D_Y_bits
    e.put(0, 1);     // pos3D_Z_sign_bits: negative
    e.put(15, 4);    // pos3D_Z_bits => -1
    e.put(0, 1);     // b_object_distance_specified
    e.put(4, 3);     // zone_constraints_idx 4 => screen only
    e.put(0, 1);     // b_enable_elevation
    e.put(0b01, 2);  // object_size_idx: isotropic
    e.put(31, 5);    // object_size_bits => 1,0 on all three axes
    e.put(0, 1);     // b_object_use_screen_ref
    e.put(1, 1);     // b_object_snap
    e.put(1, 1);     // b_additional_table_data_exists
    e.put(0, 4);     // additional_table_data_size_bits => 1 byte
    e.put(0xA5, 8);  // a byte this parser must skip, not interpret
    // block 1: inactive, so §5.5.9 codes nothing else for it at all.
    e.put(1, 1);     // b_object_not_active
    e.put(0, 1);     // b_additional_table_data_exists

    // bit_count() BEFORE take(), which empties the writer.
    const std::size_t element_bits = e.bit_count() + 1;  // + b_discard_unknown_element
    const auto element = e.take();
    const auto element_bytes = static_cast<std::uint32_t>((element_bits + 7) / 8);

    w.put(1, 4);  // oa_element_id_idx: object_element
    // oa_element_size_bits, variable_bits_max(4, 4). One group covers 0..15;
    // this element needs two, which is exactly the shape build_payload's own
    // put_variable_bits_max writes.
    const std::uint32_t size_value = element_bytes - 1;
    if (size_value < 16) {
        w.put(size_value, 4);
        w.put(0, 1);  // read_more
    } else {
        REQUIRE(size_value < 16 + 256);
        const std::uint32_t encoded = size_value - 16;
        w.put((encoded >> 4) & 0xFu, 4);
        w.put(1, 1);  // read_more
        w.put(encoded & 0xFu, 4);
        w.put(0, 1);  // read_more
    }
    w.put(0, 1);  // b_discard_unknown_element
    ac3::BitReader replay{element};
    for (std::size_t bit = 0; bit + 1 < element_bits; ++bit) {
        w.put(replay.read_bit(), 1);
    }
    for (std::size_t bit = element_bits; bit < element_bytes * 8; ++bit) {
        w.put(0, 1);  // §5.6.4.14 padding
    }

    const auto payload = w.take();
    const auto decoded = ac3::oba::parse_payload(payload);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->blocks.size() == 2);
    CHECK(decoded->blocks[0].sample_offset == 9);
    CHECK(decoded->blocks[0].ramp_duration == 1536);
    CHECK(decoded->blocks[1].block_offset_factor == 24);
    CHECK(decoded->blocks[1].ramp_duration == 512);
    REQUIRE(decoded->objects.size() == 2);

    const auto& first = decoded->blocks[0].objects[0];
    CHECK(first.gain_db == 6.0);
    CHECK(first.priority == 8.0 / 32.0);
    CHECK(first.position.x == 31.0 / 62.0);
    CHECK(first.position.z == 1.0);
    REQUIRE(first.distance.has_value());
    CHECK_FALSE(first.distance->at_infinity);
    CHECK(first.distance->factor == 2.0);
    CHECK(first.screen_reference);
    CHECK(first.screen_factor == 6.0 / 8.0);
    CHECK(first.depth_factor == 2.0);

    // Block 1 reused the basic info and stepped the position; the distance
    // it did NOT re-send is gone, because §5.6.1.1.15's flag was re-read as 0.
    const auto& stepped = decoded->blocks[1].objects[0];
    CHECK(stepped.gain_db == 6.0);
    CHECK(stepped.priority == 8.0 / 32.0);
    CHECK(stepped.position.x == 30.0 / 62.0);
    CHECK(stepped.position.z == 1.0 - 2.0 / 15.0);
    CHECK_FALSE(stepped.distance.has_value());

    // Table 18's gain_idx 3 takes the PREVIOUS OBJECT's gain in the same block.
    const auto& second = decoded->blocks[0].objects[1];
    CHECK(second.gain_db == 6.0);
    CHECK(second.priority == 1.0);
    CHECK(second.zone == ac3::oba::ZoneConstraint::kScreenOnly);
    CHECK_FALSE(second.enable_elevation);
    CHECK(second.size.width == 1.0);
    CHECK(second.size.is_isotropic());
    CHECK(second.snap);
    CHECK(decoded->blocks[1].objects[1].active == false);
}

TEST_CASE("OAMD skips an oa_element it does not recognise", "[oba][oamd]") {
    // §5.6.4.3's whole purpose: an unknown element costs a decoder a seek,
    // not the payload. Built by splicing a reserved-id element in front of a
    // real one produced by build_payload.
    const ac3::oba::Program program{.dynamic_only = true, .lfe = true, .dynamic_objects = 1};
    const std::array<ac3::oba::DynamicObject, 1> objects{
        {{.position = {.x = 0.5, .y = 0.5, .z = 0.0}}}};
    const auto original = ac3::oba::build_payload(program, objects);
    REQUIRE(ac3::oba::parse_payload(original).has_value());

    // Re-emit the payload with oa_element_count 2 and a 2-byte element of
    // reserved id 7 ahead of the real one.
    ac3::BitReader r{original};
    ac3::BitWriter w;
    for (int bit = 0; bit < 2 + 5 + 1 + 1 + 1; ++bit) {  // through b_alternate_object_data_present
        w.put(r.read_bit(), 1);
    }
    CHECK(r.read(4) == 1);  // oa_element_count_bits
    w.put(2, 4);
    w.put(7, 4);     // oa_element_id_idx 7: reserved (Table 26)
    w.put(1, 4);     // oa_element_size_bits => 2 bytes
    w.put(0, 1);     // read_more
    w.put(0, 1);     // b_discard_unknown_element
    w.put(0x5A, 8);  // contents this parser must not try to interpret
    w.put(0x7F, 7);
    const std::size_t remaining = original.size() * 8 - r.bit_position();
    for (std::size_t bit = 0; bit < remaining; ++bit) {
        w.put(r.read_bit(), 1);
    }

    const auto spliced = w.take();
    const auto decoded = ac3::oba::parse_payload(spliced);
    REQUIRE(decoded.has_value());
    CHECK(decoded->skipped_elements == std::vector<int>{7});
    REQUIRE(decoded->objects.size() == 1);
    CHECK(decoded->objects[0].position.x == 31.0 / 62.0);
}

TEST_CASE("JOC parses every Table 47 downmix configuration it can", "[oba][joc]") {
    CHECK(ac3::oba::joc::dmx_channel_count(ac3::oba::joc::kDmxConfig5X) == 5);
    CHECK(ac3::oba::joc::dmx_channel_count(ac3::oba::joc::kDmxConfig7X) == 7);
    CHECK(ac3::oba::joc::dmx_channel_count(ac3::oba::joc::kDmxConfig5XPlus2) == 7);
    CHECK(ac3::oba::joc::dmx_channel_count(ac3::oba::joc::kDmxConfig5XPhaseShift) == 5);
    CHECK(ac3::oba::joc::dmx_channel_count(ac3::oba::joc::kDmxConfig5XPlus2PhaseShift) == 7);
    // Table 48 reserves 5..7 and gives them no channel count, which is what
    // parse_payload keys its refusal off.
    CHECK(ac3::oba::joc::dmx_channel_count(5) == 0);
    CHECK(ac3::oba::joc::dmx_channel_count(7) == 0);

    for (const int config : {ac3::oba::joc::kDmxConfig5XPhaseShift, ac3::oba::joc::kDmxConfig7X}) {
        CAPTURE(config);
        const int channels = ac3::oba::joc::dmx_channel_count(config);
        ac3::BitWriter w;
        w.put(static_cast<std::uint32_t>(config), 3);
        w.put(1, 6);  // joc_num_objects_bits => 2 objects
        w.put(0, 3);  // joc_ext_config_idx
        w.put(3, 3);  // joc_clipgain_x_bits
        w.put(4, 5);  // joc_clipgain_y_bits
        w.put(7, 10);  // joc_seq_count_bits
        // Two objects with DIFFERENT band counts and quantizers - the case
        // FrameParameters used to have no room to represent.
        w.put(1, 1);  // b_joc_obj_present
        w.put(0, 3);  // joc_num_bands_idx 0 => 1 band
        w.put(0, 1);  // b_joc_sparse
        w.put(0, 1);  // joc_num_quant_idx: coarse
        w.put(0, 1);  // joc_slope_idx: smooth
        w.put(0, 1);  // joc_num_dpoints_bits => 1
        w.put(1, 1);  // b_joc_obj_present
        w.put(1, 3);  // joc_num_bands_idx 1 => 3 bands
        w.put(0, 1);  // b_joc_sparse
        w.put(1, 1);  // joc_num_quant_idx: fine
        w.put(0, 1);  // joc_slope_idx
        w.put(0, 1);  // joc_num_dpoints_bits
        // joc_data: the shortest codeword in each table is value 0, one bit.
        for (int object = 0; object < 2; ++object) {
            const int bands = object == 0 ? 1 : 3;
            for (int ch = 0; ch < channels; ++ch) {
                for (int band = 0; band < bands; ++band) {
                    w.put(0, 1);
                }
            }
        }
        const auto payload = w.take();

        const auto decoded = ac3::oba::joc::parse_payload(payload);
        REQUIRE(decoded.has_value());
        CHECK(decoded->dmx_config_idx == config);
        CHECK(decoded->channels == channels);
        CHECK(decoded->objects == 2);
        REQUIRE(decoded->shapes.size() == 2);
        CHECK(decoded->shapes[0].num_bands_idx == 0);
        CHECK_FALSE(decoded->shapes[0].fine_quant);
        CHECK(decoded->shapes[1].num_bands_idx == 1);
        CHECK(decoded->shapes[1].fine_quant);
        // §6.3.3.2 as this codebase reads it: (1 + y/32) * 2^x.
        CHECK(decoded->clip_gain == Catch::Approx((1.0 + 4.0 / 32.0) * 8.0));
        CHECK(decoded->seq_count == 7);
        // Value 0 in each table is the largest negative step; what matters
        // here is that the matrix is sized per object, not per frame.
        CHECK(decoded->matrix.size() == decoded->coefficient_count());
        CHECK(decoded->coefficient_count() ==
              static_cast<std::size_t>(channels) * (1 + 3));
    }
}

TEST_CASE("JOC refuses the two headers that carry no length", "[oba][joc]") {
    const auto header = [](int dmx_config, int ext_config) {
        ac3::BitWriter w;
        w.put(static_cast<std::uint32_t>(dmx_config), 3);
        w.put(0, 6);
        w.put(static_cast<std::uint32_t>(ext_config), 3);
        w.put(0, 3);
        w.put(0, 5);
        w.put(0, 10);
        w.put(1, 1);
        w.put(0, 3);
        w.put(0, 1);
        w.put(0, 1);
        w.put(0, 1);
        w.put(0, 1);
        for (int ch = 0; ch < 5; ++ch) {
            w.put(0, 1);
        }
        return w.take();
    };
    // A reserved joc_dmx_config_idx: Table 48 names no channel count, so
    // joc_data has no loop bound.
    CHECK_FALSE(ac3::oba::joc::parse_payload(header(6, 0)).has_value());
    // A nonzero joc_ext_config_idx: §6.2.1 gives joc_ext_data() no syntax at
    // all, so there is nothing to skip past either.
    CHECK_FALSE(ac3::oba::joc::parse_payload(header(0, 1)).has_value());
}

TEST_CASE("EMDF reports a payload configuration outside Table 56's shape", "[emdf]") {
    // Real Dolby streams mix configurations inside one container - the DD+
    // JOC fixture sends OAMD with payload_frame_aligned 0 beside JOC with it
    // set. This builds the same asymmetry by hand.
    ac3::BitWriter w;
    w.put(0, 2);  // emdf_version
    w.put(0, 3);  // key_id
    // payload 1: a sample offset and a duration, so the alignment branch is
    // never entered but priority/proc_allowed still are.
    w.put(11, 5);   // emdf_payload_id: OAMD
    w.put(1, 1);    // smploffste
    w.put(640, 11); // smploffst
    w.put(0, 1);    // reserved
    w.put(1, 1);    // duratione
    w.put(5, 11);   // duration, one variable_bits group
    w.put(0, 1);    // read_more
    w.put(0, 1);    // groupide
    w.put(0, 1);    // codecdatae
    w.put(0, 1);    // discard_unknown_payload
    w.put(9, 5);    // priority
    w.put(2, 2);    // proc_allowed
    w.put(1, 8);    // emdf_payload_size: 1 byte
    w.put(0, 1);    // read_more
    w.put(0xC3, 8);
    // payload 2: the id-extension escape (§H.2.2.2.2), and discard_unknown
    // set so the whole alignment/priority tail is absent.
    w.put(0x1F, 5);  // emdf_payload_id escape
    w.put(4, 5);     // + variable_bits(5) => id 35
    w.put(0, 1);     // read_more
    w.put(0, 1);     // smploffste
    w.put(0, 1);     // duratione
    w.put(0, 1);     // groupide
    w.put(0, 1);     // codecdatae
    w.put(1, 1);     // discard_unknown_payload
    w.put(2, 8);     // emdf_payload_size: 2 bytes
    w.put(0, 1);     // read_more
    w.put(0x11, 8);
    w.put(0x22, 8);
    w.put(0, 5);     // terminating emdf_payload_id
    w.put(0b11, 2);  // protection_length_primary: 128 bits
    w.put(0b00, 2);  // protection_length_secondary: none
    for (int i = 0; i < 4; ++i) {
        w.put(0, 32);
    }
    const auto body = w.take();

    ac3::BitWriter framed;
    framed.put(ac3::emdf::kSyncWord, 16);
    framed.put(static_cast<std::uint32_t>(body.size()), 16);
    for (const auto byte : body) {
        framed.put(std::to_integer<std::uint32_t>(byte), 8);
    }
    const auto data = framed.take();

    const auto result = ac3::emdf::parse_container(data);
    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    const auto& payloads = **result;
    REQUIRE(payloads.size() == 2);

    CHECK(payloads[0].id == ac3::emdf::kPayloadIdOamd);
    CHECK(payloads[0].config.sample_offset == 640);
    CHECK(payloads[0].config.duration == 5);
    CHECK(payloads[0].config.group_id == -1);
    CHECK_FALSE(payloads[0].config.frame_aligned);
    CHECK(payloads[0].config.priority == 9);
    CHECK(payloads[0].config.proc_allowed == 2);
    CHECK(payloads[0].bytes.size() == 1);

    CHECK(payloads[1].id == 35);
    CHECK(payloads[1].config.discard_unknown);
    CHECK(payloads[1].config.sample_offset == -1);
    CHECK(payloads[1].bytes.size() == 2);
}

TEST_CASE("EMDF still refuses a reserved primary protection length", "[emdf]") {
    // Table H.2.5 leaves 0b00 reserved, so it names no field width and the
    // container cannot be walked past it - the one shape parse_container has
    // to keep refusing now that every payload configuration is readable.
    ac3::BitWriter w;
    w.put(0, 2);     // emdf_version
    w.put(0, 3);     // key_id
    w.put(0, 5);     // terminating emdf_payload_id
    w.put(0b00, 2);  // protection_length_primary: reserved
    w.put(0b01, 2);  // protection_length_secondary
    const auto body = w.take();

    ac3::BitWriter framed;
    framed.put(ac3::emdf::kSyncWord, 16);
    framed.put(static_cast<std::uint32_t>(body.size()), 16);
    for (const auto byte : body) {
        framed.put(std::to_integer<std::uint32_t>(byte), 8);
    }
    const auto data = framed.take();

    const auto result = ac3::emdf::parse_container(data);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac3::emdf::ParseError::kUnsupportedConfig);
}
