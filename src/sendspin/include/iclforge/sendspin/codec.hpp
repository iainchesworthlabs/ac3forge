#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ac3/sendspin/messages.hpp"

// player@v1's codecs (roles/player/v1.md, Codec framing): what a server encodes into audio chunks
// and a player decodes out of them. Samples pass between these and the rest of a program as
// interleaved signed integers at the format's bit depth, held in 32 bits; Opus, whose format
// ignores the bit depth, takes and gives 16-bit samples.
//
// PCM needs nothing. FLAC and Opus come from libFLAC and Opus through vcpkg's hearth feature.

namespace ac3::sendspin::codec {

// What one audio chunk carries: whole PCM frames, whole FLAC frames, or one Opus packet.
struct Unit {
    // The stream position of the unit's first frame.
    std::int64_t first_frame = 0;
    std::vector<std::uint8_t> bytes;
};

class Encoder {
   public:
    Encoder() = default;
    virtual ~Encoder() = default;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    Encoder(Encoder&&) = delete;
    Encoder& operator=(Encoder&&) = delete;

    // stream/start's codec_header: the fLaC marker and STREAMINFO block for FLAC, nothing otherwise.
    [[nodiscard]] virtual const std::vector<std::uint8_t>& codec_header() const = 0;
    // How many frames the decoded audio lags the input (Opus's look-ahead). A server timestamps
    // each unit that much earlier, so players decoding different codecs in one group stay aligned.
    [[nodiscard]] virtual std::int32_t delay_frames() const = 0;
    // Takes whole frames of interleaved samples and returns the units they complete: a FLAC
    // encoder holds a block back until the next begins, and Opus and chunked PCM hold a part.
    [[nodiscard]] virtual std::optional<std::vector<Unit>> encode(std::span<const std::int32_t> interleaved) = 0;
    // The stream ends: the units still held, Opus's last packet padded with silence.
    [[nodiscard]] virtual std::optional<std::vector<Unit>> finish() = 0;
};

class Decoder {
   public:
    Decoder() = default;
    virtual ~Decoder() = default;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;

    // The samples in one unit, interleaved at bit_depth(); nothing when it does not decode.
    [[nodiscard]] virtual std::optional<std::vector<std::int32_t>> decode(std::span<const std::uint8_t> unit) = 0;
    [[nodiscard]] virtual std::int32_t bit_depth() const = 0;
};

struct EncoderOptions {
    // Frames in each PCM unit and FLAC block; 0 for 20 ms of PCM, and for FLAC the subset's
    // 4,096 or 100 ms, whichever is shorter, so a chunk stays inside 150 ms.
    std::int32_t frames = 0;
    // Opus's bitrate in bits a second; 0 for 64 kb/s a channel.
    std::int32_t opus_bitrate = 0;
};

// Nothing for a format no encoder here produces: Opus at a rate other than 8, 12, 16, 24 or
// 48 kHz or with more than two channels, PCM or FLAC at a depth other than 16, 24 or 32 bits.
[[nodiscard]] std::unique_ptr<Encoder> make_encoder(const messages::AudioFormat& format,
                                                    const EncoderOptions& options = {});
// Nothing for a format no decoder here reads, or a FLAC stream whose codec_header is not one.
[[nodiscard]] std::unique_ptr<Decoder> make_decoder(const messages::PlayerStream& stream);

}  // namespace ac3::sendspin::codec
