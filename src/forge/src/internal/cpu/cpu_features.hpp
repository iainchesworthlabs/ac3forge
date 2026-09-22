#pragma once

// ---------------------------------------------------------------------------
// Runtime CPU-feature detection, x86-64 only (ROADMAP PF5's follow-on
// dynamic-dispatch work).
//
// The arch seam (src/forge/src/internal/arch/{generic,x86_64,aarch64}/) is
// compile-time only, deliberately: SSE2 and NEON are both part of their
// architecture, guaranteed present, so nothing needs asking. AVX2 is
// different - a real CPU FEATURE that may or may not be present on the
// machine a binary actually runs on, which a build machine cannot know in
// advance. This is the seam's only piece of genuine runtime dispatch, and it
// exists ONLY to answer one question safely: is it safe to execute AVX2
// instructions on the CPU this process is running on, right now.
//
// Always reachable, on every platform: a generic or aarch64 build still
// needs has_avx2() to exist (and unconditionally return false) so call
// sites never need their own preprocessor conditional, which
// tools/checks/check_platform_macros.ps1 forbids under src/ and apps/
// outright. The one genuinely platform-specific piece - the raw hardware
// probe itself, CPUID+XGETBV on real MSVC vs __builtin_cpu_supports
// elsewhere - is the only part selected by directory (see this file's own
// .cpp and src/forge/CMakeLists.txt's AC3FORGE_AVX2 block); everything
// else here (caching, the debug override, the abort-not-fault guarantee)
// is ordinary portable C++, so it lives once instead of being duplicated
// per platform the way the arch seam's own primitives sometimes have to be.
// ---------------------------------------------------------------------------

namespace ac3::internal::cpu {

// True if it is safe to execute AVX2 instructions on the CPU this process is
// currently running on. Resolved exactly once per process via a
// function-local static (C++11 magic statics - thread-safe on every
// compiler in this project's matrix); every call after the first is a load
// of an already-computed bool, not a CPUID instruction.
//
// Honours AC3FORGE_SIMD_TIER=auto|sse2|avx2 (read once, inside the same
// static initialisation) so a build with the AVX2 tier compiled in can be
// forced down to SSE2 for a reproducibility comparison, or forced up to
// prove the AVX2 kernels execute (and are compared bit-for-bit against the
// SSE2 baseline) on a machine that actually has it - see
// tests/core/test_simd_kernels.cpp and tools/ci/run_codec_matrix.sh. Forcing
// up on a CPU that cannot actually run AVX2, or on a build with no AVX2
// tier compiled in at all (AC3FORGE_AVX2=OFF, or a non-x86-64 target),
// aborts with a clear message rather than ever letting an illegal
// instruction fault stand in for one.
[[nodiscard]] bool has_avx2() noexcept;

}  // namespace ac3::internal::cpu
