#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "internal.hpp"

using ac3forge_c::guard;
using ac3forge_c::to_cpp;

// Kept outside extern "C" below - see encoder.cpp's identical comment on
// -Wreturn-type-c-linkage.
namespace {

ac3::eac3::FrameConfig eac3_frame_config_to_cpp(const ac3forge_eac3_frame_config_t& config) {
    ac3::eac3::FrameConfig out;
    out.sample_rate = to_cpp(config.sample_rate);
    out.bitrate_kbps = config.bitrate_kbps;
    out.acmod = to_cpp(config.acmod);
    out.lfe = config.lfe != 0;
    out.dialnorm = config.dialnorm;

    out.auto_tools = config.auto_tools != 0;
    out.coupling = config.coupling != 0;
    out.cplbegf = config.cplbegf;
    out.enhanced = config.enhanced != 0;
    out.spx = config.spx != 0;
    out.spxbegf = config.spxbegf;
    out.spx_atten = config.spx_atten != 0;
    out.spxattencod = config.spxattencod;
    out.aht = config.aht != 0;
    out.gaqmod = config.gaqmod;
    out.transient_prenoise = config.transient_prenoise != 0;
    out.fast_mdct = config.fast_mdct != 0;

    out.strmtyp = to_cpp(config.strmtyp);
    out.substreamid = config.substreamid;
    out.chanmap = config.has_chanmap ? std::optional<std::uint16_t>(config.chanmap) : std::nullopt;
    return out;
}

ac3::eac3::FrameMetadata eac3_frame_metadata_to_cpp(const ac3forge_eac3_frame_metadata_t& metadata) {
    ac3::eac3::FrameMetadata out;
    std::copy(std::begin(metadata.dynrng), std::end(metadata.dynrng), out.dynrng.begin());
    out.compr = metadata.has_compr ? std::optional<std::uint8_t>(metadata.compr) : std::nullopt;
    std::copy(std::begin(metadata.dynrng2), std::end(metadata.dynrng2), out.dynrng2.begin());
    out.compr2 = metadata.has_compr2 ? std::optional<std::uint8_t>(metadata.compr2) : std::nullopt;
    return out;
}

}  // namespace

extern "C" {

void ac3forge_eac3_frame_config_init(ac3forge_eac3_frame_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const ac3::eac3::FrameConfig defaults{};
    *config = ac3forge_eac3_frame_config_t{
        .sample_rate = ac3forge_c::from_cpp(defaults.sample_rate),
        .bitrate_kbps = defaults.bitrate_kbps,
        .dialnorm = defaults.dialnorm,
        .acmod = ac3forge_c::from_cpp(defaults.acmod),
        .lfe = defaults.lfe ? 1 : 0,
        .auto_tools = defaults.auto_tools ? 1 : 0,
        .coupling = defaults.coupling ? 1 : 0,
        .cplbegf = defaults.cplbegf,
        .enhanced = defaults.enhanced ? 1 : 0,
        .spx = defaults.spx ? 1 : 0,
        .spxbegf = defaults.spxbegf,
        .spx_atten = defaults.spx_atten ? 1 : 0,
        .spxattencod = defaults.spxattencod,
        .aht = defaults.aht ? 1 : 0,
        .gaqmod = defaults.gaqmod,
        .transient_prenoise = defaults.transient_prenoise ? 1 : 0,
        .fast_mdct = defaults.fast_mdct ? 1 : 0,
        .strmtyp = ac3forge_c::from_cpp(defaults.strmtyp),
        .substreamid = defaults.substreamid,
        .has_chanmap = 0,
        .chanmap = 0};
}

void ac3forge_eac3_frame_metadata_init(ac3forge_eac3_frame_metadata_t* metadata) {
    if (metadata == nullptr) {
        return;
    }
    *metadata = ac3forge_eac3_frame_metadata_t{};
}

ac3forge_status_t ac3forge_eac3_encoder_create(const ac3forge_eac3_frame_config_t* config,
                                                ac3forge_eac3_encoder_t** out_encoder) {
    if (config == nullptr || out_encoder == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&] {
        *out_encoder = new ac3forge_eac3_encoder(eac3_frame_config_to_cpp(*config));
        return AC3FORGE_OK;
    });
}

void ac3forge_eac3_encoder_destroy(ac3forge_eac3_encoder_t* encoder) { delete encoder; }

size_t ac3forge_eac3_encoder_channel_count(const ac3forge_eac3_encoder_t* encoder) {
    return encoder == nullptr ? 0 : static_cast<size_t>(encoder->impl.channel_count());
}

size_t ac3forge_eac3_encoder_samples_per_frame(const ac3forge_eac3_encoder_t* encoder) {
    return encoder == nullptr ? 0 : static_cast<size_t>(encoder->impl.samples_per_frame());
}

void ac3forge_eac3_encoder_latency(const ac3forge_eac3_encoder_t* encoder,
                                    ac3forge_latency_t* out_latency) {
    if (encoder == nullptr || out_latency == nullptr) {
        return;
    }
    *out_latency = ac3forge_c::from_cpp(encoder->impl.latency());
}

int ac3forge_eac3_encoder_latency_samples(const ac3forge_eac3_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.latency_samples();
}

ac3forge_status_t ac3forge_eac3_encoder_encode_frame(
    ac3forge_eac3_encoder_t* encoder, const float* const* channels, size_t channel_count,
    size_t samples_per_channel, const ac3forge_eac3_frame_metadata_t* metadata, const uint8_t* aux,
    size_t aux_size, ac3forge_bytes_t** out_frame) {
    if (encoder == nullptr || channels == nullptr || out_frame == nullptr ||
        (aux_size > 0 && aux == nullptr)) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    if (channel_count != ac3forge_eac3_encoder_channel_count(encoder) ||
        samples_per_channel != ac3forge_eac3_encoder_samples_per_frame(encoder)) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&]() -> ac3forge_status_t {
        std::vector<std::span<const float>> spans;
        spans.reserve(channel_count);
        for (size_t i = 0; i < channel_count; ++i) {
            if (channels[i] == nullptr) {
                return AC3FORGE_ERROR_INVALID_ARGUMENT;
            }
            spans.emplace_back(channels[i], samples_per_channel);
        }
        const ac3::eac3::AuxPayload aux_payload(reinterpret_cast<const std::byte*>(aux), aux_size);
        auto result = metadata != nullptr
                           ? encoder->impl.encode_frame(spans, eac3_frame_metadata_to_cpp(*metadata),
                                                         aux_payload)
                           : encoder->impl.encode_frame(spans, aux_payload);
        if (!result) {
            return ac3forge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<ac3forge_bytes>();
        owned->data = std::move(*result);
        *out_frame = owned.release();
        return AC3FORGE_OK;
    });
}

ac3forge_status_t ac3forge_eac3_access_unit_encoder_create(
    const ac3forge_eac3_frame_config_t* independent, const ac3forge_eac3_frame_config_t* dependents,
    size_t dependent_count, ac3forge_eac3_access_unit_encoder_t** out_encoder) {
    if (independent == nullptr || out_encoder == nullptr ||
        (dependent_count > 0 && dependents == nullptr)) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&] {
        ac3::eac3::AccessUnitConfig config;
        config.independent = eac3_frame_config_to_cpp(*independent);
        config.dependents.reserve(dependent_count);
        for (size_t i = 0; i < dependent_count; ++i) {
            config.dependents.push_back(eac3_frame_config_to_cpp(dependents[i]));
        }
        *out_encoder = new ac3forge_eac3_access_unit_encoder(config);
        return AC3FORGE_OK;
    });
}

void ac3forge_eac3_access_unit_encoder_destroy(ac3forge_eac3_access_unit_encoder_t* encoder) {
    delete encoder;
}

size_t ac3forge_eac3_access_unit_encoder_channel_count(
    const ac3forge_eac3_access_unit_encoder_t* encoder) {
    return encoder == nullptr ? 0 : static_cast<size_t>(encoder->impl.channel_count());
}

void ac3forge_eac3_access_unit_encoder_latency(const ac3forge_eac3_access_unit_encoder_t* encoder,
                                                ac3forge_latency_t* out_latency) {
    if (encoder == nullptr || out_latency == nullptr) {
        return;
    }
    *out_latency = ac3forge_c::from_cpp(encoder->impl.latency());
}

int ac3forge_eac3_access_unit_encoder_latency_samples(
    const ac3forge_eac3_access_unit_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.latency_samples();
}

ac3forge_status_t ac3forge_eac3_access_unit_encoder_encode(
    ac3forge_eac3_access_unit_encoder_t* encoder, const float* const* channels,
    size_t channel_count, size_t samples_per_channel, const uint8_t* aux, size_t aux_size,
    ac3forge_eac3_access_unit_t** out_unit) {
    // channels may be NULL when channel_count is 0 - a config the constructor
    // could not build any substreams from (an invalid chanmap, too many
    // dependents, ...) reports channel_count() == 0 exactly as
    // ac3::eac3::AccessUnitEncoder does, and encode() below is how a caller
    // discovers the real reason (see the C++ constructor's own comment).
    if (encoder == nullptr || out_unit == nullptr || (channel_count > 0 && channels == nullptr) ||
        (aux_size > 0 && aux == nullptr)) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    if (channel_count != ac3forge_eac3_access_unit_encoder_channel_count(encoder) ||
        samples_per_channel != AC3FORGE_SAMPLES_PER_FRAME) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&]() -> ac3forge_status_t {
        std::vector<std::span<const float>> spans;
        spans.reserve(channel_count);
        for (size_t i = 0; i < channel_count; ++i) {
            if (channels[i] == nullptr) {
                return AC3FORGE_ERROR_INVALID_ARGUMENT;
            }
            spans.emplace_back(channels[i], samples_per_channel);
        }
        const ac3::eac3::AuxPayload aux_payload(reinterpret_cast<const std::byte*>(aux), aux_size);
        auto result = encoder->impl.encode_access_unit(spans, aux_payload);
        if (!result) {
            return ac3forge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<ac3forge_eac3_access_unit>();
        owned->data = std::move(*result);
        *out_unit = owned.release();
        return AC3FORGE_OK;
    });
}

const uint8_t* ac3forge_eac3_access_unit_data(const ac3forge_eac3_access_unit_t* unit) {
    if (unit == nullptr || unit->data.bytes.empty()) {
        return nullptr;
    }
    return reinterpret_cast<const uint8_t*>(unit->data.bytes.data());
}

size_t ac3forge_eac3_access_unit_size(const ac3forge_eac3_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.bytes.size();
}

size_t ac3forge_eac3_access_unit_substream_count(const ac3forge_eac3_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.substream_count();
}

uint32_t ac3forge_eac3_access_unit_substream_bytes(const ac3forge_eac3_access_unit_t* unit,
                                                    size_t index) {
    if (unit == nullptr || index >= unit->data.substream_bytes.size()) {
        return 0;
    }
    return unit->data.substream_bytes[index];
}

void ac3forge_eac3_access_unit_destroy(ac3forge_eac3_access_unit_t* unit) { delete unit; }

}  // extern "C"
