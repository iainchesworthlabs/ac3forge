#include <memory>
#include <span>
#include <vector>

#include "internal.hpp"

using ac3forge_c::guard;
using ac3forge_c::to_cpp;

extern "C" {

ac3forge_status_t ac3forge_loudness_meter_create(ac3forge_sample_rate_t sample_rate,
                                                   ac3forge_acmod_t acmod, int lfe,
                                                   ac3forge_loudness_meter_t** out_meter) {
    if (out_meter == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&] {
        auto owned = std::make_unique<ac3forge_loudness_meter>();
        owned->impl = std::make_unique<ac3::meta::LoudnessMeter>(to_cpp(sample_rate), to_cpp(acmod),
                                                                  lfe != 0);
        *out_meter = owned.release();
        return AC3FORGE_OK;
    });
}

ac3forge_status_t ac3forge_loudness_meter_create_for_chanmap(ac3forge_sample_rate_t sample_rate,
                                                               uint16_t chanmap,
                                                               ac3forge_loudness_meter_t** out_meter) {
    if (out_meter == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    const auto layout = ac3::eac3::chanmap::expand(chanmap);
    if (layout.count == 0) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&] {
        auto owned = std::make_unique<ac3forge_loudness_meter>();
        owned->impl = std::make_unique<ac3::meta::LoudnessMeter>(to_cpp(sample_rate), layout);
        *out_meter = owned.release();
        return AC3FORGE_OK;
    });
}

void ac3forge_loudness_meter_destroy(ac3forge_loudness_meter_t* meter) { delete meter; }

int ac3forge_loudness_meter_channel_count(const ac3forge_loudness_meter_t* meter) {
    return meter == nullptr ? 0 : meter->impl->channel_count();
}

ac3forge_status_t ac3forge_loudness_meter_push(ac3forge_loudness_meter_t* meter,
                                                const float* const* channels, size_t channel_count,
                                                size_t samples_per_channel) {
    if (meter == nullptr || channels == nullptr) {
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
        meter->impl->push(spans);
        return AC3FORGE_OK;
    });
}

int ac3forge_loudness_meter_has_integrated_lkfs(const ac3forge_loudness_meter_t* meter) {
    return meter != nullptr && meter->impl->integrated_lkfs().has_value() ? 1 : 0;
}

double ac3forge_loudness_meter_integrated_lkfs(const ac3forge_loudness_meter_t* meter) {
    if (meter == nullptr) {
        return 0.0;
    }
    const auto value = meter->impl->integrated_lkfs();
    return value.has_value() ? *value : 0.0;
}

int ac3forge_loudness_meter_has_momentary_lkfs(const ac3forge_loudness_meter_t* meter) {
    return meter != nullptr && meter->impl->momentary_lkfs().has_value() ? 1 : 0;
}

double ac3forge_loudness_meter_momentary_lkfs(const ac3forge_loudness_meter_t* meter) {
    if (meter == nullptr) {
        return 0.0;
    }
    const auto value = meter->impl->momentary_lkfs();
    return value.has_value() ? *value : 0.0;
}

int ac3forge_loudness_meter_has_short_term_lkfs(const ac3forge_loudness_meter_t* meter) {
    return meter != nullptr && meter->impl->short_term_lkfs().has_value() ? 1 : 0;
}

double ac3forge_loudness_meter_short_term_lkfs(const ac3forge_loudness_meter_t* meter) {
    if (meter == nullptr) {
        return 0.0;
    }
    const auto value = meter->impl->short_term_lkfs();
    return value.has_value() ? *value : 0.0;
}

int ac3forge_loudness_meter_has_loudness_range(const ac3forge_loudness_meter_t* meter) {
    return meter != nullptr && meter->impl->loudness_range().has_value() ? 1 : 0;
}

double ac3forge_loudness_meter_loudness_range(const ac3forge_loudness_meter_t* meter) {
    if (meter == nullptr) {
        return 0.0;
    }
    const auto value = meter->impl->loudness_range();
    return value.has_value() ? *value : 0.0;
}

int ac3forge_loudness_meter_has_true_peak_dbtp(const ac3forge_loudness_meter_t* meter) {
    return meter != nullptr && meter->impl->true_peak_dbtp().has_value() ? 1 : 0;
}

double ac3forge_loudness_meter_true_peak_dbtp(const ac3forge_loudness_meter_t* meter) {
    if (meter == nullptr) {
        return 0.0;
    }
    const auto value = meter->impl->true_peak_dbtp();
    return value.has_value() ? *value : 0.0;
}

int ac3forge_dialnorm_from_lkfs(double lkfs) { return ac3::meta::dialnorm_from_lkfs(lkfs); }

}  // extern "C"
