#include "ac3/core/fft.hpp"

#include <algorithm>
#include <cstddef>
#include <span>

#include "ac3/internal/arch/simd.hpp"

#include "fft_radix2.hpp"

namespace ac3 {

namespace {

const internal::FftRadix2Tables<static_cast<std::size_t>(kDftLength)>& tables() {
    static const internal::FftRadix2Tables<static_cast<std::size_t>(kDftLength)> t;
    return t;
}

}  // namespace

void dft512(std::span<const double, kDftLength> real_in,
           std::span<const double, kDftLength> imag_in, std::span<double, kDftLength> real_out,
           std::span<double, kDftLength> imag_out) {
    // The output spans double as the FFT's workspace (they were never
    // permitted to alias the inputs - the old direct-form sum read every
    // input element under each output index it wrote, so aliasing was
    // already incorrect before this took over).
    std::copy(real_in.begin(), real_in.end(), real_out.begin());
    std::copy(imag_in.begin(), imag_in.end(), imag_out.begin());
    internal::fft_radix2_forward<static_cast<std::size_t>(kDftLength)>(tables(), real_out,
                                                                      imag_out);
    // The spec sum's own 1/N normalisation (see fft.hpp), two bins at a time
    // through the arch seam (ROADMAP PF5). Multiplication by the reciprocal
    // rather than division: N is 512, so 1/N is exactly representable and
    // x * (1/512) and x / 512 are the correctly-rounded result of the same
    // exact real number - identical for every input, denormal results
    // included. The seam carries no divide for exactly this reason (a
    // general reciprocal-multiply would NOT be safe, and offering the
    // operation would invite one).
    constexpr double kInvN = 1.0 / static_cast<double>(kDftLength);
    const auto inv = internal::arch::f64x2::broadcast(kInvN);
    double* const rp = real_out.data();
    double* const ip = imag_out.data();
    for (std::size_t k = 0; k < static_cast<std::size_t>(kDftLength); k += 2) {
        (internal::arch::f64x2::load(rp + k) * inv).store(rp + k);
        (internal::arch::f64x2::load(ip + k) * inv).store(ip + k);
    }
}

}  // namespace ac3
