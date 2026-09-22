#include <memory>
#include <span>
#include <vector>

#include "internal.hpp"

using ac3forge_c::guard;
using ac3forge_c::to_cpp;

namespace {
ac3::analysis::MeterBallistics ballistics_to_cpp(const ac3forge_level_meter_ballistics_t* ballistics) {
    if (ballistics == nullptr) {
        return ac3::analysis::MeterBallistics{};
    }
    return ac3::analysis::MeterBallistics{.rms_integration_ms = ballistics->rms_integration_ms,
                                          .peak_decay_db_per_s = ballistics->peak_decay_db_per_s,
                                          .peak_hold_ms = ballistics->peak_hold_ms};
}
}  // namespace

extern "C" {

void ac3forge_level_meter_ballistics_init(ac3forge_level_meter_ballistics_t* ballistics) {
    if (ballistics == nullptr) {
        return;
    }
    const ac3::analysis::MeterBallistics defaults{};
    *ballistics = ac3forge_level_meter_ballistics_t{.rms_integration_ms = defaults.rms_integration_ms,
                                                     .peak_decay_db_per_s = defaults.peak_decay_db_per_s,
                                                     .peak_hold_ms = defaults.peak_hold_ms};
}

ac3forge_status_t ac3forge_level_meter_create(ac3forge_acmod_t acmod, int lfe,
                                               uint32_t sample_rate, int channels,
                                               const ac3forge_level_meter_ballistics_t* ballistics,
                                               ac3forge_level_meter_t** out_meter) {
    if (out_meter == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&] {
        const auto cpp_ballistics = ballistics_to_cpp(ballistics);
        auto owned = std::make_unique<ac3forge_level_meter>();
        if (channels > 0) {
            owned->impl = std::make_unique<ac3::analysis::LevelMeter>(
                to_cpp(acmod), lfe != 0, sample_rate, channels, cpp_ballistics);
        } else {
            owned->impl = std::make_unique<ac3::analysis::LevelMeter>(to_cpp(acmod), lfe != 0,
                                                                       sample_rate, cpp_ballistics);
        }
        *out_meter = owned.release();
        return AC3FORGE_OK;
    });
}

void ac3forge_level_meter_destroy(ac3forge_level_meter_t* meter) { delete meter; }

ac3forge_acmod_t ac3forge_level_meter_acmod(const ac3forge_level_meter_t* meter) {
    return meter == nullptr ? AC3FORGE_ACMOD_2_0 : ac3forge_c::from_cpp(meter->impl->acmod());
}

int ac3forge_level_meter_lfe(const ac3forge_level_meter_t* meter) {
    return meter != nullptr && meter->impl->lfe() ? 1 : 0;
}

int ac3forge_level_meter_channel_count(const ac3forge_level_meter_t* meter) {
    return meter == nullptr ? 0 : meter->impl->channel_count();
}

uint32_t ac3forge_level_meter_sample_rate(const ac3forge_level_meter_t* meter) {
    return meter == nullptr ? 0 : meter->impl->sample_rate();
}

ac3forge_status_t ac3forge_level_meter_process(ac3forge_level_meter_t* meter,
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
        meter->impl->process(spans);
        return AC3FORGE_OK;
    });
}

void ac3forge_level_meter_reset(ac3forge_level_meter_t* meter) {
    if (meter != nullptr) {
        meter->impl->reset();
    }
}

ac3forge_channel_level_t ac3forge_level_meter_level(const ac3forge_level_meter_t* meter,
                                                     size_t channel_index) {
    const ac3forge_channel_level_t floor{.peak_db = AC3FORGE_LEVEL_METER_FLOOR_DB,
                                         .hold_db = AC3FORGE_LEVEL_METER_FLOOR_DB,
                                         .rms_db = AC3FORGE_LEVEL_METER_FLOOR_DB,
                                         .clipped = 0};
    if (meter == nullptr) {
        return floor;
    }
    const auto levels = meter->impl->levels();
    if (channel_index >= levels.size()) {
        return floor;
    }
    const auto& level = levels[channel_index];
    return ac3forge_channel_level_t{.peak_db = level.peak_db,
                                    .hold_db = level.hold_db,
                                    .rms_db = level.rms_db,
                                    .clipped = level.clipped ? 1 : 0};
}

ac3forge_channel_summary_t ac3forge_level_meter_summary(const ac3forge_level_meter_t* meter,
                                                         size_t channel_index) {
    const ac3forge_channel_summary_t empty{.peak = 0.0,
                                           .rms = 0.0,
                                           .peak_db = AC3FORGE_LEVEL_METER_FLOOR_DB,
                                           .rms_db = AC3FORGE_LEVEL_METER_FLOOR_DB,
                                           .samples = 0,
                                           .clipped_samples = 0};
    if (meter == nullptr) {
        return empty;
    }
    const auto summary = meter->impl->summary();
    if (channel_index >= summary.size()) {
        return empty;
    }
    const auto& s = summary[channel_index];
    return ac3forge_channel_summary_t{.peak = s.peak,
                                      .rms = s.rms(),
                                      .peak_db = s.peak_db(),
                                      .rms_db = s.rms_db(),
                                      .samples = s.samples,
                                      .clipped_samples = s.clipped_samples};
}

}  // extern "C"
