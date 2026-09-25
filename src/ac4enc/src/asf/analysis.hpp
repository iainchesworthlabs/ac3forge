#pragma once

#include <span>
#include <vector>

#include "asf/layout.hpp"
#include "dsp/mdct.hpp"

// The forward transform of each block of a frame, windowed as the decoder's
// synthesis windows it (ETSI TS 103 190-1 V1.4.1 clause 5.5.2.2, Pseudocodes
// 63 and 64): over a block's first half the left window of the shorter of it
// and the block before, over its second half the right window of the shorter
// of it and the block after, each widened with zeros and ones to the block's
// length.
//
// Placement: with the frame's blocks laid end to end from the frame's first
// output sample, a block of n samples that starts k samples into the frame
// transforms the 2n samples from k + (N - n)/2, N being the frame's length.
// Those are what the decoder's overlap-add puts back at the same places.
//
// Scale: the decoder's inverse transform returns a windowed round trip at
// half gain, and writes full scale 1.0 at 32 768 (src/ac4dec/ERRATA.md, "Full
// scale, and the overlap-add's factor of two"), so the lines are the forward
// transform of the windowed samples times 65 536.

namespace ac4::detail {

class Analysis {
   public:
    // `rate_multiplier` as for dsp::TransformSet: 1 at 44.1 and 48 kHz.
    Analysis(int frame_length, int rate_multiplier);

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    // `frame` holds 2N samples from the frame's first output sample.
    // `previous` is the length of the block before the frame's first, `next`
    // that of the block after its last. The lines of each window follow one
    // another in window order in `spectrum`, frame_length of them in all.
    void transform(std::span<const double> frame, const FrameLayout& layout, int previous, int next,
                   std::vector<double>& spectrum);

   private:
    [[nodiscard]] int slot(int length) const noexcept;

    int frame_length_ = 0;
    bool valid_ = false;
    std::vector<dsp::Mdct<double>> mdct_;
    std::vector<std::vector<double>> kbd_;   // KBD_LEFT per block length
    std::vector<double> segment_;
};

}  // namespace ac4::detail
