// ac3forge_ac4_encoder_* and the table-of-contents/sync-frame helpers - see
// ac3forge.h's AC-4 section, ac4::Encoder (src/ac4enc/include/ac4enc/encoder.hpp)
// and ac4/ac4.hpp's carriage section.

#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "internal.hpp"

using ac3forge_c::guard;
using ac3forge_c::to_cpp;

// Kept outside extern "C": a C-linkage function returning a C++ class by
// value is diagnosed by Clang (-Wreturn-type-c-linkage) - see encoder.cpp's
// identical comment.
namespace {

ac4::EncoderConfig encoder_config_to_cpp(const ac3forge_ac4_encoder_config_t& config) {
    ac4::EncoderConfig out;
    out.channels = config.channels;
    out.sample_rate_hz = config.sample_rate_hz;
    out.frame_rate_index = config.frame_rate_index;
    out.bitrate_kbps = config.bitrate_kbps;
    out.rate_mode = to_cpp(config.rate_mode);
    out.codec_mode = to_cpp(config.codec_mode);
    out.iframe_interval = config.iframe_interval;
    out.dialnorm_db = config.dialnorm_db;
    return out;
}

ac3forge_status_t build_encoded_frame_array(std::vector<ac4::EncodedFrame>&& frames,
                                             ac3forge_ac4_encoded_frame_t*** out_frames,
                                             size_t* out_count) {
    if (frames.empty()) {
        *out_frames = nullptr;
        *out_count = 0;
        return AC3FORGE_OK;
    }
    auto array = std::make_unique<ac3forge_ac4_encoded_frame*[]>(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        auto owned = std::make_unique<ac3forge_ac4_encoded_frame>();
        owned->data = std::move(frames[i]);
        array[i] = owned.release();
    }
    *out_count = frames.size();
    *out_frames = array.release();
    return AC3FORGE_OK;
}

}  // namespace

extern "C" {

void ac3forge_ac4_encoder_config_init(ac3forge_ac4_encoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const ac4::EncoderConfig defaults{};
    *config = ac3forge_ac4_encoder_config_t{.channels = defaults.channels,
                                            .sample_rate_hz = defaults.sample_rate_hz,
                                            .frame_rate_index = defaults.frame_rate_index,
                                            .bitrate_kbps = defaults.bitrate_kbps,
                                            .rate_mode = ac3forge_c::from_cpp(defaults.rate_mode),
                                            .codec_mode = ac3forge_c::from_cpp(defaults.codec_mode),
                                            .iframe_interval = defaults.iframe_interval,
                                            .dialnorm_db = defaults.dialnorm_db};
}

ac3forge_status_t ac3forge_ac4_encoder_create(const ac3forge_ac4_encoder_config_t* config,
                                              ac3forge_ac4_encoder_t** out_encoder) {
    if (config == nullptr || out_encoder == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&config, &out_encoder]() -> ac3forge_status_t {
        auto result = ac4::Encoder::create(encoder_config_to_cpp(*config));
        if (!result.has_value()) {
            return ac3forge_c::from_cpp(result.error());
        }
        *out_encoder = new ac3forge_ac4_encoder(std::move(*result));
        return AC3FORGE_OK;
    });
}

void ac3forge_ac4_encoder_destroy(ac3forge_ac4_encoder_t* encoder) { delete encoder; }

ac3forge_ac4_codec_mode_t ac3forge_ac4_encoder_codec_mode(const ac3forge_ac4_encoder_t* encoder) {
    return encoder == nullptr ? AC3FORGE_AC4_CODEC_AUTO
                              : ac3forge_c::from_cpp(encoder->impl.codec_mode());
}

int ac3forge_ac4_encoder_delay_samples(const ac3forge_ac4_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.delay_samples();
}

int ac3forge_ac4_encoder_decoder_delay_samples(const ac3forge_ac4_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.decoder_delay_samples();
}

const uint8_t* ac3forge_ac4_encoded_frame_data(const ac3forge_ac4_encoded_frame_t* frame) {
    if (frame == nullptr || frame->data.raw_ac4_frame.empty()) {
        return nullptr;
    }
    return reinterpret_cast<const uint8_t*>(frame->data.raw_ac4_frame.data());
}

size_t ac3forge_ac4_encoded_frame_size(const ac3forge_ac4_encoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.raw_ac4_frame.size();
}

int ac3forge_ac4_encoded_frame_samples(const ac3forge_ac4_encoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.samples;
}

int ac3forge_ac4_encoded_frame_iframe(const ac3forge_ac4_encoded_frame_t* frame) {
    return frame != nullptr && frame->data.iframe ? 1 : 0;
}

void ac3forge_ac4_encoded_frame_destroy(ac3forge_ac4_encoded_frame_t* frame) { delete frame; }

void ac3forge_ac4_encoded_frame_array_destroy(ac3forge_ac4_encoded_frame_t** frames, size_t count) {
    if (frames == nullptr) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        delete frames[i];
    }
    delete[] frames;
}

ac3forge_status_t ac3forge_ac4_encoder_encode(ac3forge_ac4_encoder_t* encoder,
                                              const float* const* channels, size_t channel_count,
                                              size_t samples_per_channel,
                                              ac3forge_ac4_encoded_frame_t*** out_frames,
                                              size_t* out_count) {
    if (encoder == nullptr || channels == nullptr || out_frames == nullptr || out_count == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&encoder, &channels, &channel_count, &samples_per_channel, &out_frames,
                  &out_count]() -> ac3forge_status_t {
        std::vector<std::span<const float>> spans;
        spans.reserve(channel_count);
        for (size_t i = 0; i < channel_count; ++i) {
            if (channels[i] == nullptr) {
                return AC3FORGE_ERROR_INVALID_ARGUMENT;
            }
            spans.emplace_back(channels[i], samples_per_channel);
        }
        auto result = encoder->impl.encode(spans);
        if (!result.has_value()) {
            return ac3forge_c::from_cpp(result.error());
        }
        return build_encoded_frame_array(std::move(*result), out_frames, out_count);
    });
}

ac3forge_status_t ac3forge_ac4_encoder_flush(ac3forge_ac4_encoder_t* encoder,
                                             ac3forge_ac4_encoded_frame_t*** out_frames,
                                             size_t* out_count) {
    if (encoder == nullptr || out_frames == nullptr || out_count == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&encoder, &out_frames, &out_count]() -> ac3forge_status_t {
        auto result = encoder->impl.flush();
        if (!result.has_value()) {
            return ac3forge_c::from_cpp(result.error());
        }
        return build_encoded_frame_array(std::move(*result), out_frames, out_count);
    });
}

ac3forge_status_t ac3forge_ac4_encoder_toc(const ac3forge_ac4_encoder_t* encoder,
                                           ac3forge_ac4_toc_t** out_toc) {
    if (encoder == nullptr || out_toc == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&encoder, &out_toc] {
        auto owned = std::make_unique<ac3forge_ac4_toc>();
        owned->data = encoder->impl.toc();
        *out_toc = owned.release();
        return AC3FORGE_OK;
    });
}

void ac3forge_ac4_toc_destroy(ac3forge_ac4_toc_t* toc) { delete toc; }

ac3forge_status_t ac3forge_ac4_build_dac4(const ac3forge_ac4_toc_t* toc,
                                          ac3forge_bytes_t** out_box) {
    if (toc == nullptr || out_box == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&toc, &out_box] {
        auto owned = std::make_unique<ac3forge_bytes>();
        owned->data = ac4::build_dac4(toc->data);
        *out_box = owned.release();
        return AC3FORGE_OK;
    });
}

const char* ac3forge_ac4_dac4_refusal(const ac3forge_ac4_toc_t* toc) {
    // Library-owned storage valid for the process lifetime: ac4::dac4_refusal()
    // always returns a string literal naming what it cannot describe, or an
    // empty view - never a dynamically composed string. The empty case is
    // std::string_view{} (ac4/src/ac4.cpp), whose data() is NULL by the
    // standard, not a zero-length slice of a literal - "" is returned
    // instead so a caller can always treat the result as a NUL-terminated C
    // string without checking for NULL first.
    if (toc == nullptr) {
        return "";
    }
    const std::string_view refusal = ac4::dac4_refusal(toc->data);
    return refusal.empty() ? "" : refusal.data();
}

int ac3forge_ac4_media_timing(const ac3forge_ac4_toc_t* toc, uint32_t* out_timescale,
                              uint32_t* out_sample_delta) {
    if (toc == nullptr) {
        return 0;
    }
    const auto timing = ac4::media_timing(toc->data);
    if (!timing.has_value()) {
        return 0;
    }
    if (out_timescale != nullptr) {
        *out_timescale = timing->timescale;
    }
    if (out_sample_delta != nullptr) {
        *out_sample_delta = timing->sample_delta;
    }
    return 1;
}

int ac3forge_ac4_samples_per_frame(const ac3forge_ac4_toc_t* toc, uint32_t* out_samples) {
    if (toc == nullptr) {
        return 0;
    }
    const auto samples = ac4::samples_per_frame(toc->data);
    if (!samples.has_value()) {
        return 0;
    }
    if (out_samples != nullptr) {
        *out_samples = *samples;
    }
    return 1;
}

ac3forge_status_t ac3forge_ac4_sync_frame(const uint8_t* raw_frame, size_t raw_frame_size, int crc,
                                          ac3forge_bytes_t** out_bytes) {
    if (raw_frame == nullptr || out_bytes == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&raw_frame, &raw_frame_size, &crc, &out_bytes] {
        auto owned = std::make_unique<ac3forge_bytes>();
        owned->data = ac4::sync_frame(
            std::as_bytes(std::span<const uint8_t>(raw_frame, raw_frame_size)), crc != 0);
        *out_bytes = owned.release();
        return AC3FORGE_OK;
    });
}

}  // extern "C"
