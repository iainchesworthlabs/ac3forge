#pragma once

// ---------------------------------------------------------------------------
// The whole point of this file: prove the AVX2 build/link/dispatch/test
// pipeline end to end with something that carries no bit-exactness stakes
// at all, before any real kernel is built on top of it (ROADMAP PF5's
// dynamic-dispatch follow-on).
//
// avx2_probe.cpp is compiled with an AVX2 target flag (/arch:AVX2 or
// -mavx2 - see src/forge/CMakeLists.txt's forge_simd_avx2 object library
// and tests/CMakeLists.txt's matching per-source flag on the SAME file
// compiled a second time directly into the test binary) TWICE: once into
// the library proper, once into ac3tests, so the AVX2 codegen path is
// exercised - proven to compile, link and execute correctly - on both,
// independent of whether AC3FORGE's own consumer (ac3tests) links the
// static or the shared library (see the config-linux-llvm-shared CI leg).
// Only the compiled-in TEST copy needs to be directly callable without
// crossing a possible DLL export boundary, which is exactly why it is
// compiled a second time rather than linked against the library's own
// forge_simd_avx2 object - the same white-box arrangement
// tests/CMakeLists.txt already gives src/signing/src.
//
// Internal to src/forge/ on purpose - never installed, never part of the
// public ac3/ API, exactly like ac3/internal/profiling.hpp beside it.
// ---------------------------------------------------------------------------

namespace ac3::internal::avx2 {

// Computes a small, fixed result using real AVX2 intrinsics and compares it
// against the same result computed by ordinary scalar arithmetic in the
// same function - true if they agree. This function's own correctness has
// nothing to do with the codec's bit-exactness promise (it is not a codec
// kernel); it exists purely to prove AVX2 instructions execute correctly on
// whatever CPU calls it. Callers MUST confirm
// ac3::internal::cpu::has_avx2() first - calling this on incapable hardware
// is exactly the illegal-instruction fault that check exists to prevent.
[[nodiscard]] bool avx2_probe_matches_expected() noexcept;

}  // namespace ac3::internal::avx2
