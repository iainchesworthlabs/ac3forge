#include "ajoc/ajoc_syntax.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "tables/huffman_codes.hpp"
#include "tables/huffman_tables.hpp"

namespace ac4::detail {
namespace {

enum class HcbType : std::uint8_t { kF0, kDf, kDt };

// get_ajoc_hcb(), Pseudocode 27 and Table 81: the codebook of Part 2 Annex
// A.1.1 for dry or wet data at a quantisation, its codewords for writing and
// its cb_off.
struct CodebookRef {
    std::span<const HuffCode> codes;
    int cb_off = 0;
};

[[nodiscard]] CodebookRef codebook(bool wet, int quant_select, HcbType type) noexcept {
    using namespace tables;
    const bool coarse = quant_select == 1;
    const auto ref = [](std::span<const HuffCode> codes, const Codebook& cb) {
        return CodebookRef{codes, cb.cb_off};
    };
    if (!wet) {
        switch (type) {
            case HcbType::kF0:
                return coarse ? ref(kAjocHcbDryCoarseF0Codes, kAjocHcbDryCoarseF0)
                              : ref(kAjocHcbDryFineF0Codes, kAjocHcbDryFineF0);
            case HcbType::kDf:
                return coarse ? ref(kAjocHcbDryCoarseDfCodes, kAjocHcbDryCoarseDf)
                              : ref(kAjocHcbDryFineDfCodes, kAjocHcbDryFineDf);
            case HcbType::kDt:
                return coarse ? ref(kAjocHcbDryCoarseDtCodes, kAjocHcbDryCoarseDt)
                              : ref(kAjocHcbDryFineDtCodes, kAjocHcbDryFineDt);
        }
    }
    switch (type) {
        case HcbType::kF0:
            return coarse ? ref(kAjocHcbWetCoarseF0Codes, kAjocHcbWetCoarseF0)
                          : ref(kAjocHcbWetFineF0Codes, kAjocHcbWetFineF0);
        case HcbType::kDf:
            return coarse ? ref(kAjocHcbWetCoarseDfCodes, kAjocHcbWetCoarseDf)
                          : ref(kAjocHcbWetFineDfCodes, kAjocHcbWetFineDf);
        case HcbType::kDt:
            break;
    }
    return coarse ? ref(kAjocHcbWetCoarseDtCodes, kAjocHcbWetCoarseDt)
                  : ref(kAjocHcbWetFineDtCodes, kAjocHcbWetFineDt);
}

[[nodiscard]] HcbType type_of(int diff_type, bool first) noexcept {
    if (diff_type != 0) {
        return HcbType::kDt;
    }
    return first ? HcbType::kF0 : HcbType::kDf;
}

// ajoc_huff_data(data_type, data_bands, quant_select, b_dfonly), 6.2.5.5: the
// F0 codeword is huff_decode()'s, its index the value; the others
// huff_decode_diff()'s, the value plus cb_off.
void write_huff_data(BitWriter& w, bool wet, int bands, int quant_select, bool dfonly,
                     const AjocSetFields& set) {
    const int diff_type = dfonly ? 0 : set.diff_type;
    if (!dfonly) {
        w.write(1, static_cast<std::uint64_t>(diff_type), "diff_type");
    }
    for (int i = 0; i < bands; ++i) {
        const bool first = i == 0;
        const CodebookRef cb = codebook(wet, quant_select, type_of(diff_type, first));
        const int value = set.values.at(static_cast<std::size_t>(i));
        const int index = diff_type == 0 && first ? value : value + cb.cb_off;
        w.write_codeword(cb.codes, static_cast<std::size_t>(index), "ajoc_hcw");
    }
}

// Table 82: 0b0 for 0, 0b1111 for 15/15, and 0b10000 + (k - 1) for k/15 from
// 1/15 to 14/15; one record of the bits, valued at them.
void write_dlg_dmx_coeff(BitWriter& w, int fifteenths) {
    if (fifteenths <= 0) {
        w.write(1, 0b0, "de_dlg_dmx_coeff_idx");
    } else if (fifteenths >= 15) {
        w.write(4, 0b1111, "de_dlg_dmx_coeff_idx");
    } else {
        w.write(5, 0b10000U + static_cast<unsigned>(fifteenths - 1), "de_dlg_dmx_coeff_idx");
    }
}

}  // namespace

int ajoc_band_count(int num_bands_code) noexcept {
    constexpr std::array<int, 8> kBands = {23, 15, 12, 9, 7, 5, 3, 1};
    return kBands[static_cast<std::size_t>(num_bands_code & 7)];
}

bool ajoc_codable(bool wet, int quant_select, int diff_type, bool first, int value) noexcept {
    const CodebookRef cb = codebook(wet, quant_select, type_of(diff_type, first));
    const int index = diff_type == 0 && first ? value : value + cb.cb_off;
    return index >= 0 && static_cast<std::size_t>(index) < cb.codes.size();
}

void write_ajoc(BitWriter& w, int num_dmx_signals, const AjocFields& fields) {
    const auto num_decorr = static_cast<int>(fields.decorr_enable.size());
    w.write(3, static_cast<std::uint64_t>(num_decorr), "ajoc_num_decorr");
    // ajoc_ctrl_info(), 6.2.5.2.
    for (const int enable : fields.decorr_enable) {
        w.write(1, enable != 0 ? 1U : 0U, "ajoc_decorr_enable");
    }
    for (const AjocObjectFields& object : fields.objects) {
        w.write(1, object.present ? 1U : 0U, "ajoc_object_present");
    }
    // ajoc_data_point_info(), 6.2.5.4.
    w.write(2, static_cast<std::uint64_t>(fields.num_dpoints), "ajoc_num_dpoints");
    for (int dp = 0; dp < fields.num_dpoints; ++dp) {
        const auto d = static_cast<std::size_t>(dp);
        w.write(5, static_cast<std::uint64_t>(fields.start_pos[d]), "ajoc_start_pos");
        w.write(6, static_cast<std::uint64_t>(fields.ramp_len[d] - 1), "ajoc_ramp_len_minus1");
    }
    if (fields.num_dpoints != 0) {
        for (const AjocObjectFields& object : fields.objects) {
            if (!object.present) {
                continue;
            }
            w.write(3, static_cast<std::uint64_t>(object.num_bands_code), "ajoc_num_bands_code");
            w.write(1, static_cast<std::uint64_t>(object.quant_select), "ajoc_quant_select");
            w.write(1, object.sparse ? 1U : 0U, "ajoc_sparse_select");
            if (object.sparse) {
                for (int ch = 0; ch < num_dmx_signals; ++ch) {
                    w.write(1, object.dry_present.at(static_cast<std::size_t>(ch)) != 0 ? 1U : 0U,
                            "ajoc_mix_mtx_dry_present");
                }
                for (int d = 0; d < num_decorr; ++d) {
                    if (fields.decorr_enable[static_cast<std::size_t>(d)] != 0) {
                        w.write(1,
                                object.wet_present.at(static_cast<std::size_t>(d)) != 0 ? 1U : 0U,
                                "ajoc_mix_mtx_wet_present");
                    }
                }
            }
        }
    }
    // ajoc_data(), 6.2.5.3.
    w.write(1, fields.b_nodt ? 1U : 0U, "ajoc_b_nodt");
    for (const AjocObjectFields& object : fields.objects) {
        if (!object.present) {
            continue;
        }
        const int bands = ajoc_band_count(object.num_bands_code);
        for (int dp = 0; dp < fields.num_dpoints; ++dp) {
            const auto d = static_cast<std::size_t>(dp);
            const bool dfonly = dp == 0 && fields.b_nodt;
            for (int ch = 0; ch < num_dmx_signals; ++ch) {
                const auto c = static_cast<std::size_t>(ch);
                if (object.sparse && object.dry_present.at(c) == 0) {
                    continue;
                }
                write_huff_data(w, false, bands, object.quant_select, dfonly,
                                object.dry.at(d).at(c));
            }
            for (int de = 0; de < num_decorr; ++de) {
                const auto e = static_cast<std::size_t>(de);
                // A disabled decorrelator's wet_present is 0 under sparse;
                // without sparse every one is sent.
                if (object.sparse &&
                    (fields.decorr_enable[e] == 0 || object.wet_present.at(e) == 0)) {
                    continue;
                }
                write_huff_data(w, true, bands, object.quant_select, dfonly,
                                object.wet.at(d).at(e));
            }
        }
    }
}

void write_ajoc_dmx_de_data(BitWriter& w, int num_dmx_signals, const AjocDmxDeFields& fields,
                            int dialogue_objects) {
    w.write(1, fields.cfg ? 1U : 0U, "b_dmx_de_cfg");
    w.write(1, fields.keep_coeffs ? 1U : 0U, "b_keep_dmx_de_coeffs");
    int num_dlg_obj = dialogue_objects;
    if (fields.cfg) {
        w.write(2, static_cast<std::uint64_t>(fields.max_gain), "de_max_gain");
        // de_main_dlg_flag[] in one field, [0] its first bit (the decoder's
        // reading, src/ac4dec/ERRATA.md, "Arrays read as one field").
        std::uint64_t flags = 0;
        num_dlg_obj = 0;
        for (const int flag : fields.dialogue) {
            flags = (flags << 1U) | (flag != 0 ? 1U : 0U);
            num_dlg_obj += flag != 0 ? 1 : 0;
        }
        w.write(static_cast<unsigned>(fields.dialogue.size()), flags, "de_main_dlg_flag");
    }
    if (!fields.keep_coeffs) {
        const auto count = static_cast<std::size_t>(num_dlg_obj * num_dmx_signals);
        for (std::size_t i = 0; i < count; ++i) {
            write_dlg_dmx_coeff(w, fields.coeff.at(i));
        }
    }
}

void write_ajoc_bed_info(BitWriter& w, int num_obj_with_bed_render_info) {
    const bool present = num_obj_with_bed_render_info >= 0;
    w.write(1, present ? 1U : 0U, "b_obj_without_bed_info_present");
    if (present) {
        w.write(3, static_cast<std::uint64_t>(num_obj_with_bed_render_info),
                "num_obj_with_bed_render_info");
    }
}

}  // namespace ac4::detail
