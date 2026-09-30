#pragma once

#include <cmath>
#include <numbers>

// A second-order IIR section in float throughout - state, coefficients and the
// arithmetic - for the renderer's bass-management crossover
// (ac3/render/render.hpp) and the identify tone's low band
// (ac3/render/identify.hpp).
//
// Deliberately NOT ac3::dsp::Biquad (ac3/dsp/biquad.hpp): that one accumulates
// in double even though its interface is float, which is free on the
// desktop-class hardware its callers run on and expensive on a board these
// headers also serve. Every double operation on the ESP32-S3's PIE is a
// software call, and Direct Form II Transposed is nine of them a sample (5
// multiplies, 4 adds); the stream player's level meter once cost twice the
// decode it was measuring by squaring every sample in double
// (docs/platforms/bare-metal/esp32-s3.md), which is the same trap at a larger
// scale. A small duplicated type, rather than templating the shared one onto
// callers with different precision needs.
//
// The coefficients are the RBJ Audio EQ Cookbook's low-pass and high-pass
// (public-domain DSP, not sourced from any particular codebase), computed in
// double - once per configuration, not per sample - and narrowed to the float
// the per-sample arithmetic uses. Moved out of LayoutRenderer unchanged, so the
// crossover's arithmetic is what it was.

namespace ac3::render {

struct FloatBiquad {
    float b0 = 1.0F, b1 = 0.0F, b2 = 0.0F, a1 = 0.0F, a2 = 0.0F;
    float z1 = 0.0F, z2 = 0.0F;

    // Butterworth Q for a single second-order section - the standard
    // maximally-flat choice, and what gives a matched low-pass/high-pass pair
    // a flat combined response through the corner.
    static constexpr double kButterworthQ = 0.70710678118654752;  // 1/sqrt(2)

    // Direct Form II Transposed: two state variables, no separate input and
    // output delay lines to keep in sync - the structure ac3::dsp::Biquad uses.
    float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    // Silence in the state; the coefficients stay.
    void reset() {
        z1 = 0.0F;
        z2 = 0.0F;
    }

    // New coefficients, keeping the state.
    void set_lowpass(double corner_hz, double sample_rate_hz) {
        const double omega = 2.0 * std::numbers::pi * corner_hz / sample_rate_hz;
        const double cos_omega = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * kButterworthQ);
        const double a0 = 1.0 + alpha;
        const double b0_unscaled = (1.0 - cos_omega) / 2.0;
        b0 = static_cast<float>(b0_unscaled / a0);
        b1 = static_cast<float>((1.0 - cos_omega) / a0);
        b2 = b0;
        a1 = static_cast<float>((-2.0 * cos_omega) / a0);
        a2 = static_cast<float>((1.0 - alpha) / a0);
    }

    void set_highpass(double corner_hz, double sample_rate_hz) {
        const double omega = 2.0 * std::numbers::pi * corner_hz / sample_rate_hz;
        const double cos_omega = std::cos(omega);
        const double alpha = std::sin(omega) / (2.0 * kButterworthQ);
        const double a0 = 1.0 + alpha;
        const double b0_unscaled = (1.0 + cos_omega) / 2.0;
        b0 = static_cast<float>(b0_unscaled / a0);
        b1 = static_cast<float>(-(1.0 + cos_omega) / a0);
        b2 = b0;
        a1 = static_cast<float>((-2.0 * cos_omega) / a0);
        a2 = static_cast<float>((1.0 - alpha) / a0);
    }

    [[nodiscard]] static FloatBiquad lowpass(double corner_hz, double sample_rate_hz) {
        FloatBiquad out;
        out.set_lowpass(corner_hz, sample_rate_hz);
        return out;
    }

    [[nodiscard]] static FloatBiquad highpass(double corner_hz, double sample_rate_hz) {
        FloatBiquad out;
        out.set_highpass(corner_hz, sample_rate_hz);
        return out;
    }
};

}  // namespace ac3::render
