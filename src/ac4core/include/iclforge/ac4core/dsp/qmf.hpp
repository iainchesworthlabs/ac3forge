#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"

// The complex QMF analysis and synthesis banks of ETSI TS 103 190-1 V1.4.1
// clauses 5.7.3 and 5.7.4 (Pseudocodes 65 and 66), with the window QWIN of
// Annex D.3.
//
// Each time slot takes num_qmf_subbands = 64 time samples to 64 complex
// subband samples and back. With e(x) = exp(i pi x) the analysis folds its
// 640-sample window to 128 values u[n] and computes
//
//   Q[k] = sum_{n<128} u[n] e((2k + 1)(2n - 1) / 256),   k < 64,
//
// and the synthesis computes, for n < 128 and with c[k] = Q[k] / 64,
//
//   qsyn[n] = Re sum_{k<64} c[k] e((2k + 1)(2n - 255) / 256).
//
// tests/ac4core/test_ac4core_dsp.cpp holds both to the pseudocode as printed. The
// pair delays by 577 samples and reconstructs to about 78 dB, a property of QWIN.
//
// Each is one 64-point complex transform. u is real, so its even and odd
// samples are the real and imaginary parts of z[m] = u[2m] + i u[2m + 1], and
//
//   Z[k] = sum_{m<64} z[m] e(m / 64) e(2 k m / 64)
//
// (a transform of z turned by e(m / 64)) is E[k] + i O[k], where E and O are what
// the even and the odd samples give alone, at the odd multiples e((2k + 1) m / 64).
// For real samples E[63 - k] = conj E[k] and O[63 - k] = conj O[k], so
// E[k] = (Z[k] + conj Z[63 - k]) / 2 and O[k] = (Z[k] - conj Z[63 - k]) / (2i), and
// with theta = pi (2k + 1) / 256,
//
//   a = e(-theta / pi) E[k],  b = e(theta / pi) O[k],
//   Q[k] = a + b,  Q[63 - k] = i conj(b - a),          k < 32,
//
// one turn of each of E and O for two subbands. The synthesis is the same
// structure transposed: with x[k] = c[k] e(-255 (2k + 1) / 256) and
// x'[k] = x[k] e((2k + 1) / 128), the values qsyn[2m] + i qsyn[2m + 1] are
// e(m / 64) / 2 times the transform of T[k] = x[k] + conj x[63 - k] +
// i (x'[k] + conj x'[63 - k]), which pairs k with 63 - k in turn.
// dsp/qmf_kernels.hpp has each step; dsp/qmf_constants.hpp the factors, in the
// program's read-only data and shared by every bank.
//
// The delay lines are circular: a slot writes its new block over the oldest and
// moves an index, where Pseudocodes 65 and 66 shift 576 and 1 152 values.
//
// A matrix of slots is laid out slot by slot: value [ts * 64 + sb].

namespace iclforge::ac4::detail::dsp {

inline constexpr int kQmfSubbands = 64;
inline constexpr int kQmfWindowLength = 640;

// The working space of one slot of either bank: the window's 128 values and the
// transform's two buffers, real and imaginary parts apart. It holds nothing between
// slots, so one serves every bank of a substream, one after another, and a bank
// keeps only its delay line. 384 values.
template <typename Real>
struct QmfScratch {
    std::array<Real, 128> u;
    std::array<Real, 64> a_re;
    std::array<Real, 64> a_im;
    std::array<Real, 64> b_re;
    std::array<Real, 64> b_im;
};

template <typename Real>
class QmfAnalysis {
   public:
    using Complex = iclforge::ac4::detail::dsp::Complex<Real>;

    // Clears qmf_filt, the 640 delayed samples of Pseudocode 65.
    void reset() noexcept;

    // Analyses pcm.size() / 64 slots into out, which must hold 64 values per
    // slot. Nothing happens when pcm.size() is not a multiple of 64 or out
    // is too small. The first form works in a scratch of its own on the stack.
    void process(std::span<const Real> pcm, std::span<Complex> out);
    void process(std::span<const Real> pcm, std::span<Complex> out, QmfScratch<Real>& scratch);

   private:
    // qmf_filt as ten blocks of 64: block b (0 the newest) is at physical block
    // (head_ + b) mod 10 and holds the samples of the slot b slots ago, its last
    // sample first, as qmf_filt[64 b + sb] does in Pseudocode 65.
    std::array<Real, kQmfWindowLength> filt_{};
    std::size_t head_ = 0;
};

template <typename Real>
class QmfSynthesis {
   public:
    using Complex = iclforge::ac4::detail::dsp::Complex<Real>;

    // Clears qsyn_filt, the 1 280 values of Pseudocode 66.
    void reset() noexcept;

    // Synthesises in.size() / 64 slots into pcm, which must hold 64 samples
    // per slot. Nothing happens when in.size() is not a multiple of 64 or pcm
    // is too small. The first form works in a scratch of its own on the stack.
    void process(std::span<const Complex> in, std::span<Real> pcm);
    void process(std::span<const Complex> in, std::span<Real> pcm, QmfScratch<Real>& scratch);

   private:
    // qsyn_filt as ten blocks of 128, addressed as the analysis's are.
    std::array<Real, 2 * kQmfWindowLength> filt_{};
    std::size_t head_ = 0;
};

extern template class QmfAnalysis<Real>;
extern template class QmfSynthesis<Real>;

}  // namespace iclforge::ac4::detail::dsp
