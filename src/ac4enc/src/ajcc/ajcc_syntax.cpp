#include "ajcc/ajcc_syntax.hpp"

#include <cstdint>
#include <span>

#include "tables/huffman_codes.hpp"
#include "tables/huffman_tables.hpp"

namespace ac4::detail {
namespace {

enum class Kind : std::uint8_t { kAlpha, kBeta, kDry, kWet };
enum class HcbType : std::uint8_t { kF0, kDf, kDt };

// ajcc_data()'s parameters' kinds, in AjccDataFields::params' order.
[[nodiscard]] Kind kind_of(std::size_t param) noexcept {
    if (param < 2) {
        return Kind::kAlpha;
    }
    if (param < 4) {
        return Kind::kBeta;
    }
    return param < 8 ? Kind::kDry : Kind::kWet;
}

// Whether the left module's framing governs `param`: alpha1, beta1, dry1,
// dry2 and wet1 to wet3.
[[nodiscard]] bool left(std::size_t param) noexcept {
    switch (param) {
        case 0:
        case 2:
        case 4:
        case 5:
        case 8:
        case 9:
        case 10:
            return true;
        default:
            return false;
    }
}

// get_ajcc_hcb(), Pseudocode 29: A-CPL's codebooks for alpha and beta (Part 1
// Annex A.3), A-JCC's for dry and wet (Part 2 Annex A.1.2); their codewords, for
// writing, and cb_off.
struct CodebookRef {
    std::span<const HuffCode> codes;
    int cb_off = 0;
};

[[nodiscard]] CodebookRef codebook(Kind kind, int quant_mode, HcbType type) noexcept {
    using namespace tables;
    struct Set {
        std::span<const HuffCode> f0, df, dt;
        const Codebook *f0_cb, *df_cb, *dt_cb;
    };
    const bool fine = quant_mode == 0;
    const auto pick = [&]() -> Set {
        switch (kind) {
            case Kind::kAlpha:
                return fine ? Set{kAcplHcbAlphaFineF0Codes, kAcplHcbAlphaFineDfCodes,
                                  kAcplHcbAlphaFineDtCodes, &kAcplHcbAlphaFineF0,
                                  &kAcplHcbAlphaFineDf,     &kAcplHcbAlphaFineDt}
                            : Set{kAcplHcbAlphaCoarseF0Codes, kAcplHcbAlphaCoarseDfCodes,
                                  kAcplHcbAlphaCoarseDtCodes, &kAcplHcbAlphaCoarseF0,
                                  &kAcplHcbAlphaCoarseDf,     &kAcplHcbAlphaCoarseDt};
            case Kind::kBeta:
                return fine ? Set{kAcplHcbBetaFineF0Codes, kAcplHcbBetaFineDfCodes,
                                  kAcplHcbBetaFineDtCodes, &kAcplHcbBetaFineF0,
                                  &kAcplHcbBetaFineDf,     &kAcplHcbBetaFineDt}
                            : Set{kAcplHcbBetaCoarseF0Codes, kAcplHcbBetaCoarseDfCodes,
                                  kAcplHcbBetaCoarseDtCodes, &kAcplHcbBetaCoarseF0,
                                  &kAcplHcbBetaCoarseDf,     &kAcplHcbBetaCoarseDt};
            case Kind::kDry:
                return fine ? Set{kAjccHcbDryFineF0Codes, kAjccHcbDryFineDfCodes,
                                  kAjccHcbDryFineDtCodes, &kAjccHcbDryFineF0,
                                  &kAjccHcbDryFineDf,     &kAjccHcbDryFineDt}
                            : Set{kAjccHcbDryCoarseF0Codes, kAjccHcbDryCoarseDfCodes,
                                  kAjccHcbDryCoarseDtCodes, &kAjccHcbDryCoarseF0,
                                  &kAjccHcbDryCoarseDf,     &kAjccHcbDryCoarseDt};
            case Kind::kWet:
                break;
        }
        return fine ? Set{kAjccHcbWetFineF0Codes, kAjccHcbWetFineDfCodes, kAjccHcbWetFineDtCodes,
                          &kAjccHcbWetFineF0,     &kAjccHcbWetFineDf,     &kAjccHcbWetFineDt}
                    : Set{kAjccHcbWetCoarseF0Codes, kAjccHcbWetCoarseDfCodes,
                          kAjccHcbWetCoarseDtCodes, &kAjccHcbWetCoarseF0,
                          &kAjccHcbWetCoarseDf,     &kAjccHcbWetCoarseDt};
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

// ajcc_framing_data(), 6.2.6.2.
void write_framing(BitWriter& w, const AjccFramingFields& f) {
    w.write(1, static_cast<std::uint64_t>(f.interpolation_type), "ajcc_interpolation_type");
    w.write(1, static_cast<std::uint64_t>(f.num_param_sets - 1), "ajcc_num_param_sets_code");
    if (f.interpolation_type == 1) {
        for (int ps = 0; ps < f.num_param_sets; ++ps) {
            w.write(5, static_cast<std::uint64_t>(f.param_timeslot[static_cast<std::size_t>(ps)]),
                    "ajcc_param_timeslot");
        }
    }
}

// ajced() with ajcc_huff_data(), 6.2.6.3 and 6.2.6.4.
void write_ajced(BitWriter& w, Kind kind, int quant_mode, bool no_dt, int num_ps,
                 const AjccParamFields& param) {
    for (int ps = 0; ps < num_ps; ++ps) {
        const AjccSetFields& set = param.at(static_cast<std::size_t>(ps));
        if (!no_dt) {
            w.write(1, static_cast<std::uint64_t>(set.diff_type), "diff_type");
        }
        for (std::size_t i = 0; i < set.values.size(); ++i) {
            const CodebookRef cb = codebook(kind, quant_mode, type_of(set.diff_type, i == 0));
            w.write_codeword(cb.codes, static_cast<std::size_t>(set.values[i] + cb.cb_off),
                             "ajcc_hcw");
        }
    }
}

}  // namespace

int ajcc_num_param_bands(int num_param_bands_id) noexcept {
    constexpr std::array<int, 4> kBands = {15, 12, 9, 7};
    return kBands[static_cast<std::size_t>(num_param_bands_id & 3)];
}

void write_ajcc_data(BitWriter& w, const AjccDataFields& data) {
    w.write(1, data.no_dt ? 1U : 0U, "b_no_dt");
    w.write(2, static_cast<std::uint64_t>(data.num_param_bands_id), "ajcc_num_param_bands_id");
    w.write(1, static_cast<std::uint64_t>(data.core_mode), "ajcc_core_mode");
    w.write(1, static_cast<std::uint64_t>(data.qm_ab), "ajcc_qm_ab");
    w.write(1, static_cast<std::uint64_t>(data.qm_dw), "ajcc_qm_dw");
    for (const AjccFramingFields& framing : data.framing) {
        write_framing(w, framing);
    }
    for (std::size_t p = 0; p < data.params.size(); ++p) {
        const Kind kind = kind_of(p);
        const int quant = kind == Kind::kAlpha || kind == Kind::kBeta ? data.qm_ab : data.qm_dw;
        const int num_ps = data.framing[left(p) ? 0 : 1].num_param_sets;
        write_ajced(w, kind, quant, data.no_dt, num_ps, data.params[p]);
    }
}

bool ajcc_codable(std::size_t param, int quant_mode, int diff_type, bool first_band,
                  int value) noexcept {
    const CodebookRef cb = codebook(kind_of(param), quant_mode, type_of(diff_type, first_band));
    const int index = value + cb.cb_off;
    return index >= 0 && static_cast<std::size_t>(index) < cb.codes.size();
}

}  // namespace ac4::detail
