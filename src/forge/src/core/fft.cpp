#include "ac3/core/fft.hpp"

#include <cstddef>
#include <span>

#include "ac3/internal/arch/simd.hpp"

#include "fft_kernel.hpp"

namespace ac3 {

namespace {

const internal::FftTables<static_cast<std::size_t>(kDftLength)>& tables() {
    static const internal::FftTables<static_cast<std::size_t>(kDftLength)> t;
    return t;
}

}  // namespace

void dft512(std::span<const double, kDftLength> real_in,
           std::span<const double, kDftLength> imag_in, std::span<double, kDftLength> real_out,
           std::span<double, kDftLength> imag_out) {
    // The output spans double as the FFT's workspace (they were never
    // permitted to alias the inputs - the old direct-form sum read every
    // input element under each output index it wrote, so aliasing was
    // already incorrect before this took over). This copy-in is also where
    // the kernel's digit-reversal happens: it expects its input already
    // permuted, so the store index is bitrev[n] instead of n and the pass
    // the old core spent permuting in place disappears (fft_kernel.hpp).
    const auto& t = tables();
    for (std::size_t n = 0; n < static_cast<std::size_t>(kDftLength); ++n) {
        real_out[t.bitrev[n]] = real_in[n];
        imag_out[t.bitrev[n]] = imag_in[n];
    }
    internal::fft_forward_bitrev<static_cast<std::size_t>(kDftLength), double>(t, real_out,
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
