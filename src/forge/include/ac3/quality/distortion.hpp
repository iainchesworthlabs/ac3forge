#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "ac3/export.hpp"

// Decoded-domain distortion measurement for the encoder's own candidates.
//
// The encoder's only in-loop quality criterion has been the composite SNR
// offset step 9 maximises, and that number answers a narrower question than
// it looks like it does: it is the largest offset at which the frame still
// FITS. Two candidate parameter sets are only comparable through it when
// they produce the same masking curve, because the offset is measured
// against that curve rather than against the signal. Change dbpbcod, or an
// exponent strategy, or a delta segment, and the curve moves under the
// number - so "this candidate reached a higher offset" stops meaning "this
// candidate sounds better", and every search built on it either measures
// nothing (the per-frame BitAllocCodes search) or loses (the cost-based
// exponent-strategy dynamic program). Both were tried and both are recorded
// as dead ends in encoder.cpp for exactly this reason.
//
// What is missing is a criterion in the units the listener is served in:
// how much error the DECODER will reconstruct. That is computable here
// without a decoder, because the encoder already holds every input to it -
// the coefficients, the decoded exponents (§8.2.10's mirror rule guarantees
// they are the decoder's), and the bit allocation pointers it just derived.
// A/52 §7.3's quantizers are deterministic functions of those three, so the
// reconstruction error is too.
//
// This header computes that error exactly, in the coefficient domain the
// encoder's MDCT output already lives in, banded onto §7.2.2.3's own 50
// bands so the result lines up bin-for-bin with the allocator it is judging
// and band-for-band with the delta bit allocation segments (§7.2.2.6) that
// can correct it.
//
// EXACTNESS. accumulate_block() does not call quantize_mantissa() and
// dequantize_mantissa(); it evaluates their composition in closed form, as
// an integer residue plus one scaling, which is roughly an order of
// magnitude cheaper per bin and is what makes a per-candidate measurement
// affordable inside the encoder's frame loop at all. That is a duplicated
// arithmetic model of the kind this project has been bitten by before (see
// ac3/verify/mirror.hpp), so it is pinned by an exhaustive equivalence test
// rather than by inspection: tests/quality/test_distortion.cpp sweeps every
// bap against the real quantize/dequantize pair and requires bit-exact
// agreement, over the full mantissa range, at every exponent.
//
// SCOPE. This measures the quantizer, which is what a parameter search
// chooses between. It does not measure the coupling channel's decoupling
// error, spectral extension's regenerated band, or rematrixing - each of
// those is a different decision with a different candidate set, and folding
// them into one number here would price them all at once. A caller that
// wants them measures the streams it wants and sums.

namespace ac3::quality {

// §7.2.2.3's banding. The same 50 for every sample rate: the table maps
// bins, not frequencies, and a band's width in Hz follows the rate.
inline constexpr int kBands = 50;

// What an encoder searching its own transmitted parameters is trying to
// minimise. Named here rather than in the encoder because both encoders
// will want it and because it is a statement about the measurement, not
// about AC-3.
enum class Criterion : std::uint8_t {
    // No search: the parameters are whatever the fixed rules chose. This is
    // what every release before the search existed emitted, and it stays
    // the default until the measured evidence says otherwise.
    kNone,
    // Minimise the reconstruction noise power this header measures. Honest
    // and cheap, and still a waveform criterion: it prices a decibel in a
    // band nobody can hear the same as a decibel in one they can.
    kDistortion,
    // Minimise the noise-to-mask ratio against ac3::quality::PerceptualModel
    // - the same measured noise, weighted by what the signal can actually
    // hide. Costs the psychoacoustic analysis on top.
    kPerceptual,
};

// One coded stream's signal and reconstruction-noise power, banded.
//
// Both in the same units - normalized coefficient power, where a
// full-scale coefficient is 1.0 - so their ratio is a signal-to-noise
// ratio and their difference is meaningful. Accumulating: a caller
// measuring a whole frame calls accumulate_block() once per block into one
// of these, and the six blocks sum.
struct BandNoise {
    std::array<double, kBands> signal{};
    std::array<double, kBands> noise{};

    // Zeroes both arrays, for a caller reusing one across frames.
    void reset();

    // Summed over every band. Both are needed together far more often than
    // either alone, so they are one call.
    [[nodiscard]] double total_signal() const;
    [[nodiscard]] double total_noise() const;
};

// The error a decoder will reconstruct for one bin, in the coefficient
// domain: `fixed / 2^24` is the coefficient the encoder quantized, and the
// return value is that minus what §7.3's dequantizer will hand back.
//
// `exponent` is the DECODED exponent (§8.2.10's mirror rule - the value the
// decoder will derive, never the encoder's raw one), since that is what
// normalizes the mantissa on both sides. `bap` 0 returns the whole
// coefficient: a zero-bit mantissa reconstructs as zero, so all of it is
// error. Dither (§7.3.4) is deliberately not modelled - this encoder writes
// dithflag as 0 unconditionally, and a measurement that assumed otherwise
// would be describing a stream it does not emit.
//
// Exported mainly for the equivalence test and for diagnostics; the frame
// loop wants accumulate_block() below, which evaluates the same thing
// without a call per bin.
[[nodiscard]] AC3FORGE_EXPORT double reconstruction_error(std::int32_t fixed, int exponent,
                                                          int bap);

// Adds one (stream, block)'s signal and reconstruction-noise power to `out`.
//
// The three spans are indexed the way the encoder and the allocator already
// index them, which is not the same way for all three: `fixed` starts at the
// stream's own first coded bin (`start`), while `exps` and `bap` are indexed
// from bin 0 whatever the stream is, exactly as compute_bit_allocation()
// takes them. A coupling stream therefore passes cplstrtmant as `start` and
// full-length exps/bap, with no re-basing at the call site.
//
// `end` is the stream's endmant. Bins outside [start, end) are not coded and
// contribute nothing - not even to `signal`, since a band's SNR should be
// measured over what was actually offered to the quantizer.
AC3FORGE_EXPORT void accumulate_block(std::span<const std::int32_t> fixed,
                                      std::span<const std::uint8_t> exps,
                                      std::span<const std::uint8_t> bap, int start, int end,
                                      BandNoise& out);

// Signal-to-noise ratio in dB over every band, from an accumulated
// BandNoise. A frame with no coded signal at all returns kMaxSnrDb rather
// than an infinity, so a caller can average or compare results without
// special-casing silence; a band with signal and no measurable noise
// saturates at the same value.
inline constexpr double kMaxSnrDb = 200.0;

[[nodiscard]] AC3FORGE_EXPORT double snr_db(const BandNoise& measured);

// The same ratio per band, written into `out` (which must be kBands long).
// Bands the stream does not cover report kMaxSnrDb, for the same reason.
AC3FORGE_EXPORT void band_snr_db(const BandNoise& measured, std::span<double> out);

}  // namespace ac3::quality
