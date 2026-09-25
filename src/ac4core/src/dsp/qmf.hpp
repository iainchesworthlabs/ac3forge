#pragma once

#include <array>
#include <complex>
#include <span>

#include "dsp/fft.hpp"

// The complex QMF analysis and synthesis banks of ETSI TS 103 190-1 V1.4.1
// clauses 5.7.3 and 5.7.4 (Pseudocodes 65 and 66), with the window QWIN of
// Annex D.3.
//
// Each time slot takes num_qmf_subbands = 64 time samples to 64 complex
// subband samples and back. The analysis folds its 640-sample window to 128
// values u[n] and computes, with e(x) = exp(i pi x),
//
//   Q[sb] = sum_{n<128} u[n] e((sb + 1/2)(2n - 1) / 128)
//         = e(-(2sb + 1) / 256) sum_n (u[n] e(n / 128)) e(2 sb n / 128),
//
// one 128-point inverse FFT. The synthesis computes, for n < 128,
//
//   qsyn[n] = Re sum_{sb<64} Q[sb] / 64 e((sb + 1/2)(2n - 255) / 128)
//           = Re e(n / 128) sum_sb (Q[sb] / 64 e(-255 (2sb + 1) / 256)) e(2 sb n / 128),
//
// another. tests/ac4core/test_ac4core_dsp.cpp holds both to the pseudocode as
// printed. The pair delays by 577 samples and reconstructs to about 78 dB, a
// property of QWIN.
//
// A matrix of slots is laid out slot by slot: value [ts * 64 + sb].

namespace ac4::detail::dsp {

inline constexpr int kQmfSubbands = 64;
inline constexpr int kQmfWindowLength = 640;

template <typename Real>
class QmfAnalysis {
   public:
    using Complex = std::complex<Real>;

    QmfAnalysis();

    // Clears qmf_filt, the 640 delayed samples of Pseudocode 65.
    void reset() noexcept;

    // Analyses pcm.size() / 64 slots into out, which must hold 64 values per
    // slot. Nothing happens when pcm.size() is not a multiple of 64 or out
    // is too small.
    void process(std::span<const Real> pcm, std::span<Complex> out);

   private:
    std::array<Real, kQmfWindowLength> filt_{};  // qmf_filt: [0] newest
    Fft<Real> fft_;
    std::array<Complex, 128> pre_{};            // exp(i pi n / 128)
    std::array<Complex, kQmfSubbands> post_{};  // exp(-i pi (2sb + 1) / 256)
    std::array<Complex, 128> work_{};
};

template <typename Real>
class QmfSynthesis {
   public:
    using Complex = std::complex<Real>;

    QmfSynthesis();

    // Clears qsyn_filt, the 1 280 values of Pseudocode 66.
    void reset() noexcept;

    // Synthesises in.size() / 64 slots into pcm, which must hold 64 samples
    // per slot. Nothing happens when in.size() is not a multiple of 64 or pcm
    // is too small.
    void process(std::span<const Complex> in, std::span<Real> pcm);

   private:
    std::array<Real, 2 * kQmfWindowLength> filt_{};  // qsyn_filt: [0] newest
    Fft<Real> fft_;
    std::array<Complex, kQmfSubbands> pre_{};  // exp(-i pi 255 (2sb + 1) / 256) / 64
    std::array<Complex, 128> post_{};          // exp(i pi n / 128)
    std::array<Complex, 128> work_{};
};

extern template class QmfAnalysis<double>;
extern template class QmfSynthesis<double>;

}  // namespace ac4::detail::dsp
