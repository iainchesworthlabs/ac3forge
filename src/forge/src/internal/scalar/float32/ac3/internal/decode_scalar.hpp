#pragma once

// The type the DECODER carries its coefficients, transform scratch and
// overlap-add history in (roadmap PF7's float32 gap), in the FLOAT variant.
// See the double variant under src/internal/scalar/float64/ for what this seam
// is and why it is separate from ac3/internal/profile.hpp.

namespace ac3::internal {

// float. The targets the minimum-footprint profile serves have
// single-precision hardware at best - the ESP32-S3's LX7 FPU is
// single-precision and the arm-none-eabi Cortex-M3 has none at all - so a
// double coefficient buys precision nothing downstream can use and costs both
// the memory it occupies and, on those targets, a software-emulated multiply
// per operation. On the ESP32-S3 the memory was the binding constraint: the
// port failed with out_of_memory on an 86,016-byte allocation until the decode
// path moved to float32.
//
// Measured accuracy cost at the transform is 2.7e-7 peak-normalised
// (tests/core/test_mdct_fast.cpp), about one LSB at 24 bits.
//
// Selectable independently of the profile now, which is what makes that claim
// checkable: a full build - CLI, tests, gold references and all - can be
// configured with AC3FORGE_DECODE_SCALAR=float and diffed against the double
// one. Before the split this variant existed only inside a build that had no
// CLI to diff with.
using decode_scalar_t = float;

}  // namespace ac3::internal
