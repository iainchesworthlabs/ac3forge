#pragma once

#include <cstddef>

#include "iclforge/arithmetic/detail/simd.hpp"

// The sample rate converter's dot product at float (dsp/resampler.cpp), on the seam's
// 128-bit f32x4 (src/arithmetic/arch).
//
// A sequential sum of 94 to 100 products is a chain of dependent adds, and on the ESP32-P4's
// single-precision FPU each add waits for the last. The sum is therefore split over four
// lanes, and its order is part of the converter's definition at float, the same on every
// platform: lane j adds the products of taps j, j + 4, j + 8 and so on, in that order; the
// lanes are added as (0 + 1) + (2 + 3); and the taps that do not fill a vector, n mod 4 of
// them, are added to that in order. Every operation is one IEEE multiply or add (no fused
// multiply-add: the build pins -ffp-contract=off and the seam's operators are single
// operations per lane), which is the seam's contract for its three directories: the x86-64
// seam's SSE and the generic seam's four scalars, on the Cortex-M3 and the ESP32s, give the
// same float, and the aarch64 seam's NEON is held to the same. tests/ac4core/
// test_ac4core_resampler.cpp holds the kernel to a loop of that description, written out, bit
// for bit.
//
// At double the converter's sum stays a sequential loop, as it always was: the encoder's
// converters and the default build's decoder keep their output.
//
// A table that keeps half its phases (dsp/resampler.hpp) gives the others as a phase read from its
// last coefficient to its first, and dot_four_lanes_reversed() is the same sum of that: tap k is
// coefficients[taps - 1 - k] times samples[k], taken in the order above, so it gives bit for bit
// what dot_four_lanes() gives on a copy of the phase written out backwards. The seam has no
// reversing load, so the four coefficients of a vector are four scalar loads.

namespace iclforge::ac4::detail::dsp {

[[nodiscard]] inline float dot_four_lanes(const float* coefficients, const float* samples,
                                          std::size_t taps) noexcept {
    namespace arch = iclforge::internal::arch;
    arch::f32x4 lanes = arch::f32x4::broadcast(0.0F);
    std::size_t k = 0;
    for (; k + 4 <= taps; k += 4) {
        lanes = lanes + arch::f32x4::load(coefficients + k) * arch::f32x4::load(samples + k);
    }
    float sum = (lanes.lane0() + lanes.lane1()) + (lanes.lane2() + lanes.lane3());
    for (; k < taps; ++k) {
        sum += coefficients[k] * samples[k];
    }
    return sum;
}

[[nodiscard]] inline float dot_four_lanes_reversed(const float* coefficients, const float* samples,
                                                   std::size_t taps) noexcept {
    namespace arch = iclforge::internal::arch;
    arch::f32x4 lanes = arch::f32x4::broadcast(0.0F);
    std::size_t k = 0;
    for (; k + 4 <= taps; k += 4) {
        // Tap k + j takes coefficients[taps - 1 - k - j].
        const float* last = coefficients + (taps - 1 - k);
        lanes = lanes + arch::f32x4::set(last[0], last[-1], last[-2], last[-3]) *
                            arch::f32x4::load(samples + k);
    }
    float sum = (lanes.lane0() + lanes.lane1()) + (lanes.lane2() + lanes.lane3());
    for (; k < taps; ++k) {
        sum += coefficients[taps - 1 - k] * samples[k];
    }
    return sum;
}

}  // namespace iclforge::ac4::detail::dsp
