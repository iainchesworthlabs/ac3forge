#include <memory>
#include <optional>
#include <span>

#include "internal.hpp"

using iclforge_c::guard;
using iclforge_c::to_cpp;

// Kept outside extern "C" below: a C-linkage function returning a C++ class
// by value (iclforge::ac3::EncoderConfig, here) is diagnosed by Clang
// (-Wreturn-type-c-linkage) as ABI-incompatible with C, which this helper
// genuinely is not meant to be - it is a private implementation detail, never
// declared in iclforge.h.
namespace {

iclforge::ac3::EncoderConfig encoder_config_to_cpp(const iclforge_encoder_config_t& config) {
    iclforge::ac3::EncoderConfig out;
    out.sample_rate = to_cpp(config.sample_rate);
    out.bitrate_kbps = config.bitrate_kbps;
    out.dialnorm = config.dialnorm;
    out.dialnorm2 = config.has_dialnorm2 ? std::optional<int>(config.dialnorm2) : std::nullopt;
    out.chbwcod = config.chbwcod;
    out.acmod = to_cpp(config.acmod);
    out.lfe = config.lfe != 0;
    out.coupling = config.coupling != 0;
    out.cplbegf = config.cplbegf;
    out.cplendf = config.cplendf;
    out.fast_mdct = config.fast_mdct != 0;
    out.drc = config.has_drc ? std::optional<iclforge::ac3::meta::Profile>(iclforge::ac3::meta::profile(to_cpp(config.drc_profile)))
                              : std::nullopt;
    out.heavy = config.has_heavy
                    ? std::optional<iclforge::ac3::meta::HeavyConfig>(to_cpp(config.heavy))
                    : std::nullopt;
    out.drc2 = config.has_drc2
                   ? std::optional<iclforge::ac3::meta::Profile>(iclforge::ac3::meta::profile(to_cpp(config.drc2_profile)))
                   : std::nullopt;
    out.heavy2 = config.has_heavy2
                     ? std::optional<iclforge::ac3::meta::HeavyConfig>(to_cpp(config.heavy2))
                     : std::nullopt;
    out.cmixlev = to_cpp(config.cmixlev);
    out.surmixlev = to_cpp(config.surmixlev);
    return out;
}

}  // namespace

extern "C" {

void iclforge_encoder_config_init(iclforge_encoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const iclforge::ac3::EncoderConfig defaults{};
    *config = iclforge_encoder_config_t{
        .sample_rate = iclforge_c::from_cpp(defaults.sample_rate),
        .bitrate_kbps = defaults.bitrate_kbps,
        .dialnorm = defaults.dialnorm,
        .has_dialnorm2 = 0,
        .dialnorm2 = 0,
        .chbwcod = defaults.chbwcod,
        .acmod = iclforge_c::from_cpp(defaults.acmod),
        .lfe = defaults.lfe ? 1 : 0,
        .coupling = defaults.coupling ? 1 : 0,
        .cplbegf = defaults.cplbegf,
        .cplendf = defaults.cplendf,
        .fast_mdct = defaults.fast_mdct ? 1 : 0,
        .has_drc = 0,
        .drc_profile = ICLFORGE_DRC_FILM_STANDARD,
        .has_heavy = 0,
        .heavy = {},
        .has_drc2 = 0,
        .drc2_profile = ICLFORGE_DRC_FILM_STANDARD,
        .has_heavy2 = 0,
        .heavy2 = {},
        .cmixlev = iclforge_c::from_cpp(defaults.cmixlev),
        .surmixlev = iclforge_c::from_cpp(defaults.surmixlev)};
    iclforge_heavy_config_init(&config->heavy);
    iclforge_heavy_config_init(&config->heavy2);
}

iclforge_status_t iclforge_encoder_create(const iclforge_encoder_config_t* config,
                                           iclforge_encoder_t** out_encoder) {
    if (config == nullptr || out_encoder == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    // chbwcod's legal codes stop at 60 (§5.4.3.24: 61-63 fit its six bits but
    // are reserved); any negative value means "auto". iclforge::ac3::FrameEncoder only
    // asserts the range - it has no FrameError for it - so a code past 60
    // must be refused here, where the config is first seen, or it aborts the
    // caller's process at the first encode_frame().
    if (config->chbwcod > 60) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&config, &out_encoder] {
        *out_encoder = new iclforge_encoder(encoder_config_to_cpp(*config));
        return ICLFORGE_OK;
    });
}

void iclforge_encoder_destroy(iclforge_encoder_t* encoder) { delete encoder; }

size_t iclforge_encoder_channel_count(const iclforge_encoder_t* encoder) {
    if (encoder == nullptr) {
        return 0;
    }
    return static_cast<size_t>(encoder->impl.channel_count());
}

const uint8_t* iclforge_bytes_data(const iclforge_bytes_t* bytes) {
    if (bytes == nullptr || bytes->data.empty()) {
        return nullptr;
    }
    return reinterpret_cast<const uint8_t*>(bytes->data.data());
}

size_t iclforge_bytes_size(const iclforge_bytes_t* bytes) {
    return bytes == nullptr ? 0 : bytes->data.size();
}

void iclforge_bytes_destroy(iclforge_bytes_t* bytes) { delete bytes; }

iclforge_status_t iclforge_encoder_encode_frame(iclforge_encoder_t* encoder,
                                                 const float* const* channels,
                                                 size_t channel_count, size_t samples_per_channel,
                                                 iclforge_bytes_t** out_frame) {
    if (encoder == nullptr || channels == nullptr || out_frame == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    if (channel_count != iclforge_encoder_channel_count(encoder) ||
        samples_per_channel != iclforge::ac3::kSamplesPerFrame) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&encoder, &channels, &channel_count, &samples_per_channel,
                  &out_frame]() -> iclforge_status_t {
        std::vector<std::span<const float>> spans;
        spans.reserve(channel_count);
        for (size_t i = 0; i < channel_count; ++i) {
            if (channels[i] == nullptr) {
                return ICLFORGE_ERROR_INVALID_ARGUMENT;
            }
            spans.emplace_back(channels[i], samples_per_channel);
        }
        auto result = encoder->impl.encode_frame(spans);
        if (!result) {
            return iclforge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<iclforge_bytes>();
        owned->data = std::move(*result);
        *out_frame = owned.release();
        return ICLFORGE_OK;
    });
}

int iclforge_encoder_latency_samples(const iclforge_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.latency_samples();
}

void iclforge_encoder_latency(const iclforge_encoder_t* encoder,
                              iclforge_latency_t* out_latency) {
    if (encoder == nullptr || out_latency == nullptr) {
        return;
    }
    *out_latency = iclforge_c::from_cpp(encoder->impl.latency());
}

int iclforge_decoder_latency_samples(const iclforge_decoder_t* decoder) {
    // Not a use of `decoder` beyond the null check, and deliberately so: an
    // AC-3 decoder's own contribution is structurally zero (see the header).
    // Taking the handle anyway keeps the call shape identical to the E-AC-3
    // form, whose answer really does depend on the instance.
    return decoder == nullptr ? 0 : iclforge::ac3::FrameDecoder::latency_samples();
}

}  // extern "C"
