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

}  // namespace ac3::internal
