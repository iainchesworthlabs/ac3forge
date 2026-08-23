#pragma once

#include <cmath>
#include <cstdint>
#include <span>

// §7.3.4's dithflag[ch], decided from content.
//
// A bin the allocator gave no bits to is not transmitted at all, so the
// decoder has to invent it. dithflag says WHICH invention: clear means a true
// zero, set means a random value uniform on ±0.707 of that bin's own exponent
// scale (DitherGenerator, ac3/core/mantissas.hpp). Neither is the coefficient
// the encoder had, and the two fail in opposite directions - a run of true
// zeros is a hole in the spectrum, audible as the "birdies"/warbling the tool
// exists to cover, while dither over a bin that really was near-silent is
// noise added to nothing.
//
// So the decision is a comparison, per channel per block, between the energy
// the decoder will NOT receive and the energy the dither would put there
// instead:
//
//   signal = sum over zero-bap bins of the real coefficient's energy
//   noise  = sum over the same bins of the dither's own variance,
//            0.1666 * 2^(-2*exponent) per bin
//
// Dither goes on when signal >= noise. Read plainly: substitute noise only
// where what is being replaced was at least as loud as the substitute. Above
// that line the bins hold real content the allocator could not afford and
// filling them is what §7.3.4 is for; below it - and digital silence is the
// limiting case, where `signal` is exactly zero - dither would be the loudest
// thing in the band.
//
// The threshold is a ratio, which is what makes it usable on a coupled
// channel: the decoder dithers a zero-bap COUPLING bin once per receiving
// channel and then scales it by that channel's coupling coordinate, and the
// real coefficient behind it is scaled by the same coordinate, so the two
// sides of the comparison move together and the coordinate cancels.
//
// Two reference encoders were read out of tests/golden/external-baseline/ to
// check this against something other than a metric (both AC-3 5.1 at
// 448 kbit/s, 79 frames each). FFmpeg 8.0.1 sets every dithflag in every
// frame. Dolby's own DEE 6.5.4 sets dithflag to exactly the inverse of that
// channel's blksw - 392 blocks of (blksw 0, dither on) and 3 of (blksw 1,
// dither off), no exception either way. So real encoders dither by default
// and switch it off on a transient; the block-switch half of that rule is
// applied at both call sites, and the comparison above is what decides the
// rest.
//
// Internal to src/forge/src/encoder/ on purpose, like snr_search.hpp beside
// it - shared between the AC-3 and E-AC-3 encoders, not library surface.

namespace ac3::internal {

// §7.3.4's "uniform distribution of values between +1 and -1 ... scaled by
// 0.707": variance (2 * 0.707)^2 / 12, in units of the bin's own 2^-exponent
// scale.
inline constexpr double kDitherVariance = 0.16663;

// How much louder than the dither the discarded content has to be before it
// is worth covering. 1.0 is the plain reading of the paragraph above -
// substitute noise only where what it replaces was at least as loud. Lower
// values dither more freely (0 is "wherever any zero-bap bin exists at all",
// which is FFmpeg's behaviour); higher values reserve it for the deepest
// holes. Measured across 192-640 kbit/s on three materials - see the table in
// the pull request that introduced this.
inline constexpr double kSignalToDitherFloor = 1.0;

class DitherBallot {
   public:
    // Weighs one contiguous region of one stream. `coefficients`, `exponents`
    // and `bap` are all indexed from bin 0 (the same indexing
    // compute_bit_allocation uses), and [begin, end) is the part of them this
    // stream actually codes. Call it once per region a channel receives: its
    // own spectrum, plus the shared coupling channel's when it is coupled.
    void weigh(std::span<const double> coefficients, std::span<const std::uint8_t> exponents,
               std::span<const std::uint8_t> bap, int begin, int end) {
        for (int bin = begin; bin < end; ++bin) {
            const auto at = static_cast<std::size_t>(bin);
            if (bap[at] != 0) {
                continue;
            }
            const double c = coefficients[at];
            signal_ += c * c;
            noise_ += kDitherVariance *
                      std::ldexp(1.0, -2 * static_cast<int>(exponents[at]));
        }
    }

    // False when nothing was weighed (no zero-bap bin exists, so there is
    // nothing for dither to fill) as well as when the comparison fails.
    [[nodiscard]] bool on() const {
        return noise_ > 0.0 && signal_ >= kSignalToDitherFloor * noise_;
    }

   private:
    double signal_ = 0.0;
    double noise_ = 0.0;
};

}  // namespace ac3::internal
