#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"
#include "iclforge/ac4core/dsp/mdct.hpp"

// The inverse transform's windowing and overlap-add with block switching:
// ETSI TS 103 190-1 V1.4.1 clause 5.5.2.2 steps 5 and 6 (Pseudocodes 63 and
// 64) and the windows of clause 5.5.3.
//
// A frame of `full_length` samples (Part 1's frame_length, Nfull) is coded as
// one full block or as blocks of full/2, /4, /8 and /16 (Table 187). Each
// block of N lines is inverse transformed to 2N samples. Its first half is
// windowed by w[n] below and added to what the previous block left in the
// overlap buffer, after the buffer has been windowed by the previous block's
// right half; its second half waits, unwindowed, for the next block.
//
// With NW = min(N, Nprev), the window over the current block's first half is
//
//   w[n] = 0                              0 <= n < Nskip
//   w[n] = KBD_LEFT(NW, n - Nskip)        Nskip <= n < NW + Nskip
//   w[n] = 1                              NW + Nskip <= n < N,     Nskip = (N - NW) / 2
//
// and the window over the previous block's second half is
//
//   w[n] = 1                              0 <= n < Nskip
//   w[n] = KBD_RIGHT(NW, NW + n - Nskip)  Nskip <= n < NW + Nskip
//   w[n] = 0                              NW + Nskip <= n < Nprev, Nskip = (Nprev - NW) / 2
//
// The text writes KBD_RIGHT(NW, n - Nskip), an argument below the range N <= n
// < 2N it defines KBD_RIGHT on; the reading taken is the right half at the
// same position, offset by NW (src/ac4dec/ERRATA.md, "KBD_RIGHT's argument").

namespace iclforge::ac4::detail::dsp {

// The transforms and windows for one full block length at one sampling
// frequency: an inverse MDCT and a KBD_LEFT half window, with Table 186's
// alpha, for each block length from the full one down by halves to a
// sixteenth, or to the shortest Table 186 lists. The set also owns the working
// space of every transform it holds, one block's worth, since the channels of a
// substream inverse transform one block after another: ChannelSynthesis::block()
// takes it from here, so no channel and no transform holds any of its own.
template <typename Real>
class TransformSet {
   public:
    using Complex = iclforge::ac4::detail::dsp::Complex<Real>;

    // `rate_multiplier` is 1 at 44.1 and 48 kHz, 2 at 96 kHz and 4 at 192 kHz.
    TransformSet(int full_length, int rate_multiplier);

    // False when Table 186 does not list `full_length` at that multiplier.
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] int full_length() const noexcept { return full_length_; }

    // The transform and window for a block of `length` lines: nullptr and an
    // empty span for a length not in the set.
    [[nodiscard]] Imdct<Real>* imdct(int length) noexcept;
    [[nodiscard]] std::span<const Real> kbd_left(int length) const noexcept;

    // The working space, valid() or not empty: the 2 * full_length() samples of
    // the block being windowed, and the full_length() values any of the set's
    // transforms takes as its scratch.
    [[nodiscard]] std::span<Real> block_scratch() noexcept { return block_; }
    [[nodiscard]] std::span<Complex> transform_scratch() noexcept { return transform_; }

    // KBD_LEFT(length) with Table 186's alpha, narrowed, as the set computes it where no table
    // is built in (dsp/transform_tables.hpp).
    [[nodiscard]] static std::vector<Real> computed_kbd_left(int length, int rate_multiplier);

   private:
    [[nodiscard]] int slot(int length) const noexcept;

    int full_length_ = 0;
    bool valid_ = false;
    std::vector<Imdct<Real>> imdct_;
    // Each length's window: the built-in table where there is one, else computed into windows_.
    std::vector<std::span<const Real>> window_tables_;
    std::vector<std::vector<Real>> windows_;
    std::vector<Real> block_;
    std::vector<Complex> transform_;
};

// One channel's overlap buffer and the length of its last block.
template <typename Real>
class ChannelSynthesis {
   public:
    explicit ChannelSynthesis(int full_length);

    // Inverse transforms one block of spectral lines and writes as many PCM
    // samples to `pcm`, which must be at least that long. Returns false, with
    // nothing written or changed, for a length `transforms` has no transform
    // for or a `transforms` of another full length.
    bool block(TransformSet<Real>& transforms, std::span<const Real> spectrum, std::span<Real> pcm);

    // The same for lines held at the double decoder's times 2^-exponent, as the fixed tier
    // holds them (Imdct::inverse); at double and float the exponent is 0.
    bool block(TransformSet<Real>& transforms, std::span<const Real> spectrum, int exponent,
               std::span<Real> pcm);

    // Silence in the overlap buffer, and a previous block of full length.
    void reset();

    [[nodiscard]] int previous_length() const noexcept { return previous_length_; }

   private:
    int full_length_ = 0;
    int previous_length_ = 0;
    std::vector<Real> overlap_;  // Nfull values, Pseudocode 64's overlap[]
};

extern template class TransformSet<Real>;
extern template class ChannelSynthesis<Real>;

}  // namespace iclforge::ac4::detail::dsp
