#pragma once

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/ac4dec/decoder.hpp"
#include "iclforge/ac4core/dsp/qmf.hpp"
#include "iclforge/ac4core/dsp/resampler.hpp"
#include "iclforge/ac4core/dsp/synthesis.hpp"
#include "pcm/acpl.hpp"
#include "pcm/asf_reconstruct.hpp"
#include "pcm/ajcc.hpp"
#include "pcm/ajoc.hpp"
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
// 1 gives them, SIMPLE, ASPX and the A-CPL modes; and the immersive element of
// the 7.X.4 modes along Part 2 Figure 5 of ETSI TS 103 190-2 V1.3.1, with its
// SMP (clause 5.2), S-CPL between the inverse transform and the analysis
// (5.3, pcm/immersive.hpp), A-SPX with its gains (4.8.3.11), A-CPL (5.5) and
// A-JCC (5.6, pcm/ajcc.hpp), in full or core decoding (4.7). What it does not
// decode yet it refuses with DecodeError::kUnsupported and the phase of
// planning/ac4.md that brings it.
//
// Every codec mode passes through the QMF banks, SIMPLE included, as Figure
// 9 draws it, so the decoder's delay is one for all of them: d_pcm, the QMF
// pair's 577 samples and the ts_offset_hfgen slots of history the synthesis
// works behind (5.7.1), 352 + 577 + 384 samples at frame_rate_index 13. The
// LFE goes through the banks with the rest and nothing else touches it there:
// companding, A-SPX and A-CPL leave it out (Tables 212 to 214).
//
// At every frame_rate_index but 13 the frame is coded at an internal rate, and
// the sample rate converter (Part 1 clause 6.2.15, src/ac4core/include/iclforge/ac4core/dsp/
// resampler.hpp) takes the synthesis's output to 48 kHz, its phase locked to
// sequence_counter as Part 2 clause 5.11 locks it.
//
// Before the synthesis, the output processing the system configures
// (OutputConfig): dialogue enhancement (clause 5.7.8, pcm/de.hpp), then the
// output level and DRC (clause 5.7.9, pcm/drc.hpp), whose level is measured on
// the signal before dialogue enhancement (6.2.13), then the downmix (6.2.17,
// pcm/downmix.hpp), for the immersive element Part 2's channel renderer
// (clause 5.10.2, pcm/renderer.hpp), after which only the channels that come
// out are synthesised. Their values are held with the rest of the frame's control data
// until its signal reaches the QMF domain (5.7.2).
//
// A presentation of several substreams (Part 1 clause 6.2.16, Part 2 clause
// 4.8.4) decodes each of the others only as far as the QMF domain, after its
// dialogue enhancement (FrameInputs::qmf_only, then qmf_output()), and mixes
// them into the main or music and effects substream's channels ahead of its
// DRC (pcm/mixer.hpp); a hybrid dialogue enhancement method takes the
// dialogue enhancement substream's channels as its waveform. The output stages
// then run once, on the mix.
//
// An object audio substream (Part 2 clause 6.2.3) decodes its element the same
// way, in the layout pcm_layout() gives it (pcm/routing.hpp): an A-JOC
// substream's downmix, whose objects A-JOC makes after A-SPX in full decoding
// (pcm/ajoc.hpp), or a direct-coded substream's objects, the channels of its
// element. With FrameInputs::objects, decode() then puts out each object's
// PCM, with the output level gain alone (clause 5.7.9.3.3), in object order:
// the LFE first, then the upmix objects, or in core decoding the downmix's
// signals in QinAJOC's order (a static downmix's L, R, C, Ls and Rs), or the
// element's channels L, R, C, Ls and Rs as it has them.

namespace iclforge::ac4::detail {

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
    DecodingMode decoding = DecodingMode::kFull;  // Part 2 clause 4.7
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
    // An object audio substream: decode() puts out its objects' PCM in
    // `channels`, in object order, and no speakers.
    bool objects = false;
    // The substream's own tracks, where the caller holds them and needs them no more after the
    // frame: decode() frees them once the reconstruction and the stereo and multichannel steps
    // have read them, before the QMF stages (about 23 KB a channel pair at 2048 samples).
    std::vector<Track>* release_tracks = nullptr;
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
        // Built where it stays (channels_.emplace_back): a Channel is 5 KB at double,
        // too much for a temporary.
        Channel(int full_length, std::size_t delay_samples, std::size_t ext_values,
                std::size_t out_values)
            : synthesis(full_length),
              delay(delay_samples, Real{}),
              ext(ext_values),
              out_count(std::min(out_values, ext_values)) {}

        // The QMF domain's matrix, which the output stages take: ext's first num_qmf_timeslots
        // slots. A-SPX writes each of them from a slot of ext at or after it, and the next
        // frame's history is ext's last slots, which it does not reach; the history moves to
        // the front when the next frame's render() begins, after every stage has read it.
        [[nodiscard]] QmfMatrix out() noexcept { return QmfMatrix(ext).first(out_count); }

        dsp::ChannelSynthesis<Real> synthesis;
        std::vector<Real> delay;  // the last d_pcm samples of the previous frame
        dsp::QmfAnalysis<Real> analysis;
        // Q_low_ext (pcm/aspx.hpp): kTsOffsetHfadj + ts_offset_hfgen slots of
        // the previous frames' processed QMF matrix, then this frame's.
        std::vector<QmfValue> ext;
        std::size_t out_count = 0;  // out()'s values
        AspxChannelState aspx;
    };

    // A channel that comes out, after the downmix: its synthesis bank and, at
    // every frame_rate_index but 13, its sample rate converter.
    struct Output {
        dsp::QmfSynthesis<Real> synthesis;
        std::optional<dsp::Resampler<Real>> converter;
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
        std::optional<AjccFrameValues> ajcc;  // decoded when the frame was read
        std::optional<AjocFrameValues> ajoc;  // decoded and dequantised when the frame was read
        double dialogue_db = 0.0;             // G_DE, for A-JOC's dialogue enhancement
        DrcFrameValues drc;                   // the frame's DRC and dialnorm
        DeFrameValues de;                     // its dialogue enhancement
        DownmixValues downmix;                // its downmix gains
        MixValues mix;                        // its presentation's mixing
    };

    // The second channel of an aspx_data_2ch() core decoding takes the first
    // of alone (AspxUnit::first_only): its A-SPX state, which its data's
    // differences along time need, silence for its low band, and a matrix for
    // the high band nothing uses.
    struct Ghost {
        std::vector<QmfValue> ext;
        std::vector<QmfValue> out;
        AspxChannelState aspx;
    };

    // One aspx_data element's frame parameters and its channels' data and
    // matrices, for one of the units aspx_units() lists; a channel of -1 is a
    // ghost.
    struct UnitIo {
        AspxFrame frame;
        std::array<AspxChannelIo, 2> io{};
        std::array<int, 2> channels{};
        std::size_t count = 1;
    };

    // From the frame's spectra (spectra_ and lengths_) to its output: the
    // inverse transform, frame alignment and QMF analysis, the QMF domain with
    // the frame's control data, which the caller has made at the back of held_
    // (a Control is 7 KB, too much for a local), queued d_ctrl frames, the output
    // stages, synthesis and the converter.
    [[nodiscard]] ParseResult render(const FrameInputs& frame_inputs,
                                     std::vector<std::vector<float>>& channels,
                                     std::vector<Speaker>& speakers);
    [[nodiscard]] ParseResult configure(const SubstreamContext& ctx, DecodingMode decoding);
    // The output stages - DRC's channel groups, the downmix, and each channel
    // out's synthesis bank and converter - for add_ch_base, the immersive
    // element's presence flags and the output the system asks for, rebuilt
    // only where one of them changes.
    void configure_outputs(const SubstreamContext& ctx, const OutputConfig& output);
    [[nodiscard]] ParseResult check_control(const SubstreamContext& ctx, const ChannelElement& element) const;
    [[nodiscard]] UnitIo unit_io(const AspxUnit& unit, const Control& control, bool master_reset);
    [[nodiscard]] int channel_of(Speaker speaker) const noexcept;
    [[nodiscard]] ParseResult matrix(const SubstreamContext& ctx, const ChannelElement& element);
    void apply(const Control& control);
    // A-JOC on the downmix (pcm/ajoc.hpp), after A-SPX: the upmix objects in
    // full decoding, dialogue enhancement in core decoding.
    void apply_ajoc(const AjocFrameValues& values, double dialogue_db);
    // The object audio substream's objects' matrices in object order
    // (object_matrices_), and their PCM into `channels`.
    void collect_objects();
    void synthesise_objects(const FrameInputs& frame_inputs, const DrcFrameValues& drc,
                            std::vector<std::vector<float>>& channels);
    // The immersive element's A-SPX gains and core decoding's gain in place of
    // A-CPL (pcm/immersive.hpp), after A-SPX made `units`.
    void apply_immersive_gains(const Control& control, std::span<const UnitIo> units,
                               std::span<const aspx::SubbandGroups> groups);
    // Below the crossover and everywhere in SIMPLE mode a channel's `out` is its `ext` from
    // ts_offset_hfadj on. pass_through() for the whole frame does not copy it: it leaves
    // `out_in_ext_` set, and the output stages that only read the matrix read the window of `ext`.
    // materialize_out() makes the copy where something else needs `out`.
    void pass_through();
    void pass_through(Channel& channel) const;
    void materialize_out();
    // The last slots of `ext` become the next frame's history.
    void shift_history();

    int full_length_ = 0;
    int ch_mode_ = -1;  // the element's layout: pcm_layout()'s
    // How the substream codes its audio, and an A-JOC substream's downmix.
    AudioCoding coding_ = AudioCoding::kChannel;
    bool static_dmx_ = false;
    int dmx_signals_ = 0;
    int umx_signals_ = 0;
    bool object_lfe_ = false;
    DecodingMode decoding_ = DecodingMode::kFull;
    int frame_rate_index_ = -1;
    int fs_index_ = 1;
    int delay_ = 0;
    int control_delay_ = 1;  // d_ctrl
    int slots_ = 0;          // num_qmf_timeslots
    int ts_in_ats_ = 1;      // num_ts_in_ats
    int hfgen_ = 0;          // ts_offset_hfgen
    std::optional<dsp::TransformSet<Real>> transforms_;
    std::span<const Speaker> speakers_;  // the channel mode's, speakers_of()
    std::vector<Channel> channels_;      // in speakers_'s order
    // Core decoding's ASPX_SCPL, by aspx_data_2ch() index; empty otherwise.
    std::vector<Ghost> ghosts_;
    // The immersive element's codec mode of the last frame decoded, whose
    // spectra S-CPL takes after the inverse transform, concealment's included;
    // unset for the other elements.
    std::optional<int> scpl_mode_;
    std::vector<AspxUnit> units_;        // aspx_units() of the control being applied
    std::vector<int> companded_;         // its companded_speakers(), as channel indices
    std::deque<Control> held_;
    // aspx_master_freq_scale, aspx_start_freq and aspx_stop_freq of the last
    // configuration applied, for master_reset (5.7.6.3.1.1).
    std::optional<std::array<int, 3>> master_;
    // A-CPL: the stage and its state, the quantised values DIFF_TIME refers
    // to, and the codec modes of the last frame read and of the last applied,
    // a change of which starts A-CPL from its first frame's state
    // (src/ac4dec/ERRATA.md, "A change of codec mode"). The stage is 119 KB at
    // double and 65 KB at float, five decorrelators' history for the most part,
    // so it is made by the first frame whose codec mode applies A-CPL: a stream
    // with none holds none, and a stage just made is in the first frame's state.
    std::unique_ptr<AcplStage> acpl_;
    AcplQuantHistory acpl_history_;
    // The frame's A-CPL values, read before anything moves on and copied into the
    // frame's control data once it has: 4.7 KB, made with the first A-CPL frame.
    std::unique_ptr<AcplFrameValues> acpl_next_;
    // A-JCC's stage and the quantised values DIFF_TIME refers to, kept as
    // A-CPL's are; it is 169 KB at double and 103 KB at float.
    std::unique_ptr<AjccStage> ajcc_;
    AjccQuantHistory ajcc_history_;
    // A-JOC's stage, its quantised values DIFF_TIME refers to (and a copy to
    // decode a frame against), and the upmix objects' matrices of the frame;
    // whether this frame's control applied A-JOC.
    AjocStage ajoc_;
    AjocQuantHistory ajoc_history_;
    AjocQuantHistory ajoc_history_next_;
    std::vector<std::vector<QmfValue>> objects_;
    bool ajoc_applied_ = false;
    // Every channel's matrix of this frame is the window of its `ext` (pass_through()), `out`
    // stale.
    bool out_in_ext_ = false;
    std::vector<QmfMatrix> ajoc_inputs_;
    std::vector<QmfMatrix> ajoc_inputs_in_place_;
    // The objects' matrices in object order, and each object's synthesis bank
    // and converter.
    std::vector<QmfMatrix> object_matrices_;
    std::vector<Output> object_outputs_;
    std::optional<int> decoded_mode_;
    std::optional<int> applied_mode_;
    // The sample rate converter's filter, which every channel's converter
    // shares, and the phase of the last frame converted. Its table is kept in
    // Real (designed in double, rounded once), for the dot product to run in
    // Real.
    std::shared_ptr<const dsp::BasicResamplerFilter<Real>> converter_filter_;
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
    std::optional<ImmersiveLayout> layout_;     // the immersive element's, for the renderer
    std::vector<Output> outputs_;               // in downmix_.speakers()'s order
    // The last good frame, which concealment repeats, and the frames lost
    // since it.
    std::vector<std::vector<Real>> last_spectra_;
    std::vector<int> last_exponents_;
    std::vector<std::vector<int>> last_lengths_;
    ElementKind last_kind_ = ElementKind::kPair;
    DrcFrameValues last_drc_;
    DeFrameValues last_de_;
    DownmixValues last_downmix_;
    MixValues last_mix_;
    int losses_ = 0;
    std::vector<std::vector<QmfValue>> mixed_;  // the downmix's matrices
    std::vector<QmfMatrix> mixed_matrices_;
    // The matrices before dialogue enhancement, DRC's side chain, where both act.
    std::vector<std::vector<QmfValue>> side_;
    std::vector<QmfMatrix> side_matrices_;
    bool side_kept_ = false;  // whether the last frame's side chain is side_ rather than the matrices

    // Scratch, kept to save an allocation per frame.
    // The QMF banks' working space, which they use one after another: the banks
    // of the substream's channels share this one.
    dsp::QmfScratch<Real> qmf_scratch_{};
    // A-SPX's per-channel matrices (16 KB at double), made by the first frame that
    // decodes A-SPX.
    std::unique_ptr<AspxScratch> aspx_scratch_;
    ElementRoute route_;
    std::vector<StereoParameters> parameters_;  // one channel data element's, 16 or 32 KiB each
    std::vector<std::vector<Real>> scaled_;     // per track, in bitstream order
    // Each track's lines are its values times 2^-exponent, and each channel's
    // spectrum alike: 0 at double and float, the track's own at Fixed32
    // (pcm/asf_reconstruct.hpp), and one exponent for tracks or channels that
    // a matrix mixes.
    std::vector<int> scaled_exponents_;
    std::vector<int> spectra_exponents_;
    // reconstruct_track()'s 2^((sf - 100) / 4), made with the substream (1 KB at float).
    ScaleFactorGains sf_gain_ = scale_factor_gains();
    // The layouts align_tracks() gives a pair with b_dual_maxsfb, and per
    // track the one it takes, or -1 for its own.
    std::vector<SfData> dual_layouts_;
    std::vector<int> dual_layout_of_;
    std::vector<std::vector<Real>> spectra_;  // per channel, in window order
    std::vector<int> track_of_;               // per channel, the track its lines are in
    std::vector<Real> pcm_;
    // The inverse transform's frame: every channel's in a frame with S-CPL, else one channel's
    // at a time (render()).
    std::vector<std::vector<Real>> time_;
    std::vector<Real> converted_;
    std::vector<Real> aligned_;
    std::vector<std::vector<int>> lengths_;  // per channel, its blocks' lengths
    std::vector<QmfMatrix> matrices_;  // per channel, its `out`, for A-CPL
};

}  // namespace iclforge::ac4::detail
