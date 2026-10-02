#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/aspx/frequency_tables.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"
#include "iclforge/ac4core/dsp/scalar_traits.hpp"

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

namespace iclforge::ac4::detail::aspx {

inline constexpr int kTsOffsetHfadj = 4;  // Pseudocode 86

// Table 192: 2 for frame lengths of 1 536 and up, 1 below; and the delay of
// the low band in QMF slots, 3 * num_ts_in_ats.
[[nodiscard]] constexpr int num_ts_in_ats(int frame_length) noexcept {
    return frame_length >= 1536 ? 2 : 1;
}
[[nodiscard]] constexpr int ts_offset_hfgen(int frame_length) noexcept {
    return 3 * num_ts_in_ats(frame_length);
}

// The cubic fit of Pseudocode 85 projects the low band's energies in dB on four
// polynomials of t = i scaled to [-1, 1], orthonormalised by modified Gram-Schmidt in
// double. Those four vectors depend on the number of points alone, and making them
// takes 4 n calls of std::pow and a hundred multiplies and adds of the compiler's
// software `double` on a part with a single-precision FPU: about 1.5 milliseconds a
// channel on the P4, every frame (A-SPX's stage of a 5.1 frame took 7.7 ms less in its
// five channels with them kept; planning/ac4.md, D14e). A channel keeps them for the
// number of points it last had, and the frame's own work is the projection.
struct CubicBasis {
    std::size_t n = 0;  // the number of points the vectors are for; 0 when none is made
    std::array<std::vector<double>, 4> basis{};
    // A power that adds nothing new at this n (fewer points than coefficients).
    std::array<bool, 4> empty{};
};

// What Pseudocode 88 keeps from one A-SPX interval to the next, per channel.
template <typename Real>
struct HfGeneratorState {
    std::array<std::uint8_t, kMaxSbgNoise> tna_mode_prev{};
    std::array<Real, kMaxSbgNoise> chirp_prev{};
    CubicBasis cubic{};
};

// dsp::Complex<Real> here (not std::complex<Real>): a fixed-point type cannot
// instantiate std::complex, and the QMF matrices these functions read and
// write are ac4core's own complex type throughout (dsp/complex.hpp).

template <typename Real>
struct HfGeneratorInput {
    // Q_low_ext: num_qmf_timeslots + ts_offset_hfgen + kTsOffsetHfadj slots
    // of 64 subbands, [slot * 64 + subband]. Only subbands below sbx are read.
    std::span<const dsp::Complex<Real>> q_low_ext;
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
                        std::span<dsp::Complex<Real>> q_high);

// Pseudocode 85's gain vector, gain_vec[sb] for sb < sbx: 10^((mean - fit[sb]) / 20),
// with fit the least squares cubic through the low band's energies in dB. The gains are
// dsp::Energy values: Real at double and float, a mantissa and a power of two at Fixed32.
// Exposed for its test. The form with `cubic` takes the fit's vectors from it and makes
// them there when they are for another number of points; the other makes them for the
// call.
template <typename Real>
void preflattening_gains(std::span<const dsp::Complex<Real>> q_low, int sbx, int ts_begin,
                         int ts_end, std::span<dsp::Energy<Real>> gain_vec);
template <typename Real>
void preflattening_gains(std::span<const dsp::Complex<Real>> q_low, int sbx, int ts_begin,
                         int ts_end, std::span<dsp::Energy<Real>> gain_vec, CubicBasis& cubic);

// Pseudocodes 86 and 87: alpha0[sb] and alpha1[sb] for sb < sba. Exposed for
// its test.
template <typename Real>
void prediction_coefficients(std::span<const dsp::Complex<Real>> q_low_ext, int num_ts_ext, int sba,
                             std::span<dsp::Complex<Real>> alpha0,
                             std::span<dsp::Complex<Real>> alpha1);

extern template void generate_high_band<Real>(const SubbandGroups&, const PatchTables&,
                                              const HfGeneratorInput<Real>&,
                                              HfGeneratorState<Real>&,
                                              std::span<dsp::Complex<Real>>);
extern template void preflattening_gains<Real>(std::span<const dsp::Complex<Real>>, int, int,
                                               int, std::span<dsp::Energy<Real>>);
extern template void preflattening_gains<Real>(std::span<const dsp::Complex<Real>>, int, int, int,
                                               std::span<dsp::Energy<Real>>, CubicBasis&);
extern template void prediction_coefficients<Real>(std::span<const dsp::Complex<Real>>, int,
                                                   int, std::span<dsp::Complex<Real>>,
                                                   std::span<dsp::Complex<Real>>);

}  // namespace iclforge::ac4::detail::aspx
