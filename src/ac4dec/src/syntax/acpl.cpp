#include "syntax/acpl.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "acpl/acpl.hpp"
#include "tables/huffman_tables.hpp"

namespace ac4::detail {

namespace {

// Table 143: acpl_num_param_bands by acpl_num_param_bands_id.
constexpr std::array<std::uint8_t, 4> kNumParamBands = {15, 12, 9, 7};

// Pseudocode 8's codebooks, each as its F0, DF and DT tables, by data type.
using CodebookSet = std::array<const Codebook*, 3>;

constexpr std::array<CodebookSet, 4> kFineCodebooks = {{
    {{&tables::kAcplHcbAlphaFineF0, &tables::kAcplHcbAlphaFineDf, &tables::kAcplHcbAlphaFineDt}},
    {{&tables::kAcplHcbBetaFineF0, &tables::kAcplHcbBetaFineDf, &tables::kAcplHcbBetaFineDt}},
    {{&tables::kAcplHcbBeta3FineF0, &tables::kAcplHcbBeta3FineDf, &tables::kAcplHcbBeta3FineDt}},
    {{&tables::kAcplHcbGammaFineF0, &tables::kAcplHcbGammaFineDf, &tables::kAcplHcbGammaFineDt}},
}};

constexpr std::array<CodebookSet, 4> kCoarseCodebooks = {{
    {{&tables::kAcplHcbAlphaCoarseF0, &tables::kAcplHcbAlphaCoarseDf,
      &tables::kAcplHcbAlphaCoarseDt}},
    {{&tables::kAcplHcbBetaCoarseF0, &tables::kAcplHcbBetaCoarseDf,
      &tables::kAcplHcbBetaCoarseDt}},
    {{&tables::kAcplHcbBeta3CoarseF0, &tables::kAcplHcbBeta3CoarseDf,
      &tables::kAcplHcbBeta3CoarseDt}},
    {{&tables::kAcplHcbGammaCoarseF0, &tables::kAcplHcbGammaCoarseDf,
      &tables::kAcplHcbGammaCoarseDt}},
}};

[[nodiscard]] std::uint8_t u8(std::uint32_t value) noexcept {
    return static_cast<std::uint8_t>(value);
}

// One acpl_hcw. huff_decode() matches nothing only when fewer bits remain
// than the codebook's longest codeword, since Annex A's codebooks are
// complete; anything else is a table fault, reported as an invalid stream.
[[nodiscard]] ParseResult read_hcw(BitReader& r, const Codebook& codebook, std::uint16_t& out) {
    const int index = huff_decode(r, codebook, "acpl_hcw");
    if (index < 0) {
        if (r.remaining_bits() < static_cast<std::size_t>(codebook.max_bits)) {
            return fail(DecodeError::kTruncated, "an acpl_hcw runs past the end of the substream");
        }
        return fail(DecodeError::kInvalidStream, "no A-CPL Huffman codeword matches");
    }
    out = static_cast<std::uint16_t>(index);
    return {};
}

// acpl_framing_data(), Table 63.
[[nodiscard]] ParseResult parse_acpl_framing_data(BitReader& r, AcplFraming& out) {
    out.interpolation_type = u8(r.read(1, "acpl_interpolation_type"));
    out.num_param_sets_cod = u8(r.read(1, "acpl_num_param_sets_cod"));
    out.num_param_sets = static_cast<std::uint8_t>(out.num_param_sets_cod + 1);
    if (out.interpolation_type == 1) {
        const std::size_t num_param_sets = out.num_param_sets;
        for (std::size_t ps = 0; ps < num_param_sets; ++ps) {
            out.param_timeslot[ps] = u8(r.read(5, "acpl_param_timeslot"));
        }
    }
    return check(r);
}

// acpl_ec_data(data_type, data_bands, start_band, quant_mode) with
// acpl_huff_data(), Tables 64 and 65.
[[nodiscard]] ParseResult parse_acpl_ec_data(BitReader& r, const AcplFraming& framing,
                                             AcplDataType data_type, std::size_t data_bands,
                                             std::size_t start_band, std::uint8_t quant_mode,
                                             AcplParams& out) {
    out.data_type = data_type;
    out.quant_mode = quant_mode;
    const std::size_t num_param_sets = framing.num_param_sets;
    for (std::size_t ps = 0; ps < num_param_sets; ++ps) {
        AcplParamSet& set = out.sets[ps];
        set.diff_type = u8(r.read(1, "diff_type"));
        std::size_t first = start_band;
        if (set.diff_type == 0) {  // DIFF_FREQ
            const Codebook& f0 = acpl_codebook(data_type, quant_mode, AcplHcbType::kF0);
            if (auto result = read_hcw(r, f0, set.huff_index[start_band]); !result) {
                return result;
            }
            first = start_band + 1;
        }
        const AcplHcbType rest_type = set.diff_type == 0 ? AcplHcbType::kDf : AcplHcbType::kDt;
        const Codebook& rest = acpl_codebook(data_type, quant_mode, rest_type);
        for (std::size_t i = first; i < data_bands; ++i) {
            if (auto result = read_hcw(r, rest, set.huff_index[i]); !result) {
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

ParseResult parse_acpl_config_1ch(BitReader& r, AcplConfigKind kind, AcplConfig1ch& out) {
    AcplConfig1ch config;
    config.kind = kind;
    config.num_param_bands_id = u8(r.read(2, "acpl_num_param_bands_id"));
    config.num_param_bands = kNumParamBands[config.num_param_bands_id];
    config.quant_mode = u8(r.read(1, "acpl_quant_mode"));
    if (kind == AcplConfigKind::kPartial) {
        config.qmf_band = u8(r.read(3, "acpl_qmf_band_minus1") + 1U);
        // acpl_qmf_band is 1 to 8, which Table 197 maps for every band count.
        config.param_band = static_cast<std::uint8_t>(
            acpl::sb_to_pb(config.num_param_bands, config.qmf_band));
    }
    config.valid = !r.overflow();
    out = config;
    return check(r);
}

ParseResult parse_acpl_config_2ch(BitReader& r, AcplConfig2ch& out) {
    AcplConfig2ch config;
    config.num_param_bands_id = u8(r.read(2, "acpl_num_param_bands_id"));
    config.num_param_bands = kNumParamBands[config.num_param_bands_id];
    config.quant_mode_0 = u8(r.read(1, "acpl_quant_mode_0"));
    config.quant_mode_1 = u8(r.read(1, "acpl_quant_mode_1"));
    config.valid = !r.overflow();
    out = config;
    return check(r);
}

ParseResult parse_acpl_data_1ch(BitReader& r, const SubstreamContext& /*ctx*/,
                                const AcplConfig1ch& config, AcplData1ch& out) {
    out = AcplData1ch{};
    if (!config.valid) {
        return fail(DecodeError::kMissingIFrame,
                    "acpl_data_1ch() without an I-frame's acpl_config_1ch()");
    }
    if (auto result = parse_acpl_framing_data(r, out.framing); !result) {
        return result;
    }
    out.num_bands = config.num_param_bands;
    out.start_band = config.param_band;
    out.qmf_band = config.qmf_band;
    const std::size_t bands = out.num_bands;
    const std::size_t start = out.start_band;
    if (auto result = parse_acpl_ec_data(r, out.framing, AcplDataType::kAlpha, bands, start,
                                         config.quant_mode, out.alpha1);
        !result) {
        return result;
    }
    if (auto result = parse_acpl_ec_data(r, out.framing, AcplDataType::kBeta, bands, start,
                                         config.quant_mode, out.beta1);
        !result) {
        return result;
    }
    return check(r);
}

ParseResult parse_acpl_data_2ch(BitReader& r, const SubstreamContext& /*ctx*/,
                                const AcplConfig2ch& config, AcplData2ch& out) {
    out = AcplData2ch{};
    if (!config.valid) {
        return fail(DecodeError::kMissingIFrame,
                    "acpl_data_2ch() without an I-frame's acpl_config_2ch()");
    }
    if (auto result = parse_acpl_framing_data(r, out.framing); !result) {
        return result;
    }
    out.num_bands = config.num_param_bands;
    out.start_band = 0;
    const std::size_t bands = out.num_bands;
    // Table 62's order: alpha1, alpha2, beta1, beta2, beta3 at acpl_quant_mode_0,
    // then gamma1 to gamma6 at acpl_quant_mode_1.
    for (AcplParams& alpha : out.alpha) {
        if (auto result = parse_acpl_ec_data(r, out.framing, AcplDataType::kAlpha, bands, 0,
                                             config.quant_mode_0, alpha);
            !result) {
            return result;
        }
    }
    for (AcplParams& beta : out.beta) {
        if (auto result = parse_acpl_ec_data(r, out.framing, AcplDataType::kBeta, bands, 0,
                                             config.quant_mode_0, beta);
            !result) {
            return result;
        }
    }
    if (auto result = parse_acpl_ec_data(r, out.framing, AcplDataType::kBeta3, bands, 0,
                                         config.quant_mode_0, out.beta3);
        !result) {
        return result;
    }
    for (AcplParams& gamma : out.gamma) {
        if (auto result = parse_acpl_ec_data(r, out.framing, AcplDataType::kGamma, bands, 0,
                                             config.quant_mode_1, gamma);
            !result) {
            return result;
        }
    }
    return check(r);
}

const Codebook& acpl_codebook(AcplDataType data_type, int quant_mode,
                              AcplHcbType hcb_type) noexcept {
    const std::array<CodebookSet, 4>& sets = quant_mode == 0 ? kFineCodebooks : kCoarseCodebooks;
    return *sets[static_cast<std::size_t>(data_type)][static_cast<std::size_t>(hcb_type)];
}

}  // namespace ac4::detail
