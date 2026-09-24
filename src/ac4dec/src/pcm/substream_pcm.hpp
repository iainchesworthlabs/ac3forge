#pragma once

#include <optional>
#include <vector>

#include "ac4dec/decoder.hpp"
#include "dsp/synthesis.hpp"
#include "syntax/context.hpp"
#include "syntax/substream.hpp"

// One audio substream's reconstruction, frame after frame: the audio spectral
// frontend (clause 5.1), stereo processing (5.3), the inverse transform with
// block switching (5.5) and frame alignment (5.6) of ETSI TS 103 190-1 V1.4.1,
// for what this version decodes. What it does not decode yet it refuses with
// DecodeError::kUnsupported and the phase of planning/ac4.md that brings it.

namespace ac4::detail {

class SubstreamPcm {
   public:
    // Decodes one frame of `substream`, read under `ctx`, to planar PCM in
    // `channels` (one vector per output channel, frame_len_base samples each,
    // full scale 1.0) and names the channels in `speakers`. A substream whose
    // channel mode or frame length differs from the last frame's starts from
    // silence.
    [[nodiscard]] ParseResult decode(const SubstreamContext& ctx, const AudioSubstream& substream,
                                     int sequence_counter, std::vector<std::vector<float>>& channels,
                                     std::vector<Speaker>& speakers);

    // Silence in every overlap buffer and delay line.
    void reset();

    // The frame alignment delay, d_pcm of Table 188, in samples.
    [[nodiscard]] int alignment_delay() const noexcept { return delay_; }

   private:
    struct Channel {
        dsp::ChannelSynthesis<double> synthesis;
        std::vector<double> delay;  // the last d_pcm samples of the previous frame
    };

    [[nodiscard]] ParseResult configure(const SubstreamContext& ctx, std::size_t channel_count);

    int full_length_ = 0;
    int ch_mode_ = -1;
    int frame_rate_index_ = -1;
    int delay_ = 0;
    std::optional<dsp::TransformSet<double>> transforms_;
    std::vector<Channel> channels_;

    // Scratch, kept to save an allocation per frame.
    std::vector<std::vector<double>> scaled_;
    std::vector<double> spectrum_;
    std::vector<double> pcm_;
    std::vector<std::vector<int>> lengths_;  // per channel, its blocks' lengths
};

}  // namespace ac4::detail
