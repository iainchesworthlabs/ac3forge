#include <memory>
#include <span>
#include <vector>

#include "internal.hpp"

using iclforge_c::guard;
using iclforge_c::to_cpp;

namespace {
iclforge::ac3::analysis::MeterBallistics ballistics_to_cpp(const iclforge_level_meter_ballistics_t* ballistics) {
    if (ballistics == nullptr) {
        return iclforge::ac3::analysis::MeterBallistics{};
    }
    return iclforge::ac3::analysis::MeterBallistics{
        .rms_integration_ms = ballistics->rms_integration_ms,
        .peak_decay_db_per_s = ballistics->peak_decay_db_per_s,
        .peak_hold_ms = ballistics->peak_hold_ms};
}
}  // namespace

extern "C" {

void iclforge_level_meter_ballistics_init(iclforge_level_meter_ballistics_t* ballistics) {
    if (ballistics == nullptr) {
        return;
    }
    const iclforge::ac3::analysis::MeterBallistics defaults{};
    *ballistics = iclforge_level_meter_ballistics_t{.rms_integration_ms = defaults.rms_integration_ms,
                                                     .peak_decay_db_per_s = defaults.peak_decay_db_per_s,
                                                     .peak_hold_ms = defaults.peak_hold_ms};
}

iclforge_status_t iclforge_level_meter_create(iclforge_acmod_t acmod, int lfe,
                                               uint32_t sample_rate, int channels,
                                               const iclforge_level_meter_ballistics_t* ballistics,
                                               iclforge_level_meter_t** out_meter) {
    if (out_meter == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&acmod, &lfe, &sample_rate, &channels, &ballistics, &out_meter] {
        const auto cpp_ballistics = ballistics_to_cpp(ballistics);
        auto owned = std::make_unique<iclforge_level_meter>();
        if (channels > 0) {
            owned->impl = std::make_unique<iclforge::ac3::analysis::LevelMeter>(
                to_cpp(acmod), lfe != 0, sample_rate, channels, cpp_ballistics);
        } else {
            owned->impl = std::make_unique<iclforge::ac3::analysis::LevelMeter>(
                to_cpp(acmod), lfe != 0, sample_rate, cpp_ballistics);
        }
        *out_meter = owned.release();
        return ICLFORGE_OK;
    });
}

void iclforge_level_meter_destroy(iclforge_level_meter_t* meter) { delete meter; }

iclforge_acmod_t iclforge_level_meter_acmod(const iclforge_level_meter_t* meter) {
    return meter == nullptr ? ICLFORGE_ACMOD_2_0 : iclforge_c::from_cpp(meter->impl->acmod());
}

int iclforge_level_meter_lfe(const iclforge_level_meter_t* meter) {
    return meter != nullptr && meter->impl->lfe() ? 1 : 0;
}

int iclforge_level_meter_channel_count(const iclforge_level_meter_t* meter) {
    return meter == nullptr ? 0 : meter->impl->channel_count();
}

uint32_t iclforge_level_meter_sample_rate(const iclforge_level_meter_t* meter) {
    return meter == nullptr ? 0 : meter->impl->sample_rate();
}

iclforge_status_t iclforge_level_meter_process(iclforge_level_meter_t* meter,
                                                const float* const* channels, size_t channel_count,
                                                size_t samples_per_channel) {
    if (meter == nullptr || channels == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    // Fewer spans than the meter's channels is legal (the rest meter as
    // silence); more is not. Checked before the reserve() below, which would
    // otherwise size itself from an unchecked caller count - SIZE_MAX threw
    // std::length_error there - and before the loop reads past the array.
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
        meter->impl->process(spans);
        return ICLFORGE_OK;
    });
}

void iclforge_level_meter_reset(iclforge_level_meter_t* meter) {
    if (meter != nullptr) {
        meter->impl->reset();
    }
}

iclforge_channel_level_t iclforge_level_meter_level(const iclforge_level_meter_t* meter,
                                                     size_t channel_index) {
    const iclforge_channel_level_t floor{.peak_db = ICLFORGE_LEVEL_METER_FLOOR_DB,
                                         .hold_db = ICLFORGE_LEVEL_METER_FLOOR_DB,
                                         .rms_db = ICLFORGE_LEVEL_METER_FLOOR_DB,
                                         .clipped = 0};
    if (meter == nullptr) {
        return floor;
    }
    const auto levels = meter->impl->levels();
    if (channel_index >= levels.size()) {
        return floor;
    }
    const auto& level = levels[channel_index];
    return iclforge_channel_level_t{.peak_db = level.peak_db,
                                    .hold_db = level.hold_db,
                                    .rms_db = level.rms_db,
                                    .clipped = level.clipped ? 1 : 0};
}

iclforge_channel_summary_t iclforge_level_meter_summary(const iclforge_level_meter_t* meter,
                                                         size_t channel_index) {
    const iclforge_channel_summary_t empty{.peak = 0.0,
                                           .rms = 0.0,
                                           .peak_db = ICLFORGE_LEVEL_METER_FLOOR_DB,
                                           .rms_db = ICLFORGE_LEVEL_METER_FLOOR_DB,
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
    return iclforge_channel_summary_t{.peak = s.peak,
                                      .rms = s.rms(),
                                      .peak_db = s.peak_db(),
                                      .rms_db = s.rms_db(),
                                      .samples = s.samples,
                                      .clipped_samples = s.clipped_samples};
}

}  // extern "C"
