#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <span>
#include <vector>

#include "acpl/acpl.hpp"

// Advanced joint object coding's signal processing, ETSI TS 103 190-2 V1.3.1
// clause 5.7: the parameter band mapping (5.7.3.1, Table 28), differential
// decoding (Pseudocode 16), dequantisation (Tables 29 to 32), the
// interpolation (Pseudocode 17), the decorrelators and transient duckers
// (5.7.3.5, A-CPL's of Part 1 clause 5.7.7.4), the reconstruction with its
// decorrelation input matrix (Pseudocode 18 and 5.7.3.6.2), and dialogue
// enhancement in full and core decoding (clauses 5.8.2.3 and 5.8.2.4,
// Pseudocodes 22 to 24).
//
// The decoder runs these on the QMF matrices of the downmix after A-SPX; a
// matrix of slots is laid out as the QMF banks' (dsp/qmf.hpp): value
// [ts * 64 + sb]. src/ac4dec/ERRATA.md, "A-JOC", records the readings: the
// ramp's counter, its comparison, the decorrelation input matrix of objects of
// different band counts, and the rest.

namespace ac4::detail::ajoc {

inline constexpr int kSubbands = acpl::kSubbands;
inline constexpr int kMaxSlots = acpl::kMaxSlots;
inline constexpr int kMaxBands = 23;
inline constexpr int kMaxDataPoints = 2;
inline constexpr int kMaxDecorrelators = 7;

// Table 78: the parameter bands ajoc_num_bands_code gives, 23 down to 1.
[[nodiscard]] int num_bands(int num_bands_code) noexcept;

// Table 28: the parameter band of QMF subband `sb` (0 to 63) for
// `num_bands` bands (one of Table 78's); 0 for another count.
[[nodiscard]] int sb_to_pb(int num_bands, int sb) noexcept;

// Pseudocode 16's nquant: dry 51 coarse (ajoc_quant_select 1) or 101 fine,
// wet 21 or 41.
[[nodiscard]] int nquant(bool wet, int quant_select) noexcept;

// Tables 29 to 32: a quantised value's coefficient, uniform about the range's
// centre in steps of 0.2001953125 (coarse) or 0.10009765625 (fine).
[[nodiscard]] double dequantise(bool wet, int quant_select, int q) noexcept;

// Pseudocode 16 for one coefficient at one data point: `values` are what
// ajoc_huff_data() returned for bands 0 to num_bands - 1. DIFF_FREQ takes the
// first as it is and adds each next one to the band below, modulo nquant;
// DIFF_TIME adds each to `previous`, the same band's value at the data point
// before. False, with `out` undefined, where a DIFF_TIME value leaves 0 to
// nquant - 1.
[[nodiscard]] bool differential_decode(bool diff_time, int nquant, int num_bands,
                                       std::span<const int> values,
                                       std::span<const int, kMaxBands> previous,
                                       std::span<int, kMaxBands> out) noexcept;

// 5.7.3.5: decorrelator de (0 to 6) is A-CPL's D0, D2, D1, D0, D2, D1, D0.
[[nodiscard]] int decorrelator_of(int de) noexcept;

// One frame's parameters, dequantised (clause 5.7.3.3).
struct FrameParameters {
    int num_dmx = 0;
    int num_umx = 0;
    int num_decorr = 0;
    std::array<bool, kMaxDecorrelators> decorr_enable{};
    int num_dpoints = 0;
    std::array<int, kMaxDataPoints> start_pos{};
    std::array<int, kMaxDataPoints> ramp_len{};
    // Per object, its parameter bands; an object not present has 1 and every
    // coefficient 0 (5.7.3.3).
    std::vector<int> num_bands;
    // dry[((o * kMaxDataPoints + dp) * num_dmx + ch) * kMaxBands + pb] and
    // wet[((o * kMaxDataPoints + dp) * kMaxDecorrelators + de) * kMaxBands + pb].
    std::vector<double> dry;
    std::vector<double> wet;

    void resize(int dmx, int umx);
    [[nodiscard]] double& dry_at(int o, int dp, int ch, int pb);
    [[nodiscard]] double& wet_at(int o, int dp, int de, int pb);
    [[nodiscard]] double dry_at(int o, int dp, int ch, int pb) const;
    [[nodiscard]] double wet_at(int o, int dp, int de, int pb) const;
};

// The reconstruction of one A-JOC substream, frame after frame: the
// interpolation state of every coefficient in every subband, the ramp's
// counter, and the decorrelators' and duckers' history.
template <typename Real>
class Reconstruction {
   public:
    using Complex = std::complex<Real>;

    Reconstruction();

    // Every coefficient 0 and no ramp under way, as before the first frame;
    // silence in the decorrelators.
    void reset();

    // Pseudocode 18 over `num_ts` slots: `z` (num_umx matrices) from `x`
    // (num_dmx, the downmix in QinAJOC's order). Where `de_gain` is above 1,
    // the coefficients of the objects `dialogue` flags are scaled by it first
    // (Pseudocode 22, clause 5.8.2.3), after the decorrelation input matrix
    // is taken from them.
    void reconstruct(const FrameParameters& p, int num_ts,
                     std::span<const std::vector<Complex>* const> x,
                     std::span<std::vector<Complex>* const> z, double de_gain,
                     std::span<const std::uint8_t> dialogue);

    // Core decoding's dialogue enhancement (clause 5.8.2.4) on the downmix
    // in place: y = H_M H_A x + x, H_A the interpolated dry coefficients of
    // the dialogue objects scaled by `de_gain` (10^(G/20) - 1), and H_M the
    // dialogue objects' downmix coefficients `coeff` ([dialogue object][ch],
    // the objects `dialogue` flags in order), ramped from the last frame's
    // over the frame. The interpolation runs for every object, so its state
    // is ready whichever objects later carry dialogue.
    void enhance_core(const FrameParameters& p, int num_ts,
                      std::span<std::vector<Complex>* const> x, double de_gain,
                      std::span<const std::uint8_t> dialogue, std::span<const double> coeff);

   private:
    // One set of interpolated values and their ramps: Pseudocode 18's
    // mtx_*_prev and delta_inc_*, per subband.
    struct Ramped {
        std::vector<Real> prev;
        std::vector<Real> delta;
        void resize(std::size_t n);
    };

    // What each slot of the frame does: whether the ramp moves (Pseudocode
    // 17's comparison, on the counter as the slot starts), and which data
    // point, if any, starts at it.
    struct Schedule {
        std::array<bool, kMaxSlots> moves{};
        std::array<int, kMaxSlots> starts{};
    };
    [[nodiscard]] Schedule schedule(const FrameParameters& p, int num_ts);
    void configure(const FrameParameters& p);

    int num_dmx_ = -1;
    int num_umx_ = -1;
    int curr_ramp_len_ = 0;
    int target_ramp_len_ = 0;
    Ramped dry_;                     // [(sb * num_umx + o) * num_dmx + ch]
    Ramped wet_;                     // [(sb * num_umx + o) * kMaxDecorrelators + de]
    Ramped pre_;                     // [(sb * kMaxDecorrelators + de) * num_dmx + ch]
    std::vector<double> pre_param_;  // [((dp * 64 + sb) * kMaxDecorrelators + de) * num_dmx + ch]
    FrameParameters scaled_;         // the frame's coefficients after dialogue enhancement
    std::array<acpl::Decorrelator<Real>, kMaxDecorrelators> decorrelators_;
    std::array<acpl::TransientDucker<Real>, kMaxDecorrelators> duckers_;
    std::array<bool, kMaxDecorrelators> ran_{};
    std::array<std::vector<Complex>, kMaxDecorrelators> u_;
    std::array<std::vector<Complex>, kMaxDecorrelators> y_;
    std::vector<Real> h_m_prev_;  // [o * num_dmx + ch], H'_M of the last frame by upmix object
    std::vector<Real> h_m_;
};

extern template class Reconstruction<double>;

}  // namespace ac4::detail::ajoc
