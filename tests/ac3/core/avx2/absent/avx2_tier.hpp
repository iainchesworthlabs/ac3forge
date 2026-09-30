#pragma once

// The variant of tests/core/avx2/avx2_tier.hpp compiled when src/forge did NOT
// build the AVX2 tier - AC3FORGE_AVX2=OFF, or a target that is not x86_64 at
// all - so neither avx2_probe.cpp nor mdct_avx2.cpp is in this binary.
//
// It exists so tests/core/test_simd_kernels.cpp compiles unchanged on every
// platform and its AVX2 case bodies keep being type-checked even where they
// can never run. Behind the #ifdef they used to sit in, those bodies were not
// parsed there at all, and a renamed kernel or a changed signature could only
// be caught by an x86_64 run.
//
// The mdct kernels need nothing from this file: src/forge already ships
// src/internal/avx2/none/mdct_avx2.cpp, which gives every declaration in
// mdct_avx2.hpp a std::unreachable() body compiled into forge_objects in
// exactly this configuration (see src/forge/CMakeLists.txt) - the same
// directory-selected shape, for the same reason. So the header comes straight
// from src/internal/avx2, which tests/CMakeLists.txt puts on the include path
// here, and ac3tests links the stubs it already had.
//
// avx2_probe.cpp is the one gap: it has no `none/` twin, because nothing in
// the library calls it - it exists only for the test below to execute. Hence
// the single declaration here and the single definition in avx2_tier.cpp.
//
// Both variants ship this filename; CMake puts the matching directory on the
// include path. Same shape as src/core/transform/{reference,stub}/.

#include "mdct_avx2.hpp"

namespace ac3::internal::avx2 {

// Declared but never callable here - see avx2_tier.cpp. The signature matches
// src/internal/avx2/avx2_probe.hpp's exactly, so the present/ build and this
// one agree on what the test is calling.
[[nodiscard]] bool avx2_probe_matches_expected() noexcept;

}  // namespace ac3::internal::avx2

namespace ac3::test::avx2 {

// False in this variant by construction - see the present/ copy for why the
// test branches on this with a plain `if` rather than an #ifdef.
inline constexpr bool kTierCompiled = false;

}  // namespace ac3::test::avx2
