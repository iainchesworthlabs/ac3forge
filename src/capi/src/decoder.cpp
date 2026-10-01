#include <memory>
#include <span>

#include "internal.hpp"

using iclforge_c::guard;

// Kept outside extern "C" below - see encoder.cpp's identical comment on
// -Wreturn-type-c-linkage.
namespace {
iclforge::ac3::DecoderConfig decoder_config_to_cpp(const iclforge_decoder_config_t& config) {
    return iclforge::ac3::DecoderConfig{.drc_scale = config.drc_scale,
                               .heavy_compression = config.heavy_compression != 0};
}
}  // namespace

extern "C" {

void iclforge_decoder_config_init(iclforge_decoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const iclforge::ac3::DecoderConfig defaults{};
    *config = iclforge_decoder_config_t{.drc_scale = defaults.drc_scale,
                                         .heavy_compression = defaults.heavy_compression ? 1 : 0};
}

iclforge_status_t iclforge_decoder_create(const iclforge_decoder_config_t* config,
                                           iclforge_decoder_t** out_decoder) {
    if (config == nullptr || out_decoder == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&config, &out_decoder] {
        *out_decoder = new iclforge_decoder(decoder_config_to_cpp(*config));
        return ICLFORGE_OK;
    });
}

void iclforge_decoder_destroy(iclforge_decoder_t* decoder) { delete decoder; }

iclforge_status_t iclforge_decoder_decode_frame(iclforge_decoder_t* decoder, const uint8_t* frame,
                                                 size_t frame_size,
                                                 iclforge_decoded_frame_t** out_frame) {
    if (decoder == nullptr || frame == nullptr || out_frame == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&decoder, &frame, &frame_size, &out_frame]() -> iclforge_status_t {
        auto result = decoder->impl.decode_frame(
            std::as_bytes(std::span<const uint8_t>(frame, frame_size)));
        if (!result) {
            return iclforge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<iclforge_decoded_frame>();
        owned->data = std::move(*result);
        *out_frame = owned.release();
        return ICLFORGE_OK;
    });
}

iclforge_status_t iclforge_decoder_decode_frame_into(iclforge_decoder_t* decoder,
                                                       const uint8_t* frame, size_t frame_size,
                                                       float* const* channels, size_t channel_count,
                                                       size_t samples_per_channel,
                                                       iclforge_decoded_frame_t** out_frame) {
    if (decoder == nullptr || frame == nullptr || channels == nullptr || out_frame == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    if (channel_count != ICLFORGE_DECODER_MAX_CHANNELS ||
        samples_per_channel != ICLFORGE_SAMPLES_PER_FRAME) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&decoder, &frame, &frame_size, &channels, &channel_count, &samples_per_channel,
                  &out_frame]() -> iclforge_status_t {
        std::vector<std::span<float>> spans;
        spans.reserve(channel_count);
        for (size_t i = 0; i < channel_count; ++i) {
            if (channels[i] == nullptr) {
                return ICLFORGE_ERROR_INVALID_ARGUMENT;
            }
            spans.emplace_back(channels[i], samples_per_channel);
        }
        auto result = decoder->impl.decode_frame_into(
            std::as_bytes(std::span<const uint8_t>(frame, frame_size)), spans);
        if (!result) {
            return iclforge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<iclforge_decoded_frame>();
        owned->data = std::move(*result);
        *out_frame = owned.release();
        return ICLFORGE_OK;
    });
}

iclforge_sample_rate_t iclforge_decoded_frame_sample_rate(const iclforge_decoded_frame_t* frame) {
    return frame == nullptr ? ICLFORGE_SAMPLE_RATE_48000 : iclforge_c::from_cpp(frame->data.sample_rate);
}

uint32_t iclforge_decoded_frame_bitrate_kbps(const iclforge_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.bitrate_kbps;
}

iclforge_acmod_t iclforge_decoded_frame_acmod(const iclforge_decoded_frame_t* frame) {
    return frame == nullptr ? ICLFORGE_ACMOD_2_0 : iclforge_c::from_cpp(frame->data.acmod);
}

int iclforge_decoded_frame_lfe(const iclforge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.lfe ? 1 : 0;
}

int iclforge_decoded_frame_dialnorm(const iclforge_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.dialnorm;
}

int iclforge_decoded_frame_has_compr(const iclforge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.compr.has_value() ? 1 : 0;
}

uint8_t iclforge_decoded_frame_compr(const iclforge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.compr.has_value() ? *frame->data.compr : 0;
}

uint8_t iclforge_decoded_frame_dynrng(const iclforge_decoded_frame_t* frame, int block_index) {
    if (frame == nullptr || block_index < 0 || block_index >= iclforge::ac3::kBlocksPerFrame) {
        return 0;
    }
    return frame->data.dynrng[static_cast<size_t>(block_index)];
}

int iclforge_decoded_frame_has_dialnorm2(const iclforge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.dialnorm2.has_value() ? 1 : 0;
}

int iclforge_decoded_frame_dialnorm2(const iclforge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.dialnorm2.has_value() ? *frame->data.dialnorm2 : 0;
}

int iclforge_decoded_frame_has_compr2(const iclforge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.compr2.has_value() ? 1 : 0;
}

uint8_t iclforge_decoded_frame_compr2(const iclforge_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.compr2.has_value() ? *frame->data.compr2 : 0;
}

uint8_t iclforge_decoded_frame_dynrng2(const iclforge_decoded_frame_t* frame, int block_index) {
    if (frame == nullptr || block_index < 0 || block_index >= iclforge::ac3::kBlocksPerFrame) {
        return 0;
    }
    return frame->data.dynrng2[static_cast<size_t>(block_index)];
}

size_t iclforge_decoded_frame_channel_count(const iclforge_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.channels.size();
}

size_t iclforge_decoded_frame_samples_per_channel(const iclforge_decoded_frame_t*) {
    return iclforge::ac3::kSamplesPerFrame;
}

const float* iclforge_decoded_frame_channel_samples(const iclforge_decoded_frame_t* frame,
                                                      size_t channel_index) {
    if (frame == nullptr || channel_index >= frame->data.channels.size()) {
        return nullptr;
    }
    return frame->data.channels[channel_index].data();
}

int iclforge_decoded_frame_block_switched(const iclforge_decoded_frame_t* frame,
                                           size_t channel_index, int block_index) {
    if (frame == nullptr || channel_index >= frame->data.blksw.size() || block_index < 0 ||
        block_index >= iclforge::ac3::kBlocksPerFrame) {
        return 0;
    }
    return frame->data.blksw[channel_index][static_cast<size_t>(block_index)] ? 1 : 0;
}

void iclforge_decoded_frame_destroy(iclforge_decoded_frame_t* frame) { delete frame; }

}  // extern "C"
