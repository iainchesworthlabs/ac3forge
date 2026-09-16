#pragma once

#include <memory>

#include "ac3/sendspin/codec.hpp"
#include "ac3/sendspin/messages.hpp"

// Each codec's own maker, which make_encoder() and make_decoder() choose between (codec.cpp).
// Private to the library; a board picks the makers it builds.

namespace ac3::sendspin::codec {

[[nodiscard]] std::unique_ptr<Encoder> make_pcm_encoder(const messages::AudioFormat& format, const EncoderOptions& options);
[[nodiscard]] std::unique_ptr<Decoder> make_pcm_decoder(const messages::AudioFormat& format);

[[nodiscard]] std::unique_ptr<Encoder> make_flac_encoder(const messages::AudioFormat& format, const EncoderOptions& options);
[[nodiscard]] std::unique_ptr<Decoder> make_flac_decoder(const messages::PlayerStream& stream);

[[nodiscard]] std::unique_ptr<Encoder> make_opus_encoder(const messages::AudioFormat& format, const EncoderOptions& options);
[[nodiscard]] std::unique_ptr<Decoder> make_opus_decoder(const messages::AudioFormat& format);

}  // namespace ac3::sendspin::codec
