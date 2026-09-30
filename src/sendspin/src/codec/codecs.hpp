#pragma once

#include <memory>

#include "iclforge/sendspin/codec.hpp"
#include "iclforge/sendspin/messages.hpp"

// Each codec's own maker, which make_encoder() and make_decoder() choose between (codec.cpp).
// Private to the library; a board picks the makers it builds.

namespace iclforge::sendspin::codec {

[[nodiscard]] std::unique_ptr<Encoder> make_pcm_encoder(const messages::AudioFormat& format, const EncoderOptions& options);
[[nodiscard]] std::unique_ptr<Decoder> make_pcm_decoder(const messages::AudioFormat& format);

[[nodiscard]] std::unique_ptr<Encoder> make_flac_encoder(const messages::AudioFormat& format, const EncoderOptions& options);
[[nodiscard]] std::unique_ptr<Decoder> make_flac_decoder(const messages::PlayerStream& stream);

[[nodiscard]] std::unique_ptr<Encoder> make_opus_encoder(const messages::AudioFormat& format, const EncoderOptions& options);
[[nodiscard]] std::unique_ptr<Decoder> make_opus_decoder(const messages::AudioFormat& format);

}  // namespace iclforge::sendspin::codec
