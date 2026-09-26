#pragma once

#include <cstdint>
#include <optional>

// The frame grid of a frame_rate_index: ETSI TS 103 190-1 V1.4.1 Tables 83 and
// 84 (the frame length and the decoder's resampling ratio), 188 (the frame
// alignment and control data delays), 189 and 192 (the QMF and A-SPX time
// slots).
//
// At every index but 13 a frame is coded at an internal rate, frame_length
// samples a frame: 46 080 Hz at 24, 30, 48, 60 and 120 fps, 51 200 Hz at 25,
// 50 and 100, and 46 033.97 Hz at the 1000/1001 rates. The decoder converts to
// 48 kHz by decoder_up / decoder_down, and the encoder its 48 kHz input to the
// internal rate by the inverse (ac4core's dsp/resampler.hpp).
//
// The encoder's signal axis against the decoder's. A frame's transform window
// starts at the frame's first sample, so the decoder's output of frame f,
// once overlapped, is the signal's samples f N to (f + 1) N; frame alignment
// delays that by d_pcm, so the decoder's QMF slot g holds the signal's 64
// samples from 64 g - d_pcm. A frame's QMF-domain control data waits d_ctrl
// frames (5.7.2), and A-SPX's interval runs behind ts_offset_hfgen slots of
// history (5.7.6.3.2): frame f's control data applies to slots
// qmf_slots (f + d_ctrl) - hfgen_slots on, and its dialogue enhancement, DRC
// and gains to slots qmf_slots (f + d_ctrl) on.

namespace ac4::detail {

struct FrameTiming {
    int frame_rate_index = 13;
    int frame_length = 2048;    // frame_len_base: a frame's samples at the internal rate
    int qmf_slots = 32;         // num_qmf_timeslots
    int ts_in_ats = 2;          // num_ts_in_ats
    int aspx_slots = 16;        // num_aspx_timeslots
    int hfgen_slots = 6;        // ts_offset_hfgen
    int alignment_delay = 352;  // d_pcm, samples
    int control_delay = 1;      // d_ctrl, frames
    // The decoder's resampling ratio, output samples per internal sample.
    int decoder_up = 1;
    int decoder_down = 1;

    // 1 536 samples and up: b_long_frame and a transform length per half
    // (Part 1 Table 37); below that one transf_length for the frame.
    [[nodiscard]] bool long_family() const noexcept { return frame_length >= 1536; }
    [[nodiscard]] bool resampled() const noexcept { return decoder_up != decoder_down; }
    // The output samples before frame f's, floor(f R), and frame f's own,
    // R = frame_length decoder_up / decoder_down: at 29.97 fps 1 601, 1 602,
    // 1 601, 1 602 and 1 602 in turn from a frame whose sequence_counter is a
    // multiple of five, which Part 2 clause 5.11 locks the decoder's
    // converter to.
    [[nodiscard]] std::int64_t output_before(std::int64_t frame) const noexcept;
    [[nodiscard]] int output_samples(std::int64_t frame) const noexcept;
    // The decoder's delay at the internal rate: d_pcm, the QMF banks' 577
    // samples and ts_offset_hfgen QMF slots (Part 1 clause 5.7.1).
    [[nodiscard]] int decoder_delay() const noexcept {
        return alignment_delay + 577 + hfgen_slots * 64;
    }
};

// The grid of `frame_rate_index` at `sample_rate_hz`: indices 0 to 13 at 48
// kHz and 13 alone at 44.1 kHz (Table 84); nothing for the reserved ones.
[[nodiscard]] std::optional<FrameTiming> frame_timing(int frame_rate_index,
                                                      int sample_rate_hz) noexcept;

}  // namespace ac4::detail
