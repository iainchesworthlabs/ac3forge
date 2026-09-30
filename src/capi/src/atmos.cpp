#include <memory>
#include <span>
#include <vector>

#include "iclforge/ac3/oba/joc_tables.hpp"
#include "internal.hpp"

using iclforge_c::guard;

// Kept outside extern "C" below - see encoder.cpp's identical comment on
// -Wreturn-type-c-linkage.
namespace {
iclforge::oba::AtmosConfig atmos_config_to_cpp(const iclforge_atmos_config_t& config) {
    return iclforge::oba::AtmosConfig{.sample_rate = iclforge_c::to_cpp(config.sample_rate),
                                  .bitrate_kbps = config.bitrate_kbps,
                                  .dialnorm = config.dialnorm,
                                  .num_bands_idx = config.num_bands_idx,
                                  .fine_quant = config.fine_quant != 0,
                                  .emit_object_metadata = config.emit_object_metadata != 0,
                                  .fast_mdct = config.fast_mdct != 0};
}
}  // namespace

extern "C" {

void iclforge_atmos_config_init(iclforge_atmos_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const iclforge::oba::AtmosConfig defaults{};
    *config = iclforge_atmos_config_t{.sample_rate = iclforge_c::from_cpp(defaults.sample_rate),
                                       .bitrate_kbps = defaults.bitrate_kbps,
                                       .dialnorm = defaults.dialnorm,
                                       .num_bands_idx = defaults.num_bands_idx,
                                       .fine_quant = defaults.fine_quant ? 1 : 0,
                                       .emit_object_metadata = defaults.emit_object_metadata ? 1 : 0,
                                       .fast_mdct = defaults.fast_mdct ? 1 : 0};
}

void iclforge_object_placement_init(iclforge_object_placement_t* placement) {
    if (placement == nullptr) {
        return;
    }
    const iclforge::oba::ObjectPlacement defaults{};
    *placement = iclforge_object_placement_t{.x = defaults.position.x,
                                              .y = defaults.position.y,
                                              .z = defaults.position.z,
                                              .gain = defaults.gain,
                                              .lfe_send = defaults.lfe_send};
}

iclforge_status_t iclforge_atmos_encoder_create(const iclforge_atmos_config_t* config,
                                                 int object_count,
                                                 iclforge_atmos_encoder_t** out_encoder) {
    if (config == nullptr || out_encoder == nullptr || object_count < 0) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    // num_bands_idx indexes joc::kNumBands (Table 50) and kSubbandToBand
    // directly, starting in iclforge::oba::AtmosEncoder's own constructor - which
    // cannot report a failure - so an index outside the table is refused
    // before the encoder is built, not after it has already read past it.
    if (config->num_bands_idx < 0 ||
        config->num_bands_idx >= static_cast<int>(iclforge::oba::joc::kNumBands.size())) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&config, &object_count, &out_encoder] {
        *out_encoder = new iclforge_atmos_encoder(atmos_config_to_cpp(*config), object_count);
        return ICLFORGE_OK;
    });
}

void iclforge_atmos_encoder_destroy(iclforge_atmos_encoder_t* encoder) { delete encoder; }

int iclforge_atmos_encoder_dynamic_object_count(const iclforge_atmos_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.dynamic_object_count();
}

iclforge_status_t iclforge_atmos_encoder_encode_frame(
    iclforge_atmos_encoder_t* encoder, const float* const* objects, size_t object_count,
    size_t samples_per_object, const iclforge_object_placement_t* placements,
    size_t placement_count, iclforge_bytes_t** out_unit) {
    if (encoder == nullptr || out_unit == nullptr ||
        (object_count > 0 && (objects == nullptr || placements == nullptr))) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    if (object_count != static_cast<size_t>(iclforge_atmos_encoder_dynamic_object_count(encoder)) ||
        placement_count != object_count || samples_per_object != iclforge::kSamplesPerFrame) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&encoder, &objects, &object_count, &samples_per_object, &placements,
                  &placement_count, &out_unit]() -> iclforge_status_t {
        std::vector<std::span<const float>> object_spans;
        object_spans.reserve(object_count);
        for (size_t i = 0; i < object_count; ++i) {
            if (objects[i] == nullptr) {
                return ICLFORGE_ERROR_INVALID_ARGUMENT;
            }
            object_spans.emplace_back(objects[i], samples_per_object);
        }
        std::vector<iclforge::oba::ObjectPlacement> placement_values;
        placement_values.reserve(placement_count);
        for (size_t i = 0; i < placement_count; ++i) {
            const auto& p = placements[i];
            placement_values.push_back(iclforge::oba::ObjectPlacement{
                .position = iclforge::oba::Position{.x = p.x, .y = p.y, .z = p.z},
                .gain = p.gain,
                .lfe_send = p.lfe_send});
        }
        auto result = encoder->impl.encode_frame(object_spans, placement_values);
        if (!result) {
            return iclforge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<iclforge_bytes>();
        owned->data = std::move(result->bytes);
        *out_unit = owned.release();
        return ICLFORGE_OK;
    });
}

void iclforge_atmos_encoder_latency(const iclforge_atmos_encoder_t* encoder,
                                   iclforge_latency_t* out_latency) {
    if (encoder == nullptr || out_latency == nullptr) {
        return;
    }
    *out_latency = iclforge_c::from_cpp(encoder->impl.latency());
}

int iclforge_atmos_encoder_latency_samples(const iclforge_atmos_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.latency_samples();
}

void iclforge_atmos_encoder_bed_latency(const iclforge_atmos_encoder_t* encoder,
                                        iclforge_latency_t* out_latency) {
    if (encoder == nullptr || out_latency == nullptr) {
        return;
    }
    *out_latency = iclforge_c::from_cpp(encoder->impl.bed_latency());
}

}  // extern "C"
