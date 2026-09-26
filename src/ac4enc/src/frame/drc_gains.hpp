#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dsp/qmf.hpp"
#include "frame/metadata.hpp"
#include "frame/timing.hpp"

// Transmitted DRC gains (ETSI TS 103 190-1 V1.4.1 clause 5.7.9.3.2), computed
// from a compression curve as a decoder applying the curve would (5.7.9.3.1):
// a level per QMF slot relative to dialnorm, the curve's gain for it, and the
// smoothing of 5.7.9.3.1.2, with its time constants; then each gain the
// stream sends is the smoothed gain over its subframe (Table 169), in whole
// dB2 (6 dB2 a factor of 2, src/ac4dec/ERRATA.md "DRC's units").
//
// The level is the one the decoder's detector reads, which the text leaves to
// the implementation (5.7.9.3.1.1): ITU-R BS.1770's K-weighted power of the
// channels with its channel weights (1.41 at the sides, none for the LFE),
// the K-weighting read at each QMF subband's centre, in LKFS. A profile's
// curve is defined on the programme's level against dialnorm, so every
// channel group (Table 168) and band (Table 164) takes the programme's gain:
// a group or a band measured alone reads quieter, and the curve would cut it
// less, or boost it. What drc_gains_config 1 to 3 add here is the subframes'
// time resolution over config 0's one gain a frame.
//
// Frame f's gains apply to the QMF block the decoder's output stages work
// on when it decodes frame f + d_ctrl: slots num_qmf_timeslots (f + d_ctrl) -
// ts_offset_hfgen on, the synthesis working that far behind the analysis.

namespace ac4::detail {

// A compression curve's control points (dB2) and time constants (ms), from
// drc_compression_curve()'s fields as Table 166 and clauses 4.3.13.4.15 to
// 4.3.13.4.21 give them, or Table 167's defaults.
struct DrcGainCurve {
    double max_boost_gain = 0.0;
    double max_boost_level = 0.0;
    double section_boost_gain = 0.0;
    double section_boost_level = 0.0;
    double null_low = 0.0;
    double null_high = 0.0;
    double section_cut_gain = 0.0;
    double section_cut_level = 0.0;
    double max_cut_gain = 0.0;
    double max_cut_level = 0.0;
    double attack_ms = 100.0;
    double release_ms = 3000.0;
    double attack_fast_ms = 10.0;
    double release_fast_ms = 1000.0;
    bool adaptive = false;
    double attack_threshold = 15.0;
    double release_threshold = 20.0;

    // Clause 5.7.9.3.1.2's piecewise linear gain for a level relative to
    // dialnorm.
    [[nodiscard]] double gain(double level) const noexcept;
};

[[nodiscard]] DrcGainCurve drc_gain_curve(const CurveCodes& codes) noexcept;

// Table 169: DRC subframes a frame.
[[nodiscard]] int drc_subframes(int frame_length) noexcept;

// Where a channel stands for BS.1770's weights and Table 168's groups (with
// add_ch_base 0, which the encoder writes): L, R and a top front pair; C;
// the surrounds; the LFE; a back pair; a wide pair, weighted as the sides
// and grouped with L and R.
enum class DrcChannel : std::uint8_t { kFront, kCentre, kSide, kLfe, kBack, kWide };

class DrcGainEncoder {
   public:
    // `channels` names the input's channels in order; `mono_or_stereo`
    // puts them in one group, as Table 168 does.
    DrcGainEncoder(const DrcGainCurve& curve, int gains_config,
                   std::span<const DrcChannel> channels, bool mono_or_stereo,
                   const FrameTiming& timing, int rate_hz, double dialnorm_db);

    // Analyses slot slots() of each channel, from the 64 samples of the
    // delayed input (full scale 1.0) from 64 slots() - d_pcm.
    void push_slot(std::span<const std::array<double, dsp::kQmfSubbands>> samples);
    [[nodiscard]] long long slots() const noexcept { return slots_; }

    // The slot after the last one frame f's gains read.
    [[nodiscard]] long long slots_needed(long long frame) const noexcept;

    // Frame f's gains, as the slots up to slots_needed(f) give them.
    [[nodiscard]] DrcModeGains gains(long long frame);

   private:
    struct Smoothing {
        bool primed = false;
        double level = 0.0;  // L~, as a power
        double gain = 1.0;   // g~, linear
    };

    DrcGainCurve curve_;
    int gains_config_ = 0;
    FrameTiming timing_;
    double slot_ms_ = 64.0 * 1000.0 / 48000.0;
    double dialnorm_db_ = -31.0;
    int groups_ = 1;
    int bands_ = 1;
    std::vector<double> weight_;  // BS.1770's, per channel
    std::array<double, dsp::kQmfSubbands> k_weight_{};
    double qmf_gain_ = 1.0;  // sum of QWIN^2
    std::vector<dsp::QmfAnalysis<double>> analyses_;
    Smoothing smoothing_;
    // The smoothed gain of each slot analysed and not yet sent, from slot
    // `first_`.
    std::vector<double> pending_;
    long long first_ = 0;
    long long slots_ = 0;
};

}  // namespace ac4::detail
