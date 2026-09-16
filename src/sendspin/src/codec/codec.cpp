#include "ac3/sendspin/codec.hpp"

#include <memory>

#include "ac3/sendspin/messages.hpp"
#include "codecs.hpp"

namespace ac3::sendspin::codec {

std::unique_ptr<Encoder> make_encoder(const messages::AudioFormat& format, const EncoderOptions& options) {
    switch (format.codec) {
        case messages::Codec::kPcm:
            return make_pcm_encoder(format, options);
        case messages::Codec::kFlac:
            return make_flac_encoder(format, options);
        case messages::Codec::kOpus:
            return make_opus_encoder(format, options);
    }
    return nullptr;
}

std::unique_ptr<Decoder> make_decoder(const messages::PlayerStream& stream) {
    switch (stream.format.codec) {
        case messages::Codec::kPcm:
            return make_pcm_decoder(stream.format);
        case messages::Codec::kFlac:
            return make_flac_decoder(stream);
        case messages::Codec::kOpus:
            return make_opus_decoder(stream.format);
    }
    return nullptr;
}

}  // namespace ac3::sendspin::codec
