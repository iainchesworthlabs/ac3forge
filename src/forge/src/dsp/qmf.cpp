#include "ac3/dsp/qmf.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>

#include "../core/fft_kernel.hpp"
#include "qmf_prototype.hpp"

namespace ac3::dsp {

namespace {

// The fold period, and the transform length that goes with it. The
// modulation exp(-i*pi*(k + 1/2)*m/M) is ANTI-periodic in m with period 2M
// (it returns exp(-i*2*pi*(k + 1/2)) = -1), which is why the fold below
// alternates sign every 2M taps rather than simply wrapping.
constexpr std::size_t kFold = 2 * static_cast<std::size_t>(kQmfSubbands);  // 128
constexpr std::size_t kBlocks = static_cast<std::size_t>(kQmfTaps) / kFold;  // 5

// exp(-i*pi*m/(2M)), the half-bin shift that turns the odd-stacked sum
// into an ordinary 128-point DFT:
//     sum_m v[m] * exp(-i*pi*(k + 1/2)*m/M)
//   = sum_m (v[m] * exp(-i*pi*m/(2M))) * exp(-i*2*pi*k*m/(2M))
struct Twiddle {
    std::array<double, kFold> real{};
    std::array<double, kFold> imag{};
    Twiddle() {
        for (std::size_t m = 0; m < kFold; ++m) {
            const double angle =
                -std::numbers::pi * static_cast<double>(m) / static_cast<double>(kFold);
            real[m] = std::cos(angle);
            imag[m] = std::sin(angle);
        }
    }
};

const Twiddle& twiddle() {
    static const Twiddle table;
    return table;
}

const internal::FftTables<kFold>& fft_tables() {
    static const internal::FftTables<kFold> tables;
    return tables;
}

// The fold's alternating sign, indexed by 2M-block.
[[nodiscard]] constexpr double block_sign(std::size_t block) {
    return (block % 2 == 0) ? 1.0 : -1.0;
}

}  // namespace

std::span<const double, kQmfTaps> qmf_prototype() {
    return std::span<const double, kQmfTaps>{kQmfPrototype};
}

void QmfAnalysis::reset() { history_.fill(0.0); }

void QmfAnalysis::push(std::span<const float, kQmfHop> block,
                       std::span<double, kQmfSubbands> real,
                       std::span<double, kQmfSubbands> imag) {
    // Slide the window on by one hop. history_[0] is the oldest sample, so
    // after this the window spans exactly the 640 samples whose newest is
    // the last one just pushed - which is the timeslot kQmfDelaySlots back.
    for (std::size_t n = 0; n + static_cast<std::size_t>(kQmfHop) < history_.size(); ++n) {
        history_[n] = history_[n + static_cast<std::size_t>(kQmfHop)];
    }
    for (std::size_t n = 0; n < static_cast<std::size_t>(kQmfHop); ++n) {
        history_[history_.size() - static_cast<std::size_t>(kQmfHop) + n] =
            static_cast<double>(block[n]);
    }

    // Window and fold, in one pass: the prototype is applied as the taps are
    // accumulated rather than into a 640-sample temporary.
    std::array<double, kFold> folded{};
    for (std::size_t b = 0; b < kBlocks; ++b) {
        const double sign = block_sign(b);
        for (std::size_t m = 0; m < kFold; ++m) {
            const std::size_t n = b * kFold + m;
            folded[m] += sign * kQmfPrototype[n] * history_[n];
        }
    }

    const auto& tw = twiddle();
    const auto& t = fft_tables();
    // Written straight into digit-reversed position: fft_forward_bitrev
    // expects it there (fft_kernel.hpp), the same convention dft512 and the
    // fast MDCT fold use, so this pass and the kernel's own bit-reversal
    // pass never coexist.
    std::array<double, kFold> spectrum_real{};
    std::array<double, kFold> spectrum_imag{};
    for (std::size_t m = 0; m < kFold; ++m) {
        const std::size_t d = t.bitrev[m];
        spectrum_real[d] = folded[m] * tw.real[m];
        spectrum_imag[d] = folded[m] * tw.imag[m];
    }
    internal::fft_forward_bitrev<kFold>(t, spectrum_real, spectrum_imag);

    // The transform is conjugate-symmetric about k = 2M - 1 - k for real
    // input, so the upper half carries nothing the lower half does not.
    for (std::size_t k = 0; k < static_cast<std::size_t>(kQmfSubbands); ++k) {
        real[k] = spectrum_real[k];
        imag[k] = spectrum_imag[k];
    }
}

void QmfSynthesis::reset() { overlap_.fill(0.0); }

void QmfSynthesis::pull(std::span<const double, kQmfSubbands> real,
                        std::span<const double, kQmfSubbands> imag,
                        std::span<float, kQmfHop> out) {
    // Undo the transform. Only k < M is carried, so the k >= M half is
    // reconstructed implicitly by taking the real part below - exactly the
    // conjugate-symmetric partner analysis discarded.
    //
    // An inverse DFT via the forward core: conjugating the input, running
    // the forward transform and conjugating the output computes the
    // unnormalized inverse, so no second set of tables is needed.
    const auto& t = fft_tables();
    std::array<double, kFold> time_real{};
    std::array<double, kFold> time_imag{};
    for (std::size_t k = 0; k < static_cast<std::size_t>(kQmfSubbands); ++k) {
        const std::size_t d = t.bitrev[k];
        time_real[d] = real[k];
        time_imag[d] = -imag[k];
    }
    internal::fft_forward_bitrev<kFold>(t, time_real, time_imag);

    // Post-twiddle by exp(+i*pi*m/(2M)) and take the real part. With the
    // conjugation above, Re{conj(z) * conj(w)} == Re{z * w}, so the
    // post-twiddle uses the SAME table the analysis pre-twiddle did.
    const auto& tw = twiddle();
    std::array<double, kFold> folded{};
    const double scale = 1.0 / static_cast<double>(kQmfSubbands);
    for (std::size_t m = 0; m < kFold; ++m) {
        folded[m] = scale * (time_real[m] * tw.real[m] - time_imag[m] * tw.imag[m]);
    }

    // Unfold across the 2M-blocks with the same alternating sign, window
    // again, and overlap-add. Every output sample gathers one tap from each
    // of the last kQmfTaps/kQmfHop timeslots.
    for (std::size_t b = 0; b < kBlocks; ++b) {
        const double sign = block_sign(b);
        for (std::size_t m = 0; m < kFold; ++m) {
            const std::size_t n = b * kFold + m;
            overlap_[n] += sign * kQmfPrototype[n] * folded[m];
        }
    }

    for (std::size_t n = 0; n < static_cast<std::size_t>(kQmfHop); ++n) {
        out[n] = static_cast<float>(overlap_[n]);
    }
    for (std::size_t n = 0; n + static_cast<std::size_t>(kQmfHop) < overlap_.size(); ++n) {
        overlap_[n] = overlap_[n + static_cast<std::size_t>(kQmfHop)];
    }
    for (std::size_t n = overlap_.size() - static_cast<std::size_t>(kQmfHop); n < overlap_.size();
         ++n) {
        overlap_[n] = 0.0;
    }
}

}  // namespace ac3::dsp
