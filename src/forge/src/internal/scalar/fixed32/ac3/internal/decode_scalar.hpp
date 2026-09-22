#pragma once

#include "fixed32.hpp"

// The type the DECODER carries its coefficients, transform scratch and
// overlap-add history in, in the FIXED-POINT variant. See the double variant
// under src/internal/scalar/float64/ for what this seam is; the float variant
// under src/internal/scalar/float32/ is the ESP32-S3's.

namespace ac3::internal {

// Fixed32 (src/forge/src/core/fixed32.hpp): Q7.24 in a 32-bit integer, for a
// part with no floating-point unit at all - the ESP32-C3, a Cortex-M3 - where
// even float is a compiled subroutine. Its guarantee is a stated SNR to the
// double decode (planning/arithmetic-tiers.md, 100 dB the target) and PCM
// that is bit-identical on every platform, integer arithmetic having no
// rounding mode or C library to differ by.
//
// Selectable in a full build with AC3FORGE_DECODE_SCALAR=fixed, which is how
// it is measured: the same CLI, diffed against the double one by
// tools/checks/check_decode_scalar_snr.py and run through the gold-reference
// gate.
using decode_scalar_t = Fixed32;

}  // namespace ac3::internal
