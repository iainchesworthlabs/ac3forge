#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"
#include "iclforge/ac4core/dsp/scalar_traits.hpp"

// Advanced coupling's signal processing, ETSI TS 103 190-1 V1.4.1 clause 5.7.7:
// the parameter bands (5.7.7.2, Table 197), interpolation (5.7.7.3,
// Pseudocodes 109 and 110), the decorrelators (5.7.7.4.2, Pseudocode 111 and
// Tables 198 to 201), the transient ducker (5.7.7.4.3, Pseudocodes 112 to
// 114), and differential decoding and dequantisation (5.7.7.7, Pseudocode 121
// and Tables 203 to 208).
//
// The decoder runs them on its QMF matrices, after A-SPX; the encoder runs them
// to see what a decoder will make of the parameters it sends. A matrix of
// slots is laid out as the QMF banks' (dsp/qmf.hpp): value [ts * 64 + sb].

namespace iclforge::ac4::detail::acpl {

inline constexpr int kSubbands = 64;       // num_qmf_subbands
inline constexpr int kMaxSlots = 32;       // num_qmf_timeslots of a 2 048-sample frame, the longest
inline constexpr int kMaxParamBands = 15;  // Table 143
inline constexpr int kMaxParamSets = 2;    // Table 146
inline constexpr int kDecorrelators = 3;   // D0, D1 and D2

// Table 197: the parameter band of QMF subband `subband` (0 to 63) when there
// are `num_param_bands` (15, 12, 9 or 7) of them; -1 for anything else.
[[nodiscard]] int sb_to_pb(int num_param_bands, int subband) noexcept;

// --- Differential decoding and dequantisation (5.7.7.7) ---

// The parameters Pseudocode 121 decodes, by how they dequantise: alpha by
// Table 203 or 205, beta by Table 204 or 206 at its alpha's ibeta, beta3 and
// gamma by the steps of Tables 207 and 208.
enum class Kind : std::uint8_t { kAlpha, kBeta, kBeta3, kGamma };

// acpl_quant_mode and its kin (Table 144): 0 fine, 1 coarse.
enum class Quant : std::uint8_t { kFine, kCoarse };

// The quantised values a kind takes, from its F0 codebook: alpha 0 to 32 (16
// coarse), beta 0 to 8 (4), beta3 0 to 16 (8), gamma -20 to 20 (-10 to 10).
struct Range {
    int min = 0;
    int max = 0;
};
[[nodiscard]] Range quantised_range(Kind kind, Quant quant) noexcept;

// Pseudocode 121 for one parameter set of one parameter, from `start_band`
// (src/ac4dec/ERRATA.md, "Partial coupling starts at acpl_param_band"):
// `coded` holds huff_decode_diff()'s values, each codebook index less its
// cb_off, for bands start_band to num_bands - 1. DIFF_FREQ takes the first
// as it is and adds each next one to the band below; DIFF_TIME adds each to
// `previous`, the same band's value in the parameter set before, which may be
// the last one of the previous frame. Bands below start_band are 0. False,
// with `out` undefined, when a value leaves the kind's range.
[[nodiscard]] bool differential_decode(Kind kind, Quant quant, bool diff_time, int start_band,
                                       int num_bands, std::span<const int, kMaxParamBands> coded,
                                       std::span<const int, kMaxParamBands> previous,
                                       std::span<int, kMaxParamBands> out) noexcept;

// Table 203 or 205: alpha_dq, and the ibeta its beta dequantises with. `q`
// must lie in quantised_range(kAlpha, quant).
struct AlphaValue {
    double alpha = 0.0;
    int ibeta = 0;
};
[[nodiscard]] AlphaValue dequantise_alpha(int q, Quant quant) noexcept;

// Table 204 or 206: beta_dq at column `ibeta`, from dequantise_alpha().
[[nodiscard]] double dequantise_beta(int q, int ibeta, Quant quant) noexcept;

// Tables 207 and 208: the step that multiplies a beta3 or a gamma value.
[[nodiscard]] double beta3_step(Quant quant) noexcept;
[[nodiscard]] double gamma_step(Quant quant) noexcept;

// --- Interpolation (5.7.7.3) ---

// acpl_framing_data(): the interpolation and parameter sets of one data element.
struct Framing {
    bool steep = false;       // acpl_interpolation_type 1
    int num_param_sets = 1;   // acpl_num_param_sets: 1 or 2
    std::array<int, kMaxParamSets> param_timeslot{};  // for steep interpolation
};

// One parameter's dequantised values as a frame sends them, [set][band].
using ParamSets = std::array<std::array<double, kMaxParamBands>, kMaxParamSets>;

// A parameter's value per subband at the end of the previous frame,
// acpl_param_prev (Pseudocode 110): 0 before the first frame.
using ParamPrev = std::array<double, kSubbands>;

// Pseudocode 109 at one slot, from what the subbands of one parameter band share: the band's value
// in each of the frame's sets and acpl_param_prev. The expression is the pseudocode's, operation
// for operation, and so are its bits; what it saves is work the pseudocode repeats. The ramp's (ts
// + 1) is a table entry, not a conversion of an integer; and a division by a power of two is a
// multiplication by its reciprocal, which gives the bits of the division because the reciprocal is
// exact and both are correctly rounded (a frame of 2 048 samples has 32 slots, its halves 16). The
// other divisors, 24 and 30 slots and their halves, divide.
// tests/ac4core/test_ac4core_acpl_exact.cpp holds it to the expression as written.
class Interpolator {
   public:
    // One band's values for a frame, and the two differences the ramps multiply.
    struct Column {
        double prev = 0.0;    // acpl_param_prev
        double first = 0.0;   // set 0's value
        double second = 0.0;  // set 1's value
        double rise = 0.0;    // first - prev
        double step = 0.0;    // second - first
    };

    Interpolator(const Framing& framing, int num_ts) noexcept;

    [[nodiscard]] static Column column(double prev, double first, double second) noexcept {
        return {prev, first, second, first - prev, second - first};
    }

    // The value at slot `ts`, 0 to num_ts - 1.
    [[nodiscard]] double at(const Column& column, int ts) const noexcept;

   private:
    // x / n for one of Pseudocode 109's divisors.
    struct Divisor {
        double n = 1.0;
        double reciprocal = 1.0;
        bool by_multiply = false;

        Divisor() = default;
        explicit Divisor(int divisor) noexcept;
        [[nodiscard]] double operator()(double x) const noexcept {
            return by_multiply ? x * reciprocal : x / n;
        }
    };

    bool steep_ = false;
    bool two_ = false;
    int half_ = 0;  // num_ts / 2
    std::array<int, kMaxParamSets> slot_{};
    Divisor whole_;
    Divisor first_half_;
    Divisor second_half_;
};

// Pseudocode 109 at every slot and subband of a frame of `num_ts` slots:
// out[ts * 64 + sb] = interpolate(values, num_param_sets, sb, ts). A subband that shares its band
// and its acpl_param_prev with the one before it takes that one's values, which are the same to the
// bit.
void interpolate(const Framing& framing, int num_param_bands, const ParamSets& values,
                 const ParamPrev& prev, int num_ts, std::span<double> out) noexcept;

// Pseudocode 110: the last parameter set's value in each subband's band.
void end_frame(const Framing& framing, int num_param_bands, const ParamSets& values,
               ParamPrev& prev) noexcept;

// --- Decorrelator and transient ducker (5.7.7.4) ---

// Table 198: the subband regions k0 (0 to 6), k1 (7 to 22) and k2 (23 to
// 63), their delays in slots and their filter lengths; Tables 199 to 201: the
// coefficients a[i], i = 0 to the filter length, of D0, D1 and D2 in each.
struct Region {
    int first_subband = 0;
    int delay = 0;
    int length = 0;
};
inline constexpr std::array<Region, 3> kRegions = {{{0, 7, 7}, {7, 10, 4}, {23, 12, 2}}};
[[nodiscard]] int region_of(int subband) noexcept;
[[nodiscard]] std::span<const double> coefficients(int decorrelator, int region) noexcept;
// The same coefficients in Q1.30, each the double times 2^30 rounded to the nearest, for the
// fixed-point tier's decorrelators.
[[nodiscard]] std::span<const std::int32_t> coefficients_q30(int decorrelator, int region) noexcept;

// Pseudocode 111: decorrelator `index` (0 to 2), a delay and an all-pass IIR
// filter per subband, with each subband's input and output history from the
// frames before. Real coefficients on complex samples. At Fixed32 each output's
// products are summed in 64 bits, the coefficients in Q1.30, and rounded once.
template <typename Real>
class Decorrelator {
   public:
    using Complex = dsp::Complex<Real>;

    explicit Decorrelator(int index) noexcept;

    // Silence in every subband's history.
    void reset() noexcept;

    // Filters `num_ts` slots of `in` into `out`, which must not overlap;
    // nothing for more than kMaxSlots.
    void process(std::span<const Complex> in, std::span<Complex> out, int num_ts) noexcept;

   private:
    // The longest delay plus filter length (14 in every region), and the
    // longest filter.
    static constexpr int kInputHistory = 14;
    static constexpr int kOutputHistory = 7;

    int index_ = 0;
    // Tables 199 to 201's coefficients for this decorrelator, [region][i], narrowed to Real once
    // and not at every tap of every slot: a conversion from double is a call on a chip without
    // double hardware.
    std::array<std::array<Real, 8>, 3> coefficients_{};
    // The last slots before this frame, oldest first: [k * 64 + sb].
    std::array<Complex, static_cast<std::size_t>(kInputHistory) * kSubbands> x_history_{};
    std::array<Complex, static_cast<std::size_t>(kOutputHistory) * kSubbands> y_history_{};
};

// Pseudocodes 112 to 114: attenuates a decorrelator's output where its energy
// falls faster than its smoothed peak, slot by slot, per parameter band of
// the 15-band mapping. The energy is the decorrelator's output's own, the
// signal the gains apply to (src/ac4dec/ERRATA.md, "The transient ducker's
// energy"). The energies are dsp::Energy values: Real at double and float, a
// mantissa and a power of two at Fixed32.
template <typename Real>
class TransientDucker {
   public:
    using Complex = dsp::Complex<Real>;

    // The state of before the first frame: 0 (the NOTE after Pseudocode 112).
    void reset() noexcept;

    // Ducks `num_ts` slots of `inout` in place.
    void process(std::span<Complex> inout, int num_ts) noexcept;

   private:
    using Energy = dsp::Energy<Real>;
    std::array<Energy, kMaxParamBands> peak_decay_{};
    std::array<Energy, kMaxParamBands> smooth_{};
    std::array<Energy, kMaxParamBands> smooth_peak_diff_{};
};

extern template class Decorrelator<Real>;
extern template class TransientDucker<Real>;

}  // namespace iclforge::ac4::detail::acpl
