#include "cpu_features.hpp"

// ---------------------------------------------------------------------------
// has_avx2() in the MINIMUM-FOOTPRINT DECODER profile (AC3FORGE_MINIMAL_DECODER,
// roadmap PF7). Every ordinary build compiles ../cpu_features.cpp instead;
// src/forge/minimal.cmake picks this one, so no source file asks which profile
// it is in with a preprocessor conditional (tools/checks/check_platform_macros.ps1's
// rule - the same directory-selection mechanism ac3/internal/profile.hpp and the
// SIMD arch tree already use).
//
// A separate translation unit rather than an `if constexpr` inside the shared
// one, because the difference is not a branch - it is a DEPENDENCY. The shared
// implementation reads AC3FORGE_SIMD_TIER and reports a bad value through
// fmt, and reporting an abort reason through it as well; this profile targets
// a Cortex-M3 with no environment to read, no host to print to, and a ROM
// budget that would notice newlib's formatted-output machinery being linked in
// for two diagnostics that can never fire. `if constexpr` would not have helped:
// the discarded branch of a non-template is still semantically checked and its
// callees still ODR-used, so the dependency would survive the branch being
// dead.
//
// The answer is a constant, and correct by construction rather than by
// measurement: this profile only ever builds for targets whose CMake
// configuration resolves the SIMD seam to generic/ and the CPU probe to
// probe/none - there is no AVX2 instruction on a Cortex-M3 for a runtime check
// to find, and no supported way to build this profile for one that has it. The
// AVX2-flagged object library (forge_simd_avx2) is not part of this profile at
// all; mdct.cpp's calls into ac3::internal::avx2:: are linked against
// src/internal/avx2/none/mdct_avx2.cpp's std::unreachable() bodies, which this
// false makes unreachable in fact and not merely by contract.
// ---------------------------------------------------------------------------

namespace ac3::internal::cpu {

bool has_avx2() noexcept {
    return false;
}

}  // namespace ac3::internal::cpu
