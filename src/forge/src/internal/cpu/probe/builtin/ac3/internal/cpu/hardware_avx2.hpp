#pragma once

// ---------------------------------------------------------------------------
// The GCC/Clang/AppleClang member of the CPU-probe seam - real x86-64
// hardware, real AVX2 tier compiled in, selected whenever the compiler is
// NOT MSVC and NOT clang-cl (see src/forge/CMakeLists.txt's AC3FORGE_AVX2
// block, and cpu_features.hpp for what the seam is and why it exists).
//
// clang-cl deliberately does NOT use this file even though it is Clang
// underneath: __builtin_cpu_supports needs compiler-rt's cpu-model support
// linked in, which is a given on a normal Clang toolchain but not
// guaranteed under clang-cl's usual MSVC-CRT-linked configuration this
// project uses - see the msvc/ sibling directory, which clang-cl shares
// with real MSVC for exactly that reason.
// ---------------------------------------------------------------------------

namespace ac3::internal::cpu {

// __builtin_cpu_supports("avx2") already performs both halves of the real
// question - the CPUID AVX2 leaf bit AND the OS's XSAVE/XGETBV support for
// the YMM register state - correctly; hand-rolling that sequence (as the
// msvc/ sibling has to) would only risk getting the same two checks wrong a
// second time.
[[nodiscard]] inline bool cpuid_reports_avx2() noexcept {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
}

}  // namespace ac3::internal::cpu
