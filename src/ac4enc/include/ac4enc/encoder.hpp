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
// What this version writes: mono or stereo PCM at 48 kHz, or 44.1 kHz, in
// frames of 2 048 samples (frame_rate_index 13, which needs no sample rate
// converter), as one presentation of one channel-coded substream in the SIMPLE
// codec mode: the audio spectral frontend with block switching and, for stereo,
// MDCT-domain stereo processing, at a constant bit rate (wait_frames 0), each
// frame filled to its size. The table of contents is bitstream version 2 with
// presentation version 1, and the presentation substream carries the dialogue
// normalisation it is given. Everything else in the plan's later phases (A-SPX,
// A-CPL, more channels, other frame rates, DRC and dialogue enhancement data)
// is refused by name as an invalid configuration.
//
// src/ac4enc/ERRATA.md records the readings the writer alone needs; where the
// decoder depends on the same reading, src/ac4dec/ERRATA.md has it.

namespace ac4 {

enum class EncodeError : std::uint8_t {
    kInvalidConfig,  // a configuration this version does not encode - see the header comment
    kInvalidInput,   // a channel count or lengths that do not match, or a sample that is not finite
};

[[nodiscard]] AC4ENC_EXPORT std::string_view describe(EncodeError error);

struct EncoderConfig {
    int channels = 2;              // 1 (mono) or 2 (stereo)
    int sample_rate_hz = 48000;    // 48 000, or 44 100
    int bitrate_kbps = 192;        // the stream's rate, over whole raw_ac4_frame()s
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

    // Samples of silence the encoder puts before the input: an input sample at
    // index n is at index n + delay_samples() of the decoded output before the
    // decoder's own delay (Part 1 Table 188's d_pcm) is added.
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
