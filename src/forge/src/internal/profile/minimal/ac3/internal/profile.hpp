#pragma once

// Build-profile facts, in the MINIMUM-FOOTPRINT DECODER variant
// (AC3FORGE_MINIMAL_DECODER, roadmap PF7). Every ordinary build compiles the
// identically-pathed header under src/internal/profile/full/ instead;
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

// AC3FORGE_MINIMAL_DECODER. True here: this build is decode-only, has no
// direct-form transform tables, and is compiled without exceptions or RTTI.
// See docs/building.md's "Minimum-footprint decoder profile".
inline constexpr bool kMinimalDecoderProfile = true;

// Whether src/core/reference_transform.hpp's direct-form entry points are
// backed by their tables. See that header for what they cost.
inline constexpr bool kReferenceTransformAvailable = false;

// The type the DECODER carries its coefficients, transform scratch and
// overlap-add history in (roadmap PF7's float32 gap).
//
// float here. The targets this profile serves have single-precision
// hardware at best - the ESP32-S3's LX7 FPU is single-precision, and the
// arm-none-eabi Cortex-M3 has none at all - so a double coefficient buys
// precision nothing downstream can use and costs both the memory it occupies
// and, on those targets, a software-emulated multiply per operation.
//
// The memory is the binding constraint today: the per-block `coeffs` buffers
// are 100,352 bytes and aht_coeffs_ is 86,016, against 160,764 bytes of free
// internal SRAM on an ESP32-S3. Measured accuracy cost at the transform is
// 2.7e-7 peak-normalised (tests/core/test_mdct_fast.cpp), which is about one
// LSB at 24 bits and well under the coding noise of any real AC-3 stream
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
using decode_scalar_t = float;


}  // namespace ac3::internal
