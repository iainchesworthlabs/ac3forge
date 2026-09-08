#pragma once

// The type the DECODER carries its coefficients, transform scratch and
// overlap-add history in (roadmap PF7's float32 gap), in the DOUBLE variant.
// The float variant is the identically-pathed header under
// src/internal/scalar/float32/; src/forge/CMakeLists.txt picks the directory
// from AC3FORGE_DECODE_SCALAR, so no source file asks which it is with a
// preprocessor conditional (tools/checks/check_platform_macros.ps1's rule).
//
// This lives beside ac3/internal/profile.hpp rather than in it, and the split
// is the point. Those two used to be one header, so the scalar type could only
// be float in a build that was ALSO decode-only, without exceptions, without
// the direct-form tables and without a CLI. That made the float32 path
// unmeasurable by anything in this repo: docs/building.md's "~139 dB
// worst-channel SNR against the double decode" came from a hand-made build no
// preset could reproduce, and nothing has re-checked it since. They are two
// independent axes and are now spelled as two.
//
// Internal, never installed. Not part of the API either: DecodedFrame::channels
// and the decode_*_into spans were already float, so the boundary a caller sees
// does not move whichever variant is compiled - only what happens behind it.

namespace ac3::internal {

// double: what every build outside the minimum-footprint profile has always
// used. Nothing about such a build is memory- or FPU-constrained, and the gold
// references, the quality trend and the fifteen cross-platform bitstream hashes
// are all stated in terms of this path's arithmetic.
//
// Decode-side only in every variant. The encoder is not built in the
// minimum-footprint profile at all - src/forge/minimal.cmake carries
// encoder/coupling.cpp and encoder/eac3_tools.cpp solely for the dequantiser
// and the spx/ecpl geometry the DECODER calls into - so the forward transforms
// and everything that pins their output stay double on every build that has
// them.
using decode_scalar_t = double;

}  // namespace ac3::internal
