#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "internal.hpp"

using iclforge_c::guard;
using iclforge_c::to_cpp;

// Kept outside extern "C" below - see encoder.cpp's identical comment on
// -Wreturn-type-c-linkage.
namespace {

iclforge::ac3::eac3::FrameConfig eac3_frame_config_to_cpp(
    const iclforge_eac3_frame_config_t& config) {
    iclforge::ac3::eac3::FrameConfig out;
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

iclforge::ac3::eac3::FrameMetadata eac3_frame_metadata_to_cpp(const iclforge_eac3_frame_metadata_t& metadata) {
    iclforge::ac3::eac3::FrameMetadata out;
    std::copy(std::begin(metadata.dynrng), std::end(metadata.dynrng), out.dynrng.begin());
    out.compr = metadata.has_compr ? std::optional<std::uint8_t>(metadata.compr) : std::nullopt;
    std::copy(std::begin(metadata.dynrng2), std::end(metadata.dynrng2), out.dynrng2.begin());
    out.compr2 = metadata.has_compr2 ? std::optional<std::uint8_t>(metadata.compr2) : std::nullopt;
    return out;
}

}  // namespace

extern "C" {

void iclforge_eac3_frame_config_init(iclforge_eac3_frame_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const iclforge::ac3::eac3::FrameConfig defaults{};
    *config = iclforge_eac3_frame_config_t{
        .sample_rate = iclforge_c::from_cpp(defaults.sample_rate),
        .bitrate_kbps = defaults.bitrate_kbps,
        .dialnorm = defaults.dialnorm,
        .acmod = iclforge_c::from_cpp(defaults.acmod),
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
        .strmtyp = iclforge_c::from_cpp(defaults.strmtyp),
        .substreamid = defaults.substreamid,
        .has_chanmap = 0,
        .chanmap = 0};
}

void iclforge_eac3_frame_metadata_init(iclforge_eac3_frame_metadata_t* metadata) {
    if (metadata == nullptr) {
        return;
    }
    *metadata = iclforge_eac3_frame_metadata_t{};
}

iclforge_status_t iclforge_eac3_encoder_create(const iclforge_eac3_frame_config_t* config,
                                                iclforge_eac3_encoder_t** out_encoder) {
    if (config == nullptr || out_encoder == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&config, &out_encoder] {
        *out_encoder = new iclforge_eac3_encoder(eac3_frame_config_to_cpp(*config));
        return ICLFORGE_OK;
    });
}

void iclforge_eac3_encoder_destroy(iclforge_eac3_encoder_t* encoder) { delete encoder; }

size_t iclforge_eac3_encoder_channel_count(const iclforge_eac3_encoder_t* encoder) {
    return encoder == nullptr ? 0 : static_cast<size_t>(encoder->impl.channel_count());
}

size_t iclforge_eac3_encoder_samples_per_frame(const iclforge_eac3_encoder_t* encoder) {
    return encoder == nullptr ? 0 : static_cast<size_t>(encoder->impl.samples_per_frame());
}

void iclforge_eac3_encoder_latency(const iclforge_eac3_encoder_t* encoder,
                                    iclforge_latency_t* out_latency) {
    if (encoder == nullptr || out_latency == nullptr) {
        return;
    }
    *out_latency = iclforge_c::from_cpp(encoder->impl.latency());
}

int iclforge_eac3_encoder_latency_samples(const iclforge_eac3_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.latency_samples();
}

iclforge_status_t iclforge_eac3_encoder_encode_frame(
    iclforge_eac3_encoder_t* encoder, const float* const* channels, size_t channel_count,
    size_t samples_per_channel, const iclforge_eac3_frame_metadata_t* metadata, const uint8_t* aux,
    size_t aux_size, iclforge_bytes_t** out_frame) {
    if (encoder == nullptr || channels == nullptr || out_frame == nullptr ||
        (aux_size > 0 && aux == nullptr)) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    if (channel_count != iclforge_eac3_encoder_channel_count(encoder) ||
        samples_per_channel != iclforge_eac3_encoder_samples_per_frame(encoder)) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&encoder, &channels, &channel_count, &samples_per_channel, &metadata, &aux,
                  &aux_size, &out_frame]() -> iclforge_status_t {
        std::vector<std::span<const float>> spans;
        spans.reserve(channel_count);
        for (size_t i = 0; i < channel_count; ++i) {
            if (channels[i] == nullptr) {
                return ICLFORGE_ERROR_INVALID_ARGUMENT;
            }
            spans.emplace_back(channels[i], samples_per_channel);
        }
        const iclforge::ac3::eac3::AuxPayload aux_payload(reinterpret_cast<const std::byte*>(aux),
                                                     aux_size);
        auto result = metadata != nullptr
                           ? encoder->impl.encode_frame(spans, eac3_frame_metadata_to_cpp(*metadata),
                                                         aux_payload)
                           : encoder->impl.encode_frame(spans, aux_payload);
        if (!result) {
            return iclforge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<iclforge_bytes>();
        owned->data = std::move(*result);
        *out_frame = owned.release();
        return ICLFORGE_OK;
    });
}

iclforge_status_t iclforge_eac3_access_unit_encoder_create(
    const iclforge_eac3_frame_config_t* independent, const iclforge_eac3_frame_config_t* dependents,
    size_t dependent_count, iclforge_eac3_access_unit_encoder_t** out_encoder) {
    if (independent == nullptr || out_encoder == nullptr ||
        (dependent_count > 0 && dependents == nullptr)) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    // §E2.3.1.2: eight dependents at most, as the header documents. Checked
    // before the reserve() below, which would otherwise size itself from an
    // unchecked caller count - SIZE_MAX threw std::length_error there - and
    // before the loop reads past the caller's array.
    if (dependent_count > 8) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&independent, &dependents, &dependent_count, &out_encoder] {
        iclforge::ac3::eac3::AccessUnitConfig config;
        config.independent = eac3_frame_config_to_cpp(*independent);
        config.dependents.reserve(dependent_count);
        for (size_t i = 0; i < dependent_count; ++i) {
            config.dependents.push_back(eac3_frame_config_to_cpp(dependents[i]));
        }
        *out_encoder = new iclforge_eac3_access_unit_encoder(config);
        return ICLFORGE_OK;
    });
}

void iclforge_eac3_access_unit_encoder_destroy(iclforge_eac3_access_unit_encoder_t* encoder) {
    delete encoder;
}

size_t iclforge_eac3_access_unit_encoder_channel_count(
    const iclforge_eac3_access_unit_encoder_t* encoder) {
    return encoder == nullptr ? 0 : static_cast<size_t>(encoder->impl.channel_count());
}

void iclforge_eac3_access_unit_encoder_latency(const iclforge_eac3_access_unit_encoder_t* encoder,
                                                iclforge_latency_t* out_latency) {
    if (encoder == nullptr || out_latency == nullptr) {
        return;
    }
    *out_latency = iclforge_c::from_cpp(encoder->impl.latency());
}

int iclforge_eac3_access_unit_encoder_latency_samples(
    const iclforge_eac3_access_unit_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.latency_samples();
}

iclforge_status_t iclforge_eac3_access_unit_encoder_encode(
    iclforge_eac3_access_unit_encoder_t* encoder, const float* const* channels,
    size_t channel_count, size_t samples_per_channel, const uint8_t* aux, size_t aux_size,
    iclforge_eac3_access_unit_t** out_unit) {
    // channels may be NULL when channel_count is 0 - a config the constructor
    // could not build any substreams from (an invalid chanmap, a dependent at
    // another sample rate, ...) reports channel_count() == 0 exactly as
    // iclforge::ac3::eac3::AccessUnitEncoder does, and encode() below is how a caller
    // discovers the real reason (see the C++ constructor's own comment).
    if (encoder == nullptr || out_unit == nullptr || (channel_count > 0 && channels == nullptr) ||
        (aux_size > 0 && aux == nullptr)) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    if (channel_count != iclforge_eac3_access_unit_encoder_channel_count(encoder) ||
        samples_per_channel != ICLFORGE_SAMPLES_PER_FRAME) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&encoder, &channels, &channel_count, &samples_per_channel, &aux, &aux_size,
                  &out_unit]() -> iclforge_status_t {
        std::vector<std::span<const float>> spans;
        spans.reserve(channel_count);
        for (size_t i = 0; i < channel_count; ++i) {
            if (channels[i] == nullptr) {
                return ICLFORGE_ERROR_INVALID_ARGUMENT;
            }
            spans.emplace_back(channels[i], samples_per_channel);
        }
        const iclforge::ac3::eac3::AuxPayload aux_payload(reinterpret_cast<const std::byte*>(aux),
                                                     aux_size);
        auto result = encoder->impl.encode_access_unit(spans, aux_payload);
        if (!result) {
            return iclforge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<iclforge_eac3_access_unit>();
        owned->data = std::move(*result);
        *out_unit = owned.release();
        return ICLFORGE_OK;
    });
}

const uint8_t* iclforge_eac3_access_unit_data(const iclforge_eac3_access_unit_t* unit) {
    if (unit == nullptr || unit->data.bytes.empty()) {
        return nullptr;
    }
    return reinterpret_cast<const uint8_t*>(unit->data.bytes.data());
}

size_t iclforge_eac3_access_unit_size(const iclforge_eac3_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.bytes.size();
}

size_t iclforge_eac3_access_unit_substream_count(const iclforge_eac3_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.substream_count();
}

uint32_t iclforge_eac3_access_unit_substream_bytes(const iclforge_eac3_access_unit_t* unit,
                                                    size_t index) {
    if (unit == nullptr || index >= unit->data.substream_bytes.size()) {
        return 0;
    }
    return unit->data.substream_bytes[index];
}

void iclforge_eac3_access_unit_destroy(iclforge_eac3_access_unit_t* unit) { delete unit; }

}  // extern "C"
