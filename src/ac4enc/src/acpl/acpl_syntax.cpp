#include "acpl/acpl_syntax.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include "acpl/acpl.hpp"
#include "tables/huffman_codes.hpp"
#include "tables/huffman_tables.hpp"

namespace ac4::detail {
namespace {

enum class HcbType : std::uint8_t { kF0, kDf, kDt };

// get_acpl_hcb(), Pseudocode 8: the codebook's codewords, for writing, and
// its cb_off, from Annex A.
struct CodebookRef {
    std::span<const HuffCode> codes;
    int cb_off = 0;
};

[[nodiscard]] CodebookRef codebook(AcplKind kind, int quant_mode, HcbType type) {
    using namespace tables;
    struct Set {
        std::span<const HuffCode> f0, df, dt;
        const Codebook *f0_cb, *df_cb, *dt_cb;
    };
    const bool fine = quant_mode == 0;
    const auto pick = [&]() -> Set {
        switch (kind) {
            case AcplKind::kAlpha:
                return fine ? Set{kAcplHcbAlphaFineF0Codes, kAcplHcbAlphaFineDfCodes, kAcplHcbAlphaFineDtCodes,
                                  &kAcplHcbAlphaFineF0, &kAcplHcbAlphaFineDf, &kAcplHcbAlphaFineDt}
                            : Set{kAcplHcbAlphaCoarseF0Codes, kAcplHcbAlphaCoarseDfCodes, kAcplHcbAlphaCoarseDtCodes,
                                  &kAcplHcbAlphaCoarseF0, &kAcplHcbAlphaCoarseDf, &kAcplHcbAlphaCoarseDt};
            case AcplKind::kBeta:
                return fine ? Set{kAcplHcbBetaFineF0Codes, kAcplHcbBetaFineDfCodes, kAcplHcbBetaFineDtCodes,
                                  &kAcplHcbBetaFineF0, &kAcplHcbBetaFineDf, &kAcplHcbBetaFineDt}
                            : Set{kAcplHcbBetaCoarseF0Codes, kAcplHcbBetaCoarseDfCodes, kAcplHcbBetaCoarseDtCodes,
                                  &kAcplHcbBetaCoarseF0, &kAcplHcbBetaCoarseDf, &kAcplHcbBetaCoarseDt};
            case AcplKind::kBeta3:
                return fine ? Set{kAcplHcbBeta3FineF0Codes, kAcplHcbBeta3FineDfCodes, kAcplHcbBeta3FineDtCodes,
                                  &kAcplHcbBeta3FineF0, &kAcplHcbBeta3FineDf, &kAcplHcbBeta3FineDt}
                            : Set{kAcplHcbBeta3CoarseF0Codes, kAcplHcbBeta3CoarseDfCodes, kAcplHcbBeta3CoarseDtCodes,
                                  &kAcplHcbBeta3CoarseF0, &kAcplHcbBeta3CoarseDf, &kAcplHcbBeta3CoarseDt};
            case AcplKind::kGamma:
                break;
        }
        return fine ? Set{kAcplHcbGammaFineF0Codes, kAcplHcbGammaFineDfCodes, kAcplHcbGammaFineDtCodes,
                          &kAcplHcbGammaFineF0, &kAcplHcbGammaFineDf, &kAcplHcbGammaFineDt}
                    : Set{kAcplHcbGammaCoarseF0Codes, kAcplHcbGammaCoarseDfCodes, kAcplHcbGammaCoarseDtCodes,
                          &kAcplHcbGammaCoarseF0, &kAcplHcbGammaCoarseDf, &kAcplHcbGammaCoarseDt};
    };
    const Set set = pick();
    switch (type) {
        case HcbType::kF0:
            return {set.f0, set.f0_cb->cb_off};
        case HcbType::kDf:
            return {set.df, set.df_cb->cb_off};
        case HcbType::kDt:
            return {set.dt, set.dt_cb->cb_off};
    }
    return {};
}

[[nodiscard]] HcbType type_of(int diff_type, bool first_band) noexcept {
    if (diff_type != 0) {
        return HcbType::kDt;
    }
    return first_band ? HcbType::kF0 : HcbType::kDf;
}

// acpl_framing_data(), Table 63.
void write_framing(BitWriter& w, const AcplFramingFields& f) {
    w.write(1, static_cast<std::uint64_t>(f.interpolation_type), "acpl_interpolation_type");
    w.write(1, static_cast<std::uint64_t>(f.num_param_sets - 1), "acpl_num_param_sets_cod");
    if (f.interpolation_type == 1) {
        for (int ps = 0; ps < f.num_param_sets; ++ps) {
            w.write(5, static_cast<std::uint64_t>(f.param_timeslot[static_cast<std::size_t>(ps)]),
                    "acpl_param_timeslot");
        }
    }
}

// acpl_ec_data() with acpl_huff_data(), Tables 64 and 65.
void write_ec_data(BitWriter& w, AcplKind kind, int quant_mode, const AcplFramingFields& framing,
                   const AcplParamFields& param) {
    for (int ps = 0; ps < framing.num_param_sets; ++ps) {
        const AcplSetFields& set = param.at(static_cast<std::size_t>(ps));
        w.write(1, static_cast<std::uint64_t>(set.diff_type), "diff_type");
        for (std::size_t i = 0; i < set.values.size(); ++i) {
            const CodebookRef cb = codebook(kind, quant_mode, type_of(set.diff_type, i == 0));
            w.write_codeword(cb.codes, static_cast<std::size_t>(set.values[i] + cb.cb_off), "acpl_hcw");
        }
    }
}

}  // namespace

int acpl_num_param_bands(int num_param_bands_id) noexcept {
    constexpr std::array<int, 4> kBands = {15, 12, 9, 7};
    return kBands[static_cast<std::size_t>(num_param_bands_id & 3)];
}

int acpl_param_band(const AcplConfig1chFields& config) noexcept {
    return config.partial ? acpl::sb_to_pb(acpl_num_param_bands(config.num_param_bands_id), config.qmf_band) : 0;
}

void write_acpl_config_1ch(BitWriter& w, const AcplConfig1chFields& config) {
    w.write(2, static_cast<std::uint64_t>(config.num_param_bands_id), "acpl_num_param_bands_id");
    w.write(1, static_cast<std::uint64_t>(config.quant_mode), "acpl_quant_mode");
    if (config.partial) {
        w.write(3, static_cast<std::uint64_t>(config.qmf_band - 1), "acpl_qmf_band_minus1");
    }
}

void write_acpl_config_2ch(BitWriter& w, const AcplConfig2chFields& config) {
    w.write(2, static_cast<std::uint64_t>(config.num_param_bands_id), "acpl_num_param_bands_id");
    w.write(1, static_cast<std::uint64_t>(config.quant_mode_0), "acpl_quant_mode_0");
    w.write(1, static_cast<std::uint64_t>(config.quant_mode_1), "acpl_quant_mode_1");
}

void write_acpl_data_1ch(BitWriter& w, const AcplConfig1chFields& config, const AcplData1chFields& data) {
    write_framing(w, data.framing);
    write_ec_data(w, AcplKind::kAlpha, config.quant_mode, data.framing, data.alpha1);
    write_ec_data(w, AcplKind::kBeta, config.quant_mode, data.framing, data.beta1);
}

void write_acpl_data_2ch(BitWriter& w, const AcplConfig2chFields& config, const AcplData2chFields& data) {
    write_framing(w, data.framing);
    for (const AcplParamFields& alpha : data.alpha) {
        write_ec_data(w, AcplKind::kAlpha, config.quant_mode_0, data.framing, alpha);
    }
    for (const AcplParamFields& beta : data.beta) {
        write_ec_data(w, AcplKind::kBeta, config.quant_mode_0, data.framing, beta);
    }
    write_ec_data(w, AcplKind::kBeta3, config.quant_mode_0, data.framing, data.beta3);
    for (const AcplParamFields& gamma : data.gamma) {
        write_ec_data(w, AcplKind::kGamma, config.quant_mode_1, data.framing, gamma);
    }
}

bool acpl_codable(AcplKind kind, int quant_mode, int diff_type, bool first_band, int value) noexcept {
    const CodebookRef cb = codebook(kind, quant_mode, type_of(diff_type, first_band));
    const int index = value + cb.cb_off;
    return index >= 0 && static_cast<std::size_t>(index) < cb.codes.size();
}

}  // namespace ac4::detail
