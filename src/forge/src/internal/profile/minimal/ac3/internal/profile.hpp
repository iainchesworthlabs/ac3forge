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

// decode_scalar_t used to live here. It is ac3/internal/decode_scalar.hpp
// now, on its own CMake-selected seam: which profile this is and which
// scalar the decoder carries are independent questions, and welding them
// together meant the float32 path could only exist in a build with no CLI
// to measure it against. See that header.


}  // namespace ac3::internal
