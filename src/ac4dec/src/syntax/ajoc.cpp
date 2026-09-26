#include "syntax/ajoc.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "huffman.hpp"
#include "tables/huffman_tables.hpp"

namespace ac4::detail {

namespace {

// Table 78.
constexpr std::array<int, 8> kNumBands = {23, 15, 12, 9, 7, 5, 3, 1};

using CodebookSet = std::array<const Codebook*, 3>;  // F0, DF, DT

// Pseudocode 27's codebooks by data type, fine then coarse (Table 79).
constexpr std::array<CodebookSet, 2> kFine = {{
    {{&tables::kAjocHcbDryFineF0, &tables::kAjocHcbDryFineDf, &tables::kAjocHcbDryFineDt}},
    {{&tables::kAjocHcbWetFineF0, &tables::kAjocHcbWetFineDf, &tables::kAjocHcbWetFineDt}},
}};
constexpr std::array<CodebookSet, 2> kCoarse = {{
    {{&tables::kAjocHcbDryCoarseF0, &tables::kAjocHcbDryCoarseDf, &tables::kAjocHcbDryCoarseDt}},
    {{&tables::kAjocHcbWetCoarseF0, &tables::kAjocHcbWetCoarseDf, &tables::kAjocHcbWetCoarseDt}},
}};

[[nodiscard]] int read_int(BitReader& r, int bits, std::string_view name) {
    return static_cast<int>(r.read(bits, name));
}

// One ajoc_hcw. As for A-CPL's and A-JCC's, a miss with fewer bits left than
// the codebook's longest codeword is the substream's end, and any other miss a
// table fault, since the codebooks are complete.
[[nodiscard]] ParseResult read_hcw(BitReader& r, const Codebook& codebook, std::uint16_t& out) {
    const int index = huff_decode(r, codebook, "ajoc_hcw");
    if (index < 0) {
        if (r.remaining_bits() < static_cast<std::size_t>(codebook.max_bits)) {
            return fail(DecodeError::kTruncated, "an ajoc_hcw runs past the end of the substream");
        }
        return fail(DecodeError::kInvalidStream, "no A-JOC Huffman codeword matches");
    }
    out = static_cast<std::uint16_t>(index);
    return {};
}

// ajoc_huff_data(data_type, data_bands, quant_select, b_dfonly), 6.2.5.5.
[[nodiscard]] ParseResult parse_huff_data(BitReader& r, AjocDataType data_type, int data_bands,
                                          int quant_select, bool b_dfonly, AjocHuffData& out) {
    out.diff_type = b_dfonly ? std::uint8_t{0} : static_cast<std::uint8_t>(r.read(1, "diff_type"));
    int first = 0;
    if (out.diff_type == 0) {
        if (auto ok =
                read_hcw(r, ajoc_codebook(data_type, quant_select, AjocHcbType::kF0), out.index[0]);
            !ok) {
            return ok;
        }
        first = 1;
    }
    const Codebook& rest = ajoc_codebook(data_type, quant_select,
                                         out.diff_type == 0 ? AjocHcbType::kDf : AjocHcbType::kDt);
    for (int i = first; i < data_bands; ++i) {
        if (auto ok = read_hcw(r, rest, out.index[static_cast<std::size_t>(i)]); !ok) {
            return ok;
        }
    }
    return check(r);
}

// Up to 64 bits read as one field, one record valued at them.
[[nodiscard]] std::uint64_t read_field(BitReader& r, int bits, std::string_view name) {
    const std::size_t start = r.position();
    std::uint64_t value = 0;
    for (int i = 0; i < bits; ++i) {
        value = (value << 1U) | r.peek_raw(1);
        r.consume(1);
    }
    r.emit(start, bits, value, name);
    return value;
}

// de_dlg_dmx_coeff_idx (Table 82): 0b0 for 0, 0b1111 for 1, and 0b1xxxx for
// (xxxx + 1)/15 from 0b10000 to 0b11101. One record of the bits read, valued
// at them. Returns the coefficient in fifteenths.
[[nodiscard]] std::uint8_t read_dlg_dmx_coeff(BitReader& r) {
    const std::size_t start = r.position();
    if (r.peek_raw(1) == 0) {
        r.consume(1);
        r.emit(start, 1, 0, "de_dlg_dmx_coeff_idx");
        return 0;
    }
    if (r.peek_raw(4) == 0b1111) {
        r.consume(4);
        r.emit(start, 4, 0b1111, "de_dlg_dmx_coeff_idx");
        return 15;
    }
    const std::uint32_t code = r.peek_raw(5);
    r.consume(5);
    r.emit(start, 5, code, "de_dlg_dmx_coeff_idx");
    return static_cast<std::uint8_t>((code & 0b1111U) + 1U);
}

}  // namespace

const Codebook& ajoc_codebook(AjocDataType data_type, int quant_select,
                              AjocHcbType hcb_type) noexcept {
    const std::array<CodebookSet, 2>& sets = quant_select == 0 ? kFine : kCoarse;
    return *sets[static_cast<std::size_t>(data_type)][static_cast<std::size_t>(hcb_type)];
}

int ajoc_num_bands(int code) noexcept {
    return code >= 0 && code < 8 ? kNumBands[static_cast<std::size_t>(code)] : 0;
}

ParseResult parse_ajoc(BitReader& r, int num_dmx_signals, int num_umx_signals, AjocData& out) {
    out = AjocData{};
    if (num_dmx_signals < 1 || num_dmx_signals > kMaxAjocDmxSignals) {
        return fail(DecodeError::kInvalidStream, "an A-JOC downmix of no signals");
    }
    out.num_dmx = num_dmx_signals;
    out.num_umx = num_umx_signals;
    out.num_decorr = read_int(r, 3, "ajoc_num_decorr");

    // 6.2.5.2 ajoc_ctrl_info()
    for (int d = 0; d < out.num_decorr; ++d) {
        out.decorr_enable[static_cast<std::size_t>(d)] = r.read_flag("ajoc_decorr_enable");
    }
    out.objects.assign(static_cast<std::size_t>(num_umx_signals), AjocObjectConfig{});
    for (AjocObjectConfig& object : out.objects) {
        object.present = r.read_flag("ajoc_object_present");
    }
    // 6.2.5.4 ajoc_data_point_info(); 5.7.3.4 allows 0, 1 or 2 data points
    // (src/ac4dec/ERRATA.md, "ajoc_num_dpoints of 3").
    out.num_dpoints = read_int(r, 2, "ajoc_num_dpoints");
    if (out.num_dpoints > kMaxAjocDataPoints) {
        return fail(DecodeError::kInvalidStream,
                    "ajoc_num_dpoints of 3, where 5.7.3.4 allows 0 to 2");
    }
    for (int dp = 0; dp < out.num_dpoints; ++dp) {
        out.start_pos[static_cast<std::size_t>(dp)] = read_int(r, 5, "ajoc_start_pos");
        out.ramp_len[static_cast<std::size_t>(dp)] = read_int(r, 6, "ajoc_ramp_len_minus1") + 1;
    }
    if (out.num_dpoints != 0) {
        for (AjocObjectConfig& object : out.objects) {
            if (!object.present) {
                continue;
            }
            object.num_bands_code = read_int(r, 3, "ajoc_num_bands_code");
            object.num_bands = ajoc_num_bands(object.num_bands_code);
            object.quant_select = read_int(r, 1, "ajoc_quant_select");
            object.sparse_select = read_int(r, 1, "ajoc_sparse_select");
            if (object.sparse_select == 1) {
                for (int ch = 0; ch < num_dmx_signals; ++ch) {
                    object.dry_present[static_cast<std::size_t>(ch)] =
                        r.read_flag("ajoc_mix_mtx_dry_present");
                }
                for (int d = 0; d < out.num_decorr; ++d) {
                    object.wet_present[static_cast<std::size_t>(d)] =
                        out.decorr_enable[static_cast<std::size_t>(d)] &&
                        r.read_flag("ajoc_mix_mtx_wet_present");
                }
            }
            if (auto ok = check(r); !ok) {
                return ok;
            }
        }
    }
    if (auto ok = check(r); !ok) {
        return ok;
    }

    // 6.2.5.3 ajoc_data()
    out.b_nodt = r.read_flag("ajoc_b_nodt");
    out.dry.assign(static_cast<std::size_t>(num_umx_signals * kMaxAjocDataPoints * num_dmx_signals),
                   AjocData::Entry{});
    out.wet.assign(static_cast<std::size_t>(num_umx_signals * kMaxAjocDataPoints * kMaxAjocDecorr),
                   AjocData::Entry{});
    for (int o = 0; o < num_umx_signals; ++o) {
        const AjocObjectConfig& object = out.objects[static_cast<std::size_t>(o)];
        if (!object.present) {
            continue;
        }
        for (int dp = 0; dp < out.num_dpoints; ++dp) {
            const bool b_dfonly = dp == 0 && out.b_nodt;
            const bool sparse = object.sparse_select == 1;
            for (int ch = 0; ch < num_dmx_signals; ++ch) {
                if (sparse && !object.dry_present[static_cast<std::size_t>(ch)]) {
                    continue;
                }
                AjocData::Entry& entry = out.dry[static_cast<std::size_t>(
                    (o * kMaxAjocDataPoints + dp) * num_dmx_signals + ch)];
                entry.sent = true;
                if (auto ok = parse_huff_data(r, AjocDataType::kDry, object.num_bands,
                                              object.quant_select, b_dfonly, entry.data);
                    !ok) {
                    return ok;
                }
            }
            for (int de = 0; de < out.num_decorr; ++de) {
                if (sparse && !object.wet_present[static_cast<std::size_t>(de)]) {
                    continue;
                }
                AjocData::Entry& entry = out.wet[static_cast<std::size_t>(
                    (o * kMaxAjocDataPoints + dp) * kMaxAjocDecorr + de)];
                entry.sent = true;
                if (auto ok = parse_huff_data(r, AjocDataType::kWet, object.num_bands,
                                              object.quant_select, b_dfonly, entry.data);
                    !ok) {
                    return ok;
                }
            }
        }
    }
    return check(r);
}

ParseResult parse_ajoc_dmx_de_data(BitReader& r, int num_dmx_signals, int num_umx_signals,
                                   bool b_iframe, AjocDmxDeState& state, AjocDmxDeData& out) {
    out = AjocDmxDeData{};
    out.b_dmx_de_cfg = r.read_flag("b_dmx_de_cfg");
    out.b_keep_dmx_de_coeffs = r.read_flag("b_keep_dmx_de_coeffs");
    if (out.b_dmx_de_cfg) {
        AjocDmxDeConfig config;
        config.de_max_gain = read_int(r, 2, "de_max_gain");
        // de_main_dlg_flag[] is one field, [0] its first bit, a flag per
        // upmix object in their order (src/ac4dec/ERRATA.md, "Arrays read as
        // one field").
        const std::uint64_t flags = read_field(r, num_umx_signals, "de_main_dlg_flag");
        config.de_main_dlg_flag.assign(static_cast<std::size_t>(num_umx_signals), 0);
        for (int obj = 0; obj < num_umx_signals; ++obj) {
            const std::uint64_t bit = std::uint64_t{1}
                                      << static_cast<unsigned>(num_umx_signals - 1 - obj);
            if ((flags & bit) != 0) {
                config.de_main_dlg_flag[static_cast<std::size_t>(obj)] = 1;
                ++config.num_dlg_obj;
            }
        }
        state.config = config;
    } else if (b_iframe) {
        // An I-frame depends on nothing before it: without a configuration
        // there is none (Part 1 clause 4.3.14.3.2 sets Gmax to 0 dB alike).
        state.config.reset();
    }
    if (auto ok = check(r); !ok) {
        return ok;
    }
    if (out.b_keep_dmx_de_coeffs == false) {
        if (!state.config) {
            if (!b_iframe) {
                return fail(
                    DecodeError::kMissingIFrame,
                    "de_dlg_dmx_coeff_idx needs a dialogue configuration no I-frame has sent");
            }
        } else {
            const int count = state.config->num_dlg_obj * num_dmx_signals;
            state.coeff.assign(static_cast<std::size_t>(count), 0);
            for (int i = 0; i < count; ++i) {
                state.coeff[static_cast<std::size_t>(i)] = read_dlg_dmx_coeff(r);
            }
            state.coeff_valid = true;
            out.coeffs_read = true;
        }
    } else if (b_iframe || out.b_dmx_de_cfg) {
        // 6.3.6.6.2: the flag "shall be ignored" here, so nothing is kept from
        // before; the syntax reads no coefficients, so there are none.
        state.coeff_valid = false;
        state.coeff.clear();
    }
    if (auto ok = check(r); !ok) {
        return ok;
    }
    out.config = state.config;
    if (state.coeff_valid && state.config &&
        state.coeff.size() ==
            static_cast<std::size_t>(state.config->num_dlg_obj * num_dmx_signals)) {
        out.coeff = state.coeff;
    }
    return {};
}

ParseResult parse_ajoc_bed_info(BitReader& r, AjocBedInfo& out) {
    out = AjocBedInfo{};
    out.b_obj_without_bed_info_present = r.read_flag("b_obj_without_bed_info_present");
    if (out.b_obj_without_bed_info_present) {
        out.num_obj_with_bed_render_info = read_int(r, 3, "num_obj_with_bed_render_info");
    }
    return check(r);
}

}  // namespace ac4::detail
