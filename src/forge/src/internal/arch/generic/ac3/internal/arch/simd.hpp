#pragma once

#include <cmath>
#include <cstdint>

// ---------------------------------------------------------------------------
// The portable (no-SIMD) member of the arch seam.
//
// One of src/forge/src/internal/arch/{generic,x86_64,aarch64}/ is put on
// forge_objects's PRIVATE include path by src/forge/CMakeLists.txt, so every
// `#include "ac3/internal/arch/simd.hpp"` in the codec core resolves to
// exactly one of these three identically-pathed headers - the mechanism
// src/internal/profiling/tracy_{enabled,disabled}/ already uses for the
// profiling seam, and src/audio's backend tree uses for the operating
// system. No translation unit ever asks which architecture it is being
// compiled for, which is what tools/checks/check_platform_macros.ps1
// enforces (no preprocessor conditional anywhere under src/ or apps/).
//
// WHAT THE SEAM IS. Two vector types, both 128 bits wide - two doubles or
// four 32-bit integers. 128 bits is not a compromise width picked to keep
// the code simple: it is the native width of every platform this work names
// as the reason to do it at all (NEON on the Raspberry Pi and the Shield's
// Tegra X1, WASM's simd128), and it is the only x86-64 width that needs no
// -march= flag and therefore no runtime dispatch. Anything wider on x86-64
// (AVX/AVX2's 256 bits) is a CPU-feature question, not an architecture
// question, and belongs behind a cpuid dispatch this seam deliberately does
// not have - see docs/building.md.
//
// WHY IT IS THIS SMALL. Every kernel that uses the seam lives ONCE, in
// shared code (mdct.cpp's dct4_scaled and IMDCT twiddle stages,
// bitalloc.cpp's exponents_to_psd, exponents.cpp's to_fixed25_block,
// fft.cpp's dft512 normalisation), written against these types. The
// per-architecture directories carry the type and its operations, not a
// copy of each kernel. That is what makes the bit-exactness argument
// tractable: every operation below maps to a single IEEE-754
// add/subtract/multiply (or a single exact integer operation) per lane, so
// a kernel written against f64x2 performs exactly the operations, in
// exactly the order, that the scalar loop it replaced performed -
// tests/core/test_simd_kernels.cpp asserts that bit-for-bit for every
// primitive here, and the kernels built from them inherit the guarantee
// rather than needing their own bit-exact unit test (see that file's own
// header comment). fft_kernel.hpp's radix-4 FFT/DCT-IV core (ROADMAP PF4)
// is NOT part of this seam - it is an algorithmic change, not a
// wider-lane one, and carries its own correctness argument.
//
// FUSED MULTIPLY-ADD is the one way that argument can fail, and it is
// disabled project-wide rather than worked around here: the top-level
// CMakeLists.txt pins -ffp-contract=off. See that file's own comment, and
// docs/building.md's "Floating-point contraction" section.
//
// NaN is out of contract for every operation below. Nothing in the codec
// core produces one (the fuzz harnesses assert that at the API boundary),
// round_ties_away propagates it because the instruction sequences happen to,
// and no other member of the seam makes a promise about it.
// ---------------------------------------------------------------------------

namespace ac3::internal::arch {

// Reported by version_details() (`--version`) so a binary says which of the
// three directories it was built from, and printed by
// tests/core/test_simd_kernels.cpp so a CI log does too.
inline constexpr const char* kSimdName = "generic";

// Two IEEE-754 doubles. Deliberately an aggregate of two named scalars
// rather than an array: the generic build is the reference the other two are
// measured against, so every operation below should read as plainly the
// scalar arithmetic the kernel would otherwise have written by hand.
struct f64x2 {
    double lo{};
    double hi{};

    [[nodiscard]] static f64x2 load(const double* p) { return f64x2{p[0], p[1]}; }
    [[nodiscard]] static f64x2 set(double a, double b) { return f64x2{a, b}; }
    [[nodiscard]] static f64x2 broadcast(double a) { return f64x2{a, a}; }

    void store(double* p) const {
        p[0] = lo;
        p[1] = hi;
    }

    [[nodiscard]] double lane0() const { return lo; }
    [[nodiscard]] double lane1() const { return hi; }
};

[[nodiscard]] inline f64x2 operator+(f64x2 a, f64x2 b) {
    return f64x2{a.lo + b.lo, a.hi + b.hi};
}
[[nodiscard]] inline f64x2 operator-(f64x2 a, f64x2 b) {
    return f64x2{a.lo - b.lo, a.hi - b.hi};
}
[[nodiscard]] inline f64x2 operator*(f64x2 a, f64x2 b) {
    return f64x2{a.lo * b.lo, a.hi * b.hi};
}
[[nodiscard]] inline f64x2 operator-(f64x2 a) { return f64x2{-a.lo, -a.hi}; }

// IEEE-754 roundToIntegralTiesAway, lane by lane: exactly std::round() for
// every finite input, NaN propagated. The contract every member of the seam
// holds to - see the x86_64 header for the one that has to construct it out
// of SSE2 arithmetic rather than name it in a single instruction.
[[nodiscard]] inline f64x2 round_ties_away(f64x2 a) {
    return f64x2{std::round(a.lo), std::round(a.hi)};
}

// Four 32-bit signed integers. Only the operations §7.2.2.2's
// exponent-to-PSD conversion needs, all of them exact by construction.
struct i32x4 {
    std::int32_t v0{};
    std::int32_t v1{};
    std::int32_t v2{};
    std::int32_t v3{};

    // Four consecutive unsigned bytes, zero-extended to 32 bits. AC-3
    // exponents are 0..24, so nothing here ever sees a value that would make
    // the widening lossy or the sign ambiguous.
    [[nodiscard]] static i32x4 load_u8_widen(const std::uint8_t* p) {
        return i32x4{static_cast<std::int32_t>(p[0]), static_cast<std::int32_t>(p[1]),
                     static_cast<std::int32_t>(p[2]), static_cast<std::int32_t>(p[3])};
    }
    [[nodiscard]] static i32x4 broadcast(std::int32_t a) { return i32x4{a, a, a, a}; }

    void store(std::int32_t* p) const {
        p[0] = v0;
        p[1] = v1;
        p[2] = v2;
        p[3] = v3;
    }
};

[[nodiscard]] inline i32x4 operator-(i32x4 a, i32x4 b) {
    return i32x4{a.v0 - b.v0, a.v1 - b.v1, a.v2 - b.v2, a.v3 - b.v3};
}

// Logical left shift by a compile-time count. The inputs are the
// zero-extended exponents above, so this never shifts a set sign bit out.
template <int Bits>
[[nodiscard]] i32x4 shift_left(i32x4 a) {
    return i32x4{a.v0 << Bits, a.v1 << Bits, a.v2 << Bits, a.v3 << Bits};
}

}  // namespace ac3::internal::arch
