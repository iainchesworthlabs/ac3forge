#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
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
// What this version writes: mono, stereo, 5.0 or 5.1 PCM at 48 kHz, or 44.1
// kHz, in frames of 2 048 samples (frame_rate_index 13, which needs no sample
// rate converter), as one presentation of one channel-coded substream, at a
// constant bit rate (wait_frames 0), each frame filled to its size. The codec
// mode is SIMPLE, the audio spectral frontend with block switching and
// MDCT-domain stereo processing for each channel pair; or ASPX, which codes
// the spectral frontend up to a crossover and recreates the band above it
// with A-SPX, with companding at the lower rates in mono and stereo. 5.0 and
// 5.1 take the 5.X element in the form DEE's streams have it (coding_config
// 0 and 2ch_mode 0: L and R as a pair, Ls and Rs as a pair, C alone, and the
// LFE); its other coding configurations, and 7.0 and 7.1 in the 7.X element,
// are experimental. The table of contents is bitstream version 2 with
// presentation version 1, and the presentation substream carries the dialogue
// normalisation it is given. Everything else in the plan's later phases
// (A-CPL, immersive layouts, other frame rates, DRC and dialogue enhancement
// data) is refused by name as an invalid configuration.
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
    // ASPX below 96 kbps a channel in mono and stereo and below 76.8 in the
    // 5.X and 7.X elements (5.1's LFE not counted), as DEE's streams switch at
    // 192 kbps in stereo and 384 in 5.1; SIMPLE from there.
    kAuto,
    kSimple,  // the audio spectral frontend over the whole band
    // The spectral frontend up to A-SPX's crossover and A-SPX above it. In
    // mono and stereo 7.5 kHz below 32 kbps a channel, 10.5 kHz below 48 and
    // 13.5 kHz from there, with companding below 64 kbps a channel; in 5.X and
    // 7.X 12 kHz from 38.4 kbps a channel and 12.75 kHz from 51.2, without
    // companding.
    kAspx,
};

// The 7.X element's pair beyond L, R, C, Ls and Rs (Part 1 Table 88).
enum class AdditionalPair : std::uint8_t {
    kNone,
    kBack,      // 3/4/0: Lb and Rb
    kWide,      // 5/2/0: Lw and Rw
    kTopFront,  // 3/2/2: Tfl and Tfr
};

struct EncoderConfig {
    // The input's channels, in the order ac4::Decoder writes them: 1, mono; 2,
    // stereo, L R; 5, 5.0, L R C Ls Rs; 6, 5.1, L R C LFE Ls Rs; and with
    // experimental.seven_x, 7 or 8, 7.0 or 7.1, L R C, the LFE of 7.1, Ls Rs
    // and the additional pair.
    int channels = 2;
    int sample_rate_hz = 48000;    // 48 000, or 44 100
    int bitrate_kbps = 192;        // the stream's rate, over whole raw_ac4_frame()s
    CodecMode codec_mode = CodecMode::kAuto;
    // An I-frame every this many frames, the first frame being one; 1 makes
    // every frame an I-frame. The containers need one at every fragment's start
    // (Part 1 Annex E.5, Part 2 Annex E.3).
    int iframe_interval = 24;
    // The input reference level, Part 1 clause 4.3.12.2.1: 0 to -31.75 dBFS in
    // steps of 0.25 dB.
    double dialnorm_db = -31.0;
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
    };
    Experimental experimental{};
};

// One coded frame: what an MP4 sample holds as it is, and what sync_frame()
// wraps for a raw .ac4 file or MPEG-2 TS.
struct EncodedFrame {
    std::vector<std::byte> raw_ac4_frame;
    int samples = 0;       // PCM samples per channel the frame codes
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

    // Ends the stream: pads the input with silence to the end of its last
    // frame and returns the frames the delay still held, so that a decoder's
    // output covers every input sample. The encoder takes no input after it.
    [[nodiscard]] std::expected<std::vector<EncodedFrame>, EncodeError> flush();

    // The table of contents every frame carries. sequence_counter and
    // b_iframe_global change from frame to frame, and substream_sizes with each
    // frame's content; the rest is fixed for the stream, which is what
    // ac4::build_dac4() and ac4::rfc6381_codec_string() read.
    [[nodiscard]] const Toc& toc() const noexcept;

    // The codec mode the stream is coded in: kSimple or kAspx, what kAuto
    // chose from the rate.
    [[nodiscard]] CodecMode codec_mode() const noexcept;

    // Samples of silence the encoder puts before the input: an input sample at
    // index n is at index n + delay_samples() of the decoded output before the
    // decoder's own delay is added: at frame_rate_index 13, 1 313 samples
    // (Part 1 Table 188's d_pcm, 352, the QMF banks' 577 and six QMF slots).
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
