// The AC-4 decoder's ASF syntax (src/ac4dec/src/syntax/asf.cpp) and scale
// factor band tables (tables/sfb_tables.cpp) on hand-built bitstreams:
// sf_info() for long, short and differently framed transforms at each frame
// length family, sf_data() with every spectral codebook, escapes, scale
// factors and noise fill, chparam_info()'s M/S and SAP data, and the HSF
// extension's interleaved sf_data()/sf_hsf_data(). The committed streams all
// run at 2048 samples a frame and do not reach the short frame lengths or the
// 96/192 kHz extension at all.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4dec_bits.hpp"
#include "bit_reader.hpp"
#include "huffman.hpp"
#include "syntax/asf.hpp"
#include "syntax/context.hpp"
#include "iclforge/ac4core/tables/huffman_tables.hpp"
#include "iclforge/ac4core/tables/sfb_tables.hpp"

namespace {

using iclforge::ac4::DecodeError;
using iclforge::ac4::detail::BitReader;
using iclforge::ac4::detail::ChparamInfo;
using iclforge::ac4::detail::Codebook;
using iclforge::ac4::detail::HsfExtHeader;
using iclforge::ac4::detail::HsfSfData;
using iclforge::ac4::detail::ParseResult;
using iclforge::ac4::detail::SfData;
using iclforge::ac4::detail::SfInfo;
using iclforge::ac4::detail::SubstreamContext;
using ac4dec_test::BitWriter;
using ac4dec_test::Recorder;
namespace tables = iclforge::ac4::detail::tables;

SubstreamContext context(int frame_len_base) {
    SubstreamContext ctx;
    ctx.frame_len_base = frame_len_base;
    ctx.b_iframe = true;
    return ctx;
}

// Runs `parse` over `w`'s bytes and requires, on success, that it stopped at
// the end of `w` when `exact`.
template <typename F>
ParseResult read(const BitWriter& w, F&& parse, bool exact = true) {
    const std::vector<std::byte> bytes = w.bytes();
    Recorder rec;
    BitReader reader(bytes, 0, rec);
    const ParseResult result = parse(reader);
    if (result && exact) {
        CHECK(reader.position() == w.size());
    }
    return result;
}

// The ASF spectrum codeword for `lines` (signed values; an unsigned book's
// signs and cb 11's escapes follow it, as Table 41 reads them).
void put_lines(BitWriter& w, int cb, const std::array<int, 4>& lines) {
    const Codebook& book = *tables::kAsfSpectrumCodebooks[static_cast<std::size_t>(cb)];
    const int dim = tables::kCbDim[static_cast<std::size_t>(cb)];
    const bool is_unsigned = tables::kUnsignedCb[static_cast<std::size_t>(cb)];
    std::array<int, 4> q{};
    for (int d = 0; d < dim; ++d) {
        const int line = lines[static_cast<std::size_t>(d)];
        int value = is_unsigned ? std::abs(line) : line;
        if (cb == 11 && value > 16) {
            value = 16;
        }
        q[static_cast<std::size_t>(d)] = value + book.cb_off;
    }
    const int index = dim == 4 ? q[0] * book.cb_mod3 + q[1] * book.cb_mod2 + q[2] * book.cb_mod + q[3]
                               : q[0] * book.cb_mod + q[1];
    w.code(book, index);
    if (is_unsigned) {
        for (int d = 0; d < dim; ++d) {
            if (lines[static_cast<std::size_t>(d)] != 0) {
                w.flag(lines[static_cast<std::size_t>(d)] < 0);
            }
        }
    }
    if (cb == 11) {
        for (int d = 0; d < 2; ++d) {
            const auto magnitude = static_cast<unsigned>(std::abs(lines[static_cast<std::size_t>(d)]));
            if (magnitude >= 16) {
                const int n_ext = static_cast<int>(std::bit_width(magnitude)) - 5;
                w.put((std::uint64_t{1} << n_ext) - 1, n_ext);
                w.flag(false);
                w.put(magnitude - (1U << static_cast<unsigned>(n_ext + 4)), n_ext + 4);
            }
        }
    }
}

void put_shortest(BitWriter& w, const Codebook& codebook) {
    w.put(codebook.sorted[0].code, codebook.sorted[0].bits);
}

int shortest_index(const Codebook& codebook) { return codebook.sorted[0].index; }

// sf_info() of a long frame at 2048 with `max_sfb`.
SfInfo long_info(int max_sfb) {
    BitWriter w;
    w.flag(true);
    w.put(static_cast<std::uint64_t>(max_sfb), 6);
    SfInfo info;
    REQUIRE(read(w, [&](BitReader& r) { return iclforge::ac4::detail::parse_sf_info(r, context(2048), 0, false, false, info); })
                .has_value());
    return info;
}

}  // namespace

// --- sf_info() ---------------------------------------------------------------

TEST_CASE("sf_info groups the windows of a differently framed short frame", "[ac4dec][asf]") {
    // transf_length 2 then 3 at 2048: two 512-sample windows and one of 1024,
    // with one grouping bit (Table 109) that joins the first two.
    BitWriter w;
    w.flag(false);   // b_long_frame
    w.put(2, 2);     // transf_length[0]
    w.put(3, 2);     // transf_length[1]
    w.put(2, 6);     // max_sfb[0]
    w.put(1, 6);     // max_sfb[1]
    w.put(1, 1);     // scale_factor_grouping_bit
    SfInfo info;
    const SubstreamContext ctx = context(2048);
    REQUIRE(read(w, [&](BitReader& r) { return iclforge::ac4::detail::parse_sf_info(r, ctx, 0, false, false, info); })
                .has_value());
    const auto& psy = info.psy;
    CHECK_FALSE(psy.b_long_frame);
    CHECK(psy.b_different_framing);
    CHECK(psy.n_grp_bits == 1);
    CHECK(psy.num_windows == 3);
    CHECK(psy.num_window_groups == 2);
    CHECK(psy.num_win_in_group[0] == 2);
    CHECK(psy.num_win_in_group[1] == 1);
    CHECK(iclforge::ac4::detail::get_transf_length(ctx, psy, 0) == 2);
    CHECK(iclforge::ac4::detail::get_transf_length(ctx, psy, 1) == 3);
    CHECK(iclforge::ac4::detail::get_max_sfb(ctx, psy, 0, false) == 2);
    CHECK(iclforge::ac4::detail::get_max_sfb(ctx, psy, 1, false) == 1);

    SECTION("its sf_data() has a section list per group, of their own widths") {
        BitWriter d;
        d.put(0, 4);     // group 0 (512-sample transforms): sect_cb 0
        d.put(1, 3);     // sect_len_incr, three bits: two bands
        d.put(0, 4);     // group 1 (1024): sect_cb 0
        d.put(0, 5);     // sect_len_incr, five bits: one band
        d.put(77, 8);    // reference_scale_factor
        d.flag(true);    // b_snf_data_exists: three bands of noise fill
        for (int i = 0; i < 3; ++i) {
            put_shortest(d, tables::kAsfHcbSnf);
        }
        SfData out;
        HsfSfData hsf;
        REQUIRE(read(d, [&](BitReader& r) {
                    return iclforge::ac4::detail::parse_sf_data(r, ctx, info, false, nullptr, out,
                                                                hsf);
                }).has_value());
        CHECK(out.sections[0].size() == 1);
        CHECK(out.sections[1].size() == 1);
        CHECK(out.max_sfb[0] == 2);
        // Group 0's bands span both of its windows.
        CHECK(out.sect_sfb_offset[0][2] == tables::sfb_offsets_48(512)[2] * 2);
        CHECK(out.sect_sfb_offset[1][0] == tables::sfb_offsets_48(512)[2] * 2);
        CHECK(out.reference_scale_factor == 77);
        CHECK(out.snf_present[1][0]);
        CHECK(out.dpcm_snf[0][1] == shortest_index(tables::kAsfHcbSnf));
    }
    SECTION("chparam_info reads M/S flags for every band of both groups") {
        BitWriter c;
        c.put(1, 2);         // sap_mode 1
        c.put(0b101, 3);     // ms_used: two bands, then one
        ChparamInfo out;
        REQUIRE(read(c, [&](BitReader& r) { return iclforge::ac4::detail::parse_chparam_info(r, ctx, info, out); }).has_value());
        CHECK(out.ms_used[0][0]);
        CHECK_FALSE(out.ms_used[0][1]);
        CHECK(out.ms_used[1][0]);
    }
    SECTION("chparam_info reads SAP data per band pair, and delta_code_time for two groups") {
        BitWriter c;
        c.put(3, 2);         // sap_mode 3
        c.flag(false);       // sap_coeff_all
        c.flag(true);        // sap_coeff_used, group 0 bands 0-1
        c.flag(false);       // group 1 band 0
        c.flag(true);        // delta_code_time
        c.code(tables::kAsfHcbScalefac, 50);  // sap_hcw for group 0 bands 0-1
        ChparamInfo out;
        REQUIRE(read(c, [&](BitReader& r) { return iclforge::ac4::detail::parse_chparam_info(r, ctx, info, out); }).has_value());
        CHECK_FALSE(out.sap_coeff_all);
        CHECK(out.sap_coeff_used[0][0]);
        CHECK(out.sap_coeff_used[0][1]);
        CHECK_FALSE(out.sap_coeff_used[1][0]);
        CHECK(out.delta_code_time);
        CHECK(out.dpcm_alpha_q[0][0] == 50);
    }
}

TEST_CASE("chparam_info reads SAP coefficients for all bands", "[ac4dec][asf]") {
    const SfInfo info = long_info(3);
    BitWriter c;
    c.put(3, 2);                          // sap_mode 3
    c.flag(true);                         // sap_coeff_all
    c.code(tables::kAsfHcbScalefac, 60);  // bands 0-1
    c.code(tables::kAsfHcbScalefac, 61);  // band 2
    ChparamInfo out;
    REQUIRE(read(c, [&](BitReader& r) { return iclforge::ac4::detail::parse_chparam_info(r, context(2048), info, out); })
                .has_value());
    CHECK(out.sap_coeff_all);
    CHECK(out.sap_coeff_used[0][2]);
    CHECK_FALSE(out.delta_code_time);
    CHECK(out.dpcm_alpha_q[0][0] == 60);
    CHECK(out.dpcm_alpha_q[0][2] == 61);
}

TEST_CASE("sf_info reads side-limited and dual max_sfb for both halves of a split frame", "[ac4dec][asf]") {
    const SubstreamContext ctx = context(2048);
    SECTION("side-limited: n_side_bits wide") {
        BitWriter w;
        w.flag(false);
        w.put(1, 2);     // 256 samples
        w.put(3, 2);     // 1024
        w.put(9, 4);     // max_sfb_side[0]: n_side_bits of 256 is 4
        w.put(17, 5);    // max_sfb_side[1]: of 1024, 5
        w.put(0, 3);     // Table 109 [1][3]: three grouping bits
        SfInfo info;
        REQUIRE(read(w, [&](BitReader& r) { return iclforge::ac4::detail::parse_sf_info(r, ctx, 0, false, true, info); })
                    .has_value());
        CHECK(info.psy.max_sfb_side[0] == 9);
        CHECK(info.psy.max_sfb_side[1] == 17);
        CHECK(info.psy.num_window_groups == 5);
        CHECK(iclforge::ac4::detail::get_max_sfb(ctx, info.psy, 4, false) == 17);
    }
    SECTION("dual: max_sfb and max_sfb_side in each half") {
        BitWriter w;
        w.flag(false);
        w.put(3, 2);
        w.put(2, 2);
        w.put(30, 6);    // max_sfb[0]
        w.put(31, 6);    // max_sfb_side[0]
        w.put(20, 6);    // max_sfb[1]
        w.put(21, 6);    // max_sfb_side[1]
        w.put(1, 1);     // Table 109 [3][2]: one grouping bit
        SfInfo info;
        REQUIRE(read(w, [&](BitReader& r) { return iclforge::ac4::detail::parse_sf_info(r, ctx, 0, true, false, info); })
                    .has_value());
        CHECK(info.psy.max_sfb[1] == 20);
        CHECK(info.psy.max_sfb_side[1] == 21);
        CHECK(iclforge::ac4::detail::get_max_sfb(ctx, info.psy, 0, true) == 31);
        CHECK(iclforge::ac4::detail::get_max_sfb(ctx, info.psy, 0, false) == 30);
    }
    SECTION("a max_sfb past either transform's bands") {
        for (const bool second : {false, true}) {
            BitWriter w;
            w.flag(false);
            w.put(second ? 3 : 0, 2);
            w.put(second ? 0 : 3, 2);
            if (second) {
                w.put(0, 6);   // max_sfb[0] of 1024: fine
                w.put(15, 4);  // max_sfb[1] of 128: 15 of 14 bands
            } else {
                w.put(15, 4);
            }
            w.put(0, 16);
            SfInfo info;
            const auto result =
                read(w, [&](BitReader& r) { return iclforge::ac4::detail::parse_sf_info(r, ctx, 0, false, false, info); });
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().error == DecodeError::kInvalidStream);
        }
    }
}

TEST_CASE("sf_info at the shorter frame lengths reads one transform length", "[ac4dec][asf]") {
    struct Case {
        int frame_len_base;
        int transf_length;
        int grouping_bits;
        int max_sfb_bits;
    };
    // Table 110's two families: kLong (1024, 960, 768) and kShort (512, 384).
    for (const Case c : {Case{1024, 0, 7, 4}, Case{1024, 3, 0, 6}, Case{960, 1, 3, 5}, Case{768, 2, 1, 6},
                         Case{512, 0, 3, 4}, Case{512, 2, 0, 6}, Case{384, 1, 1, 5}}) {
        INFO("frame_len_base " << c.frame_len_base << ", transf_length " << c.transf_length);
        BitWriter w;
        w.put(static_cast<std::uint64_t>(c.transf_length), 2);
        w.put(1, c.max_sfb_bits);
        for (int i = 0; i < c.grouping_bits; ++i) {
            w.flag(i % 2 == 0);
        }
        SfInfo info;
        const SubstreamContext ctx = context(c.frame_len_base);
        REQUIRE(read(w, [&](BitReader& r) { return iclforge::ac4::detail::parse_sf_info(r, ctx, 0, false, false, info); })
                    .has_value());
        CHECK_FALSE(info.psy.b_long_frame);
        CHECK(info.psy.single_transf_length);
        CHECK(info.psy.n_grp_bits == c.grouping_bits);
        CHECK(info.psy.num_windows == c.grouping_bits + 1);
        CHECK(info.psy.max_sfb[0] == 1);
        CHECK(iclforge::ac4::detail::get_transf_length(ctx, info.psy, 0) == c.transf_length);
    }
    SECTION("a transform length past the frame's") {
        BitWriter w;
        w.put(3, 2);
        w.put(0, 14);
        SfInfo info;
        const auto result = read(w, [&](BitReader& r) {
            return iclforge::ac4::detail::parse_sf_info(r, context(384), 0, false, false, info);
        });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kInvalidStream);
    }
    SECTION("the speech frontend") {
        BitWriter w;
        w.put(0, 16);
        SfInfo info;
        const auto result = read(w, [&](BitReader& r) {
            return iclforge::ac4::detail::parse_sf_info(r, context(2048), 1, false, false, info);
        });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kUnsupported);
        SfData data;
        HsfSfData hsf;
        const auto refused = read(w, [&](BitReader& r) {
            return iclforge::ac4::detail::parse_sf_data(r, context(2048), info, false, nullptr,
                                                        data, hsf);
        });
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().error == DecodeError::kUnsupported);
    }
}

TEST_CASE("sf_info_lfe takes the whole frame's transform", "[ac4dec][asf]") {
    SECTION("at 1024, two bits of max_sfb and the whole-frame index") {
        BitWriter w;
        w.put(3, 2);
        SfInfo info;
        REQUIRE(read(w, [&](BitReader& r) { return iclforge::ac4::detail::parse_sf_info_lfe(r, context(1024), info); })
                    .has_value());
        CHECK(info.is_lfe);
        CHECK(info.psy.max_sfb[0] == 3);
        CHECK(info.psy.transf_length[0] == 3);
        CHECK(iclforge::ac4::detail::transform_length_samples(context(1024), info.psy.transf_length[0]) == 1024);
    }
    SECTION("at 384, the whole frame is index 2") {
        BitWriter w;
        w.put(1, 2);
        SfInfo info;
        REQUIRE(read(w, [&](BitReader& r) { return iclforge::ac4::detail::parse_sf_info_lfe(r, context(384), info); })
                    .has_value());
        CHECK(info.psy.transf_length[0] == 2);
    }
    SECTION("a frame length Table 106 does not list") {
        BitWriter w;
        w.put(0, 8);
        SfInfo info;
        const auto result =
            read(w, [&](BitReader& r) { return iclforge::ac4::detail::parse_sf_info_lfe(r, context(1000), info); });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kInvalidStream);
    }
}

TEST_CASE("transform lengths and widths follow Tables 103 and 106", "[ac4dec][asf]") {
    using iclforge::ac4::detail::n_side_bits;
    using iclforge::ac4::detail::transform_length_samples;
    CHECK(transform_length_samples(context(2048), 0) == 128);
    CHECK(transform_length_samples(context(2048), 4) == 2048);
    CHECK(transform_length_samples(context(1920), 3) == 960);
    CHECK(transform_length_samples(context(2048), -1) == 0);
    CHECK(transform_length_samples(context(1024), 0) == 128);
    CHECK(transform_length_samples(context(1024), 4) == 0);
    CHECK(transform_length_samples(context(512), 2) == 512);
    CHECK(transform_length_samples(context(512), 3) == 0);
    CHECK(n_side_bits(context(2048), 4) == 5);
    CHECK(n_side_bits(context(2048), 1) == 4);
    CHECK(n_side_bits(context(2048), 0) == 3);
    CHECK(n_side_bits(context(384), 0) == 3);  // 96 samples
    CHECK(n_side_bits(context(1000), 4) == 0);
}

// --- sf_data() ---------------------------------------------------------------

TEST_CASE("sf_data decodes the lines of every spectral codebook", "[ac4dec][asf]") {
    // Two bands of one codebook, a band of none; scale factors for the two
    // coded bands (the first taken as the reference, the second sent) and
    // noise fill for the silent one.
    const SfInfo info = long_info(3);
    const auto offsets = tables::sfb_offsets_48(2048);
    const int lines = offsets[2] - offsets[0];
    for (int cb = 1; cb <= 11; ++cb) {
        INFO("codebook " << cb);
        const int dim = tables::kCbDim[static_cast<std::size_t>(cb)];
        std::array<int, 4> pattern{};
        switch (cb) {
            case 1:
            case 2:
                pattern = {1, -1, 0, 1};
                break;
            case 3:
            case 4:
                pattern = {2, -1, 0, 1};
                break;
            case 5:
            case 6:
                pattern = {3, -4, 0, 0};
                break;
            case 7:
            case 8:
                pattern = {7, -5, 0, 0};
                break;
            case 9:
            case 10:
                pattern = {-12, 1, 0, 0};
                break;
            default:
                pattern = {100, -16, 0, 0};
                break;
        }
        BitWriter w;
        w.put(static_cast<std::uint64_t>(cb), 4);
        w.put(1, 5);     // two bands
        w.put(0, 4);
        w.put(0, 5);     // one band of none
        for (int k = 0; k < lines; k += dim) {
            put_lines(w, cb, pattern);
        }
        w.put(100, 8);   // reference_scale_factor
        w.code(tables::kAsfHcbScalefac, 58);
        w.flag(true);    // b_snf_data_exists
        w.code(tables::kAsfHcbSnf, 3);
        SfData out;
        HsfSfData hsf;
        REQUIRE(read(w, [&](BitReader& r) {
                    return iclforge::ac4::detail::parse_sf_data(r, context(2048), info, false, nullptr, out, hsf);
                }).has_value());
        for (int k = 0; k < lines; ++k) {
            CHECK(out.quant_spec[static_cast<std::size_t>(k)] == pattern[static_cast<std::size_t>(k % dim)]);
        }
        CHECK(out.quant_spec[static_cast<std::size_t>(lines)] == 0);
        int peak = 0;
        for (int d = 0; d < dim; ++d) {
            peak = std::max(peak, std::abs(pattern[static_cast<std::size_t>(d)]));
        }
        CHECK(out.max_quant_idx[0][0] == peak);
        CHECK(out.scale_factor_present[0][0]);
        CHECK(out.dpcm_sf[0][1] == 58);
        CHECK_FALSE(out.scale_factor_present[0][2]);
        CHECK(out.snf_present[0][2]);
        CHECK(out.dpcm_snf[0][2] == 3);
        CHECK(out.num_sec_lsf[0] == 2);
    }
}

TEST_CASE("sf_data escapes a long section and refuses malformed ones", "[ac4dec][asf]") {
    SECTION("sect_len_incr's escape") {
        const SfInfo info = long_info(40);
        BitWriter w;
        w.put(0, 4);
        w.put(31, 5);    // escape
        w.put(8, 5);     // 1 + 31 + 8 = 40 bands
        w.put(0, 8);
        w.flag(false);
        SfData out;
        HsfSfData hsf;
        REQUIRE(read(w, [&](BitReader& r) {
                    return iclforge::ac4::detail::parse_sf_data(r, context(2048), info, false, nullptr, out, hsf);
                }).has_value());
        REQUIRE(out.sections[0].size() == 1);
        CHECK(out.sections[0][0].end == 40);
    }
    const auto refused = [](const BitWriter& w, const SfInfo& info) {
        SfData out;
        HsfSfData hsf;
        const auto result = read(w, [&](BitReader& r) {
            return iclforge::ac4::detail::parse_sf_data(r, context(2048), info, false, nullptr, out,
                                                        hsf);
        });
        REQUIRE_FALSE(result.has_value());
        return result.error().error;
    };
    SECTION("sect_cb 12") {
        BitWriter w;
        w.put(12, 4);
        w.put(0, 5);
        w.put(0, 16);
        CHECK(refused(w, long_info(1)) == DecodeError::kInvalidStream);
    }
    SECTION("a section past max_sfb") {
        BitWriter w;
        w.put(0, 4);
        w.put(4, 5);
        w.put(0, 16);
        CHECK(refused(w, long_info(3)) == DecodeError::kInvalidStream);
    }
    SECTION("escapes to the end of the data") {
        // Four escapes end the data exactly, so the fifth sect_len_incr is
        // read from nothing.
        BitWriter w;
        w.put(0, 4);
        for (int i = 0; i < 4; ++i) {
            w.put(31, 5);
        }
        REQUIRE(w.size() % 8 == 0);
        CHECK(refused(w, long_info(60)) == DecodeError::kTruncated);
    }
    SECTION("an ext_code longer than 21 bits") {
        BitWriter w;
        w.put(11, 4);
        w.put(0, 5);
        put_lines(w, 11, {0, 0, 0, 0});
        // The first band holds four lines: the first codeword covers two
        // zeros, the second (16, 0), whose sign bit and then an ext_code of
        // nine leading ones follow.
        w.code(*tables::kAsfSpectrumCodebooks[11], 16 * 17);
        w.flag(false);     // the sign of the 16
        w.put(0x1FF, 9);
        w.put(0, 30);
        CHECK(refused(w, long_info(1)) == DecodeError::kInvalidStream);
    }
    SECTION("a max_sfb past the transform's bands in a hand-made sf_info") {
        SfInfo info = long_info(1);
        info.psy.max_sfb[0] = 64;
        BitWriter w;
        w.put(0, 16);
        CHECK(refused(w, info) == DecodeError::kInvalidStream);
    }
}

// --- the HSF extension -------------------------------------------------------

namespace {

// sf_info() of a 128-sample short frame at frame_len_base 512, all four
// windows in one group.
SfInfo short_info_512(int max_sfb) {
    BitWriter w;
    w.put(0, 2);
    w.put(static_cast<std::uint64_t>(max_sfb), 4);
    w.put(0b111, 3);
    SfInfo info;
    REQUIRE(read(w, [&](BitReader& r) { return iclforge::ac4::detail::parse_sf_info(r, context(512), 0, false, false, info); })
                .has_value());
    return info;
}

}  // namespace

TEST_CASE("sf_data and sf_hsf_data share a section straddling the 48 kHz bands", "[ac4dec][asf]") {
    // max_sfb 13 and max_sfb_ext_hsf 3 at 96 kHz: bands 0 to 15, of which 14
    // and 15 are the extension's. Sections: none for 0-12, codebook 1 for
    // 13-14 (split at 14), none for 15. Each band is 16 lines of four windows.
    SubstreamContext ctx = context(512);
    ctx.sf_multiplier = 0;
    const SfInfo info = short_info_512(13);

    BitWriter ext;
    ext.put(3, 6);   // max_sfb_ext_hsf[0]
    BitWriter core;
    core.put(0, 4);
    core.put(7, 3);  // escape
    core.put(5, 3);  // 13 bands
    core.put(1, 4);
    core.put(1, 3);  // two bands
    core.put(0, 4);
    core.put(0, 3);  // one band
    put_lines(core, 1, {1, 0, 0, 0});
    for (int k = 1; k < 16; ++k) {
        put_lines(core, 1, {0, 0, 0, 0});
    }
    core.put(40, 8);  // reference_scale_factor
    core.flag(true);  // b_snf_data_exists: bands 0 to 12
    for (int band = 0; band < 13; ++band) {
        put_shortest(core, tables::kAsfHcbSnf);
    }
    // The extension's sf_hsf_data(): band 14's lines, its scale factor (the
    // core's first was band 13), and band 15's noise fill.
    put_lines(ext, 1, {0, -1, 0, 0});
    for (int k = 1; k < 16; ++k) {
        put_lines(ext, 1, {0, 0, 0, 0});
    }
    ext.code(tables::kAsfHcbScalefac, 55);
    ext.code(tables::kAsfHcbSnf, 7);

    const std::vector<std::byte> core_bytes = core.bytes();
    const std::vector<std::byte> ext_bytes = ext.bytes();
    Recorder rec;
    BitReader core_reader(core_bytes, 0, rec);
    BitReader ext_reader(ext_bytes, 1, rec);
    HsfExtHeader header;
    REQUIRE(iclforge::ac4::detail::parse_hsf_ext_header(ext_reader, false, header).has_value());
    CHECK(header.max_sfb_ext_hsf[0] == 3);
    CHECK(iclforge::ac4::detail::get_max_sfb_hsf(ctx, info.psy, 0, header) == 16);
    SfData out;
    HsfSfData hsf;
    REQUIRE(iclforge::ac4::detail::parse_sf_data(core_reader, ctx, info, false, &header, out, hsf).has_value());
    CHECK(core_reader.position() == core.size());
    CHECK(out.max_sfb[0] == 14);
    CHECK(out.num_sec_lsf[0] == 2);
    REQUIRE(out.sections[0].size() == 2);
    CHECK(out.sections[0][1].end == 14);
    CHECK(out.quant_spec[static_cast<std::size_t>(out.sect_sfb_offset[0][13])] == 1);
    CHECK(out.scale_factor_present[0][13]);
    CHECK(hsf.start_sfb[0] == 14);
    CHECK(hsf.max_sfb_hsf[0] == 16);
    REQUIRE(hsf.sections[0].size() == 2);
    CHECK(hsf.sections[0][0].cb == 1);
    CHECK(hsf.sections[0][0].start == 14);
    CHECK(hsf.sections[0][1].start == 15);

    REQUIRE(iclforge::ac4::detail::parse_sf_hsf_data(ext_reader, info.psy.num_window_groups, out, hsf).has_value());
    CHECK(ext_reader.position() == ext.size());
    CHECK(hsf.quant_spec[1] == -1);
    CHECK(hsf.max_quant_idx[0][0] == 1);
    CHECK(hsf.max_quant_idx[0][1] == 0);
    CHECK(hsf.scale_factor_present[0][0]);
    CHECK(hsf.dpcm_sf[0][0] == 55);
    CHECK(hsf.snf_present[0][1]);
    CHECK(hsf.dpcm_snf[0][1] == 7);
}

TEST_CASE("the HSF extension at 192 kHz and its limits", "[ac4dec][asf]") {
    const SfInfo info = short_info_512(2);
    SECTION("192 kHz: four times the transform, and no extension bands") {
        SubstreamContext ctx = context(512);
        ctx.sf_multiplier = 1;
        HsfExtHeader header;
        BitWriter core;
        core.put(0, 4);
        core.put(1, 3);  // two bands of none
        core.put(0, 8);
        core.flag(false);
        SfData out;
        HsfSfData hsf;
        REQUIRE(read(core, [&](BitReader& r) {
                    return iclforge::ac4::detail::parse_sf_data(r, ctx, info, false, &header, out,
                                                                hsf);
                }).has_value());
        CHECK(hsf.start_sfb[0] == 14);
        CHECK(hsf.max_sfb_hsf[0] == 2);
        CHECK(hsf.quant_spec.empty());
    }
    SECTION("an extension past the 96 kHz bands") {
        SubstreamContext ctx = context(512);
        ctx.sf_multiplier = 0;
        HsfExtHeader header;
        header.max_sfb_ext_hsf[0] = 63;
        BitWriter core;
        core.put(0, 32);
        SfData out;
        HsfSfData hsf;
        const auto result = read(core, [&](BitReader& r) {
            return iclforge::ac4::detail::parse_sf_data(r, ctx, info, false, &header, out, hsf);
        });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kInvalidStream);
    }
    SECTION("the header of a differently framed track reads two widths") {
        BitWriter w;
        w.put(5, 6);
        w.put(9, 6);
        HsfExtHeader header;
        REQUIRE(read(w, [&](BitReader& r) { return iclforge::ac4::detail::parse_hsf_ext_header(r, true, header); }).has_value());
        CHECK(header.max_sfb_ext_hsf[0] == 5);
        CHECK(header.max_sfb_ext_hsf[1] == 9);
    }
}

// --- the scale factor band tables --------------------------------------------

TEST_CASE("the scale factor band tables answer only for the lengths they list", "[ac4dec][asf]") {
    CHECK(tables::num_sfb_48(128) == 14);
    CHECK(tables::num_sfb_48(2048) == 63);
    CHECK(tables::num_sfb_48(100) == 0);
    CHECK(tables::sfb_offsets_48(100).empty());
    CHECK(tables::sfb_offsets_48(2048).back() == 2048);
    CHECK(tables::num_sfb_96(256) == 22);
    CHECK(tables::num_sfb_96(100) == 0);
    CHECK(tables::sfb_offsets_96(100).empty());
    CHECK(tables::sfb_offsets_96(4096).back() == 4096);
    CHECK(tables::num_sfb_192(512) == 30);
    CHECK(tables::num_sfb_192(256) == 0);
    CHECK(tables::sfb_offsets_192(100).empty());
    CHECK(tables::sfb_offsets_192(512).back() == 512);
}

TEST_CASE("max_sfb_from_master maps a master max_sfb to shorter transforms", "[ac4dec][asf]") {
    using tables::max_sfb_from_master;
    CHECK(max_sfb_from_master(2048, 10, 2048) == 10);
    CHECK(max_sfb_from_master(2048, 64, 2048) == -1);
    CHECK(max_sfb_from_master(100, 1, 100) == -1);
    CHECK(max_sfb_from_master(2048, -1, 1024) == -1);
    // A shorter length takes its column of the master's table: never more
    // bands than its own transform has, and monotonic in max_sfb_master.
    int previous = 0;
    for (int master = 0; master < 32; ++master) {
        const int mapped = max_sfb_from_master(2048, master, 1024);
        REQUIRE(mapped >= 0);
        CHECK(mapped <= tables::num_sfb_48(1024));
        CHECK(mapped >= previous);
        previous = mapped;
    }
    CHECK(max_sfb_from_master(2048, 32, 1024) == -1);  // past the table's 2^5 rows
    CHECK(max_sfb_from_master(2048, 3, 1920) == -1);   // another family's length
    CHECK(max_sfb_from_master(1024, 3, 2048) == -1);   // longer than the master
    CHECK(max_sfb_from_master(128, 3, 96) == -1);      // no table for 128
}

// --- A codeword cut short by the end of its substream ------------------------

namespace {

// A longest codeword of `codebook`: `sorted` is ordered by length.
int longest_index(const Codebook& codebook) {
    return codebook.sorted.back().index;
}

// `w`'s bytes up to the first byte boundary after bit `start`, which the
// codeword starting there and `bits` long must straddle: a codeword of nine
// bits or more does wherever it starts, a shorter one where it starts off a
// byte boundary.
std::vector<std::byte> cut_inside(const BitWriter& w, std::size_t start, int bits) {
    const std::size_t cut = start / 8 + 1;
    REQUIRE(cut * 8 < start + static_cast<std::size_t>(bits));
    std::vector<std::byte> bytes = w.bytes();
    bytes.resize(cut);
    return bytes;
}

template <typename F>
ParseResult read_cut(const std::vector<std::byte>& bytes, F&& parse) {
    Recorder rec;
    BitReader reader(bytes, 0, rec);
    return parse(reader);
}

}  // namespace

TEST_CASE("an ASF codeword the substream ends inside fails as truncated as in every tool",
          "[ac4dec][asf]") {
    // Each element's longest codeword, placed so that a byte boundary falls
    // inside it; the substream ends there. The
    // A-SPX, A-CPL and dialogue enhancement codewords always reported this
    // as kTruncated, the ASF ones as kInvalidStream (review of #700).
    SfData out;
    HsfSfData hsf;
    const auto sf_data = [&](const SfInfo& info) {
        return [&out, &hsf, info](BitReader& r) {
            return iclforge::ac4::detail::parse_sf_data(r, context(2048), info, false, nullptr, out,
                                                        hsf);
        };
    };
    std::size_t start = 0;
    int bits = 0;
    const auto write_longest = [&start, &bits](BitWriter& w, const Codebook& codebook) {
        start = w.size();
        w.code(codebook, longest_index(codebook));
        bits = static_cast<int>(w.size() - start);
        w.put(0, 64);
    };
    SECTION("asf_qspec_hcw") {
        BitWriter w;
        w.put(11, 4);  // sect_cb 11
        w.put(0, 5);   // one band
        write_longest(w, *tables::kAsfSpectrumCodebooks[11]);
        const auto result = read_cut(cut_inside(w, start, bits), sf_data(long_info(1)));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
        CHECK(result.error().reason ==
              "an ASF spectrum codeword runs past the end of the substream");
    }
    SECTION("asf_sf_hcw") {
        BitWriter w;
        w.put(1, 4);  // sect_cb 1
        w.put(1, 5);  // two bands
        const auto offsets = tables::sfb_offsets_48(2048);
        for (int k = 0; k < offsets[2] - offsets[0]; k += 4) {
            put_lines(w, 1, {1, -1, 0, 1});
        }
        w.put(100, 8);  // reference_scale_factor, for the first band
        write_longest(w, tables::kAsfHcbScalefac);
        const auto result = read_cut(cut_inside(w, start, bits), sf_data(long_info(2)));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
        CHECK(result.error().reason ==
              "an ASF scale factor codeword runs past the end of the substream");
    }
    SECTION("asf_snf_hcw") {
        BitWriter w;
        w.put(0, 4);    // sect_cb 0
        w.put(0, 5);    // one band
        w.put(100, 8);  // reference_scale_factor
        w.flag(true);   // b_snf_data_exists
        write_longest(w, tables::kAsfHcbSnf);
        const auto result = read_cut(cut_inside(w, start, bits), sf_data(long_info(1)));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
        CHECK(result.error().reason ==
              "an ASF noise fill codeword runs past the end of the substream");
    }
    SECTION("sap_hcw") {
        BitWriter c;
        c.put(3, 2);   // sap_mode 3
        c.flag(true);  // sap_coeff_all
        write_longest(c, tables::kAsfHcbScalefac);
        const SfInfo info = long_info(3);
        ChparamInfo chparam;
        const auto result = read_cut(cut_inside(c, start, bits), [&](BitReader& r) {
            return iclforge::ac4::detail::parse_chparam_info(r, context(2048), info, chparam);
        });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
        CHECK(result.error().reason == "a SAP codeword runs past the end of the substream");
    }
}

TEST_CASE("huff_codeword reports a codeword cut short as truncated in every codebook",
          "[ac4dec][asf]") {
    // Every tool reads its codewords through huff_codeword(): each codebook's
    // longest codeword with its last bit cut off by the end of the substream
    // is a miss the syntax reports as truncated, consuming nothing.
    const std::vector<const Codebook*> books = {
        &tables::kAsfHcbScalefac,
        &tables::kAsfHcb1,
        &tables::kAsfHcb2,
        &tables::kAsfHcb3,
        &tables::kAsfHcb4,
        &tables::kAsfHcb5,
        &tables::kAsfHcb6,
        &tables::kAsfHcb7,
        &tables::kAsfHcb8,
        &tables::kAsfHcb9,
        &tables::kAsfHcb10,
        &tables::kAsfHcb11,
        &tables::kAsfHcbSnf,
        &tables::kAspxHcbEnvLevel15F0,
        &tables::kAspxHcbEnvLevel30Dt,
        &tables::kAspxHcbNoiseBalanceDf,
        &tables::kAcplHcbAlphaFineF0,
        &tables::kAcplHcbBetaCoarseDt,
        &tables::kAcplHcbGammaFineDf,
        &tables::kDeHcbAbs0,
        &tables::kDeHcbDiff0,
    };
    const iclforge::ac4::detail::CodewordReasons reasons{.truncated = "cut", .invalid = "none"};
    for (const Codebook* book : books) {
        INFO(book->name);
        BitWriter w;
        const int bits = book->sorted.back().bits;
        REQUIRE(bits >= 2);
        // Padding so that the codeword's last bit starts a byte, which the
        // substream then leaves out.
        const int pad = ((8 - (bits - 1) % 8) % 8);
        w.put(0, pad);
        w.code(*book, longest_index(*book));
        std::vector<std::byte> bytes = w.bytes();
        bytes.resize((static_cast<std::size_t>(pad + bits) - 1) / 8);
        Recorder rec;
        BitReader reader(bytes, 0, rec);
        reader.skip(static_cast<std::size_t>(pad));
        const auto result = iclforge::ac4::detail::huff_codeword(reader, *book, "hcw", reasons);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
        CHECK(result.error().reason == "cut");
        CHECK(reader.position() == static_cast<std::size_t>(pad));
    }
}

namespace {

// The reader as it was before its 64-bit cache: every peek a loop over single
// bits, zeros past the end, the same sticky overflow and the same seek. The
// cached BitReader is held to it operation by operation.
class BitByBitReader {
   public:
    explicit BitByBitReader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] std::uint32_t peek(int bits) const {
        std::uint32_t value = 0;
        for (int i = 0; i < bits; ++i) {
            value = (value << 1U) | bit_at(pos_ + static_cast<std::size_t>(i));
        }
        return value;
    }
    std::uint32_t read(int bits) {
        const std::uint32_t value = peek(bits);
        skip(static_cast<std::size_t>(bits));
        return value;
    }
    void skip(std::size_t bits) {
        pos_ += bits;
        if (pos_ > size_bits()) {
            overflow_ = true;
        }
    }
    void align() { skip((8 - (pos_ % 8)) % 8); }
    void seek(std::size_t bit_position) {
        if (bit_position > size_bits()) {
            overflow_ = true;
            pos_ = size_bits();
        } else {
            pos_ = bit_position;
        }
    }
    [[nodiscard]] std::size_t position() const { return pos_; }
    [[nodiscard]] std::size_t size_bits() const { return data_.size() * 8U; }
    [[nodiscard]] std::size_t remaining_bits() const {
        return pos_ >= size_bits() ? 0 : size_bits() - pos_;
    }
    [[nodiscard]] bool overflow() const { return overflow_; }

   private:
    [[nodiscard]] std::uint32_t bit_at(std::size_t bit) const {
        if (bit >= size_bits()) {
            return 0;
        }
        return (static_cast<std::uint32_t>(data_[bit / 8U]) >>
                (7U - static_cast<unsigned>(bit % 8U))) &
               1U;
    }
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    bool overflow_ = false;
};

std::vector<std::byte> random_bytes(std::size_t count, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<std::byte> bytes(count);
    for (std::byte& b : bytes) {
        b = static_cast<std::byte>(rng() & 0xFFU);
    }
    return bytes;
}

}  // namespace

TEST_CASE("the bit reader's cache gives what reading bit by bit gives", "[ac4dec][asf][reader]") {
    // Data of every length from nothing to a few words, so that the cache is loaded
    // whole, near the end and past it; then a long run of reads, peeks, skips,
    // alignments and seeks, some past the end.
    std::mt19937 rng(20260929);
    for (std::size_t length = 0; length <= 41; ++length) {
        const std::vector<std::byte> data =
            random_bytes(length, static_cast<unsigned>(length) + 7U);
        BitReader fast(data, 0, {});
        BitByBitReader slow(data);
        for (int step = 0; step < 300; ++step) {
            const int bits = static_cast<int>(rng() % 33U);  // 0 to 32
            switch (rng() % 7U) {
                case 0:
                case 1:
                    REQUIRE(fast.peek_raw(bits) == slow.peek(bits));
                    break;
                case 2:
                case 3:
                    if (bits > 0) {
                        REQUIRE(fast.read(bits, "x") == slow.read(bits));
                    }
                    break;
                case 4: {
                    const std::size_t count = rng() % 70U;
                    fast.skip(count);
                    slow.skip(count);
                    break;
                }
                case 5:
                    fast.align();
                    slow.align();
                    break;
                default: {
                    // An absolute position, mostly inside the data, sometimes past it.
                    const std::size_t target = rng() % (data.size() * 8U + 40U);
                    fast.seek(target);
                    slow.seek(target);
                    break;
                }
            }
            REQUIRE(fast.position() == slow.position());
            REQUIRE(fast.overflow() == slow.overflow());
            REQUIRE(fast.remaining_bits() == slow.remaining_bits());
            // Wherever it stands, a peek of every width agrees.
            for (const int width : {1, 7, 8, 9, 16, 25, 31, 32}) {
                REQUIRE(fast.peek_raw(width) == slow.peek(width));
            }
            if (slow.overflow() && rng() % 4U == 0U) {
                break;  // past the end there is nothing more to compare, and a new run starts
            }
        }
    }
}

namespace {

// huff_decode() as it was before the shortcut: at each length up to the bits
// left, the leading bits looked up among that length's entries, here by a linear
// scan; the index and length of the codeword, or -1.
std::pair<int, int> search_decode(std::span<const std::byte> data, std::size_t pos,
                                  const Codebook& book) {
    BitByBitReader at(data);
    at.seek(pos);
    const int max_bits = std::min<int>(book.max_bits, static_cast<int>(at.remaining_bits()));
    const std::uint32_t window = at.peek(max_bits);
    for (int length = 1; length <= max_bits; ++length) {
        const std::uint32_t code = window >> static_cast<unsigned>(max_bits - length);
        for (const auto& entry : book.sorted) {
            if (entry.bits == length && entry.code == code) {
                return {entry.index, length};
            }
        }
    }
    return {-1, 0};
}

}  // namespace

TEST_CASE("every codebook's shortcut table names only real codewords", "[ac4dec][asf][huffman]") {
    for (const Codebook* book : tables::kAllCodebooks) {
        CAPTURE(book->name);
        REQUIRE(book->fast.size() == iclforge::ac4::detail::kHuffFastSize);
        std::size_t filled = 0;
        for (std::size_t v = 0; v < book->fast.size(); ++v) {
            const std::uint16_t entry = book->fast[v];
            if (entry == 0) {
                continue;
            }
            ++filled;
            const int length = entry & 15;
            const int index = entry >> 4;
            REQUIRE(length >= 1);
            REQUIRE(length <= iclforge::ac4::detail::kHuffFastBits);
            // The codeword this value begins: its leading `length` bits.
            const auto code = static_cast<std::uint32_t>(
                v >> static_cast<unsigned>(iclforge::ac4::detail::kHuffFastBits - length));
            bool found = false;
            for (const auto& e : book->sorted) {
                if (e.bits == length && e.code == code && e.index == index) {
                    found = true;
                }
            }
            REQUIRE(found);
        }
        // Every codeword of kHuffFastBits or fewer fills its share of the table.
        std::size_t expected = 0;
        for (const auto& e : book->sorted) {
            if (e.bits <= iclforge::ac4::detail::kHuffFastBits) {
                expected += std::size_t{1} << (iclforge::ac4::detail::kHuffFastBits - e.bits);
            }
        }
        CHECK(filled == expected);
    }
}

TEST_CASE("huff_decode with its shortcut reads every codeword of every codebook",
          "[ac4dec][asf][huffman]") {
    std::mt19937_64 rng(929);
    for (const Codebook* book : tables::kAllCodebooks) {
        CAPTURE(book->name);
        for (const auto& entry : book->sorted) {
            // The codeword, then random bits, over 8 bytes; and again cut to the
            // bytes the codeword needs, whose last byte is zero-padded.
            const std::uint64_t tail = rng() >> entry.bits;
            const std::uint64_t word =
                (static_cast<std::uint64_t>(entry.code) << (64 - entry.bits)) | tail;
            std::vector<std::byte> full(8);
            for (std::size_t i = 0; i < 8; ++i) {
                full[i] = static_cast<std::byte>((word >> (56 - 8 * i)) & 0xFFU);
            }
            BitReader whole(full, 0, {});
            REQUIRE(iclforge::ac4::detail::huff_decode(whole, *book, "hcw") == entry.index);
            REQUIRE(whole.position() == entry.bits);
            const std::vector<std::byte> cut(full.begin(), full.begin() + (entry.bits + 7) / 8);
            BitReader exact(cut, 0, {});
            REQUIRE(iclforge::ac4::detail::huff_decode(exact, *book, "hcw") == entry.index);
            REQUIRE(exact.position() == entry.bits);
            // One bit less than the codeword needs: none found, nothing consumed.
            if (entry.bits % 8 != 0) {
                continue;  // the padding bits are still data; only a whole-byte cut ends inside
            }
            const std::vector<std::byte> shorter(full.begin(), full.begin() + entry.bits / 8 - 1);
            BitReader short_reader(shorter, 0, {});
            REQUIRE(iclforge::ac4::detail::huff_decode(short_reader, *book, "hcw") == -1);
            REQUIRE(short_reader.position() == 0);
        }
    }
}

TEST_CASE(
    "huff_decode with its shortcut agrees with the search on random bits and on the end of the "
    "data",
    "[ac4dec][asf][huffman]") {
    std::mt19937 rng(930);
    for (const Codebook* book : tables::kAllCodebooks) {
        CAPTURE(book->name);
        for (int trial = 0; trial < 400; ++trial) {
            // 1 to 6 bytes, read from a random bit: the codeword lands anywhere, and
            // often runs past the end.
            const std::vector<std::byte> data =
                random_bytes(1 + rng() % 6U, static_cast<unsigned>(rng() & 0xFFFFFFFFU));
            const std::size_t pos = rng() % (data.size() * 8U + 8U);
            BitReader reader(data, 0, {});
            reader.seek(pos);
            const std::size_t start = reader.position();
            const auto [index, length] = search_decode(data, start, *book);
            const int got = iclforge::ac4::detail::huff_decode(reader, *book, "hcw");
            REQUIRE(got == index);
            REQUIRE(reader.position() == start + static_cast<std::size_t>(length));
        }
    }
}
