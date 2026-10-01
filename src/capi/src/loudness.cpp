#include <memory>
#include <span>
#include <vector>

#include "internal.hpp"

using iclforge_c::guard;
using iclforge_c::to_cpp;

extern "C" {

iclforge_status_t iclforge_loudness_meter_create(iclforge_sample_rate_t sample_rate,
                                                   iclforge_acmod_t acmod, int lfe,
                                                   iclforge_loudness_meter_t** out_meter) {
    if (out_meter == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&sample_rate, &acmod, &lfe, &out_meter] {
        auto owned = std::make_unique<iclforge_loudness_meter>();
        owned->impl = std::make_unique<iclforge::ac3::meta::LoudnessMeter>(to_cpp(sample_rate),
                                                                      to_cpp(acmod), lfe != 0);
        *out_meter = owned.release();
        return ICLFORGE_OK;
    });
}

iclforge_status_t iclforge_loudness_meter_create_for_chanmap(iclforge_sample_rate_t sample_rate,
                                                               uint16_t chanmap,
                                                               iclforge_loudness_meter_t** out_meter) {
    if (out_meter == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    const auto layout = iclforge::ac3::eac3::chanmap::expand(chanmap);
    if (layout.count == 0) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&sample_rate, &layout, &out_meter] {
        auto owned = std::make_unique<iclforge_loudness_meter>();
        owned->impl =
            std::make_unique<iclforge::ac3::meta::LoudnessMeter>(to_cpp(sample_rate), layout);
        *out_meter = owned.release();
        return ICLFORGE_OK;
    });
}

void iclforge_loudness_meter_destroy(iclforge_loudness_meter_t* meter) { delete meter; }

int iclforge_loudness_meter_channel_count(const iclforge_loudness_meter_t* meter) {
    return meter == nullptr ? 0 : meter->impl->channel_count();
}

iclforge_status_t iclforge_loudness_meter_push(iclforge_loudness_meter_t* meter,
                                                const float* const* channels, size_t channel_count,
                                                size_t samples_per_channel) {
    if (meter == nullptr || channels == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    // At most channel_count() spans. Checked before the reserve() below,
    // which would otherwise size itself from an unchecked caller count -
    // SIZE_MAX threw std::length_error there - and before the loop reads past
    // the array.
    if (channel_count > static_cast<size_t>(meter->impl->channel_count())) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&meter, &channels, &channel_count, &samples_per_channel]() -> iclforge_status_t {
        std::vector<std::span<const float>> spans;
        spans.reserve(channel_count);
        for (size_t i = 0; i < channel_count; ++i) {
            if (channels[i] == nullptr) {
                return ICLFORGE_ERROR_INVALID_ARGUMENT;
            }
            spans.emplace_back(channels[i], samples_per_channel);
        }
        meter->impl->push(spans);
        return ICLFORGE_OK;
    });
}

int iclforge_loudness_meter_has_integrated_lkfs(const iclforge_loudness_meter_t* meter) {
    return meter != nullptr && meter->impl->integrated_lkfs().has_value() ? 1 : 0;
}

double iclforge_loudness_meter_integrated_lkfs(const iclforge_loudness_meter_t* meter) {
    if (meter == nullptr) {
        return 0.0;
    }
    const auto value = meter->impl->integrated_lkfs();
    return value.has_value() ? *value : 0.0;
}

int iclforge_loudness_meter_has_momentary_lkfs(const iclforge_loudness_meter_t* meter) {
    return meter != nullptr && meter->impl->momentary_lkfs().has_value() ? 1 : 0;
}

double iclforge_loudness_meter_momentary_lkfs(const iclforge_loudness_meter_t* meter) {
    if (meter == nullptr) {
        return 0.0;
    }
    const auto value = meter->impl->momentary_lkfs();
    return value.has_value() ? *value : 0.0;
}

int iclforge_loudness_meter_has_short_term_lkfs(const iclforge_loudness_meter_t* meter) {
    return meter != nullptr && meter->impl->short_term_lkfs().has_value() ? 1 : 0;
}

double iclforge_loudness_meter_short_term_lkfs(const iclforge_loudness_meter_t* meter) {
    if (meter == nullptr) {
        return 0.0;
    }
    const auto value = meter->impl->short_term_lkfs();
    return value.has_value() ? *value : 0.0;
}

int iclforge_loudness_meter_has_loudness_range(const iclforge_loudness_meter_t* meter) {
    return meter != nullptr && meter->impl->loudness_range().has_value() ? 1 : 0;
}

double iclforge_loudness_meter_loudness_range(const iclforge_loudness_meter_t* meter) {
    if (meter == nullptr) {
        return 0.0;
    }
    const auto value = meter->impl->loudness_range();
    return value.has_value() ? *value : 0.0;
}

int iclforge_loudness_meter_has_true_peak_dbtp(const iclforge_loudness_meter_t* meter) {
    return meter != nullptr && meter->impl->true_peak_dbtp().has_value() ? 1 : 0;
}

double iclforge_loudness_meter_true_peak_dbtp(const iclforge_loudness_meter_t* meter) {
    if (meter == nullptr) {
        return 0.0;
    }
    const auto value = meter->impl->true_peak_dbtp();
    return value.has_value() ? *value : 0.0;
}

int iclforge_dialnorm_from_lkfs(double lkfs) {
    return iclforge::ac3::meta::dialnorm_from_lkfs(lkfs);
}

}  // extern "C"
