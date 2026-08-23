#pragma once

#include <cstdint>
#include <span>

#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

// Where the coded spectrum should stop.
//
// Both encoders used to answer this from the bit rate alone - AC-3 from a
// per-channel-kbps curve, E-AC-3 not at all (a fixed chbwcod 60, the whole
// 23.7 kHz, even at 96 kbit/s per channel). The bit rate says how many bits
// there are to spend; it says nothing about whether there is anything up
// there to spend them on, and the two questions have different answers on
// different programme material at the same rate.
//
// The trap this has to avoid is documented in encoder.cpp: narrowing the
// band improves every waveform metric this project measures, on real
// programme material as much as on the checked-in fixtures, because the
// energy discarded is a vanishing fraction of the total whatever the source
// (measured: 3.5e-8 of a solo piano recording's energy sits above 14.7 kHz,
// against 7e-5 of reference_51.wav's). An SNR-led rule therefore narrows
// until it is plainly audible and still reports a win. So the criterion here
// is not "is this band small" - it always is - but "is this band audible",
// which is a question A/52's own psychoacoustic model already answers.

namespace ac3::encoder {

// §7.1.3: how many mantissas a chbwcod codes.
[[nodiscard]] constexpr int endmant_for_chbwcod(int chbwcod) { return ((chbwcod + 12) * 3) + 37; }

// The inverse, rounded UP to the transmitted grid so a partly-audible band is
// kept whole rather than clipped. Clamped to the legal 0..60.
[[nodiscard]] AC3FORGE_EXPORT int chbwcod_for_endmant(int endmant);

// Folds one channel-block of MDCT coefficients into a running per-bin
// exponent minimum, the form audible_endmant() below wants. Start the array
// at kMaxExponent (an empty spectrum) and call this once per full-bandwidth
// channel per block.
//
// The exponent comes from the same to_fixed25/exponent_from_fixed pair the
// encoder's own step 4 uses, so this measures the spectrum the coder is
// about to see rather than a second, subtly different, view of it.
AC3FORGE_EXPORT void accumulate_peak_exponents(std::span<const double> coefficients,
                                               std::span<std::uint8_t> peak_exponents);

// The highest bin worth coding, as an endmant (exclusive), given the frame's
// own spectrum.
//
// `peak_exponents` is one exponent per bin from bin 0 - the per-bin MINIMUM
// over every full-bandwidth channel and every block of the frame, a minimum
// exponent being a maximum magnitude. That makes this the frame's loudest
// spectrum rather than an average: a band is kept if ANY channel in ANY block
// has something audible in it, which is the only safe direction when one
// decision covers all six blocks and every coupled channel.
//
// The test is §7.2.2.2's psd curve, banded by §7.2.2.3, against Table 7.15's
// absolute hearing threshold - the same `hth` compute_bit_allocation() floors
// its own masking curve with. A band below it is a band whose mask is the
// threshold itself, so the allocator would have to be handed a positive SNR
// offset before it put a single bit there; dropping it costs nothing coded
// and returns its exponents. Above 17 kHz Table 7.15 rises 34 dB in one band
// step, which is why this keeps roughly 17 kHz on ordinary material and more
// only when the material is genuinely bright up there.
//
// Returns at least `endmant_for_chbwcod(0)` (73 bins, 6.9 kHz): the floor
// exists because a frame of near-silence has nothing above the threshold
// anywhere, and a bandwidth that collapses on quiet passages and reopens on
// loud ones would be audible as pumping rather than as detail.
[[nodiscard]] AC3FORGE_EXPORT int audible_endmant(std::span<const std::uint8_t> peak_exponents,
                                                  SampleRate sample_rate);

// What the RATE alone can afford, as a chbwcod. A ceiling, never a target:
// below roughly 90 kbit/s per channel the bits the top of the band costs are
// bits the rest of the spectrum needed, and no amount of content up there
// changes that (measured, AC-3 5.1 at 192 kbit/s on real material: MOS 3.145
// at chbwcod 24 falling to 2.411 at 59). The curve is unchanged from the one
// AC-3 has used since 0.7.0.
[[nodiscard]] AC3FORGE_EXPORT int rate_ceiling_chbwcod(std::uint32_t bitrate_kbps, int nfchans);

// The whole decision, shared by both encoders so they cannot drift apart:
// the rate ceiling above, the content edge under it, and a limit on how fast
// the answer may fall.
//
// `previous_chbwcod` is what this encoder last transmitted, or negative on
// the first frame - which then takes the content's answer outright, having
// nothing to pump against yet. Narrowing is capped at `kMaxNarrowStep` codes
// per frame because a band edge that chases a quiet passage down and a loud
// one back up is a 31 Hz modulation of the top of the spectrum, audible as
// pumping in a way the missing band itself is not. Widening is not capped:
// being a frame late to widen means a real transient's high band arrives
// after the transient.
[[nodiscard]] AC3FORGE_EXPORT int choose_chbwcod(std::uint32_t bitrate_kbps, int nfchans,
                                                 std::span<const std::uint8_t> peak_exponents,
                                                 SampleRate sample_rate, int previous_chbwcod);

// Two codes is 6 bins, 560 Hz a frame: a full 60 -> 24 collapse still takes
// 18 frames (0.58 s).
inline constexpr int kMaxNarrowStep = 2;

}  // namespace ac3::encoder
