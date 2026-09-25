#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac4/ac4.hpp"
#include "ac4/syntax.hpp"
#include "ac4enc/export.hpp"

// An AC-4 encoder: ETSI TS 103 190-1 V1.4.1 (2025-07), "Part 1: Channel based
// coding", and ETSI TS 103 190-2 V1.3.1 (2025-07), "Part 2: Immersive and
// personalized audio", written from the published texts. Clause numbers below
// name the part that defines the element.
//
// What this version writes: mono, stereo, 5.0 or 5.1 PCM at 48 kHz, at every
// frame rate of Part 1 Table 83, or at 44.1 kHz in frames of 2 048 samples
// (frame_rate_index 13, the one Table 84 has), as one presentation of one
// channel-coded substream, at a constant, average or variable bit rate
// (RateMode). At every frame rate but index 13's the input is
// converted to the rate the frames are coded at, the inverse of the
// decoder's conversion (Tables 83 and 84's resampling ratio). The codec
// mode is SIMPLE, the audio spectral frontend with block switching and
// MDCT-domain stereo processing for each channel pair; or ASPX, which codes
// the spectral frontend up to a crossover and recreates the band above it
// with A-SPX, with companding at the lower rates in mono and stereo; or, in
// 5.0 and 5.1, ASPX_ACPL_2 or ASPX_ACPL_3, which code a downmix in the ASPX
// way and rebuild the channels from it with A-CPL. 5.0 and 5.1 take the 5.X
// element in the form DEE's streams have it (coding_config 0 and 2ch_mode 0:
// L and R as a pair, Ls and Rs as a pair, C alone, and the LFE); its other
// coding configurations, 7.0 and 7.1 in the 7.X element, ASPX_ACPL_1, and
// A-CPL in stereo are experimental. The table of contents is bitstream
// version 2 with presentation version 1, and the presentation substream
// carries the dialogue normalisation it is given and, as configured, further
// loudness values, DRC's decoder modes and the stereo downmix's values; the
// audio substream's metadata() carries dialogue enhancement's parameters.
// Everything else in the plan's later phases (immersive layouts, several
// presentations) is refused by name as an invalid configuration.
//
// src/ac4enc/ERRATA.md records the readings the writer alone needs; where the
// decoder depends on the same reading, src/ac4dec/ERRATA.md has it.

namespace ac4 {

enum class EncodeError : std::uint8_t {
    kInvalidConfig,  // a configuration this version does not encode - see the header comment
    kInvalidInput,   // a channel count or lengths that do not match, or a sample that is not finite
};

[[nodiscard]] AC4ENC_EXPORT std::string_view describe(EncodeError error);

// The channel element's codec mode (Part 1 clause 4.3.6.1).
enum class CodecMode : std::uint8_t {
    // In 5.0 and 5.1, ASPX_ACPL_3 below 22.4 kbps a channel (the LFE not
    // counted) and ASPX_ACPL_2 below 33.6, as DEE's 5.1 streams are
    // ASPX_ACPL_3 at 96 kbps and ASPX_ACPL_2 at 128 and 144; then ASPX below 96
    // kbps a channel in mono and stereo and below 76.8 in the 5.X and 7.X
    // elements, as DEE's streams switch at 192 kbps in stereo and 384 in 5.1;
    // SIMPLE from there.
    kAuto,
    kSimple,  // the audio spectral frontend over the whole band
    // The spectral frontend up to A-SPX's crossover and A-SPX above it. In
    // mono and stereo 7.5 kHz below 32 kbps a channel, 10.5 kHz below 48 and
    // 13.5 kHz from there, with companding below 64 kbps a channel; in 5.X and
    // 7.X 12 kHz from 38.4 kbps a channel and 12.75 kHz from 51.2, without
    // companding.
    kAspx,
    // With experimental.acpl: ASPX_ACPL_2 with each pair's residual, what the
    // downmix leaves out of it, coded up to 3 kHz (acpl_qmf_band 8), below
    // which the decoder rebuilds the pair from the two as they are.
    kAspxAcpl1,
    // 5.0 and 5.1: the downmixes (L + Ls / sqrt 2) / 2 and (R + Rs / sqrt 2) / 2
    // coded as a pair and C alone, in the ASPX way from 12.75 kHz, and L, R,
    // Ls and Rs rebuilt from them by A-CPL (Part 1 clause 5.7.7.6.1). Stereo,
    // with experimental.acpl: (L + R) / 2 coded alone, in the ASPX way as
    // stereo is at the rate, and L and R rebuilt from it (clause 5.7.7.5).
    kAspxAcpl2,
    // 5.0 and 5.1: the Lo/Ro downmix coded as a pair, in the ASPX way from 12
    // kHz, and all five channels rebuilt from it by A-CPL (clause 5.7.7.6.2).
    kAspxAcpl3,
};

// How frames share the rate (Part 1 Table 81's wait_frames).
enum class RateMode : std::uint8_t {
    // Every frame bitrate_kbps' share, to the byte (wait_frames 0).
    kConstant,
    // Each frame as long as its content needs at its masking thresholds, as
    // far as the decoder's input buffer of Part 1 clause 6.2.4 (six frames at
    // the rate, twelve above 60 fps) lets frames lend each other bytes, with
    // bitrate_kbps over the long term (wait_frames 1 to 6: the frames a
    // decoder that starts at the frame waits before its output, and Part 2
    // Table 52's br_code carrying the rate, Part 2 Annex B).
    kAverage,
    // As kAverage without the buffer: frames lend each other up to two
    // seconds' share of the rate (wait_frames 7).
    kVariable,
};

// The 7.X element's pair beyond L, R, C, Ls and Rs (Part 1 Table 88).
enum class AdditionalPair : std::uint8_t {
    kNone,
    kBack,      // 3/4/0: Lb and Rb
    kWide,      // 5/2/0: Lw and Rw
    kTopFront,  // 3/2/2: Tfl and Tfr
};

// --- Metadata ----------------------------------------------------------------
//
// What the presentation substream (Part 2 clause 6.2.2.3) and the audio
// substream's metadata() (6.2.7) carry beside the audio, as the caller
// configures it; the semantics are Part 1 clauses 4.3.12 to 4.3.14. Each is
// written only where it is configured. As DEE's streams do, the values that
// hold for the stream go in I-frames, and a decoder keeps them until the next.

// Part 1 Table 156: the practice the programme loudness was measured by.
enum class LoudnessPractice : std::uint8_t {
    kNotIndicated = 0,
    kAtscA85 = 1,
    kEbuR128 = 2,
    kAribTrB32 = 3,
    kFreeTvOp59 = 4,
    kManual = 14,
    kConsumerLeveller = 15,
};

// Part 1 Table 157: how dialogue was gated.
enum class DialogueGating : std::uint8_t {
    kNotIndicated = 0,
    kCentreOrLeftRight = 1,  // automated, on C or on the power sum of L and R
    kLeftCentreRight = 2,    // automated, on each front channel
    kManual = 3,
};

// further_loudness_info() (Part 2 clause 6.2.7.3, Part 1 clause 4.3.12.3): the
// programme's loudness as the caller measured it, without dialogue
// normalisation or DRC applied. Written in every frame, the values in
// I-frames, in steps of 0.1 dB.
struct FurtherLoudness {
    LoudnessPractice practice = LoudnessPractice::kNotIndicated;  // loud_prac_type
    // With a practice: the dialogue gating the programme's loudness was
    // corrected with, if any, and whether the correction ran in real time
    // rather than over the whole file.
    std::optional<DialogueGating> corrected_with_gating;
    bool corrected_in_real_time = false;
    std::optional<double> integrated_lkfs;    // loudrelgat: BS.1770, relative gated
    std::optional<double> speech_gated_lkfs;  // loudspchgat, gated as `speech_gating` says
    DialogueGating speech_gating = DialogueGating::kNotIndicated;
    std::optional<double> max_short_term_lufs;  // max_loudstrm3s: the loudest 3 s
    std::optional<double> max_true_peak_dbtp;   // max_truepk
    std::optional<double> loudness_range_lu;    // lra, EBU Tech 3342
    bool loudness_range_v2 = true;              // lra_prac_type: EBU Tech 3342 v2, or v1
    std::optional<double> max_momentary_lufs;   // max_loudmntry
};

// Part 1 Table 160: drc_eac3_profile, and Table 162's default profiles.
enum class DrcProfile : std::uint8_t {
    kNone,
    kFilmStandard,
    kFilmLight,
    kMusicStandard,
    kMusicLight,
    kSpeech,
};

// One DRC decoder mode (Part 1 clause 4.3.13.3, Table 72).
struct DrcModeConfig {
    // Table 161: 0 home theatre, 1 flat panel TV, 2 portable speakers, 3
    // portable headphones; 4 to 7 for the output levels from
    // `output_level_from_db` down to `output_level_to_db`, 0 to -31 dBFS.
    int id = 0;
    int output_level_from_db = 0;
    int output_level_to_db = 0;
    // What the mode compresses with: the stream's default profile, sent as
    // drc_default_profile_flag; another profile, sent as its compression curve
    // (Table 166's parameters, Table 162's values); or another mode's
    // configuration, by that mode's id (drc_repeat_profile_flag).
    std::optional<DrcProfile> profile;
    std::optional<int> repeat_of;
    // With experimental.drc_gains: the gains the profile (the stream's
    // default where unset) gives the input, computed frame by frame as a
    // decoder applying it would and sent in every frame
    // (drc_compression_curve_flag 0), as Table 163's drc_gains_config: 0 one
    // gain a frame for every channel, 1 a gain per channel group (Table 168)
    // and subframe (Table 169), 2 and 3 those in 2 and 4 bands (Table 164).
    // Every group and band takes the programme's gain, the curve being
    // defined on the programme's level; configurations 1 to 3 add the
    // subframes' resolution in time.
    std::optional<int> gains_config;
};

// DRC (Part 1 clause 4.3.13): drc_config() in I-frames, which is where
// DEE's streams send it.
struct DrcConfig {
    // drc_eac3_profile: the profile the modes without their own take, and the
    // one a transcoder to E-AC-3 applies (Part 1 clause 5.7.9.4).
    DrcProfile profile = DrcProfile::kFilmLight;
    // The modes; empty sends the four of Table 161 on the default profile, as
    // DEE's streams do.
    std::vector<DrcModeConfig> modes;
};

// Part 1 Table 150: the downmix the stream prefers.
enum class PreferredDownmix : std::uint8_t {
    kNotIndicated,
    kLoRo,
    kLtRt,
    kLtRtProLogicII,
};

// The stereo downmix's values (Part 2 clause 6.2.9.2's custom_dmx_data() and
// 6.2.9.1's loud_corr(); Part 1 clauses 4.3.12.2.8 to 4.3.12.2.19), for 5.X
// and 7.X. Gains in dB, each one of the values its table gives.
struct DownmixConfig {
    // Table 149: +3, +1.5, 0, -1.5, -3, -4.5 or -6 dB, or -infinity.
    double loro_centre_db = -3.0;
    // Table 149a: 0, -1.5, -3, -4.5 or -6 dB, or -infinity.
    double loro_surround_db = -3.0;
    // Lt/Rt's, where they differ from Lo/Ro's (b_ltrt_mixinfo).
    std::optional<double> ltrt_centre_db;
    std::optional<double> ltrt_surround_db;
    // The LFE into the stereo downmix, 5.5 - lfe_mixgain dB: +5.5 to -25.5 in
    // steps of 1 dB. Unset leaves the LFE out.
    std::optional<double> lfe_db;
    PreferredDownmix preferred = PreferredDownmix::kLoRo;
    // The loudness correction each downmix takes, in dB2 (6 dB2 a factor of
    // 2): -7.5 to +7.5 in steps of 0.5 (loro_dmx_loud_corr, ltrt_dmx_loud_corr).
    std::optional<double> loro_correction_db2;
    std::optional<double> ltrt_correction_db2;
};

// Where dialogue enhancement's parameters come from (planning/ac4.md,
// decision 18: no speech detector).
enum class DialogueSource : std::uint8_t {
    // The channels DialogueConfig marks carry dialogue alone: their
    // parameters are 1 in every band.
    kMarkedChannels,
    // A dialogue stem, given to encode() beside the programme: each band's
    // parameter is the stem's share of the channel.
    kStem,
};

// How dialogue enhancement's parameters raise the dialogue (Part 1 Table 170
// and clause 5.7.8). The hybrid methods, 2 and 3, add a dialogue waveform in
// a substream of its own, which phase E6's presentations bring.
enum class DialogueMethod : std::uint8_t {
    // de_method 0: each channel scaled, band by band, by its own parameter,
    // the dialogue's share of it.
    kChannelIndependent,
    // de_method 0 with de_ms_proc_flag, for L and R alone: their Mid scaled,
    // where dialogue centred between them sits.
    kMid,
    // de_method 1, from a stem, over two or three channels: a mix of the
    // channels that follows the dialogue, panned back onto them as the
    // dialogue is panned (de_mix_coef1_idx and 2).
    kCrossChannel,
};

// Dialogue enhancement (Part 1 clauses 4.3.14 and 5.7.8): de_config() in
// I-frames and each frame's parameters in de_data().
struct DialogueConfig {
    DialogueMethod method = DialogueMethod::kChannelIndependent;
    DialogueSource source = DialogueSource::kMarkedChannels;
    // Which of L, R and C carry dialogue, in de_channel_config's order (Table
    // 171); a mono programme has only C, a stereo one L and R.
    bool left = false;
    bool right = false;
    bool centre = true;
    // The most a decoder may raise the dialogue: 3, 6, 9 or 12 dB (de_max_gain).
    int max_gain_db = 9;
};

struct EncoderConfig {
    // The input's channels, in the order ac4::Decoder writes them: 1, mono; 2,
    // stereo, L R; 5, 5.0, L R C Ls Rs; 6, 5.1, L R C LFE Ls Rs; and with
    // experimental.seven_x, 7 or 8, 7.0 or 7.1, L R C, the LFE of 7.1, Ls Rs
    // and the additional pair.
    int channels = 2;
    int sample_rate_hz = 48000;    // 48 000, or 44 100
    // Part 1 Table 83 at 48 kHz: 0 23.976 fps, 1 24, 2 25, 3 29.97, 4 30, 5
    // 47.95, 6 48, 7 50, 8 59.94, 9 60, 10 100, 11 119.88, 12 120, and 13
    // the 2 048-sample frame, 23.4375 fps, which alone needs no converter; 13
    // alone at 44.1 kHz.
    int frame_rate_index = 13;
    int bitrate_kbps = 192;        // the stream's rate, over whole raw_ac4_frame()s
    RateMode rate_mode = RateMode::kConstant;
    CodecMode codec_mode = CodecMode::kAuto;
    // An I-frame every this many frames, the first frame being one; 1 makes
    // every frame an I-frame. The containers need one at every fragment's start
    // (Part 1 Annex E.5, Part 2 Annex E.3).
    int iframe_interval = 24;
    // I-frames besides those: the frames, counted from 0, that must be ones,
    // in any order.
    std::vector<std::int64_t> iframes;
    // Where the caller's fragments start, in samples of the decoded output
    // from its first, which is the media time an MP4 track counts: the frame
    // whose output starts there, or the first to start after it, is an
    // I-frame, so that a fragment can start with it.
    std::vector<std::int64_t> fragment_starts;
    // The input reference level, Part 1 clause 4.3.12.2.1: 0 to -31.75 dBFS in
    // steps of 0.25 dB.
    double dialnorm_db = -31.0;
    // The metadata above, each written where it is set: the programme's
    // further loudness values, DRC's decoder modes, the stereo downmix's
    // values (5.X and 7.X only) and dialogue enhancement.
    std::optional<FurtherLoudness> loudness;
    std::optional<DrcConfig> drc;
    std::optional<DownmixConfig> downmix;
    std::optional<DialogueConfig> dialogue;
    // One record per syntax element written, in the shape ac4/syntax.hpp
    // states, for comparing what was written with what a reader reads. The
    // callable must outlive the Encoder.
    SyntaxSink trace{};
    // Syntax only this project's readers have read from this encoder, off
    // unless asked for (planning/ac4.md, "What the encoder writes by
    // default"): each leaves the list when a reader outside the project
    // agrees with the encoder's use of it.
    struct Experimental {
        // In the ASPX mode, a pair coded as sum and balance (aspx_balance)
        // where the two channels share a framing and that takes fewer bits.
        bool aspx_balance = false;
        // In the ASPX mode, VARVAR framing: an attack in an interval that
        // starts where the last ran on ends it on a border of its own.
        bool aspx_varvar = false;
        // In the ASPX mode, frequency interleaved waveform coding: a steady
        // tone above the crossover that A-SPX would not recreate is coded by
        // the spectral frontend, and A-SPX adds nothing there.
        bool aspx_interleave = false;
        // In the 5.X and 7.X elements, coding_config 1 to 3 and 2ch_mode 1
        // besides DEE's coding_config 0 with 2ch_mode 0 (Part 1 Tables 25,
        // 33 and 180), with each three and five channel matrix's chel_matsel
        // (Tables 178 and 179): each frame takes the one whose matrices and
        // side information cost fewest bits. The five channels then share
        // one transform layout.
        bool coding_configs = false;
        // Seven or eight input channels in the 7.X element, with this pair
        // beyond L, R, C, Ls and Rs.
        AdditionalPair seven_x = AdditionalPair::kNone;
        // The A-CPL modes DEE's streams do not use, which codec_mode then
        // takes: ASPX_ACPL_1 in 5.0 and 5.1, and ASPX_ACPL_1 and ASPX_ACPL_2
        // in stereo, the channel pair element's.
        bool acpl = false;
        // DRC modes that send gains (DrcModeConfig::gains_config), which no
        // DEE stream has.
        bool drc_gains = false;
    };
    Experimental experimental{};
};

// One coded frame: what an MP4 sample holds as it is, and what sync_frame()
// wraps for a raw .ac4 file or MPEG-2 TS.
struct EncodedFrame {
    std::vector<std::byte> raw_ac4_frame;
    // PCM samples per channel the frame decodes to, at the input's rate: the
    // frame's length at index 13, and elsewhere what the decoder's converter
    // gives the frame, which at 29.97, 59.94 and 119.88 fps changes from
    // frame to frame in a cycle of five (Part 2 clause 5.11): 1 601, 1 602,
    // 1 601, 1 602 and 1 602 at 29.97.
    int samples = 0;
    bool iframe = false;   // b_iframe_global
};

class AC4ENC_EXPORT Encoder {
   public:
    // Fails with EncodeError::kInvalidConfig for a configuration outside what
    // this version encodes.
    [[nodiscard]] static std::expected<Encoder, EncodeError> create(const EncoderConfig& config);

    ~Encoder();
    Encoder(Encoder&&) noexcept;
    Encoder& operator=(Encoder&&) noexcept;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    // Planar samples at full scale 1.0, one span per input channel, all the
    // same length, any length. Returns the frames this input completes, in
    // order; the encoder's delay holds back the frames the last input still
    // needs.
    [[nodiscard]] std::expected<std::vector<EncodedFrame>, EncodeError> encode(
        std::span<const std::span<const float>> channels);
    // With DialogueSource::kStem: the programme and, sample for sample, the
    // dialogue in it, in the programme's channels.
    [[nodiscard]] std::expected<std::vector<EncodedFrame>, EncodeError> encode(
        std::span<const std::span<const float>> channels,
        std::span<const std::span<const float>> dialogue);

    // Ends the stream: pads the input with silence to the end of its last
    // frame and returns the frames the delay still held, so that a decoder's
    // output covers every input sample. The encoder takes no input after it.
    [[nodiscard]] std::expected<std::vector<EncodedFrame>, EncodeError> flush();

    // The table of contents every frame carries. sequence_counter and
    // b_iframe_global change from frame to frame, and substream_sizes with each
    // frame's content; the rest is fixed for the stream, which is what
    // ac4::build_dac4() and ac4::rfc6381_codec_string() read.
    [[nodiscard]] const Toc& toc() const noexcept;

    // The codec mode the stream is coded in: what kAuto chose from the rate,
    // never kAuto.
    [[nodiscard]] CodecMode codec_mode() const noexcept;

    // Samples of silence the encoder puts before the input: an input sample at
    // index n is at index n + delay_samples() of the decoded output before the
    // decoder's own delay is added: at frame_rate_index 13, 1 313 samples
    // (Part 1 Table 188's d_pcm, 352, the QMF banks' 577 and six QMF slots).
    // At the other frame rates, a frame and a half at the internal rate and
    // the converter's delay, at the input's rate to the nearest sample: the
    // delay itself is a fraction of a sample off it.
    [[nodiscard]] int delay_samples() const noexcept;

   private:
    struct Impl;
    explicit Encoder(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Part 2 Annex G.3.1's ac4_syncframe(): the sync word 0xAC40, or 0xAC41 and a
// trailing crc_word (Annex G.4.2) when `crc` is set, then frame_size and the
// raw frame.
[[nodiscard]] AC4ENC_EXPORT std::vector<std::byte> sync_frame(std::span<const std::byte> raw_ac4_frame,
                                                              bool crc);

}  // namespace ac4
