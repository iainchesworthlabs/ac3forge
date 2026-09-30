#include <memory>
#include <span>

#include "internal.hpp"

using iclforge_c::guard;

static_assert(static_cast<int>(iclforge::eac3::chanmap::Location::kLeft) == ICLFORGE_LOCATION_L);
static_assert(static_cast<int>(iclforge::eac3::chanmap::Location::kLfe) == ICLFORGE_LOCATION_LFE);
static_assert(static_cast<int>(iclforge::eac3::chanmap::kMaxChannels) == 22);

static_assert(iclforge::oba::bed::kLR == ICLFORGE_BED_LR);
static_assert(iclforge::oba::bed::kLfe2 == ICLFORGE_BED_LFE2);

extern "C" {

iclforge_status_t iclforge_eac3_decoder_create(const iclforge_decoder_config_t* config,
                                                iclforge_eac3_decoder_t** out_decoder) {
    if (config == nullptr || out_decoder == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&config, &out_decoder] {
        *out_decoder = new iclforge_eac3_decoder(
            iclforge::DecoderConfig{.drc_scale = config->drc_scale,
                               .heavy_compression = config->heavy_compression != 0});
        return ICLFORGE_OK;
    });
}

void iclforge_eac3_decoder_destroy(iclforge_eac3_decoder_t* decoder) { delete decoder; }

iclforge_status_t iclforge_eac3_decoder_decode_substream(
    iclforge_eac3_decoder_t* decoder, const uint8_t* frame, size_t frame_size,
    iclforge_decoded_substream_t** out_substream) {
    if (decoder == nullptr || frame == nullptr || out_substream == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&decoder, &frame, &frame_size, &out_substream]() -> iclforge_status_t {
        auto result = decoder->impl.decode_substream(
            std::as_bytes(std::span<const uint8_t>(frame, frame_size)));
        if (!result.has_value()) {
            return iclforge_c::from_cpp(result.error());
        }
        if (!result->has_value()) {
            *out_substream = nullptr;
            return ICLFORGE_OK;
        }
        auto owned = std::make_unique<iclforge_decoded_substream>();
        owned->data = std::move(**result);
        *out_substream = owned.release();
        return ICLFORGE_OK;
    });
}

iclforge_status_t iclforge_eac3_decoder_decode_access_unit(
    iclforge_eac3_decoder_t* decoder, const uint8_t* unit, size_t unit_size,
    iclforge_decoded_access_unit_t** out_unit) {
    if (decoder == nullptr || unit == nullptr || out_unit == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&decoder, &unit, &unit_size, &out_unit]() -> iclforge_status_t {
        auto result = decoder->impl.decode_access_unit(
            std::as_bytes(std::span<const uint8_t>(unit, unit_size)));
        if (!result.has_value()) {
            return iclforge_c::from_cpp(result.error());
        }
        if (!result->has_value()) {
            *out_unit = nullptr;
            return ICLFORGE_OK;
        }
        auto owned = std::make_unique<iclforge_decoded_access_unit>();
        owned->data = std::move(**result);
        *out_unit = owned.release();
        return ICLFORGE_OK;
    });
}

iclforge_status_t iclforge_eac3_decoder_decode_access_unit_into(
    iclforge_eac3_decoder_t* decoder, const uint8_t* unit, size_t unit_size,
    float* const* channels, size_t channel_count, size_t samples_per_channel,
    iclforge_decoded_access_unit_t** out_unit) {
    if (decoder == nullptr || unit == nullptr || channels == nullptr || out_unit == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    if (channel_count != ICLFORGE_EAC3_DECODER_MAX_CHANNELS ||
        samples_per_channel != ICLFORGE_SAMPLES_PER_FRAME) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&decoder, &unit, &unit_size, &channels, &channel_count, &samples_per_channel,
                  &out_unit]() -> iclforge_status_t {
        std::vector<std::span<float>> spans;
        spans.reserve(channel_count);
        for (size_t i = 0; i < channel_count; ++i) {
            if (channels[i] == nullptr) {
                return ICLFORGE_ERROR_INVALID_ARGUMENT;
            }
            spans.emplace_back(channels[i], samples_per_channel);
        }
        auto result = decoder->impl.decode_access_unit_into(
            std::as_bytes(std::span<const uint8_t>(unit, unit_size)), spans);
        if (!result.has_value()) {
            return iclforge_c::from_cpp(result.error());
        }
        if (!result->has_value()) {
            *out_unit = nullptr;
            return ICLFORGE_OK;
        }
        auto owned = std::make_unique<iclforge_decoded_access_unit>();
        owned->data = std::move(**result);
        *out_unit = owned.release();
        return ICLFORGE_OK;
    });
}

iclforge_status_t iclforge_eac3_decoder_flush(iclforge_eac3_decoder_t* decoder,
                                               iclforge_decoded_substream_t*** out_substreams,
                                               size_t* out_count) {
    if (decoder == nullptr || out_substreams == nullptr || out_count == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&decoder, &out_substreams, &out_count] {
        std::vector<iclforge::DecodedSubstream> flushed = decoder->impl.flush();
        if (flushed.empty()) {
            *out_substreams = nullptr;
            *out_count = 0;
            return ICLFORGE_OK;
        }
        auto array = std::make_unique<iclforge_decoded_substream*[]>(flushed.size());
        for (size_t i = 0; i < flushed.size(); ++i) {
            auto owned = std::make_unique<iclforge_decoded_substream>();
            owned->data = std::move(flushed[i]);
            array[i] = owned.release();
        }
        *out_count = flushed.size();
        *out_substreams = array.release();
        return ICLFORGE_OK;
    });
}

void iclforge_decoded_substream_array_destroy(iclforge_decoded_substream_t** substreams,
                                               size_t count) {
    if (substreams == nullptr) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        delete substreams[i];
    }
    delete[] substreams;
}

// --- DecodedSubstream accessors -----------------------------------------

int iclforge_decoded_substream_is_independent(const iclforge_decoded_substream_t* substream) {
    // kIndependent and kConvertible both begin an access unit and decode
    // alone; only kDependent extends a preceding one (see StreamType's own
    // comment, ac3/core/eac3_tables.hpp).
    return substream != nullptr && substream->data.strmtyp != iclforge::eac3::StreamType::kDependent
               ? 1
               : 0;
}

int iclforge_decoded_substream_id(const iclforge_decoded_substream_t* substream) {
    return substream == nullptr ? 0 : substream->data.substreamid;
}

iclforge_sample_rate_t iclforge_decoded_substream_sample_rate(
    const iclforge_decoded_substream_t* substream) {
    return substream == nullptr ? ICLFORGE_SAMPLE_RATE_48000
                                 : iclforge_c::from_cpp(substream->data.sample_rate);
}

iclforge_acmod_t iclforge_decoded_substream_acmod(const iclforge_decoded_substream_t* substream) {
    return substream == nullptr ? ICLFORGE_ACMOD_2_0 : iclforge_c::from_cpp(substream->data.acmod);
}

int iclforge_decoded_substream_lfe(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.lfe ? 1 : 0;
}

int iclforge_decoded_substream_dialnorm(const iclforge_decoded_substream_t* substream) {
    return substream == nullptr ? 0 : substream->data.dialnorm;
}

int iclforge_decoded_substream_has_compr(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.compr.has_value() ? 1 : 0;
}

uint8_t iclforge_decoded_substream_compr(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.compr.has_value() ? *substream->data.compr : 0;
}

uint8_t iclforge_decoded_substream_dynrng(const iclforge_decoded_substream_t* substream,
                                           int block_index) {
    if (substream == nullptr || block_index < 0 || block_index >= iclforge::kBlocksPerFrame) {
        return 0;
    }
    return substream->data.dynrng[static_cast<size_t>(block_index)];
}

int iclforge_decoded_substream_has_dialnorm2(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.dialnorm2.has_value() ? 1 : 0;
}

int iclforge_decoded_substream_dialnorm2(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.dialnorm2.has_value() ? *substream->data.dialnorm2
                                                                          : 0;
}

int iclforge_decoded_substream_has_compr2(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.compr2.has_value() ? 1 : 0;
}

uint8_t iclforge_decoded_substream_compr2(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.compr2.has_value() ? *substream->data.compr2 : 0;
}

uint8_t iclforge_decoded_substream_dynrng2(const iclforge_decoded_substream_t* substream,
                                            int block_index) {
    if (substream == nullptr || block_index < 0 || block_index >= iclforge::kBlocksPerFrame) {
        return 0;
    }
    return substream->data.dynrng2[static_cast<size_t>(block_index)];
}

int iclforge_decoded_substream_numblkscod(const iclforge_decoded_substream_t* substream) {
    return substream == nullptr ? 0 : substream->data.numblkscod;
}

int iclforge_decoded_substream_has_chanmap(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.chanmap.has_value() ? 1 : 0;
}

uint16_t iclforge_decoded_substream_chanmap(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.chanmap.has_value() ? *substream->data.chanmap
                                                                        : 0;
}

int iclforge_decoded_substream_last_dependent(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.last_dependent ? 1 : 0;
}

uint16_t iclforge_decoded_substream_location_map(const iclforge_decoded_substream_t* substream) {
    return substream == nullptr ? 0 : substream->data.location_map();
}

size_t iclforge_decoded_substream_channel_count(const iclforge_decoded_substream_t* substream) {
    return substream == nullptr ? 0 : substream->data.channels.size();
}

size_t iclforge_decoded_substream_samples_per_channel(const iclforge_decoded_substream_t*) {
    return iclforge::kSamplesPerFrame;
}

const float* iclforge_decoded_substream_channel_samples(
    const iclforge_decoded_substream_t* substream, size_t channel_index) {
    if (substream == nullptr || channel_index >= substream->data.channels.size()) {
        return nullptr;
    }
    return substream->data.channels[channel_index].data();
}

int iclforge_decoded_substream_block_switched(const iclforge_decoded_substream_t* substream,
                                               size_t channel_index, int block_index) {
    if (substream == nullptr || channel_index >= substream->data.blksw.size() || block_index < 0 ||
        block_index >= iclforge::kBlocksPerFrame) {
        return 0;
    }
    return substream->data.blksw[channel_index][static_cast<size_t>(block_index)] ? 1 : 0;
}

int iclforge_decoded_substream_has_object_metadata(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.object_metadata.has_value() ? 1 : 0;
}

int iclforge_decoded_substream_program_dynamic_only(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.object_metadata.has_value() &&
                   substream->data.object_metadata->program.dynamic_only
               ? 1
               : 0;
}

int iclforge_decoded_substream_program_lfe(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.object_metadata.has_value() &&
                   substream->data.object_metadata->program.lfe
               ? 1
               : 0;
}

uint16_t iclforge_decoded_substream_program_bed(const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.object_metadata.has_value()
               ? substream->data.object_metadata->program.bed
               : 0;
}

int iclforge_decoded_substream_program_dynamic_object_count(
    const iclforge_decoded_substream_t* substream) {
    return substream != nullptr && substream->data.object_metadata.has_value()
               ? substream->data.object_metadata->program.dynamic_objects
               : 0;
}

void iclforge_decoded_substream_dynamic_object(const iclforge_decoded_substream_t* substream,
                                                int object_index, double* out_x, double* out_y,
                                                double* out_z, double* out_gain_db) {
    if (out_x != nullptr) *out_x = 0.5;
    if (out_y != nullptr) *out_y = 0.5;
    if (out_z != nullptr) *out_z = 0.0;
    if (out_gain_db != nullptr) *out_gain_db = 0.0;
    if (substream == nullptr || !substream->data.object_metadata.has_value() || object_index < 0) {
        return;
    }
    const auto& objects = substream->data.object_metadata->objects;
    if (static_cast<size_t>(object_index) >= objects.size()) {
        return;
    }
    const auto& object = objects[static_cast<size_t>(object_index)];
    if (out_x != nullptr) *out_x = object.position.x;
    if (out_y != nullptr) *out_y = object.position.y;
    if (out_z != nullptr) *out_z = object.position.z;
    if (out_gain_db != nullptr) *out_gain_db = object.gain_db;
}

size_t iclforge_decoded_substream_object_audio_count(const iclforge_decoded_substream_t* substream) {
    return substream == nullptr ? 0 : substream->data.object_audio.size();
}

const float* iclforge_decoded_substream_object_audio(const iclforge_decoded_substream_t* substream,
                                                       size_t object_index) {
    if (substream == nullptr || object_index >= substream->data.object_audio.size()) {
        return nullptr;
    }
    return substream->data.object_audio[object_index].data();
}

void iclforge_decoded_substream_destroy(iclforge_decoded_substream_t* substream) {
    delete substream;
}

// --- DecodedAccessUnit accessors ----------------------------------------

iclforge_sample_rate_t iclforge_decoded_access_unit_sample_rate(
    const iclforge_decoded_access_unit_t* unit) {
    return unit == nullptr ? ICLFORGE_SAMPLE_RATE_48000 : iclforge_c::from_cpp(unit->data.sample_rate);
}

iclforge_acmod_t iclforge_decoded_access_unit_acmod(const iclforge_decoded_access_unit_t* unit) {
    return unit == nullptr ? ICLFORGE_ACMOD_2_0 : iclforge_c::from_cpp(unit->data.acmod);
}

int iclforge_decoded_access_unit_dialnorm(const iclforge_decoded_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.dialnorm;
}

int iclforge_decoded_access_unit_has_compr(const iclforge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.compr.has_value() ? 1 : 0;
}

uint8_t iclforge_decoded_access_unit_compr(const iclforge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.compr.has_value() ? *unit->data.compr : 0;
}

uint8_t iclforge_decoded_access_unit_dynrng(const iclforge_decoded_access_unit_t* unit,
                                             int block_index) {
    if (unit == nullptr || block_index < 0 || block_index >= iclforge::kBlocksPerFrame) {
        return 0;
    }
    return unit->data.dynrng[static_cast<size_t>(block_index)];
}

int iclforge_decoded_access_unit_has_dialnorm2(const iclforge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.dialnorm2.has_value() ? 1 : 0;
}

int iclforge_decoded_access_unit_dialnorm2(const iclforge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.dialnorm2.has_value() ? *unit->data.dialnorm2 : 0;
}

int iclforge_decoded_access_unit_numblkscod(const iclforge_decoded_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.numblkscod;
}

int iclforge_decoded_access_unit_substream_count(const iclforge_decoded_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.substream_count;
}

size_t iclforge_decoded_access_unit_channel_count(const iclforge_decoded_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.channels.size();
}

size_t iclforge_decoded_access_unit_samples_per_channel(const iclforge_decoded_access_unit_t*) {
    return iclforge::kSamplesPerFrame;
}

const float* iclforge_decoded_access_unit_channel_samples(const iclforge_decoded_access_unit_t* unit,
                                                            size_t channel_index) {
    if (unit == nullptr || channel_index >= unit->data.channels.size()) {
        return nullptr;
    }
    return unit->data.channels[channel_index].data();
}

size_t iclforge_decoded_access_unit_layout_count(const iclforge_decoded_access_unit_t* unit) {
    return unit == nullptr ? 0 : static_cast<size_t>(unit->data.layout.count);
}

iclforge_location_t iclforge_decoded_access_unit_layout_location(
    const iclforge_decoded_access_unit_t* unit, size_t index) {
    if (unit == nullptr || index >= static_cast<size_t>(unit->data.layout.count)) {
        return ICLFORGE_LOCATION_L;
    }
    return static_cast<iclforge_location_t>(unit->data.layout[static_cast<int>(index)]);
}

int iclforge_decoded_access_unit_has_object_metadata(const iclforge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.object_metadata.has_value() ? 1 : 0;
}

int iclforge_decoded_access_unit_program_dynamic_only(const iclforge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.object_metadata.has_value() &&
                   unit->data.object_metadata->program.dynamic_only
               ? 1
               : 0;
}

int iclforge_decoded_access_unit_program_lfe(const iclforge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.object_metadata.has_value() &&
                   unit->data.object_metadata->program.lfe
               ? 1
               : 0;
}

uint16_t iclforge_decoded_access_unit_program_bed(const iclforge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.object_metadata.has_value()
               ? unit->data.object_metadata->program.bed
               : 0;
}

int iclforge_decoded_access_unit_program_dynamic_object_count(
    const iclforge_decoded_access_unit_t* unit) {
    return unit != nullptr && unit->data.object_metadata.has_value()
               ? unit->data.object_metadata->program.dynamic_objects
               : 0;
}

void iclforge_decoded_access_unit_dynamic_object(const iclforge_decoded_access_unit_t* unit,
                                                  int object_index, double* out_x, double* out_y,
                                                  double* out_z, double* out_gain_db) {
    if (out_x != nullptr) *out_x = 0.5;
    if (out_y != nullptr) *out_y = 0.5;
    if (out_z != nullptr) *out_z = 0.0;
    if (out_gain_db != nullptr) *out_gain_db = 0.0;
    if (unit == nullptr || !unit->data.object_metadata.has_value() || object_index < 0) {
        return;
    }
    const auto& objects = unit->data.object_metadata->objects;
    if (static_cast<size_t>(object_index) >= objects.size()) {
        return;
    }
    const auto& object = objects[static_cast<size_t>(object_index)];
    if (out_x != nullptr) *out_x = object.position.x;
    if (out_y != nullptr) *out_y = object.position.y;
    if (out_z != nullptr) *out_z = object.position.z;
    if (out_gain_db != nullptr) *out_gain_db = object.gain_db;
}

size_t iclforge_decoded_access_unit_object_audio_count(const iclforge_decoded_access_unit_t* unit) {
    return unit == nullptr ? 0 : unit->data.object_audio.size();
}

const float* iclforge_decoded_access_unit_object_audio(const iclforge_decoded_access_unit_t* unit,
                                                         size_t object_index) {
    if (unit == nullptr || object_index >= unit->data.object_audio.size()) {
        return nullptr;
    }
    return unit->data.object_audio[object_index].data();
}

void iclforge_decoded_access_unit_destroy(iclforge_decoded_access_unit_t* unit) { delete unit; }

// --- stream framing helpers ----------------------------------------------

size_t iclforge_spans_count(const iclforge_spans_t* spans) {
    return spans == nullptr ? 0 : spans->items.size();
}

iclforge_span_t iclforge_spans_get(const iclforge_spans_t* spans, size_t index) {
    if (spans == nullptr || index >= spans->items.size()) {
        return iclforge_span_t{0, 0};
    }
    return spans->items[index];
}

void iclforge_spans_destroy(iclforge_spans_t* spans) { delete spans; }

namespace {

iclforge_status_t split_into_spans(const uint8_t* stream, size_t stream_size,
                                    iclforge_spans_t** out_spans, bool access_units) {
    if (stream == nullptr || out_spans == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&stream, &stream_size, &out_spans, &access_units]() -> iclforge_status_t {
        const auto bytes = std::as_bytes(std::span<const uint8_t>(stream, stream_size));
        auto result =
            access_units ? iclforge::split_access_units(bytes) : iclforge::split_frames(bytes);
        if (!result.has_value()) {
            return iclforge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<iclforge_spans>();
        owned->items.reserve(result->size());
        for (const auto& span : *result) {
            const auto offset = static_cast<size_t>(span.data() - reinterpret_cast<const std::byte*>(
                                                                        static_cast<const void*>(stream)));
            owned->items.push_back(iclforge_span_t{offset, span.size()});
        }
        *out_spans = owned.release();
        return ICLFORGE_OK;
    });
}

}  // namespace

iclforge_status_t iclforge_split_frames(const uint8_t* stream, size_t stream_size,
                                         iclforge_spans_t** out_spans) {
    return split_into_spans(stream, stream_size, out_spans, /*access_units=*/false);
}

iclforge_status_t iclforge_split_access_units(const uint8_t* stream, size_t stream_size,
                                               iclforge_spans_t** out_spans) {
    return split_into_spans(stream, stream_size, out_spans, /*access_units=*/true);
}

iclforge_status_t iclforge_stream_bsid(const uint8_t* frame, size_t frame_size, int* out_bsid) {
    if (frame == nullptr || out_bsid == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&frame, &frame_size, &out_bsid]() -> iclforge_status_t {
        auto result =
            iclforge::stream_bsid(std::as_bytes(std::span<const uint8_t>(frame, frame_size)));
        if (!result.has_value()) {
            return iclforge_c::from_cpp(result.error());
        }
        *out_bsid = *result;
        return ICLFORGE_OK;
    });
}

int iclforge_eac3_decoder_latency_samples(const iclforge_eac3_decoder_t* decoder) {
    return decoder == nullptr ? 0 : decoder->impl.latency_samples();
}

}  // extern "C"
