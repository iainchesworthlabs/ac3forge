#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>

#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"
#include "ac3/quality/distortion.hpp"

// A psychoacoustic model for the AC-3 encoder's transmitted knobs.
//
// WHY THERE IS ONE AT ALL. A/52 §7.2.2's bit allocation is a masking model,
// but it is a masking model built from the exponents alone: psd[bin] is
// 3072 - (exp << 7) and nothing else, so two bands with the same energy get
// the same treatment whether one is a held violin note and the other is
// cymbal wash. That is not a defect of the format - the routine has to be
// reproducible bit-for-bit inside every decoder, so it can only use what is
// transmitted - but it does mean the ENCODER is the only side that can know
// the difference, and this encoder has never computed it. Nothing
// psychoacoustic existed in the tree before this file: no tonality
// estimate, no spreading, no absolute threshold, no perceptual entropy.
//
// What the encoder can do about it is bounded and specific. It can move the
// whole curve (csnroffst/fsnroffst), change its shape globally
// (BitAllocCodes - dbpbcod's knee, fgaincod/sgaincod's leak rates), or
// correct it per band by up to +-3 steps (§7.2.2.6 delta bit allocation).
// This model exists to price those choices: it produces, per band, the
// noise power that band can actually hide, which ac3::quality's measured
// reconstruction noise can then be compared against.
//
// THE MODEL. Johnston's perceptual-entropy formulation (J. D. Johnston,
// "Transform Coding of Audio Signals Using Perceptual Noise Criteria",
// IEEE Journal on Selected Areas in Communications 6(2), February 1988,
// pp. 314-323), with the tonality estimate taken from the unpredictability
// measure of ISO/IEC 11172-3:1993 Annex D.2 (MPEG-1 psychoacoustic model
// 2). Concretely, per band:
//
//   1. Band energy over A/52 Table 7.13's own 50 bands - deliberately the
//      codec's banding rather than a separate critical-band partition, so
//      that a threshold lands exactly where a delta bit allocation segment
//      can act on it and in the same bands ac3::quality::BandNoise
//      measures.
//   2. Tonality from inter-block spectral unpredictability (Annex D.2.4):
//      this block's magnitude spectrum against a linear extrapolation of
//      the previous two, normalised. Steady tones extrapolate well and
//      score near 1; noise and transients do not and score near 0.
//   3. Spreading across bands by Schroeder's function (M. R. Schroeder,
//      B. S. Atal, J. L. Hall, "Optimizing digital speech coders by
//      exploiting masking properties of the human ear", JASA 66(6), 1979,
//      pp. 1647-1652), over Bark distances from Zwicker and Terhardt's
//      analytical expression (JASA 68(5), 1980, pp. 1523-1525).
//   4. A signal-to-mask requirement interpolated between the tone-masking-
//      noise and noise-masking-tone limits by tonality, as Johnston §II.
//   5. An absolute threshold of hearing floor (Terhardt's approximation),
//      optional and off-by-default-able, because this project has already
//      been caught once by a change that scored well precisely because it
//      discarded high frequencies (see encoder.cpp's chbwcod comment).
//   6. Perceptual entropy per block, Johnston §III - an estimate of the
//      bits needed to code the block transparently, which is the number
//      that says whether a frame is tight or comfortable independent of
//      the rate table.
//
// WHAT IS APPROXIMATE, AND WHY IT IS SAID OUT LOUD. Annex D.2.4's
// unpredictability is computed on a complex FFT spectrum, using magnitude
// AND phase. This encoder has an MDCT, which is real; its coefficients
// carry phase as sign in a form that does not linearly extrapolate. What is
// implemented is therefore the magnitude half of that measure, on a
// three-bin smoothed magnitude - smoothed because a stationary sinusoid's
// MDCT magnitude is modulated block to block by time-domain aliasing, and
// an unsmoothed per-bin magnitude would call a held note unpredictable.
// The published model is not being claimed here; a documented reduction of
// it is. tests/quality/test_perceptual.cpp pins the behaviour that matters
// - tones score tonal, noise and clicks do not.

namespace ac3::quality {

// The calibration between the encoder's coefficient domain and sound
// pressure level, needed only by the absolute threshold.
//
// Measured, not assumed: a full-scale sine at a bin centre, through
// apply_analysis_window() and mdct512_forward(), produces exactly this
// total block energy (test_perceptual.cpp asserts it, so a change to the
// transform's scaling cannot silently move the threshold). The SPL that
// corresponds to it is the ordinary 16-bit convention - full scale is
// 96 dB SPL - and it is a config field because it is a convention rather
// than a measurement.
inline constexpr double kFullScaleBlockEnergy = 0.25;
inline constexpr double kFullScaleSplDb = 96.0;

struct PerceptualConfig {
    // What a full-scale signal is taken to be, in dB SPL. Only the absolute
    // threshold depends on it; masking is relative throughout.
    double full_scale_spl_db = kFullScaleSplDb;
    // Floor the masking threshold at the absolute threshold of hearing.
    // On by default, but a switch rather than a constant: Terhardt's curve
    // passes 60 dB SPL around 17 kHz and rises steeply after, so at the top
    // of the band it is the term that decides whether the encoder codes
    // anything at all - and this repository has a recorded case of exactly
    // that kind of change measuring well for the wrong reason.
    bool absolute_threshold = true;
    // Ceiling on the absolute threshold, dB SPL. Terhardt's expression
    // reaches 160 dB SPL by 20 kHz, which would let the top bands be
    // discarded outright on any material; capping it keeps the floor a
    // floor rather than a bandwidth decision in disguise.
    double max_absolute_threshold_db = 60.0;
};

// One block's analysis for one channel.
struct BlockAnalysis {
    // The noise power each band can hide, in the same normalised
    // coefficient-power units BandNoise::noise carries, so the two divide.
    std::array<double, kBands> threshold{};
    // The band's own signal power, in the same units.
    std::array<double, kBands> energy{};
    // 0 (noise-like) to 1 (tone-like), per band.
    std::array<double, kBands> tonality{};
    // Johnston §III: bits needed to code this block transparently.
    double perceptual_entropy = 0.0;

    void reset();
};

// Carries the inter-block spectral history the tonality estimate needs, so
// it is a stateful object per encoder rather than a free function.
//
// One instance serves every channel of one stream; `channel` selects which
// history to use. The history is what makes blocks 0 and 1 of a frame as
// good as blocks 2-5 - without it the first two blocks of every frame would
// have no predecessor to extrapolate from and would read as noise, which on
// tonal material is exactly backwards.
class AC3FORGE_EXPORT PerceptualModel {
   public:
    PerceptualModel(SampleRate sample_rate, int channels, PerceptualConfig config = {});
    ~PerceptualModel();
    PerceptualModel(PerceptualModel&&) noexcept;
    PerceptualModel& operator=(PerceptualModel&&) noexcept;
    PerceptualModel(const PerceptualModel&) = delete;
    PerceptualModel& operator=(const PerceptualModel&) = delete;

    // Discards the spectral history - for a seek, or a new programme. The
    // next two blocks of each channel then behave as they do at start-up:
    // no predecessor, so no tonality claim, so the conservative
    // noise-masking-tone requirement.
    void reset();

    // The same, for one channel. An AC-3 encoder needs this because its
    // stream numbering is not stable across frames: the coupling channel
    // exists only in frames that couple (`cplinu` is off wherever a channel
    // block-switched, §8.2.4.1), so the slot at index nchans may hold the
    // history of a coupling channel from several frames ago. Extrapolating
    // this frame's coupling spectrum from that is not a prediction, it is a
    // coincidence, and it would read as tonality.
    void reset(int channel);

    // Analyses one channel's block. `coefficients` is the MDCT output
    // indexed from bin 0, `end` the coded bandwidth (endmant); bins at or
    // above `end` are ignored and their bands report zero.
    //
    // Advances that channel's history, so it must be called once per block
    // per channel, in block order.
    void analyse(int channel, std::span<const double> coefficients, int end, BlockAnalysis& out);

    // The band-centre frequencies this instance is working with, in Hz -
    // exposed for diagnostics and for tests that need to know where a band
    // sits without duplicating the table arithmetic.
    [[nodiscard]] std::span<const double> band_centre_hz() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// How far a measured reconstruction noise sits above what the model says
// the signal can hide. Zero dB is "exactly at threshold"; negative is
// inaudible by this model's reckoning.
struct NoiseToMask {
    // 10*log10 of the mean of the per-band ratios. The mean of the RATIOS,
    // not the ratio of the sums: a loud band with slack must not be allowed
    // to pay for a quiet band that is over threshold, which is precisely
    // the failure the composite SNR offset already has.
    //
    // A reporting number, not a search objective - see `audible_bits`.
    double mean_db = 0.0;
    // The worst single band, and which one. What a listener meets first.
    double worst_db = 0.0;
    int worst_band = -1;
    // Bands that carried signal and so contributed. Zero means nothing was
    // coded, and every number above is then 0.
    int bands = 0;
    // THE SEARCH OBJECTIVE: sum over bands of n_b * log2(1 + N_b / T_b),
    // with n_b the band's width in transform bins. Johnston's perceptual
    // entropy applied to the ERROR rather than to the signal - an estimate
    // of how many bits of audible information the reconstruction gets
    // wrong.
    //
    // `mean_db` was tried as the objective first and is the wrong shape for
    // one, in two ways that both push the same direction. A mean over the
    // 50 bands weights them equally, but Table 7.13's first 37 bands are one
    // bin each and the last 13 cover about 216 between them - so an
    // unweighted mean hands 74% of the vote to the bottom 3.5 kHz. And a
    // mean of raw RATIOS is dominated by its largest term, so one quiet band
    // with a small threshold decides the whole frame. Measured on real
    // programme material, a search minimising it chose dbpbcod 2 over 3 in
    // 45% of frames - the value this encoder's own sweep found worse at
    // every rate - and lost 0.05 MOS for it. Weighting by width and taking
    // the log of each ratio fixes both: a band's say is proportional to how
    // much spectrum it is, and a band 40 dB over threshold counts for more
    // than one 10 dB over without counting for a thousand times more.
    double audible_bits = 0.0;
};

// `threshold` is normally a sum of BlockAnalysis::threshold over the same
// blocks BandNoise accumulated, so the two describe the same span of audio.
[[nodiscard]] AC3FORGE_EXPORT NoiseToMask noise_to_mask(const BandNoise& measured,
                                                        std::span<const double> threshold);

}  // namespace ac3::quality
