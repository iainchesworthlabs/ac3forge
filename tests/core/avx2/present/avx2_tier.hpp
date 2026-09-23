#pragma once

#include "avx2_probe.hpp"
#include "mdct_avx2.hpp"

// The variant of tests/core/avx2/avx2_tier.hpp compiled when src/forge really
// built the AVX2 tier (x86_64, AC3FORGE_AVX2=ON). It is a pass-through: the
// real declarations come straight from src/forge/src/internal/avx2/, which
// tests/CMakeLists.txt puts on this target's include path in exactly the
// build that compiles avx2_probe.cpp and mdct_avx2.cpp into it a second time.
//
// Both variants ship this filename; CMake puts the matching directory on the
// include path, so tests/core/test_simd_kernels.cpp includes it unconditionally
// and never asks with an #ifdef. Same shape as
// src/core/transform/{reference,stub}/ and ac3/internal/profiling.hpp's three
// variants - see tools/checks/check_platform_macros.ps1 for why a feature-flag
// #ifdef is no more welcome here than a platform one.

namespace ac3::test::avx2 {

// True in this variant by construction. The test file branches on this with a
// plain `if`, not a preprocessor conditional, which is what keeps BOTH arms
// compiled and type-checked on every platform - the AVX2 case bodies stopped
// being parsed at all on a non-x86_64 build while they sat behind an #ifdef,
// so a rename or a signature change on the absent legs went unnoticed until
// an x86_64 leg ran.
inline constexpr bool kTierCompiled = true;

}  // namespace ac3::test::avx2
