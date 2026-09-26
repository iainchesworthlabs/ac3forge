#include "syntax/ajcc.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "tables/huffman_tables.hpp"

namespace ac4::detail {

namespace {

// Part 2 Table 83: ajcc_num_bands_table by ajcc_num_param_bands_id.
constexpr std::array<std::uint8_t, 4> kNumParamBands = {15, 12, 9, 7};

// Pseudocode 29's codebooks, each as its F0, DF and DT tables, by data type.
using CodebookSet = std::array<const Codebook*, 3>;

constexpr std::array<CodebookSet, 4> kFineCodebooks = {{
    {{&tables::kAcplHcbAlphaFineF0, &tables::kAcplHcbAlphaFineDf, &tables::kAcplHcbAlphaFineDt}},
    {{&tables::kAcplHcbBetaFineF0, &tables::kAcplHcbBetaFineDf, &tables::kAcplHcbBetaFineDt}},
    {{&tables::kAjccHcbDryFineF0, &tables::kAjccHcbDryFineDf, &tables::kAjccHcbDryFineDt}},
    {{&tables::kAjccHcbWetFineF0, &tables::kAjccHcbWetFineDf, &tables::kAjccHcbWetFineDt}},
}};

constexpr std::array<CodebookSet, 4> kCoarseCodebooks = {{
    {{&tables::kAcplHcbAlphaCoarseF0, &tables::kAcplHcbAlphaCoarseDf, &tables::kAcplHcbAlphaCoarseDt}},
    {{&tables::kAcplHcbBetaCoarseF0, &tables::kAcplHcbBetaCoarseDf, &tables::kAcplHcbBetaCoarseDt}},
    {{&tables::kAjccHcbDryCoarseF0, &tables::kAjccHcbDryCoarseDf, &tables::kAjccHcbDryCoarseDt}},
    {{&tables::kAjccHcbWetCoarseF0, &tables::kAjccHcbWetCoarseDf, &tables::kAjccHcbWetCoarseDt}},
}};

[[nodiscard]] std::uint8_t u8(std::uint32_t value) noexcept {
    return static_cast<std::uint8_t>(value);
}

// One ajcc_hcw. As for A-CPL's codewords, a miss with fewer bits left than
// the codebook's longest codeword is the substream's end, and any other miss
// a table fault, since the codebooks are complete.
[[nodiscard]] ParseResult read_hcw(BitReader& r, const Codebook& codebook, std::uint16_t& out) {
    const int index = huff_decode(r, codebook, "ajcc_hcw");
    if (index < 0) {
        if (r.remaining_bits() < static_cast<std::size_t>(codebook.max_bits)) {
            return fail(DecodeError::kTruncated, "an ajcc_hcw runs past the end of the substream");
        }
        return fail(DecodeError::kInvalidStream, "no A-JCC Huffman codeword matches");
    }
    out = static_cast<std::uint16_t>(index);
    return {};
}

// ajcc_framing_data(), 6.2.6.2.
[[nodiscard]] ParseResult parse_framing(BitReader& r, AjccFraming& out) {
    out.interpolation_type = u8(r.read(1, "ajcc_interpolation_type"));
    out.num_param_sets_code = u8(r.read(1, "ajcc_num_param_sets_code"));
    out.num_param_sets = static_cast<std::uint8_t>(out.num_param_sets_code + 1);
    if (out.interpolation_type == 1) {
        for (std::size_t ps = 0; ps < out.num_param_sets; ++ps) {
            out.param_timeslot[ps] = u8(r.read(5, "ajcc_param_timeslot"));
        }
    }
    return check(r);
}

// ajced(data_type, data_bands, quant_mode, b_no_dt, num_ps) with
// ajcc_huff_data(), 6.2.6.3 and 6.2.6.4.
[[nodiscard]] ParseResult parse_ajced(BitReader& r, AjccDataType data_type, std::size_t data_bands,
                                      std::uint8_t quant_mode, bool b_no_dt, std::uint8_t num_ps,
                                      AjccParams& out) {
    out.data_type = data_type;
    out.quant_mode = quant_mode;
    out.num_param_sets = num_ps;
    for (std::size_t ps = 0; ps < num_ps; ++ps) {
        AjccParamSet& set = out.sets[ps];
        set.diff_type = b_no_dt ? std::uint8_t{0} : u8(r.read(1, "diff_type"));
        std::size_t first = 0;
        if (set.diff_type == 0) {  // DIFF_FREQ
            if (auto result = read_hcw(r, ajcc_codebook(data_type, quant_mode, AjccHcbType::kF0), set.huff_index[0]);
                !result) {
                return result;
            }
            first = 1;
        }
        const AjccHcbType rest_type = set.diff_type == 0 ? AjccHcbType::kDf : AjccHcbType::kDt;
        const Codebook& rest = ajcc_codebook(data_type, quant_mode, rest_type);
        for (std::size_t band = first; band < data_bands; ++band) {
            if (auto result = read_hcw(r, rest, set.huff_index[band]); !result) {
                return result;
            }
        }
        if (auto result = check(r); !result) {
            return result;
        }
    }
    return {};
}

}  // namespace

ParseResult parse_ajcc_data(BitReader& r, AjccData& out) {
    out = AjccData{};
    out.b_no_dt = r.read_flag("b_no_dt");
    out.num_param_bands_id = u8(r.read(2, "ajcc_num_param_bands_id"));
    out.num_bands = kNumParamBands[out.num_param_bands_id];
    // b_5fronts is 0: ajcc_core_mode, ajcc_qm_ab and ajcc_qm_dw, then the
    // framing of the left and right modules.
    out.core_mode = u8(r.read(1, "ajcc_core_mode"));
    out.qm_ab = u8(r.read(1, "ajcc_qm_ab"));
    out.qm_dw = u8(r.read(1, "ajcc_qm_dw"));
    for (AjccFraming& framing : out.framing) {
        if (auto result = parse_framing(r, framing); !result) {
            return result;
        }
    }
    const std::uint8_t nps_l = out.framing[0].num_param_sets;
    const std::uint8_t nps_r = out.framing[1].num_param_sets;
    const std::size_t bands = out.num_bands;
    // 6.2.6.1's order: alpha1 (L), alpha2 (R), beta1 (L), beta2 (R) at
    // ajcc_qm_ab; dry1 and dry2 (L), dry3 and dry4 (R), wet1 to wet3 (L) and
    // wet4 to wet6 (R) at ajcc_qm_dw.
    struct Step {
        AjccParams* params;
        AjccDataType type;
        std::uint8_t quant;
        std::uint8_t num_ps;
    };
    const std::array<Step, 14> steps = {{
        {&out.alpha[0], AjccDataType::kAlpha, out.qm_ab, nps_l},
        {&out.alpha[1], AjccDataType::kAlpha, out.qm_ab, nps_r},
        {&out.beta[0], AjccDataType::kBeta, out.qm_ab, nps_l},
        {&out.beta[1], AjccDataType::kBeta, out.qm_ab, nps_r},
        {&out.dry[0], AjccDataType::kDry, out.qm_dw, nps_l},
        {&out.dry[1], AjccDataType::kDry, out.qm_dw, nps_l},
        {&out.dry[2], AjccDataType::kDry, out.qm_dw, nps_r},
        {&out.dry[3], AjccDataType::kDry, out.qm_dw, nps_r},
        {&out.wet[0], AjccDataType::kWet, out.qm_dw, nps_l},
        {&out.wet[1], AjccDataType::kWet, out.qm_dw, nps_l},
        {&out.wet[2], AjccDataType::kWet, out.qm_dw, nps_l},
        {&out.wet[3], AjccDataType::kWet, out.qm_dw, nps_r},
        {&out.wet[4], AjccDataType::kWet, out.qm_dw, nps_r},
        {&out.wet[5], AjccDataType::kWet, out.qm_dw, nps_r},
    }};
    for (const Step& step : steps) {
        if (auto result = parse_ajced(r, step.type, bands, step.quant, out.b_no_dt, step.num_ps, *step.params);
            !result) {
            return result;
        }
    }
    return check(r);
}

const Codebook& ajcc_codebook(AjccDataType data_type, int quant_mode, AjccHcbType hcb_type) noexcept {
    const std::array<CodebookSet, 4>& sets = quant_mode == 0 ? kFineCodebooks : kCoarseCodebooks;
    return *sets[static_cast<std::size_t>(data_type)][static_cast<std::size_t>(hcb_type)];
}

}  // namespace ac4::detail
