#pragma once

#include "ac3/core/tables.hpp"

// The latency budget of an encode -> decode chain, in samples at the coded
// sample rate (roadmap PF6).
//
// Every term below is ALGORITHMIC delay - the delay the coding scheme itself
// imposes on an infinitely fast machine. Compute time is a separate question
// answered by docs/performance-trend.md's ms/frame series, and transport,
// device buffers and resampling are the integrator's own to add.
//
// The four terms, and where each one comes from in this codebase:
//
//   frame_samples      The encoder's input granularity. Every encode_frame()
//                      here takes exactly one frame of PCM per channel, so a
//                      live chain must fill that buffer before it can call at
//                      all. Six-block AC-3 and E-AC-3 syncframes carry
//                      kSamplesPerFrame; a short E-AC-3 syncframe
//                      (numblkscod 0-2, §E2.3.1.4) carries 256, 512 or 768 -
//                      eac3::blocks_per_syncframe() * kSamplesPerBlock, which
//                      is what eac3_latency() reports since roadmap EQ11 made
//                      the encoder able to emit them (it emitted six-block
//                      frames only before that). The decoder reads all four
//                      codes. See the "Latency" section of
//                      docs/library/encoding-eac3.md.
//
//   transform_samples  The MDCT/IMDCT time-domain-alias-cancellation overlap.
//                      Block b's analysis window spans input samples
//                      [256b - 256, 256b + 256), and a decoder cannot finish
//                      any 256-sample segment until it holds BOTH blocks whose
//                      windows cover it - so the frame the encoder emits
//                      reconstructs input samples [-256, frame_samples - 256).
//                      The frame's last block-worth of input is still sitting
//                      in FrameEncoder::history_ and only reaches the wire in
//                      the next frame. Equivalently: decoded output sample k
//                      is input sample k - transform_samples. This is the one
//                      term that is a genuine sample-domain SHIFT rather than
//                      a wait, and it is the one an impulse test locates -
//                      see tests/decoder/test_latency.cpp.
//
//   lookahead_samples  Input the encoder must see BEYOND the frame it is
//                      coding. Zero throughout this codebase, and not by
//                      accident: TransientDetector::detect() decides blksw
//                      for a block from the same 256 new samples that block
//                      codes (§8.2.2 defines the decision on the window's
//                      second half only), and §3.7's transprocloc is picked
//                      from that same frame's blksw rather than from a second,
//                      forward-looking detector. A basic-encoder recipe that
//                      needs no lookahead is why AC-3's number is as low as it
//                      is; a psychoacoustic encoder that delayed the
//                      block-switch decision by a block would add 256 here.
//
//   holdback_samples   Decoder-side only: §3.7 transient pre-noise processing
//                      lets a correction reach BACKWARDS out of one frame into
//                      the one before it, so a substream that sets transproce
//                      returns frame N-1's PCM from the call that supplies
//                      frame N (Eac3Decoder::decode_substream's own doc
//                      comment). One frame period, permanently, from the first
//                      frame that uses the tool onwards. Zero for AC-3, and
//                      zero for any E-AC-3 stream that never turns the tool on.
//
// total_samples() is the figure to budget with: no sample entering the encoder
// is delayed by more than that many samples before the matching decoded sample
// leaves the decoder, and the best case (the last sample of a frame) is
// transform_samples + holdback_samples.

namespace ac3 {

// The MDCT/IMDCT overlap, §7.9.4/§8.2.3.2: one audio block. Named separately
// from kSamplesPerBlock because it is the transform's property, not the
// frame's - a codec with the same 256-sample blocks and a non-overlapping
// transform would have kSamplesPerBlock unchanged and this at zero.
inline constexpr int kTransformDelaySamples = kSamplesPerBlock;

struct LatencyBudget {
    int frame_samples = kSamplesPerFrame;
    int transform_samples = kTransformDelaySamples;
    int lookahead_samples = 0;
    int holdback_samples = 0;

    [[nodiscard]] constexpr int total_samples() const {
        return frame_samples + transform_samples + lookahead_samples + holdback_samples;
    }
};

// Milliseconds for a sample count at a given coded rate. double, not a
// rounded integer: at 44.1 kHz the frame period is 34.83 ms and rounding it
// here would lose the third of a millisecond an A/V sync budget cares about.
[[nodiscard]] constexpr double latency_ms(int samples, SampleRate sample_rate) {
    const auto hz = sample_rate_hz(sample_rate);
    return hz == 0 ? 0.0 : 1000.0 * static_cast<double>(samples) / static_cast<double>(hz);
}

[[nodiscard]] constexpr double latency_ms(const LatencyBudget& budget, SampleRate sample_rate) {
    return latency_ms(budget.total_samples(), sample_rate);
}

}  // namespace ac3
