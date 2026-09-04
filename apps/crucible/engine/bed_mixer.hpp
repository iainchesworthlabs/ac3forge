#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "slots.hpp"

// Folding what a tap delivers into what a slot takes
// (docs/platforms/windows-demo.md, "Objects and the bed").
//
// A tap arrives interleaved at the channel count the tap was opened with -
// two for most applications, eight for one that renders surround into the
// 7.1 null sink. A positioned application becomes one mono object; a bed
// application is summed into the five bed slots by channel. Both are plain
// arithmetic on interleaved float, with no allocation once the outputs are
// sized, so they can run on the engine's frame thread.
//
// Channel orders assumed, all WASAPI's (which are WAVEFORMATEXTENSIBLE's, which
// are SMPTE's): 2 = L R; 4 = L R Ls Rs; 6 = L R C LFE Ls Rs; 8 = L R C LFE
// Lss Rss Lrs Rrs. Anything else is summed to mono and spread front left and
// right, which keeps it audible without pretending to know where it goes.
//
// The LFE of a surround tap is dropped, as ITU-R BS.775's downmixes drop it:
// an object cannot carry someone else's LFE (lfe_send routes the object's own
// signal), and folding it into the fronts would put bass where a bass-managed
// receiver has already decided it does not belong.

namespace ac3::crucible {

// Per-frame scratch for one application's fold, sized once.
struct BedMix {
    // One mono buffer per bed slot, BedChannel order, `frames` long.
    std::array<std::vector<float>, kBedSlots> slots;
    void resize(std::size_t frames);
    void clear();
};

// interleaved.size() must be a multiple of `channels`; the frame count is
// interleaved.size() / channels and the outputs are sized to it by the caller.

// Folds one tap to mono into `out` (out.size() == frames). The fold keeps a
// full-scale mono source at full scale: stereo averages, surround weights
// the centre and surrounds at -3 dB and normalises so a signal present on
// every channel does not clip.
void fold_to_mono(std::span<const float> interleaved, std::uint16_t channels,
                  std::span<float> out);

// Folds one tap to a left/right pair for a split application (outs the
// same length as fold_to_mono's). Stereo passes straight through; 5.1 and
// 7.1 fold each side's surrounds at -3 dB and share the centre between the
// two at -3 dB, normalised like fold_to_mono; mono and anything else land
// on both sides.
void fold_to_pair(std::span<const float> interleaved, std::uint16_t channels,
                  std::span<float> left, std::span<float> right);

// Adds one tap into the bed slots, scaled by `gain`. Stereo lands on L and
// R; 5.1/7.1 map by channel with the rear pairs folded at -3 dB into Ls/Rs.
void add_to_bed(std::span<const float> interleaved, std::uint16_t channels, float gain,
                BedMix& bed);

}  // namespace ac3::crucible
