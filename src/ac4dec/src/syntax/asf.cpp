#include "syntax/asf.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>

#include "huffman.hpp"
#include "tables/huffman_tables.hpp"
#include "tables/sfb_tables.hpp"

namespace ac4::detail {

namespace {

using std::size_t;

[[nodiscard]] size_t at(int index) noexcept { return static_cast<size_t>(index); }

// Table 106, the 44.1/48 kHz columns: n_msfb_bits, n_side_bits and
// n_msfbl_bits by transform length in samples. 0 marks N/A.
struct WidthRow {
    int length;
    int n_msfb_bits;
    int n_side_bits;
    int n_msfbl_bits;
};

constexpr std::array<WidthRow, 15> kTable106 = {{
    {2048, 6, 5, 3},
    {1920, 6, 5, 3},
    {1536, 6, 5, 3},
    {1024, 6, 5, 2},
    {960, 6, 5, 2},
    {768, 6, 5, 2},
    {512, 6, 5, 2},
    {480, 6, 5, 0},
    {384, 6, 4, 2},
    {256, 5, 4, 0},
    {240, 5, 4, 0},
    {192, 5, 3, 0},
    {128, 4, 3, 0},
    {120, 4, 3, 0},
    {96, 4, 3, 0},
}};

[[nodiscard]] const WidthRow* width_row(int length) noexcept {
    for (const auto& row : kTable106) {
        if (row.length == length) {
            return &row;
        }
    }
    return nullptr;
}

// Tables 109 and 110.
[[nodiscard]] int n_grp_bits(const SubstreamContext& ctx, const AsfPsyInfo& psy) noexcept {
    if (ctx.frame_len_base >= 1536) {
        if (psy.b_long_frame) {
            return 0;
        }
        static constexpr std::array<std::array<int, 4>, 4> kTable109 = {{
            {15, 10, 8, 7},
            {10, 7, 4, 3},
            {8, 4, 3, 1},
            {7, 3, 1, 1},
        }};
        return kTable109[at(psy.transf_length[0])][at(psy.transf_length[1])];
    }
    if (ctx.frame_len_base == 512 || ctx.frame_len_base == 384) {
        static constexpr std::array<int, 3> kShort = {3, 1, 0};
        return psy.transf_length[0] < 3 ? kShort[at(psy.transf_length[0])] : -1;
    }
    static constexpr std::array<int, 4> kLong = {7, 3, 1, 0};
    return kLong[at(psy.transf_length[0])];
}

// The index whose transform covers the whole frame when frame_len_base is
// below 1536: 3 for 1024, 960 and 768; 2 for 512 and 384 (Table 103).
[[nodiscard]] int full_frame_index(const SubstreamContext& ctx) noexcept {
    return (ctx.frame_len_base == 512 || ctx.frame_len_base == 384) ? 2 : 3;
}

[[nodiscard]] int n_msfb_bits_for(const SubstreamContext& ctx, int index) noexcept {
    const WidthRow* row = width_row(transform_length_samples(ctx, index));
    return row != nullptr ? row->n_msfb_bits : 0;
}

// A max_sfb, or max_sfb_side, past the scale factor bands of its transform
// (Tables B.4 to B.7) names bands that do not exist. Checked as each is read,
// where the Python transcription checks it too.
[[nodiscard]] ParseResult check_max_sfb(const SubstreamContext& ctx, const AsfPsyInfo& psy, int half,
                                        int transform_index) {
    const int num_sfb = tables::num_sfb_48(transform_length_samples(ctx, transform_index));
    if (psy.max_sfb[at(half)] > num_sfb || psy.max_sfb_side[at(half)] > num_sfb) {
        return fail(DecodeError::kInvalidStream, "max_sfb exceeds the scale factor bands of its transform");
    }
    return {};
}

}  // namespace

int transform_length_samples(const SubstreamContext& ctx, int index) noexcept {
    if (ctx.frame_len_base >= 1536) {
        if (index >= kLongFrameIndex) {
            return ctx.frame_len_base;
        }
        return index >= 0 ? ctx.frame_len_base >> (4 - index) : 0;
    }
    const int full = full_frame_index(ctx);
    if (index < 0 || index > full) {
        return 0;
    }
    return ctx.frame_len_base >> (full - index);
}

int n_side_bits(const SubstreamContext& ctx, int transform_index) noexcept {
    const WidthRow* row = width_row(transform_length_samples(ctx, transform_index));
    return row != nullptr ? row->n_side_bits : 0;
}

int get_transf_length(const SubstreamContext& ctx, const AsfPsyInfo& psy, int g) noexcept {
    if (ctx.frame_len_base >= 1536) {
        if (!psy.b_long_frame) {
            const int num_windows_0 = 1 << (3 - psy.transf_length[0]);
            if (g < psy.window_to_group[at(num_windows_0)]) {
                return psy.transf_length[0];
            }
            return psy.transf_length[1];
        }
        return kLongFrameIndex;
    }
    return psy.transf_length[0];
}

int get_max_sfb(const SubstreamContext& ctx, const AsfPsyInfo& psy, int g, bool side_channel) noexcept {
    int idx = 0;
    if (ctx.frame_len_base >= 1536 && !psy.b_long_frame &&
        psy.transf_length[0] != psy.transf_length[1]) {
        const int num_windows_0 = 1 << (3 - psy.transf_length[0]);
        if (g >= psy.window_to_group[at(num_windows_0)]) {
            idx = 1;
        }
    }
    if (psy.b_side_limited || (psy.b_dual_maxsfb && side_channel)) {
        return psy.max_sfb_side[at(idx)];
    }
    return psy.max_sfb[at(idx)];
}

ParseResult parse_sf_info(BitReader& r, const SubstreamContext& ctx, int spec_frontend, bool b_dual_maxsfb,
                          bool b_side_limited, SfInfo& out) {
    out = SfInfo{};
    out.spec_frontend = spec_frontend;
    if (spec_frontend != 0) {
        return fail(DecodeError::kUnsupported, "the speech spectral frontend (SSF) is not decoded");
    }
    AsfPsyInfo& psy = out.psy;

    // 4.2.8.1 asf_transform_info()
    if (ctx.frame_len_base >= 1536) {
        psy.b_long_frame = r.read_flag("b_long_frame");
        if (!psy.b_long_frame) {
            psy.transf_length[0] = static_cast<int>(r.read(2, "transf_length"));
            psy.transf_length[1] = static_cast<int>(r.read(2, "transf_length"));
        }
    } else {
        // Not transmitted below 1536; Pseudocode 3 then counts windows from
        // the grouping bits, which is what b_long_frame == 0 does.
        psy.b_long_frame = false;
        psy.single_transf_length = true;
        psy.transf_length[0] = static_cast<int>(r.read(2, "transf_length"));
        psy.transf_length[1] = psy.transf_length[0];
        if (psy.transf_length[0] > full_frame_index(ctx)) {
            return fail(DecodeError::kInvalidStream, "transf_length names no transform at this frame length");
        }
    }

    // 4.2.8.2 asf_psy_info()
    psy.b_dual_maxsfb = b_dual_maxsfb;
    psy.b_side_limited = b_side_limited;
    psy.b_different_framing = ctx.frame_len_base >= 1536 && !psy.b_long_frame &&
                              psy.transf_length[0] != psy.transf_length[1];
    const int index0 = psy.b_long_frame ? kLongFrameIndex : psy.transf_length[0];
    if (b_side_limited) {
        psy.max_sfb_side[0] = static_cast<int>(r.read(n_side_bits(ctx, index0), "max_sfb_side"));
    } else {
        psy.max_sfb[0] = static_cast<int>(r.read(n_msfb_bits_for(ctx, index0), "max_sfb"));
        if (b_dual_maxsfb) {
            psy.max_sfb_side[0] = static_cast<int>(r.read(n_msfb_bits_for(ctx, index0), "max_sfb_side"));
        }
    }
    if (auto ok = check_max_sfb(ctx, psy, 0, index0); !ok) {
        return ok;
    }
    if (psy.b_different_framing) {
        const int index1 = psy.transf_length[1];
        if (b_side_limited) {
            psy.max_sfb_side[1] = static_cast<int>(r.read(n_side_bits(ctx, index1), "max_sfb_side"));
        } else {
            psy.max_sfb[1] = static_cast<int>(r.read(n_msfb_bits_for(ctx, index1), "max_sfb"));
            if (b_dual_maxsfb) {
                psy.max_sfb_side[1] =
                    static_cast<int>(r.read(n_msfb_bits_for(ctx, index1), "max_sfb_side"));
            }
        }
        if (auto ok = check_max_sfb(ctx, psy, 1, index1); !ok) {
            return ok;
        }
    }
    psy.n_grp_bits = n_grp_bits(ctx, psy);
    if (psy.n_grp_bits < 0) {
        return fail(DecodeError::kInvalidStream, "transf_length names no transform at this frame length");
    }
    for (int i = 0; i < psy.n_grp_bits; ++i) {
        psy.scale_factor_grouping[at(i)] =
            static_cast<std::uint8_t>(r.read(1, "scale_factor_grouping_bit"));
    }
    if (auto ok = check(r); !ok) {
        return ok;
    }

    // Pseudocode 3.
    psy.num_windows = 1;
    psy.num_window_groups = 1;
    psy.window_to_group[0] = 0;
    if (!psy.b_long_frame) {
        psy.num_windows = psy.n_grp_bits + 1;
        if (psy.b_different_framing) {
            const int num_windows_0 = 1 << (3 - psy.transf_length[0]);
            for (int i = psy.n_grp_bits; i >= num_windows_0; --i) {
                psy.scale_factor_grouping[at(i)] = psy.scale_factor_grouping[at(i - 1)];
            }
            psy.scale_factor_grouping[at(num_windows_0 - 1)] = 0;
            ++psy.num_windows;
        }
        if (psy.num_windows > kMaxWindows) {
            return fail(DecodeError::kInvalidStream, "more than sixteen transform windows");
        }
        for (int i = 0; i < psy.num_windows - 1; ++i) {
            if (psy.scale_factor_grouping[at(i)] == 0) {
                ++psy.num_window_groups;
            }
            psy.window_to_group[at(i + 1)] = static_cast<std::uint8_t>(psy.num_window_groups - 1);
        }
    }
    psy.num_win_in_group = {};
    for (int w = 0; w < psy.num_windows; ++w) {
        ++psy.num_win_in_group[psy.window_to_group[at(w)]];
    }
    return {};
}

ParseResult parse_sf_info_lfe(BitReader& r, const SubstreamContext& ctx, SfInfo& out) {
    out = SfInfo{};
    out.is_lfe = true;
    AsfPsyInfo& psy = out.psy;
    psy.b_long_frame = true;
    if (ctx.frame_len_base < 1536) {
        // sf_info_lfe() sets b_long_frame but never transf_length, which is
        // what Pseudocode 2 returns below 1536; the only reading under which
        // the LFE's transform is the frame is the whole-frame index.
        psy.single_transf_length = true;
        psy.transf_length[0] = full_frame_index(ctx);
        psy.transf_length[1] = psy.transf_length[0];
    }
    const WidthRow* row = width_row(ctx.frame_len_base);
    if (row == nullptr || row->n_msfbl_bits == 0) {
        return fail(DecodeError::kInvalidStream, "no LFE max_sfb width at this frame length");
    }
    psy.max_sfb[0] = static_cast<int>(r.read(row->n_msfbl_bits, "max_sfb"));
    if (psy.max_sfb[0] > tables::num_sfb_48(ctx.frame_len_base)) {
        return fail(DecodeError::kInvalidStream, "max_sfb exceeds the scale factor bands of its transform");
    }
    psy.num_windows = 1;
    psy.num_window_groups = 1;
    psy.num_win_in_group[0] = 1;
    return check(r);
}

namespace {

// Pseudocode 19, for one codeword: the quantised lines before signs and
// escapes.
void split_codeword(const Codebook& cb, int dim, int index, std::array<std::int32_t, 4>& lines) {
    int idx = index;
    if (dim == 4) {
        const int q1 = idx / cb.cb_mod3 - cb.cb_off;
        idx -= (q1 + cb.cb_off) * cb.cb_mod3;
        const int q2 = idx / cb.cb_mod2 - cb.cb_off;
        idx -= (q2 + cb.cb_off) * cb.cb_mod2;
        const int q3 = idx / cb.cb_mod - cb.cb_off;
        idx -= (q3 + cb.cb_off) * cb.cb_mod;
        lines = {q1, q2, q3, idx - cb.cb_off};
    } else {
        const int q1 = idx / cb.cb_mod - cb.cb_off;
        idx -= (q1 + cb.cb_off) * cb.cb_mod;
        lines = {q1, idx - cb.cb_off, 0, 0};
    }
}

// Pseudocode 20: ext_decode(ext_code), recorded as one element. Table 40
// gives ext_code 5 to 21 bits, 2 * N_ext + 5, so N_ext is at most 8 and a
// magnitude at most 8191; the pseudocode's loop has no bound of its own. -1
// for a ninth leading one, with nothing recorded.
[[nodiscard]] std::int32_t read_ext_code(BitReader& r) {
    static constexpr int kMaxNExt = 8;
    const size_t start = r.position();
    int n_ext = 0;
    while (r.peek_raw(1) != 0 && !r.overflow()) {
        r.consume(1);
        if (++n_ext > kMaxNExt) {
            return -1;
        }
    }
    r.consume(1);
    const std::uint32_t ext_val = r.peek_raw(n_ext + 4);
    r.consume(n_ext + 4);
    const auto magnitude = static_cast<std::int32_t>((std::uint32_t{1} << static_cast<unsigned>(n_ext + 4)) + ext_val);
    r.emit(start, static_cast<int>(r.position() - start), static_cast<std::uint64_t>(magnitude), "ext_code");
    return magnitude;
}

}  // namespace

ParseResult parse_sf_data(BitReader& r, const SubstreamContext& ctx, const SfInfo& info, bool side_channel,
                          SfData& out) {
    if (info.spec_frontend != 0) {
        return fail(DecodeError::kUnsupported, "the speech spectral frontend (SSF) is not decoded");
    }
    const AsfPsyInfo& psy = info.psy;
    out = SfData{};

    // Pseudocode 4, with each group's own band table.
    int group_offset = 0;
    for (int g = 0; g < psy.num_window_groups; ++g) {
        const int length = transform_length_samples(ctx, get_transf_length(ctx, psy, g));
        const int max_sfb = get_max_sfb(ctx, psy, g, side_channel);
        const auto offsets = tables::sfb_offsets_48(length);
        if (offsets.empty() || max_sfb < 0 || max_sfb > tables::num_sfb_48(length)) {
            return fail(DecodeError::kInvalidStream, "max_sfb exceeds the scale factor bands of its transform");
        }
        out.max_sfb[at(g)] = max_sfb;
        const int wins = psy.num_win_in_group[at(g)];
        for (int sfb = 0; sfb <= max_sfb; ++sfb) {
            out.sect_sfb_offset[at(g)][at(sfb)] =
                static_cast<std::uint16_t>(group_offset + offsets[at(sfb)] * wins);
        }
        group_offset += offsets[at(max_sfb)] * wins;
    }
    out.quant_spec.assign(static_cast<size_t>(group_offset), 0);

    // 4.2.8.3 asf_section_data()
    for (int g = 0; g < psy.num_window_groups; ++g) {
        const int transf_length_g = get_transf_length(ctx, psy, g);
        const int sect_esc_val = transf_length_g <= 2 ? 7 : 31;
        const int n_sect_bits = transf_length_g <= 2 ? 3 : 5;
        const int num_sfb = tables::num_sfb_48(transform_length_samples(ctx, transf_length_g));
        const int max_sfb = out.max_sfb[at(g)];
        auto& sections = out.sections[at(g)];
        int k = 0;
        while (k < max_sfb) {
            AsfSection section;
            section.cb = static_cast<std::uint8_t>(r.read(4, "sect_cb"));
            int sect_len = 1;
            int incr = static_cast<int>(r.read(n_sect_bits, "sect_len_incr"));
            while (incr == sect_esc_val) {
                sect_len += sect_esc_val;
                incr = static_cast<int>(r.read(n_sect_bits, "sect_len_incr"));
                if (r.overflow()) {
                    return check(r);
                }
            }
            sect_len += incr;
            if (section.cb > 11) {
                return fail(DecodeError::kInvalidStream, "sect_cb 12 to 15 name no codebook");
            }
            if (k + sect_len > max_sfb) {
                return fail(DecodeError::kInvalidStream, "a section runs past max_sfb");
            }
            section.start = static_cast<std::uint8_t>(k);
            section.end = static_cast<std::uint8_t>(k + sect_len);
            if (section.start < num_sfb && section.end >= num_sfb) {
                out.num_sec_lsf[at(g)] = static_cast<int>(sections.size()) + 1;
            }
            for (int sfb = k; sfb < k + sect_len; ++sfb) {
                out.sfb_cb[at(g)][at(sfb)] = section.cb;
            }
            sections.push_back(section);
            k += sect_len;
            if (auto ok = check(r); !ok) {
                return ok;
            }
        }
        if (out.num_sec_lsf[at(g)] == 0) {
            out.num_sec_lsf[at(g)] = static_cast<int>(sections.size());
        }
    }

    // 4.2.8.4 asf_spectral_data()
    for (int g = 0; g < psy.num_window_groups; ++g) {
        const auto& sections = out.sections[at(g)];
        for (int i = 0; i < out.num_sec_lsf[at(g)]; ++i) {
            const AsfSection& section = sections[at(i)];
            if (section.cb == 0) {
                continue;
            }
            const Codebook& cb = *tables::kAsfSpectrumCodebooks[section.cb];
            const int dim = tables::kCbDim[section.cb];
            const bool is_unsigned = tables::kUnsignedCb[section.cb];
            const int start_line = out.sect_sfb_offset[at(g)][section.start];
            const int end_line = out.sect_sfb_offset[at(g)][section.end];
            for (int k = start_line; k < end_line; k += dim) {
                const int index = huff_decode(r, cb, "asf_qspec_hcw");
                if (index < 0) {
                    return fail(DecodeError::kInvalidStream, "no ASF spectrum codeword matches");
                }
                std::array<std::int32_t, 4> lines{};
                split_codeword(cb, dim, index, lines);
                if (is_unsigned) {
                    int nonzero = 0;
                    for (int d = 0; d < dim; ++d) {
                        nonzero += lines[at(d)] != 0 ? 1 : 0;
                    }
                    const std::uint32_t signs = r.read(nonzero, dim == 4 ? "quad_sign_bits" : "pair_sign_bits");
                    int bit = nonzero - 1;
                    for (int d = 0; d < dim; ++d) {
                        if (lines[at(d)] != 0) {
                            if (((signs >> static_cast<unsigned>(bit)) & 1U) != 0) {
                                lines[at(d)] = -lines[at(d)];
                            }
                            --bit;
                        }
                    }
                }
                if (section.cb == 11) {
                    for (int d = 0; d < 2; ++d) {
                        if (std::abs(lines[at(d)]) == 16) {
                            const std::int32_t magnitude = read_ext_code(r);
                            if (magnitude < 0) {
                                return fail(DecodeError::kInvalidStream, "ext_code is longer than 21 bits");
                            }
                            lines[at(d)] = lines[at(d)] < 0 ? -magnitude : magnitude;
                        }
                    }
                }
                if (k + dim > end_line) {
                    return fail(DecodeError::kInvalidStream, "a codeword runs past its section");
                }
                for (int d = 0; d < dim; ++d) {
                    out.quant_spec[at(k + d)] = lines[at(d)];
                }
                if (auto ok = check(r); !ok) {
                    return ok;
                }
            }
        }
    }

    // max_quant_idx[g][sfb], the note under Table 41.
    for (int g = 0; g < psy.num_window_groups; ++g) {
        for (int sfb = 0; sfb < out.max_sfb[at(g)]; ++sfb) {
            std::int32_t peak = 0;
            for (int k = out.sect_sfb_offset[at(g)][at(sfb)]; k < out.sect_sfb_offset[at(g)][at(sfb + 1)]; ++k) {
                peak = std::max(peak, std::abs(out.quant_spec[at(k)]));
            }
            out.max_quant_idx[at(g)][at(sfb)] = static_cast<std::uint16_t>(std::min(peak, 65535));
        }
    }

    // 4.2.8.5 asf_scalefac_data()
    out.reference_scale_factor = static_cast<int>(r.read(8, "reference_scale_factor"));
    bool first_scf_found = false;
    for (int g = 0; g < psy.num_window_groups; ++g) {
        const int num_sfb = tables::num_sfb_48(transform_length_samples(ctx, get_transf_length(ctx, psy, g)));
        const int max_sfb = std::min(out.max_sfb[at(g)], num_sfb);
        for (int sfb = 0; sfb < max_sfb; ++sfb) {
            if (out.sfb_cb[at(g)][at(sfb)] != 0 && out.max_quant_idx[at(g)][at(sfb)] > 0) {
                out.scale_factor_present[at(g)][at(sfb)] = true;
                if (first_scf_found) {
                    const int index = huff_decode(r, tables::kAsfHcbScalefac, "asf_sf_hcw");
                    if (index < 0) {
                        return fail(DecodeError::kInvalidStream, "no ASF scale factor codeword matches");
                    }
                    out.dpcm_sf[at(g)][at(sfb)] = static_cast<std::int16_t>(index);
                } else {
                    first_scf_found = true;
                }
            }
        }
    }
    if (auto ok = check(r); !ok) {
        return ok;
    }

    // 4.2.8.6 asf_snf_data()
    out.b_snf_data_exists = r.read_flag("b_snf_data_exists");
    if (out.b_snf_data_exists) {
        for (int g = 0; g < psy.num_window_groups; ++g) {
            const int num_sfb = tables::num_sfb_48(transform_length_samples(ctx, get_transf_length(ctx, psy, g)));
            const int max_sfb = std::min(out.max_sfb[at(g)], num_sfb);
            for (int sfb = 0; sfb < max_sfb; ++sfb) {
                if (out.sfb_cb[at(g)][at(sfb)] == 0 || out.max_quant_idx[at(g)][at(sfb)] == 0) {
                    const int index = huff_decode(r, tables::kAsfHcbSnf, "asf_snf_hcw");
                    if (index < 0) {
                        return fail(DecodeError::kInvalidStream, "no ASF noise fill codeword matches");
                    }
                    out.snf_present[at(g)][at(sfb)] = true;
                    out.dpcm_snf[at(g)][at(sfb)] = static_cast<std::int16_t>(index);
                }
            }
        }
    }
    return check(r);
}

ParseResult parse_chparam_info(BitReader& r, const SubstreamContext& ctx, const SfInfo& info, ChparamInfo& out) {
    out = ChparamInfo{};
    const AsfPsyInfo& psy = info.psy;
    out.sap_mode = static_cast<int>(r.read(2, "sap_mode"));
    if (out.sap_mode == 1) {
        for (int g = 0; g < psy.num_window_groups; ++g) {
            const int max_sfb_g = get_max_sfb(ctx, psy, g, false);
            for (int sfb = 0; sfb < max_sfb_g && sfb < kMaxSfb; ++sfb) {
                out.ms_used[at(g)][at(sfb)] = r.read_flag("ms_used");
            }
        }
    }
    if (out.sap_mode == 3) {
        // 4.2.10.2 sap_data()
        out.sap_coeff_all = r.read_flag("sap_coeff_all");
        if (!out.sap_coeff_all) {
            for (int g = 0; g < psy.num_window_groups; ++g) {
                const int max_sfb_g = get_max_sfb(ctx, psy, g, false);
                for (int sfb = 0; sfb < max_sfb_g && sfb < kMaxSfb; sfb += 2) {
                    out.sap_coeff_used[at(g)][at(sfb)] = r.read_flag("sap_coeff_used");
                    if (sfb + 1 < max_sfb_g && sfb + 1 < kMaxSfb) {
                        out.sap_coeff_used[at(g)][at(sfb + 1)] = out.sap_coeff_used[at(g)][at(sfb)];
                    }
                }
            }
        } else {
            for (int g = 0; g < psy.num_window_groups; ++g) {
                const int max_sfb_g = get_max_sfb(ctx, psy, g, false);
                for (int sfb = 0; sfb < max_sfb_g && sfb < kMaxSfb; ++sfb) {
                    out.sap_coeff_used[at(g)][at(sfb)] = true;
                }
            }
        }
        if (psy.num_window_groups != 1) {
            out.delta_code_time = r.read_flag("delta_code_time");
        }
        for (int g = 0; g < psy.num_window_groups; ++g) {
            const int max_sfb_g = get_max_sfb(ctx, psy, g, false);
            for (int sfb = 0; sfb < max_sfb_g && sfb < kMaxSfb; sfb += 2) {
                if (out.sap_coeff_used[at(g)][at(sfb)]) {
                    const int index = huff_decode(r, tables::kAsfHcbScalefac, "sap_hcw");
                    if (index < 0) {
                        return fail(DecodeError::kInvalidStream, "no SAP codeword matches");
                    }
                    out.dpcm_alpha_q[at(g)][at(sfb)] = static_cast<std::int16_t>(index);
                }
            }
            if (auto ok = check(r); !ok) {
                return ok;
            }
        }
    }
    return check(r);
}

}  // namespace ac4::detail
