#pragma once

// Build-profile facts, in the variant every ORDINARY build compiles
// (roadmap PF7). The minimum-footprint decoder profile compiles the
// identically-pathed header under src/internal/profile/minimal/ instead;
// src/forge/CMakeLists.txt picks the directory, so no source file here asks
// which profile it is in with a preprocessor conditional
// (tools/checks/check_platform_macros.ps1's rule, the same mechanism
// ac3/internal/profiling.hpp and src/audio's platform backends use).
//
// Internal, never installed: these are facts about how this library was
// built, not part of its API. A caller that needs to know whether the
// reference transform is present finds out the way any caller does - by
// asking for it and being refused (DecodeError::kUnsupported).

namespace ac3::internal {

// AC3FORGE_MINIMAL_DECODER. False here: this build carries the whole codec.
inline constexpr bool kMinimalDecoderProfile = false;

// Whether src/core/reference_transform.hpp's direct-form entry points are
// backed by their tables. See that header for what they cost.
inline constexpr bool kReferenceTransformAvailable = true;

// The type the DECODER carries its coefficients, transform scratch and
// overlap-add history in (roadmap PF7's float32 gap).
//
// double here, which is what every build outside the minimum-footprint
// profile has always used and will keep using. Nothing about this build is
// memory- or FPU-constrained, and the gold references, the quality trend and
// the cross-platform bitstream hashes are all stated in terms of this path's
// arithmetic
//
// This is a decode-side choice only. The encoder is not built in the
// minimum-footprint profile at all - src/forge/minimal.cmake carries
// encoder/coupling.cpp and encoder/eac3_tools.cpp solely for the dequantiser
// and the spx/ecpl geometry the DECODER calls into - so the forward transforms
// and everything that pins their output (tests/golden/bitstream-hashes.json's
// fifteen SHA-256s) stay double on every build that has them.
//
// It is also not part of the API. DecodedFrame::channels and the decode_*_into
// spans were already float, so the boundary the caller sees does not move; only
// what happens behind it does.
using decode_scalar_t = double;


}  // namespace ac3::internal
