#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <span>

#include "aspx/frequency_tables.hpp"

// A-SPX's high frequency generator: ETSI TS 103 190-1 V1.4.1 clause
// 5.7.6.4.1, Pseudocodes 85 to 89. It patches subbands of the low band Q_low
// up to the A-SPX range, whitened by a second order linear predictor whose
// strength each noise subband group's aspx_tna_mode sets, and, with
// aspx_preflat, divided by a cubic fit to the low band's spectral envelope.
//
// Time runs along Q_low's slots (5.7.6.3.2): Q_low is the QMF matrix delayed
// by ts_offset_hfgen slots, and an A-SPX interval's borders count from its
// slot 0. The generator reads Q_low_ext, which starts ts_offset_hfadj = 4
// slots earlier still, from the end of the previous Q_low (Pseudocode 86).
//
// The decoder runs it on its QMF matrices; the encoder runs it to see what a
// decoder will make of the band it codes.

namespace ac4::detail::aspx {

inline constexpr int kTsOffsetHfadj = 4;  // Pseudocode 86

// Table 192: 2 for frame lengths of 1 536 and up, 1 below; and the delay of
// the low band in QMF slots, 3 * num_ts_in_ats.
[[nodiscard]] constexpr int num_ts_in_ats(int frame_length) noexcept {
    return frame_length >= 1536 ? 2 : 1;
}
[[nodiscard]] constexpr int ts_offset_hfgen(int frame_length) noexcept {
    return 3 * num_ts_in_ats(frame_length);
}

// What Pseudocode 88 keeps from one A-SPX interval to the next, per channel.
template <typename Real>
struct HfGeneratorState {
    std::array<std::uint8_t, kMaxSbgNoise> tna_mode_prev{};
    std::array<Real, kMaxSbgNoise> chirp_prev{};
};

template <typename Real>
struct HfGeneratorInput {
    // Q_low_ext: num_qmf_timeslots + ts_offset_hfgen + kTsOffsetHfadj slots
    // of 64 subbands, [slot * 64 + subband]. Only subbands below sbx are read.
    std::span<const std::complex<Real>> q_low_ext;
    int num_qmf_timeslots = 0;
    int ts_offset_hfgen = 0;
    // The interval, in Q_low's slots: atsg_sig[0] * num_ts_in_ats up to
    // atsg_sig[num_atsg_sig] * num_ts_in_ats.
    int ts_begin = 0;
    int ts_end = 0;
    bool preflat = false;                    // aspx_preflat
    std::span<const std::uint8_t> tna_mode;  // aspx_tna_mode, num_sbg_noise of them
};

// Pseudocodes 85 to 89. Writes Q_high for subbands sbx to sbz - 1 and slots
// ts_begin to ts_end - 1 into q_high, laid out like Q_low (slot ts at
// [ts * 64]); nothing else of q_high is touched. Moves `state` on to this
// interval's chirp factors and aspx_tna_mode.
template <typename Real>
void generate_high_band(const SubbandGroups& groups, const PatchTables& patches,
                        const HfGeneratorInput<Real>& in, HfGeneratorState<Real>& state,
                        std::span<std::complex<Real>> q_high);

// Pseudocode 85's gain vector, gain_vec[sb] for sb < sbx: 10^((mean - fit[sb]) / 20),
// with fit the least squares cubic through the low band's energies in dB.
// Exposed for its test.
template <typename Real>
void preflattening_gains(std::span<const std::complex<Real>> q_low, int sbx, int ts_begin,
                         int ts_end, std::span<Real> gain_vec);

// Pseudocodes 86 and 87: alpha0[sb] and alpha1[sb] for sb < sba. Exposed for
// its test.
template <typename Real>
void prediction_coefficients(std::span<const std::complex<Real>> q_low_ext, int num_ts_ext, int sba,
                             std::span<std::complex<Real>> alpha0,
                             std::span<std::complex<Real>> alpha1);

extern template void generate_high_band<double>(const SubbandGroups&, const PatchTables&,
                                                const HfGeneratorInput<double>&,
                                                HfGeneratorState<double>&,
                                                std::span<std::complex<double>>);
extern template void preflattening_gains<double>(std::span<const std::complex<double>>, int, int,
                                                 int, std::span<double>);
extern template void prediction_coefficients<double>(std::span<const std::complex<double>>, int,
                                                     int, std::span<std::complex<double>>,
                                                     std::span<std::complex<double>>);

}  // namespace ac4::detail::aspx
