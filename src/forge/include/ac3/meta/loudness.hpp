#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

// Programme loudness to ITU-R BS.1770-4, and the dialnorm (§5.4.2.8) that
// follows from it. Also the rest of an R128-style meter built on the same
// K-weighted, channel-summed signal: momentary/short-term loudness
// (BS.1770-4 §2's un-gated block power at two window sizes), Loudness Range
// (EBU Tech 3342's own gated-percentile statistic) and true-peak level
// (BS.1770-4 Annex 2's oversampled peak, independent of the loudness path
// entirely). Roadmap item C1.
//
// A/52 defines dialnorm as how far "the average dialogue level is below
// digital 100 percent" and never says how to measure it — the standard is
// from 1995 and BS.1770 is from 2006, so in the spec's own era the answer was
// a trained listener with a level meter. Every modern delivery specification
// that fills the gap names BS.1770 gated loudness (ATSC A/85, EBU R 128), so
// that is what this encoder measures, and dialnorm is the negated integrated
// loudness rounded to the nearest dB. For material with no dialogue at all —
// music, effects — the integrated programme loudness IS the anchor A/85 asks
// for, so one measurement serves both cases.
//
// The measurement is inherently two-pass: the relative gate needs the whole
// programme before any block's contribution is known. A streaming encoder
// therefore cannot derive dialnorm from the frame it is encoding; the caller
// measures first and configures the encoder second, which is exactly what
// real encoders do with an analysis pass.

namespace ac3::meta {

// ITU-R BS.1770-5 (11/2023) Annex 3, "Extended loudness measurement algorithm
// for loudspeaker configurations of advanced sound systems", Table 4: the
// weighting coefficient Gi of a channel depends only on where that channel
// sits, given as an azimuth (theta) and an elevation (phi). Gi is
// 1.41 (+1.5 dB) when |phi| < 30 degrees AND 60 <= |theta| <= 120 degrees,
// and 1.00 (0 dB) everywhere else - including every upper-layer position,
// which is outside the |phi| < 30 row entirely. Annex 3 changes nothing else
// about the algorithm: "First, second and fourth stages of the algorithm
// (filtering and gating procedure) are the same as in the algorithm for the
// 3/2 multichannel format".
//
// Each Table E2.5 location below is placed at the BS.2051 loudspeaker label
// it stands for and its weight read off Table 4. Table 5 of the same Annex
// then tabulates that weight per BS.2051 configuration, which gives a second
// and independent check on every value: M+000/M±030 and M±SC at 1.00,
// M±060/M±090/M±110 at 1.41, M±135/M+180 at 1.00, and every U/T/B entry at
// 1.00.
//
// std::nullopt for the two LFE-type locations. That is not a zero weight:
// Annex 3 weights "each channel except the LFE channels", so an LFE-type
// channel is not a term in the sum at all - which is also why true peak,
// which does measure it, reads it from a separate path.
[[nodiscard]] AC3FORGE_EXPORT std::optional<double> position_weight(
    eac3::chanmap::Location location);

class AC3FORGE_EXPORT LoudnessMeter {
   public:
    // BS.1770 Annex 1's basic algorithm for the 3/2 multichannel system:
    // channel weights follow its Table 3 - unity for the front channels,
    // +1.5 dB for the surrounds, and the LFE excluded outright - keyed on the
    // Table 5.8 acmod, whose coded order push() then expects.
    //
    // Table 3 names exactly five channels (L, R, C, Ls, Rs), so the lone
    // surround of 2/1 and 3/1 is not in it. This constructor reads that
    // surround as the surround FIELD collapsed to one channel - the pair's
    // own +1.5 dB - which is what A/52's own downmix does with it (it feeds
    // both surround outputs). The Annex 3 constructor below reaches the other
    // answer for the same coded channel, because there it is Table E2.5's Cs,
    // a discrete rear centre at 180 degrees, and Table 4 puts 180 degrees
    // outside the +1.5 dB sector. Both are faithful to their own algorithm;
    // the two algorithms genuinely differ here, and that is the only layout
    // for which they do.
    LoudnessMeter(SampleRate rate, Acmod acmod, bool lfe);

    // BS.1770-5 Annex 3's extended algorithm over a rendered Table E2.5
    // layout - the wide layouts (7.1, 5.1.2, 5.1.4, 7.1.4) an acmod cannot
    // name, because a dependent substream's height/wide/rear channels are not
    // members of Table 5.8 at all. `layout` is the rendered program's channel
    // order exactly as Eac3Decoder::decode_access_unit reports it, and push()
    // expects spans in that same order.
    //
    // For any layout whose full-bandwidth channels are all Table 5.8's own
    // (mono through 5.1), this agrees with the acmod constructor above
    // channel for channel - Ls/Rs are M±110, squarely inside Table 4's
    // +1.5 dB sector, which is where Table 3's 1.41 came from in the first
    // place. 1+1 dual mono has no layout to pass here: it is two unrelated
    // programmes rather than one soundfield, so it keeps its own two meters.
    LoudnessMeter(SampleRate rate, const eac3::chanmap::Layout& layout);

    // Any number of samples; spans are the coded channels in AC-3 order with
    // LFE last, matching the encoder's own input convention.
    void push(std::span<const std::span<const float>> channels);

    // std::nullopt until at least one 400 ms block has passed the absolute
    // gate — silence has no meaningful loudness, and inventing one would put
    // a wrong dialnorm on the stream.
    [[nodiscard]] std::optional<double> integrated_lkfs() const;

    // BS.1770-4 §2's un-gated block loudness: the same 400 ms/75%-overlap
    // window integrated_lkfs() gates internally, reported directly instead.
    // Reflects the most recently completed 400 ms block; std::nullopt until
    // one has elapsed. A block whose power is exactly zero is also
    // std::nullopt rather than -inf LKFS, matching integrated_lkfs()'s own
    // "no meaningful loudness" stance on silence.
    [[nodiscard]] std::optional<double> momentary_lkfs() const;

    // The same, over a 3 s window instead of 400 ms, still un-gated.
    // std::nullopt until 3 s have elapsed.
    [[nodiscard]] std::optional<double> short_term_lkfs() const;

    // EBU Tech 3342 §3.1 Loudness Range: the 95th minus the 10th percentile
    // of short-term loudness values, themselves passed through Tech 3342's
    // own cascaded gate — an absolute threshold at −70 LUFS then a relative
    // one at −20 LU below the mean of what survives it. This is NOT the same
    // relative gate integrated_lkfs() uses (−10 LU): Tech 3342 §3.1 specifies
    // −20 LU for LRA specifically, and the population being gated is
    // short-term (3 s) blocks rather than integrated-loudness (400 ms) ones.
    // std::nullopt until at least one short-term value survives both gates.
    [[nodiscard]] std::optional<double> loudness_range() const;

    // ITU-R BS.1770-4 Annex 2: the highest absolute sample value found in a
    // 4x-oversampled reconstruction of every pushed channel, LFE included —
    // true peak is about physical overload headroom, not perceived
    // loudness, so unlike every measure above it does not exclude LFE or
    // apply the surround weighting. In dBTP (decibels relative to 100% full
    // scale, true-peak measurement). std::nullopt until at least one sample
    // has been pushed.
    [[nodiscard]] std::optional<double> true_peak_dbtp() const;

    [[nodiscard]] int channel_count() const { return channels_; }

   private:
    // Both constructors, once each has decided which pushed channel slots
    // carry loudness and with what weight: everything from the K-weighting
    // design down to the per-channel state sizing is common to Annex 1 and
    // Annex 3, which is exactly Annex 3's own "first, second and fourth
    // stages ... are the same".
    void init(SampleRate rate, int channels, std::span<const int> loudness_slots,
              std::span<const double> weights);
    void push_block();
    void push_true_peak(int channel, float sample);

    // BS.1770 K-weighting: a high-shelf pre-filter then the RLB high-pass,
    // both biquads, both per channel with their own state.
    struct Biquad {
        std::array<double, 3> b{};
        std::array<double, 2> a{};  // a0 normalised out
    };
    struct State {
        std::array<double, 2> x{};
        std::array<double, 2> y{};
    };

    Biquad shelf_{};
    Biquad highpass_{};
    std::vector<State> shelf_state_;
    std::vector<State> highpass_state_;
    // The pushed channel slots that are terms in the loudness sum, and their
    // Table 3 / Table 4 weights - parallel, both sized fullbw_. An LFE-type
    // slot is in neither, since BS.1770 drops it from the sum rather than
    // weighting it zero; true peak still reads it, straight out of `channels`
    // by slot. Table 5.8's coded order and Table E2.5's bit order both put
    // the LFE-type channels last, so loudness_slots_ is in practice the
    // leading run 0..fullbw_-1 - but every loop below indexes through it
    // rather than assuming that.
    std::vector<int> loudness_slots_;
    std::vector<double> weights_;
    // Mean-square accumulator per channel over the current 100 ms step, plus
    // the four most recent steps, which is how the 400 ms window with 75%
    // overlap is built without buffering audio.
    std::vector<double> step_sum_;
    std::vector<std::array<double, 4>> recent_;
    // Same idea as recent_, widened to a 3 s/30-step window for short-term
    // loudness and (via short_term_power_history_ below) Loudness Range.
    // EBU Tech 3342 §3.1 asks for at least 10 Hz sampling of the short-term
    // series; the existing 100 ms step already gives exactly that, so no
    // separate timer is needed.
    std::vector<std::array<double, 30>> short_term_recent_;
    // Weighted power sum of each gated 400 ms block. One double per 100 ms of
    // programme is all the gating needs: the weights are constant, so the mean
    // of the weighted sums equals the weighted sum of the means.
    std::vector<double> block_power_;
    // The whole-programme series of un-gated short-term (3 s) block power,
    // one entry per 100 ms once the first 3 s has elapsed — loudness_range()
    // applies Tech 3342's own gate to this at read time, since it is a
    // different gate to the one block_power_ was already filtered through.
    std::vector<double> short_term_power_history_;
    double momentary_power_ = 0.0;
    double short_term_power_ = 0.0;

    // Per-channel delay line for the true-peak oversampler (BS.1770-4
    // Annex 2), sized to channels_ (LFE included) rather than fullbw_.
    std::vector<std::array<double, 12>> true_peak_history_;
    double true_peak_abs_max_ = 0.0;
    bool true_peak_seen_ = false;

    // Every pushed channel, LFE-type included - the width true peak reads.
    int channels_ = 0;
    // How many of them are terms in the loudness sum, i.e. loudness_slots_'
    // and weights_' shared length, and the width of every per-channel filter
    // and accumulator above.
    int fullbw_ = 0;
    int step_samples_ = 0;
    int step_filled_ = 0;
    int steps_seen_ = 0;
};

// §5.4.2.8: dialnorm is how many dB dialogue sits below digital 100%, valid
// 1..31. A programme louder than −1 LKFS or quieter than −31 clamps; the
// clamp at 31 is why a stream that never measured anything says 31, and why
// 31 is a poor default rather than a neutral one.
[[nodiscard]] AC3FORGE_EXPORT int dialnorm_from_lkfs(double lkfs);

}  // namespace ac3::meta
