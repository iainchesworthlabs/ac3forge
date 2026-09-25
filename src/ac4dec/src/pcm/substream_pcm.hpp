#pragma once

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ac4dec/decoder.hpp"
#include "dsp/qmf.hpp"
#include "dsp/resampler.hpp"
#include "dsp/synthesis.hpp"
#include "pcm/acpl.hpp"
#include "pcm/aspx.hpp"
#include "pcm/de.hpp"
#include "pcm/downmix.hpp"
#include "pcm/drc.hpp"
#include "pcm/mixer.hpp"
#include "pcm/routing.hpp"
#include "pcm/stereo.hpp"
#include "syntax/channel_elements.hpp"
#include "syntax/context.hpp"
#include "syntax/substream.hpp"

// One audio substream's reconstruction, frame after frame, along Part 1
// Figure 9 of ETSI TS 103 190-1 V1.4.1: the audio spectral frontend (clause
// 5.1), stereo and multichannel processing (5.3), the inverse transform with
// block switching (5.5) and frame alignment (5.6), then the QMF domain (5.7):
// analysis, companding, A-SPX, A-CPL and synthesis, for what this version
// decodes: the mono, pair, 3.0, 5.X and 7.X elements in every codec mode Part
// 1 gives them, SIMPLE, ASPX and the A-CPL modes. What it does not decode yet
// it refuses with DecodeError::kUnsupported and the phase of planning/ac4.md
// that brings it.
//
// Every codec mode passes through the QMF banks, SIMPLE included, as Figure
// 9 draws it, so the decoder's delay is one for all of them: d_pcm, the QMF
// pair's 577 samples and the ts_offset_hfgen slots of history the synthesis
// works behind (5.7.1), 352 + 577 + 384 samples at frame_rate_index 13. The
// LFE goes through the banks with the rest and nothing else touches it there:
// companding, A-SPX and A-CPL leave it out (Tables 212 to 214).
//
// At every frame_rate_index but 13 the frame is coded at an internal rate, and
// the sample rate converter (Part 1 clause 6.2.15, src/ac4core/src/dsp/
// resampler.hpp) takes the synthesis's output to 48 kHz, its phase locked to
// sequence_counter as Part 2 clause 5.11 locks it.
//
// Before the synthesis, the output processing the system configures
// (OutputConfig): dialogue enhancement (clause 5.7.8, pcm/de.hpp), then the
// output level and DRC (clause 5.7.9, pcm/drc.hpp), whose level is measured on
// the signal before dialogue enhancement (6.2.13), then the downmix (6.2.17,
// pcm/downmix.hpp), after which only the channels that come out are
// synthesised. Their values are held with the rest of the frame's control data
// until its signal reaches the QMF domain (5.7.2).
//
// A presentation of several substreams (Part 1 clause 6.2.16, Part 2 clause
// 4.8.4) decodes each of the others only as far as the QMF domain, after its
// dialogue enhancement (FrameInputs::qmf_only, then qmf_output()), and mixes
// them into the main or music and effects substream's channels ahead of its
// DRC (pcm/mixer.hpp); a hybrid dialogue enhancement method takes the
// dialogue enhancement substream's channels as its waveform. The output stages
// then run once, on the mix.

namespace ac4::detail {

// What decode() takes besides the substream: the frame's place in the stream,
// the output processing the system asks for, and what the frame's metadata
// gives it.
struct FrameInputs {
    int sequence_counter = 0;
    int converter_phase = 0;  // Part 2 clause 5.11's phi_t
    // The first frame decoded since a change of source (Part 1 clause
    // 4.3.3.2.2), which is read without the values of the frames before it.
    bool new_source = false;
    OutputConfig output{};
    DrcFrameValues drc{};
    DeFrameValues de{};
    DownmixValues downmix{};
    // A substream another's decode() mixes in: decode() stops in the QMF
    // domain after dialogue enhancement and puts out nothing; qmf_output()
    // then gives the frame's matrices.
    bool qmf_only = false;
    // For the substream the others are mixed into: this frame's mixing, held
    // with its control data, and each other substream's qmf_output() of this
    // frame, whose signal is of the same frame as this one's.
    MixValues mix{};
    std::span<const MixSource> sources{};
    // The dialogue enhancement substream's qmf_output() of this frame, the
    // waveform of a hybrid dialogue enhancement method (clause 5.7.8.9).
    std::optional<MixSource> dialogue{};
};

class SubstreamPcm {
   public:
    // Decodes one frame of `substream`, read under `ctx`, to planar PCM in
    // `channels` (one vector per output channel, full scale 1.0) and names the
    // channels in `speakers`, in speakers_of()'s order. A frame is
    // frame_len_base samples at frame_rate_index 13, and otherwise as many as
    // the converter gives at the frame's phase: at 29.97 fps 1 601 or 1 602. A
    // substream whose channel mode or frame length differs from the last
    // frame's starts from silence.
    [[nodiscard]] ParseResult decode(const SubstreamContext& ctx, const AudioSubstream& substream,
                                     const FrameInputs& frame,
                                     std::vector<std::vector<float>>& channels,
                                     std::vector<Speaker>& speakers);

    // A frame of output for a frame that would not decode, as `policy` says:
    // silence, or the last good frame repeated, fading 20 dB for each 32 ms
    // lost in a row, through the frame's own inverse transform and output
    // stages, the QMF domain passing it through. Fails before any frame has
    // decoded.
    [[nodiscard]] ParseResult conceal(ConcealmentPolicy policy, const FrameInputs& frame,
                                      std::vector<std::vector<float>>& channels,
                                      std::vector<Speaker>& speakers);

    // Whether a frame has decoded since the last configuration, which
    // concealment needs.
    [[nodiscard]] bool can_conceal() const noexcept {
        return !last_spectra_.empty() && last_spectra_.size() == channels_.size();
    }

    // Silence in every overlap buffer, delay line and filter bank, and no
    // control data held.
    void reset();

    // The frame alignment delay, d_pcm of Table 188, in samples.
    [[nodiscard]] int alignment_delay() const noexcept { return delay_; }

    // The whole decoder's delay in samples: d_pcm, the QMF pair's 577
    // samples and ts_offset_hfgen QMF slots.
    [[nodiscard]] int delay_samples() const noexcept;

    // The same at the output rate: at every frame_rate_index but 13 the
    // converter's delay added and the sum taken through its ratio, to the
    // nearest sample (Decoder::latency_samples()).
    [[nodiscard]] int output_delay_samples() const noexcept;

    // After a decode() or conceal() with FrameInputs::qmf_only: the frame's
    // QMF-domain matrices, one per channel of the channel mode, and the same
    // before its dialogue enhancement. Valid until the next call.
    [[nodiscard]] MixSource qmf_output(int key) const noexcept;

   private:
    struct Channel {
        dsp::ChannelSynthesis<double> synthesis;
        std::vector<double> delay;  // the last d_pcm samples of the previous frame
        dsp::QmfAnalysis<double> analysis;
        // Q_low_ext (pcm/aspx.hpp): kTsOffsetHfadj + ts_offset_hfgen slots of
        // the previous frames' processed QMF matrix, then this frame's.
        std::vector<QmfValue> ext;
        std::vector<QmfValue> out;  // the QMF domain's matrix, which the output stages take
        AspxChannelState aspx;
    };

    // A channel that comes out, after the downmix: its synthesis bank and, at
    // every frame_rate_index but 13, its sample rate converter.
    struct Output {
        dsp::QmfSynthesis<double> synthesis;
        std::optional<dsp::Resampler<double>> converter;
    };

    // The QMF-domain control data of one frame, held d_ctrl frames until the
    // signal it belongs to reaches the QMF domain (5.7.2).
    struct Control {
        int codec_mode = codec_mode::kSimple;
        ElementKind kind = ElementKind::kPair;
        bool add_ch_base = false;
        bool new_source = false;  // FrameInputs::new_source, for A-SPX's time differences
        std::optional<AspxConfig> aspx_config;
        std::optional<CompandingControl> companding;
        std::vector<AspxData1ch> aspx_1ch;
        std::vector<AspxData2ch> aspx_2ch;
        std::optional<AcplFrameValues> acpl;  // dequantised when the frame was read
        DrcFrameValues drc;                   // the frame's DRC and dialnorm
        DeFrameValues de;                     // its dialogue enhancement
        DownmixValues downmix;                // its downmix gains
        MixValues mix;                        // its presentation's mixing
    };

    // One aspx_data element's frame parameters and its channels' data and
    // matrices, for one of the units aspx_units() lists.
    struct UnitIo {
        AspxFrame frame;
        std::array<AspxChannelIo, 2> io{};
        std::array<int, 2> channels{};
        std::size_t count = 1;
    };

    // From the frame's spectra (spectra_ and lengths_) to its output: the
    // inverse transform, frame alignment and QMF analysis, the QMF domain with
    // `control` queued d_ctrl frames, the output stages, synthesis and the
    // converter.
    [[nodiscard]] ParseResult render(Control control, const FrameInputs& frame_inputs,
                                     std::vector<std::vector<float>>& channels,
                                     std::vector<Speaker>& speakers);
    [[nodiscard]] ParseResult configure(const SubstreamContext& ctx);
    // The output stages - DRC's channel groups, the downmix, and each channel
    // out's synthesis bank and converter - for add_ch_base and the output the
    // system asks for, rebuilt only where one of them changes.
    void configure_outputs(const SubstreamContext& ctx, const OutputConfig& output);
    [[nodiscard]] ParseResult check_control(const SubstreamContext& ctx, const ChannelElement& element) const;
    [[nodiscard]] UnitIo unit_io(const AspxUnit& unit, const Control& control, bool master_reset);
    [[nodiscard]] int channel_of(Speaker speaker) const noexcept;
    [[nodiscard]] ParseResult matrix(const SubstreamContext& ctx, const ChannelElement& element);
    void apply(const Control& control);
    void pass_through();
    void pass_through(Channel& channel) const;

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
    std::span<const Speaker> speakers_;  // the channel mode's, speakers_of()
    std::vector<Channel> channels_;      // in speakers_'s order
    std::vector<AspxUnit> units_;        // aspx_units() of the control being applied
    std::vector<int> companded_;         // its companded_speakers(), as channel indices
    std::deque<Control> held_;
    // aspx_master_freq_scale, aspx_start_freq and aspx_stop_freq of the last
    // configuration applied, for master_reset (5.7.6.3.1.1).
    std::optional<std::array<int, 3>> master_;
    // A-CPL: the stage and its state, the quantised values DIFF_TIME refers
    // to, and the codec modes of the last frame read and of the last applied,
    // a change of which starts A-CPL from its first frame's state
    // (src/ac4dec/ERRATA.md, "A change of codec mode").
    AcplStage acpl_;
    AcplQuantHistory acpl_history_;
    std::optional<int> decoded_mode_;
    std::optional<int> applied_mode_;
    // The sample rate converter's filter, which every channel's converter
    // shares, and the phase of the last frame converted.
    std::shared_ptr<const dsp::ResamplerFilter> converter_filter_;
    std::optional<int> converter_phase_;
    DeStage de_;
    DrcStage drc_;
    DownmixStage downmix_;
    MixStage mix_;
    double internal_rate_ = 48000.0;  // the rate the QMF banks run at
    bool outputs_valid_ = false;
    bool add_ch_base_ = false;
    DownmixTarget downmix_target_ = DownmixTarget::kAsCoded;
    bool mix_lfe_ = true;
    std::vector<Output> outputs_;               // in downmix_.speakers()'s order
    // The last good frame, which concealment repeats, and the frames lost
    // since it.
    std::vector<std::vector<double>> last_spectra_;
    std::vector<std::vector<int>> last_lengths_;
    ElementKind last_kind_ = ElementKind::kPair;
    DrcFrameValues last_drc_;
    DeFrameValues last_de_;
    DownmixValues last_downmix_;
    MixValues last_mix_;
    int losses_ = 0;
    std::vector<std::vector<QmfValue>> mixed_;  // the downmix's matrices
    std::vector<std::vector<QmfValue>*> mixed_matrices_;
    // The matrices before dialogue enhancement, DRC's side chain, where both act.
    std::vector<std::vector<QmfValue>> side_;
    std::vector<std::vector<QmfValue>*> side_matrices_;
    bool side_kept_ = false;  // whether the last frame's side chain is side_ rather than the matrices

    // Scratch, kept to save an allocation per frame.
    ElementRoute route_;
    std::vector<StereoParameters> parameters_;  // one channel data element's, 32 KiB each
    std::vector<std::vector<double>> scaled_;   // per track, in bitstream order
    // The layouts align_tracks() gives a pair with b_dual_maxsfb, and per
    // track the one it takes, or -1 for its own.
    std::vector<SfData> dual_layouts_;
    std::vector<int> dual_layout_of_;
    std::vector<std::vector<double>> spectra_;  // per channel, in window order
    std::vector<int> track_of_;                 // per channel, the track its lines are in
    std::vector<double> pcm_;
    std::vector<double> converted_;
    std::vector<double> aligned_;
    std::vector<std::vector<int>> lengths_;  // per channel, its blocks' lengths
    std::vector<std::vector<QmfValue>*> matrices_;  // per channel, its `out`, for A-CPL
};

}  // namespace ac4::detail
