#pragma once

#include <span>

#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

// A/52:2018 §3.7.2 / Figure E3.2: transient pre-noise time-scaling synthesis.
// A pure post-process on decoded PCM - it touches no bitstream, transform or
// bit-allocation state, only the sample buffer - that overwrites the
// pre-echo a low-bit-rate transient leaves ahead of it with a synthesized
// copy of the (clean) audio already decoded just before that pre-echo.
//
// This is unrelated to enhanced coupling (ac3::eac3::ecpl_*) and to block
// switching (ac3::TransientDetector) - both are encoder-side or transform-
// domain tools; this one runs entirely after IMDCT, on the time-domain
// output, and is signaled per full-bandwidth channel by the decoder's own
// transproce/chintransproc/transprocloc/transproclen fields.

namespace ac3 {

// TC1/TC2 in the spec's own naming: the two synthesis-buffer/cross-fade
// system constants used throughout §3.7.2's pseudocode.
inline constexpr int kTransientPrenoiseTC1 = 256;
inline constexpr int kTransientPrenoiseTC2 = 128;

// Where transprocloc counts from, in an overlap-add decoder's output for the
// frame that carries it. §3.7.2 measures the transient "relative to the first
// sample of decoded PCM channel data in the audio frame", and A/52 defines an
// audio block as "256 samples of the preceding audio block, and 256 new time
// samples": a frame's PCM is its blocks' new samples. Block 0's new samples are
// the second half of its window, which the overlap-add finishes one block into
// the frame's output, so a transient at transprocloc * 4 sits that many samples
// past output sample kTransientPrenoiseOrigin.
//
// Dolby's own decoder counts from here too. On the Dolby Encoding Engine's
// streams its output gives way from the synthesized audio back to the decoded
// audio at transprocloc * 4 + kTransientPrenoiseOrigin, to within a few
// samples, and DEE's transients sit just past the source's onsets on the same
// count. Counting from the output frame's first sample instead ends every
// correction a block early, leaving in place the pre-noise closest to the
// transient - the part the tool exists to remove.
inline constexpr int kTransientPrenoiseOrigin = kSamplesPerBlock;

// How far back from its transient a correction reaches. The synthesis buffer
// starts 2*TC1 + 2*pnlen before the transient (§3.7.2), and pnlen - the
// distance to the transient from the leading edge of the block before the one
// it falls in - is at most two blocks less transprocloc's four-sample step.
// Everything a correction reads or writes lies in [transient - this,
// transient).
//
// Measured from the frame that signals it instead: the largest transprocloc,
// 1023, puts the transient 4092 samples past the origin, in the frame after
// next (§2.3.2.22 lets a transient fall in a later frame), and a transient in
// the frame's first block reaches back at most
// kTransientPrenoiseMaxReach - kTransientPrenoiseOrigin - 252 = 1020 samples
// before the frame's first output sample.
inline constexpr int kTransientPrenoiseMaxReach =
    2 * kTransientPrenoiseTC1 + 2 * (2 * kSamplesPerBlock - 4);  // 1528

// The half-open sample range [first, last) apply_transient_prenoise needs to
// both read from and write into for a given transloc/translen - both the
// synthesis-buffer source (which reaches furthest back) and the corrected
// region itself fall inside it. apply_transient_prenoise does not bounds-
// check its own span (it is a plain, unchecked buffer view), so a caller
// working from untrusted bitstream fields must check this range against
// whatever history it actually has before calling it.
struct TransientPrenoiseRange {
    int first = 0;
    int last = 0;
};
[[nodiscard]] AC3FORGE_EXPORT TransientPrenoiseRange transient_prenoise_range(int transloc,
                                                                             int translen);

// Applies the correction in place. `pcm` is one full-bandwidth channel's
// decoded samples, indexed so that `pcm[transloc]` is the sample the
// transient itself starts at (§3.7.2's transprocloc, already multiplied by
// 4 and offset to this buffer's own indexing - not the raw bitstream field;
// see kTransientPrenoiseOrigin for the offset a decoder's own output needs).
// `translen` is transproclen, unscaled (already in samples). Index 0 must fall
// on a block boundary, since pnlen is derived from where the blocks are.
//
// `pcm` must hold valid history reaching back to index
// `transloc - (2*kTransientPrenoiseTC1 + 2*pnlen)`, where pnlen is derived
// internally from transloc (the distance back to the leading edge of the
// audio coding block immediately before the one the transient falls in -
// §3.7.1: this is derived, not transmitted). The correction itself is
// written into [transloc - (pnlen + translen + TC1), transloc). Both bounds
// are the caller's responsibility to keep in range; this function does not
// itself know where a frame boundary sits.
//
// The cross-fades run in the decoder's own scalar - double, float or the
// fixed-point tier's Q7.24, whichever this library was built with - the way
// the output stage's per-sample products do, so a fixed-point build stays
// integer arithmetic here too.
AC3FORGE_EXPORT void apply_transient_prenoise(std::span<float> pcm, int transloc, int translen);

}  // namespace ac3
