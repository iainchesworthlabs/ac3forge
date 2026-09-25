#pragma once

#include <array>
#include <deque>
#include <optional>
#include <vector>

#include "ac4dec/decoder.hpp"
#include "dsp/qmf.hpp"
#include "dsp/synthesis.hpp"
#include "pcm/aspx.hpp"
#include "syntax/channel_elements.hpp"
#include "syntax/context.hpp"
#include "syntax/substream.hpp"

// One audio substream's reconstruction, frame after frame, along Part 1
// Figure 9 of ETSI TS 103 190-1 V1.4.1: the audio spectral frontend (clause
// 5.1), stereo processing (5.3), the inverse transform with block switching
// (5.5) and frame alignment (5.6), then the QMF domain (5.7): analysis,
// companding, A-SPX and synthesis, for what this version decodes. What it
// does not decode yet it refuses with DecodeError::kUnsupported and the phase
// of planning/ac4.md that brings it.
//
// Every codec mode passes through the QMF banks, SIMPLE included, as Figure
// 9 draws it, so the decoder's delay is one for all of them: d_pcm, the QMF
// pair's 577 samples and the ts_offset_hfgen slots of history the synthesis
// works behind (5.7.1), 352 + 577 + 384 samples at frame_rate_index 13.

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

    // Silence in every overlap buffer, delay line and filter bank, and no
    // control data held.
    void reset();

    // The frame alignment delay, d_pcm of Table 188, in samples.
    [[nodiscard]] int alignment_delay() const noexcept { return delay_; }

    // The whole decoder's delay in samples: d_pcm, the QMF pair's 577
    // samples and ts_offset_hfgen QMF slots.
    [[nodiscard]] int delay_samples() const noexcept;

   private:
    struct Channel {
        dsp::ChannelSynthesis<double> synthesis;
        std::vector<double> delay;  // the last d_pcm samples of the previous frame
        dsp::QmfAnalysis<double> analysis;
        dsp::QmfSynthesis<double> qmf_synthesis;
        // Q_low_ext (pcm/aspx.hpp): kTsOffsetHfadj + ts_offset_hfgen slots of
        // the previous frames' processed QMF matrix, then this frame's.
        std::vector<QmfValue> ext;
        std::vector<QmfValue> out;  // what the synthesis bank takes
        AspxChannelState aspx;
    };

    // The QMF-domain control data of one frame, held d_ctrl frames until the
    // signal it belongs to reaches the QMF domain (5.7.2).
    struct Control {
        int codec_mode = codec_mode::kSimple;
        std::optional<AspxConfig> aspx_config;
        std::optional<CompandingControl> companding;
        std::vector<AspxData1ch> aspx_1ch;
        std::vector<AspxData2ch> aspx_2ch;
    };

    [[nodiscard]] ParseResult configure(const SubstreamContext& ctx, std::size_t channel_count);
    [[nodiscard]] ParseResult check_control(const SubstreamContext& ctx, const ChannelElement& element) const;
    void apply(const Control& control);
    void pass_through();

    int full_length_ = 0;
    int ch_mode_ = -1;
    int frame_rate_index_ = -1;
    int fs_index_ = 1;
    int delay_ = 0;
    int control_delay_ = 1;  // d_ctrl
    int slots_ = 0;          // num_qmf_timeslots
    int ts_in_ats_ = 1;      // num_ts_in_ats
    int hfgen_ = 0;          // ts_offset_hfgen
    std::optional<dsp::TransformSet<double>> transforms_;
    std::vector<Channel> channels_;
    std::deque<Control> held_;
    // aspx_master_freq_scale, aspx_start_freq and aspx_stop_freq of the last
    // configuration applied, for master_reset (5.7.6.3.1.1).
    std::optional<std::array<int, 3>> master_;

    // Scratch, kept to save an allocation per frame.
    std::vector<std::vector<double>> scaled_;
    std::vector<double> spectrum_;
    std::vector<double> pcm_;
    std::vector<double> aligned_;
    std::vector<std::vector<int>> lengths_;  // per channel, its blocks' lengths
};

}  // namespace ac4::detail
