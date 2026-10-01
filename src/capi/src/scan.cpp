#include <memory>
#include <span>

#include "internal.hpp"

using iclforge_c::guard;

extern "C" {

iclforge_status_t iclforge_scan(const uint8_t* stream, size_t stream_size,
                                 iclforge_scanned_stream_t** out_stream) {
    if (stream == nullptr || out_stream == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&stream, &stream_size, &out_stream]() -> iclforge_status_t {
        auto result =
            iclforge::ac3::io::scan(std::as_bytes(std::span<const uint8_t>(stream, stream_size)));
        if (!result) {
            return iclforge_c::from_cpp(result.error());
        }
        auto owned = std::make_unique<iclforge_scanned_stream>();
        owned->data = std::move(*result);

        // Every span in the scan result points into `stream` - see
        // iclforge_scanned_stream's own comment on why these are converted
        // to offset/length pairs once, here, rather than kept as pointers.
        const auto* base = reinterpret_cast<const std::byte*>(static_cast<const void*>(stream));
        owned->access_units.reserve(owned->data.access_units.size());
        for (const auto& span : owned->data.access_units) {
            owned->access_units.push_back(
                iclforge_span_t{static_cast<size_t>(span.data() - base), span.size()});
        }
        owned->programme_access_units.reserve(owned->data.programmes.size());
        for (const auto& programme : owned->data.programmes) {
            std::vector<iclforge_span_t> spans;
            spans.reserve(programme.access_units.size());
            for (const auto& span : programme.access_units) {
                spans.push_back(
                    iclforge_span_t{static_cast<size_t>(span.data() - base), span.size()});
            }
            owned->programme_access_units.push_back(std::move(spans));
        }

        *out_stream = owned.release();
        return ICLFORGE_OK;
    });
}

iclforge_stream_kind_t iclforge_scanned_stream_kind(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? ICLFORGE_STREAM_KIND_AC3
                              : static_cast<iclforge_stream_kind_t>(stream->data.kind);
}

iclforge_sample_rate_t iclforge_scanned_stream_sample_rate(
    const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? ICLFORGE_SAMPLE_RATE_48000
                              : iclforge_c::from_cpp(stream->data.sample_rate);
}

iclforge_acmod_t iclforge_scanned_stream_acmod(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? ICLFORGE_ACMOD_2_0 : iclforge_c::from_cpp(stream->data.acmod);
}

int iclforge_scanned_stream_lfe(const iclforge_scanned_stream_t* stream) {
    return stream != nullptr && stream->data.lfe ? 1 : 0;
}

int iclforge_scanned_stream_channels(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? 0 : stream->data.channels;
}

size_t iclforge_scanned_stream_access_unit_count(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? 0 : stream->access_units.size();
}

iclforge_span_t iclforge_scanned_stream_access_unit(const iclforge_scanned_stream_t* stream,
                                                     size_t index) {
    if (stream == nullptr || index >= stream->access_units.size()) {
        return iclforge_span_t{0, 0};
    }
    return stream->access_units[index];
}

uint32_t iclforge_scanned_stream_access_unit_samples(const iclforge_scanned_stream_t* stream,
                                                      size_t index) {
    if (stream == nullptr || index >= stream->data.access_unit_samples.size()) {
        return 0;
    }
    return stream->data.access_unit_samples[index];
}

size_t iclforge_scanned_stream_substreams_per_unit(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? 0 : stream->data.substreams_per_unit;
}

int iclforge_scanned_stream_bsid(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? 0 : stream->data.bsid;
}

int iclforge_scanned_stream_bsmod(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? 0 : stream->data.bsmod;
}

int iclforge_scanned_stream_bit_rate_code(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? 0 : stream->data.bit_rate_code;
}

int iclforge_scanned_stream_has_oba_complexity_index(const iclforge_scanned_stream_t* stream) {
    return stream != nullptr && stream->data.oba_complexity_index.has_value() ? 1 : 0;
}

int iclforge_scanned_stream_oba_complexity_index(const iclforge_scanned_stream_t* stream) {
    return stream != nullptr && stream->data.oba_complexity_index.has_value()
               ? *stream->data.oba_complexity_index
               : 0;
}

int iclforge_scanned_stream_bsmod_present(const iclforge_scanned_stream_t* stream) {
    return stream != nullptr && stream->data.bsmod_present ? 1 : 0;
}

int iclforge_scanned_stream_dsurmod(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? 0 : stream->data.dsurmod;
}

int iclforge_scanned_stream_mix_metadata(const iclforge_scanned_stream_t* stream) {
    return stream != nullptr && stream->data.mix_metadata ? 1 : 0;
}

uint8_t iclforge_scanned_stream_independent_substreams(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? 0 : stream->data.independent_substreams;
}

uint16_t iclforge_scanned_stream_channel_map(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? 0 : stream->data.channel_map;
}

namespace {
const iclforge::ac3::io::SubstreamService* associated_substream(
    const iclforge_scanned_stream_t* stream, int index) {
    if (stream == nullptr || index < 0 ||
        static_cast<size_t>(index) >= stream->data.associated_substreams.size()) {
        return nullptr;
    }
    return &stream->data.associated_substreams[static_cast<size_t>(index)];
}
}  // namespace

int iclforge_scanned_stream_associated_substream_present(const iclforge_scanned_stream_t* stream,
                                                           int index) {
    const auto* service = associated_substream(stream, index);
    return service != nullptr && service->present ? 1 : 0;
}

int iclforge_scanned_stream_associated_substream_bsmod(const iclforge_scanned_stream_t* stream,
                                                         int index) {
    const auto* service = associated_substream(stream, index);
    return service == nullptr ? 0 : service->bsmod;
}

int iclforge_scanned_stream_associated_substream_bsmod_present(
    const iclforge_scanned_stream_t* stream, int index) {
    const auto* service = associated_substream(stream, index);
    return service != nullptr && service->bsmod_present ? 1 : 0;
}

iclforge_acmod_t iclforge_scanned_stream_associated_substream_acmod(
    const iclforge_scanned_stream_t* stream, int index) {
    const auto* service = associated_substream(stream, index);
    return service == nullptr ? ICLFORGE_ACMOD_2_0 : iclforge_c::from_cpp(service->acmod);
}

int iclforge_scanned_stream_associated_substream_lfe(const iclforge_scanned_stream_t* stream,
                                                       int index) {
    const auto* service = associated_substream(stream, index);
    return service != nullptr && service->lfe ? 1 : 0;
}

int iclforge_scanned_stream_associated_substream_mix_metadata(
    const iclforge_scanned_stream_t* stream, int index) {
    const auto* service = associated_substream(stream, index);
    return service != nullptr && service->mix_metadata ? 1 : 0;
}

namespace {
const iclforge::ac3::io::ScannedProgramme* scanned_programme(
    const iclforge_scanned_stream_t* stream, size_t programme_index) {
    if (stream == nullptr || programme_index >= stream->data.programmes.size()) {
        return nullptr;
    }
    return &stream->data.programmes[programme_index];
}
}  // namespace

size_t iclforge_scanned_stream_programme_count(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? 0 : stream->data.programmes.size();
}

int iclforge_scanned_stream_programme_substream_id(const iclforge_scanned_stream_t* stream,
                                                     size_t programme_index) {
    const auto* programme = scanned_programme(stream, programme_index);
    return programme == nullptr ? 0 : programme->substreamid;
}

iclforge_acmod_t iclforge_scanned_stream_programme_acmod(const iclforge_scanned_stream_t* stream,
                                                          size_t programme_index) {
    const auto* programme = scanned_programme(stream, programme_index);
    return programme == nullptr ? ICLFORGE_ACMOD_2_0 : iclforge_c::from_cpp(programme->acmod);
}

int iclforge_scanned_stream_programme_lfe(const iclforge_scanned_stream_t* stream,
                                           size_t programme_index) {
    const auto* programme = scanned_programme(stream, programme_index);
    return programme != nullptr && programme->lfe ? 1 : 0;
}

int iclforge_scanned_stream_programme_channels(const iclforge_scanned_stream_t* stream,
                                                size_t programme_index) {
    const auto* programme = scanned_programme(stream, programme_index);
    return programme == nullptr ? 0 : programme->channels;
}

int iclforge_scanned_stream_programme_bsid(const iclforge_scanned_stream_t* stream,
                                            size_t programme_index) {
    const auto* programme = scanned_programme(stream, programme_index);
    return programme == nullptr ? 0 : programme->bsid;
}

int iclforge_scanned_stream_programme_bsmod(const iclforge_scanned_stream_t* stream,
                                             size_t programme_index) {
    const auto* programme = scanned_programme(stream, programme_index);
    return programme == nullptr ? 0 : programme->bsmod;
}

size_t iclforge_scanned_stream_programme_substreams_per_unit(
    const iclforge_scanned_stream_t* stream, size_t programme_index) {
    const auto* programme = scanned_programme(stream, programme_index);
    return programme == nullptr ? 0 : programme->substreams_per_unit;
}

int iclforge_scanned_stream_programme_has_oba_complexity_index(
    const iclforge_scanned_stream_t* stream, size_t programme_index) {
    const auto* programme = scanned_programme(stream, programme_index);
    return programme != nullptr && programme->oba_complexity_index.has_value() ? 1 : 0;
}

int iclforge_scanned_stream_programme_oba_complexity_index(
    const iclforge_scanned_stream_t* stream, size_t programme_index) {
    const auto* programme = scanned_programme(stream, programme_index);
    return programme != nullptr && programme->oba_complexity_index.has_value()
               ? *programme->oba_complexity_index
               : 0;
}

size_t iclforge_scanned_stream_programme_access_unit_count(const iclforge_scanned_stream_t* stream,
                                                            size_t programme_index) {
    if (stream == nullptr || programme_index >= stream->programme_access_units.size()) {
        return 0;
    }
    return stream->programme_access_units[programme_index].size();
}

iclforge_span_t iclforge_scanned_stream_programme_access_unit(
    const iclforge_scanned_stream_t* stream, size_t programme_index, size_t au_index) {
    if (stream == nullptr || programme_index >= stream->programme_access_units.size()) {
        return iclforge_span_t{0, 0};
    }
    const auto& spans = stream->programme_access_units[programme_index];
    if (au_index >= spans.size()) {
        return iclforge_span_t{0, 0};
    }
    return spans[au_index];
}

void iclforge_scanned_stream_destroy(iclforge_scanned_stream_t* stream) { delete stream; }

int iclforge_scanned_stream_access_unit_timing(const iclforge_scanned_stream_t* stream,
                                                size_t index, uint64_t* out_start_sample,
                                                uint32_t* out_duration_samples,
                                                uint32_t* out_sample_rate) {
    if (stream == nullptr) {
        return 0;
    }
    const auto timing = iclforge::ac3::io::access_unit_timing(stream->data, index);
    if (!timing.has_value()) {
        return 0;
    }
    if (out_start_sample != nullptr) *out_start_sample = timing->start_sample;
    if (out_duration_samples != nullptr) *out_duration_samples = timing->duration_samples;
    if (out_sample_rate != nullptr) *out_sample_rate = timing->sample_rate;
    return 1;
}

uint64_t iclforge_scanned_stream_duration_samples(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? 0 : iclforge::ac3::io::stream_duration_samples(stream->data);
}

double iclforge_scanned_stream_duration_seconds(const iclforge_scanned_stream_t* stream) {
    return stream == nullptr ? 0.0 : iclforge::ac3::io::stream_duration_seconds(stream->data);
}

int iclforge_scanned_stream_access_unit_at_sample(const iclforge_scanned_stream_t* stream,
                                                   uint64_t sample, size_t* out_index) {
    if (stream == nullptr) {
        return 0;
    }
    const auto index = iclforge::ac3::io::access_unit_at_sample(stream->data, sample);
    if (!index.has_value()) {
        return 0;
    }
    if (out_index != nullptr) *out_index = *index;
    return 1;
}

int iclforge_scanned_stream_access_unit_at_seconds(const iclforge_scanned_stream_t* stream,
                                                    double seconds, size_t* out_index) {
    if (stream == nullptr) {
        return 0;
    }
    const auto index = iclforge::ac3::io::access_unit_at_seconds(stream->data, seconds);
    if (!index.has_value()) {
        return 0;
    }
    if (out_index != nullptr) *out_index = *index;
    return 1;
}

int iclforge_scanned_stream_uniform_access_unit_samples(const iclforge_scanned_stream_t* stream,
                                                          uint32_t* out_samples) {
    if (stream == nullptr) {
        return 0;
    }
    const auto samples = iclforge::ac3::io::uniform_access_unit_samples(stream->data);
    if (!samples.has_value()) {
        return 0;
    }
    if (out_samples != nullptr) *out_samples = *samples;
    return 1;
}

}  // extern "C"
