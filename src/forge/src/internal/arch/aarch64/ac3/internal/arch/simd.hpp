#pragma once

#include <arm_neon.h>

#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// The AArch64 (Advanced SIMD / NEON) member of the arch seam. See
// src/forge/src/internal/arch/generic/ac3/internal/arch/simd.hpp for what the
// seam is, how CMake selects between the three directories, and why no
// header here needs a preprocessor conditional to name its architecture.
//
// Everything below is base ARMv8-A: double-precision Advanced SIMD is
// mandatory in the 64-bit architecture, so - exactly as with SSE2 on x86-64 -
// there is no feature to detect, no -march= flag to pass, and no runtime
// dispatch. This is the half of the seam the work exists for: the Raspberry
// Pi 4B and the Shield's Tegra X1 are the platforms with the least headroom
// (docs/platforms/raspberry-pi.md, docs/platforms/android.md).
//
// AArch64 is also the half where FUSED MULTIPLY-ADD is a live hazard rather
// than a theoretical one. FMLA/FMLS are base instructions here, GCC and
// Clang both contract by default, and a contracted a*b - c*d keeps one extra
// rounding step's worth of precision - which sounds harmless and is not, in
// a codec whose exponent extraction turns a last-bit difference into a whole
// 6.02 dB exponent step. -ffp-contract=off is pinned in the top-level
// CMakeLists.txt for exactly this reason; the intrinsics below are then
// genuinely one IEEE-754 operation each, which is what makes them
// bit-identical to the scalar reference. vfmaq_f64 is deliberately absent
// from this file.
// ---------------------------------------------------------------------------

namespace ac3::internal::arch {

inline constexpr const char* kSimdName = "aarch64-neon";

struct f64x2 {
    float64x2_t v;

    [[nodiscard]] static f64x2 load(const double* p) { return f64x2{vld1q_f64(p)}; }
    [[nodiscard]] static f64x2 set(double a, double b) {
        return f64x2{vsetq_lane_f64(b, vdupq_n_f64(a), 1)};
    }
    [[nodiscard]] static f64x2 broadcast(double a) { return f64x2{vdupq_n_f64(a)}; }

    void store(double* p) const { vst1q_f64(p, v); }

    [[nodiscard]] double lane0() const { return vgetq_lane_f64(v, 0); }
    [[nodiscard]] double lane1() const { return vgetq_lane_f64(v, 1); }
};

[[nodiscard]] inline f64x2 operator+(f64x2 a, f64x2 b) { return f64x2{vaddq_f64(a.v, b.v)}; }
[[nodiscard]] inline f64x2 operator-(f64x2 a, f64x2 b) { return f64x2{vsubq_f64(a.v, b.v)}; }
[[nodiscard]] inline f64x2 operator*(f64x2 a, f64x2 b) { return f64x2{vmulq_f64(a.v, b.v)}; }
[[nodiscard]] inline f64x2 operator-(f64x2 a) { return f64x2{vnegq_f64(a.v)}; }

// FRINTA: "round to integral, to nearest with ties away from zero" - the
// IEEE-754 roundToIntegralTiesAway operation, which is precisely what
// std::round() is specified to compute, in one instruction and for every
// input including infinities and NaN. The x86_64 header has to construct
// this out of SSE2 arithmetic; here it is simply the instruction's own
// definition, so the seam's contract holds with nothing to argue about.
[[nodiscard]] inline f64x2 round_ties_away(f64x2 a) { return f64x2{vrndaq_f64(a.v)}; }

// Four IEEE-754 single-precision floats. See the generic header for what this
// type is for and why it carries no round_ties_away.
//
// Single-precision Advanced SIMD is fully IEEE-754 in AArch64: the vector
// unit reads the same FPCR the scalar one does, whose default has FZ clear,
// so vaddq_f32 and its siblings produce the floats scalar `float` arithmetic
// produces, denormals included. That is what makes this type's bit-exactness
// claim the same claim f64x2 makes. It is worth stating because it is a
// property of A64 specifically - AArch32's Advanced SIMD flushed
// single-precision denormals to zero whatever FPCR said, and a lane type like
// this could not have been bit-exact against a scalar reference there.
//
// -ffp-contract=off (top-level CMakeLists.txt) carries the same weight here
// as it does for f64x2: FMLA contracts a*b - c*d just as readily in single
// precision, and a float has 29 fewer significand bits for the extra rounding
// step to hide in. vfmaq_f32 is deliberately absent, as vfmaq_f64 is.
struct f32x4 {
    float32x4_t v;

    [[nodiscard]] static f32x4 load(const float* p) { return f32x4{vld1q_f32(p)}; }
    [[nodiscard]] static f32x4 set(float a, float b, float c, float d) {
        float32x4_t r = vdupq_n_f32(a);
        r = vsetq_lane_f32(b, r, 1);
        r = vsetq_lane_f32(c, r, 2);
        r = vsetq_lane_f32(d, r, 3);
        return f32x4{r};
    }
    [[nodiscard]] static f32x4 broadcast(float a) { return f32x4{vdupq_n_f32(a)}; }

    void store(float* p) const { vst1q_f32(p, v); }

    [[nodiscard]] float lane0() const { return vgetq_lane_f32(v, 0); }
    [[nodiscard]] float lane1() const { return vgetq_lane_f32(v, 1); }
    [[nodiscard]] float lane2() const { return vgetq_lane_f32(v, 2); }
    [[nodiscard]] float lane3() const { return vgetq_lane_f32(v, 3); }
};

[[nodiscard]] inline f32x4 operator+(f32x4 a, f32x4 b) { return f32x4{vaddq_f32(a.v, b.v)}; }
[[nodiscard]] inline f32x4 operator-(f32x4 a, f32x4 b) { return f32x4{vsubq_f32(a.v, b.v)}; }
[[nodiscard]] inline f32x4 operator*(f32x4 a, f32x4 b) { return f32x4{vmulq_f32(a.v, b.v)}; }
[[nodiscard]] inline f32x4 operator-(f32x4 a) { return f32x4{vnegq_f32(a.v)}; }

struct i32x4 {
    int32x4_t v;

    [[nodiscard]] static i32x4 load_u8_widen(const std::uint8_t* p) {
        // Four bytes -> four lanes. Brought in through a 32-bit scalar
        // rather than vld1_u8, whose narrowest form still READS eight bytes:
        // the caller may be standing on the last four exponents of an
        // allocation, and the upper half would be discarded anyway.
        std::uint32_t packed = 0;
        std::memcpy(&packed, p, sizeof(packed));
        const uint8x8_t bytes = vreinterpret_u8_u32(vdup_n_u32(packed));
        const uint32x4_t widened = vmovl_u16(vget_low_u16(vmovl_u8(bytes)));
        return i32x4{vreinterpretq_s32_u32(widened)};
    }
    [[nodiscard]] static i32x4 broadcast(std::int32_t a) { return i32x4{vdupq_n_s32(a)}; }

    void store(std::int32_t* p) const { vst1q_s32(p, v); }
};

[[nodiscard]] inline i32x4 operator-(i32x4 a, i32x4 b) { return i32x4{vsubq_s32(a.v, b.v)}; }

template <int Bits>
[[nodiscard]] i32x4 shift_left(i32x4 a) {
    return i32x4{vshlq_n_s32(a.v, Bits)};
}

}  // namespace ac3::internal::arch
