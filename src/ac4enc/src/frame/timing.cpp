#include "frame/timing.hpp"

#include <array>
#include <cstddef>

namespace ac4::detail {
namespace {

// By frame_rate_index, 0 to 13, at 48 kHz: Table 83's frame length, Table
// 188's d_pcm and d_ctrl, and the resampling ratio, 25/24, 1001/1000 x 25/24
// = 1001/960, 15/16 or 1.
constexpr std::array<int, 14> kFrameLength = {1920, 1920, 2048, 1536, 1536, 960, 960,
                                              1024, 768,  768,  512,  384,  384, 2048};
constexpr std::array<int, 14> kAlignmentDelay = {288,  288, 352, 96,   96,  960, 960,
                                                 1056, 672, 672, 1312, 864, 864, 352};
constexpr std::array<int, 14> kControlDelay = {1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4, 1};
constexpr std::array<int, 14> kUp = {1001, 25,   15, 1001, 25,   1001, 25,
                                     15,   1001, 25, 15,   1001, 25,   1};
constexpr std::array<int, 14> kDown = {960, 24, 16, 960, 24, 960, 24, 16, 960, 24, 16, 960, 24, 1};

}  // namespace

std::int64_t FrameTiming::output_before(std::int64_t frame) const noexcept {
    return frame * frame_length * decoder_up / decoder_down;
}

int FrameTiming::output_samples(std::int64_t frame) const noexcept {
    return static_cast<int>(output_before(frame + 1) - output_before(frame));
}

std::optional<FrameTiming> frame_timing(int frame_rate_index, int sample_rate_hz) noexcept {
    if (frame_rate_index < 0 || frame_rate_index > 13) {
        return std::nullopt;
    }
    if (sample_rate_hz != 48000 && !(sample_rate_hz == 44100 && frame_rate_index == 13)) {
        return std::nullopt;
    }
    const auto i = static_cast<std::size_t>(frame_rate_index);
    FrameTiming t;
    t.frame_rate_index = frame_rate_index;
    t.frame_length = kFrameLength[i];
    t.qmf_slots = t.frame_length / 64;
    // Table 192: two QMF slots an A-SPX slot, and six of history, from 1 536
    // samples; one and three below.
    t.ts_in_ats = t.frame_length >= 1536 ? 2 : 1;
    t.aspx_slots = t.qmf_slots / t.ts_in_ats;
    t.hfgen_slots = 3 * t.ts_in_ats;
    t.alignment_delay = kAlignmentDelay[i];
    t.control_delay = kControlDelay[i];
    t.decoder_up = kUp[i];
    t.decoder_down = kDown[i];
    return t;
}

}  // namespace ac4::detail
