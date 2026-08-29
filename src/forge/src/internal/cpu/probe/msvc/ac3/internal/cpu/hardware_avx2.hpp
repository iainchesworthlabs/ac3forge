#pragma once

#include <array>
#include <cstdint>

#include <intrin.h>

// ---------------------------------------------------------------------------
// The MSVC/clang-cl member of the CPU-probe seam - real x86-64 hardware,
// real AVX2 tier compiled in, selected whenever the compiler is MSVC proper
// or clang-cl (see src/forge/CMakeLists.txt's AC3FORGE_AVX2 block, and
// cpu_features.hpp for what the seam is and why it exists). clang-cl shares
// this file rather than the GCC/Clang builtin/ sibling: __builtin_cpu_supports
// needs compiler-rt's cpu-model support linked in, which this project's
// clang-cl configuration (MSVC-CRT-linked, not compiler-rt-linked) does not
// guarantee - <intrin.h> has no such dependency on either compiler.
//
// Neither MSVC nor clang-cl has an equivalent of GCC/Clang's
// __builtin_cpu_supports, so both halves of the real question are done by
// hand, IN ORDER - each step is a precondition for the next, and skipping
// the order can fault:
//   1. CPUID leaf 0 - is leaf 7 (where the AVX2 bit lives) even present.
//   2. CPUID leaf 1, ECX bits 27 (OSXSAVE) and 28 (AVX) - is the OS
//      exposing the XSAVE mechanism at all, and does the CPU have AVX.
//      XGETBV itself is only a legal instruction once OSXSAVE is set;
//      running it before confirming that can fault on real hardware.
//   3. XGETBV(0), XCR0 bits 1-2 - has the OS actually enabled saving the
//      XMM and YMM register state (a CPU can report AVX2 in CPUID while an
//      old OS has never turned this on for the wider registers; running
//      AVX2 instructions in that case is undefined, not merely slow).
//   4. CPUID leaf 7, EBX bit 5 - the AVX2 bit itself, now that steps 1-3
//      have established it is safe to act on if it is set.
// ---------------------------------------------------------------------------

namespace ac3::internal::cpu {

[[nodiscard]] inline bool cpuid_reports_avx2() noexcept {
    std::array<int, 4> leaf{};

    __cpuid(leaf.data(), 0);
    const int highest_leaf = leaf[0];
    if (highest_leaf < 7) {
        return false;
    }

    __cpuidex(leaf.data(), 1, 0);
    constexpr int kOsxsaveBit = 1 << 27;
    constexpr int kAvxBit = 1 << 28;
    if ((leaf[2] & kOsxsaveBit) == 0 || (leaf[2] & kAvxBit) == 0) {
        return false;
    }

    constexpr std::uint64_t kXmmYmmState = 0x6;
    const std::uint64_t xcr0 = _xgetbv(0);
    if ((xcr0 & kXmmYmmState) != kXmmYmmState) {
        return false;
    }

    __cpuidex(leaf.data(), 7, 0);
    constexpr int kAvx2Bit = 1 << 5;
    return (leaf[1] & kAvx2Bit) != 0;
}

}  // namespace ac3::internal::cpu
