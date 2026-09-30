// iclforge_ac4_encoder_* and the table-of-contents/sync-frame helpers - see
// iclforge.h's AC-4 section, iclforge::ac4::Encoder
// (src/ac4enc/include/iclforge/ac4enc/encoder.hpp) and ac4/ac4.hpp's carriage section.

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "internal.hpp"
#include "internal_ac4.hpp"

using iclforge_c::guard;
using iclforge_c::to_cpp;

// Kept outside extern "C": a C-linkage function returning a C++ class by
// value is diagnosed by Clang (-Wreturn-type-c-linkage) - see encoder.cpp's
// identical comment.
namespace {

// What iclforge_ac4_encoder_refusal_reason() says of a configuration that is
// not a valid argument, where iclforge_ac4_encoder_create() answers
// ICLFORGE_ERROR_INVALID_ARGUMENT instead of ICLFORGE_ERROR_AC4_ENCODE_INVALID_CONFIG.
constexpr const char* kNoConfiguration = "a NULL configuration";
constexpr const char* kNotAnArgument =
    "an array pointer that is NULL where the array has entries, or an enumerator outside its "
    "enumeration";
constexpr const char* kInternalError = "an exception reached the C boundary";

// The objects of `in`, as iclforge::ac4::ObjectsConfig. Only as many entries are read as
// the encoder's own limit lets it accept, one more than which it refuses by
// count first: a caller's absurd object_count then costs no more than 65.
std::expected<iclforge::ac4::ObjectsConfig, iclforge_status_t> objects_config_to_cpp(
    const iclforge_ac4_objects_config_t& in) {
    if ((in.object_count > 0 && in.objects == nullptr) || !iclforge_c::valid(in.coding) ||
        !iclforge_c::valid(in.downmix)) {
        return std::unexpected(ICLFORGE_ERROR_INVALID_ARGUMENT);
    }
    iclforge::ac4::ObjectsConfig out;
    const std::size_t count =
        std::min(in.object_count, static_cast<std::size_t>(ICLFORGE_AC4_MAX_OBJECTS) + 1);
    for (std::size_t i = 0; i < count; ++i) {
        const iclforge_ac4_object_config_t& object = in.objects[i];
        if (object.has_bed != 0 && !iclforge_c::valid(object.bed)) {
            return std::unexpected(ICLFORGE_ERROR_INVALID_ARGUMENT);
        }
        iclforge::ac4::ObjectConfig& converted = out.objects.emplace_back();
        if (object.has_bed != 0) {
            converted.bed = static_cast<iclforge::ac4::BedChannel>(object.bed);
        }
        converted.lfe = object.lfe != 0;
        converted.properties = iclforge_c::to_cpp(object.properties);
    }
    out.coding = to_cpp(in.coding);
    out.downmix = to_cpp(in.downmix);
    if (in.has_downmix_signals != 0) {
        out.downmix_signals = in.downmix_signals;
    }
    out.decorrelation = in.decorrelation != 0;
    if (in.has_parameter_bands != 0) {
        out.parameter_bands = in.parameter_bands;
    }
    if (in.has_coarse != 0) {
        out.coarse = in.coarse != 0;
    }
    if (in.has_screen_size_ratio_code != 0) {
        out.screen_size_ratio_code = in.screen_size_ratio_code;
    }
    out.bed_object_chan_distribute = in.bed_object_chan_distribute != 0;
    return out;
}

// `count` entries of `array` as a vector; nothing for a count of 0, whatever
// the pointer.
std::vector<std::int64_t> copy_of(const std::int64_t* array, std::size_t count) {
    std::vector<std::int64_t> out;
    if (count > 0) {
        const std::span<const std::int64_t> view(array, count);
        out.assign(view.begin(), view.end());
    }
    return out;
}

std::expected<iclforge::ac4::EncoderConfig, iclforge_status_t> encoder_config_to_cpp(
    const iclforge_ac4_encoder_config_t& config) {
    if ((config.iframe_count > 0 && config.iframes == nullptr) ||
        (config.fragment_start_count > 0 && config.fragment_starts == nullptr) ||
        !iclforge_c::valid(config.experimental.seven_x)) {
        return std::unexpected(ICLFORGE_ERROR_INVALID_ARGUMENT);
    }
    iclforge::ac4::EncoderConfig out;
    out.channels = config.channels;
    out.sample_rate_hz = config.sample_rate_hz;
    out.frame_rate_index = config.frame_rate_index;
    out.bitrate_kbps = config.bitrate_kbps;
    out.rate_mode = to_cpp(config.rate_mode);
    out.codec_mode = to_cpp(config.codec_mode);
    out.iframe_interval = config.iframe_interval;
    out.dialnorm_db = config.dialnorm_db;
    out.iframes = copy_of(config.iframes, config.iframe_count);
    out.fragment_starts = copy_of(config.fragment_starts, config.fragment_start_count);
    const iclforge_ac4_experimental_t& experimental = config.experimental;
    out.experimental.aspx_balance = experimental.aspx_balance != 0;
    out.experimental.aspx_varvar = experimental.aspx_varvar != 0;
    out.experimental.aspx_interleave = experimental.aspx_interleave != 0;
    out.experimental.coding_configs = experimental.coding_configs != 0;
    out.experimental.seven_x = to_cpp(experimental.seven_x);
    out.experimental.acpl = experimental.acpl != 0;
    out.experimental.back_pair = experimental.back_pair != 0;
    out.experimental.ajcc = experimental.ajcc != 0;
    out.experimental.objects = experimental.objects != 0;
    if (config.objects != nullptr) {
        auto objects = objects_config_to_cpp(*config.objects);
        if (!objects.has_value()) {
            return std::unexpected(objects.error());
        }
        // With substreams set, the substreams' own codec_mode is the one in
        // force (iclforge::ac4::EncoderConfig::substreams), and the object substream is
        // the stream's only one: the codec mode given is its, and the
        // stream's own stays at kAuto, as ac3cli's ac4-encode objects= leaves it.
        iclforge::ac4::SubstreamConfig substream;
        substream.codec_mode = out.codec_mode;
        out.codec_mode = iclforge::ac4::CodecMode::kAuto;
        substream.objects = std::move(*objects);
        out.substreams.push_back(std::move(substream));
    }
    return out;
}

// The channel spans of a C array, or the status that says the array is not
// one: `count` pointers, each to `samples` floats.
std::expected<std::vector<std::span<const float>>, iclforge_status_t> spans_of(
    const float* const* channels, std::size_t count, std::size_t samples) {
    std::vector<std::span<const float>> spans;
    spans.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (channels[i] == nullptr) {
            return std::unexpected(ICLFORGE_ERROR_INVALID_ARGUMENT);
        }
        spans.emplace_back(channels[i], samples);
    }
    return spans;
}

iclforge_status_t build_encoded_frame_array(std::vector<iclforge::ac4::EncodedFrame>&& frames,
                                             iclforge_ac4_encoded_frame_t*** out_frames,
                                             size_t* out_count) {
    if (frames.empty()) {
        *out_frames = nullptr;
        *out_count = 0;
        return ICLFORGE_OK;
    }
    auto array = std::make_unique<iclforge_ac4_encoded_frame*[]>(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        auto owned = std::make_unique<iclforge_ac4_encoded_frame>();
        owned->data = std::move(frames[i]);
        array[i] = owned.release();
    }
    *out_count = frames.size();
    *out_frames = array.release();
    return ICLFORGE_OK;
}

}  // namespace

extern "C" {

void iclforge_ac4_object_config_init(iclforge_ac4_object_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const iclforge::ac4::ObjectConfig defaults{};
    *config = iclforge_ac4_object_config_t{.has_bed = defaults.bed.has_value() ? 1 : 0,
                                           .bed = defaults.bed.has_value()
                                                      ? iclforge_c::from_cpp(*defaults.bed)
                                                      : ICLFORGE_AC4_BED_LEFT,
                                           .lfe = defaults.lfe ? 1 : 0,
                                           .properties = iclforge_c::from_cpp(defaults.properties)};
}

void iclforge_ac4_objects_config_init(iclforge_ac4_objects_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const iclforge::ac4::ObjectsConfig defaults{};
    *config = iclforge_ac4_objects_config_t{
        .objects = nullptr,
        .object_count = 0,
        .coding = defaults.coding == iclforge::ac4::ObjectCoding::kAjoc
                      ? ICLFORGE_AC4_OBJECT_CODING_AJOC
                      : ICLFORGE_AC4_OBJECT_CODING_DIRECT,
        .downmix = static_cast<iclforge_ac4_ajoc_downmix_t>(defaults.downmix),
        .has_downmix_signals = defaults.downmix_signals.has_value() ? 1 : 0,
        .downmix_signals = defaults.downmix_signals.value_or(0),
        .decorrelation = defaults.decorrelation ? 1 : 0,
        .has_parameter_bands = defaults.parameter_bands.has_value() ? 1 : 0,
        .parameter_bands = defaults.parameter_bands.value_or(0),
        .has_coarse = defaults.coarse.has_value() ? 1 : 0,
        .coarse = defaults.coarse.value_or(false) ? 1 : 0,
        .has_screen_size_ratio_code = defaults.screen_size_ratio_code.has_value() ? 1 : 0,
        .screen_size_ratio_code = defaults.screen_size_ratio_code.value_or(0),
        .bed_object_chan_distribute = defaults.bed_object_chan_distribute ? 1 : 0};
}

void iclforge_ac4_object_metadata_update_init(iclforge_ac4_object_metadata_update_t* update) {
    if (update == nullptr) {
        return;
    }
    const iclforge::ac4::ObjectMetadataUpdate defaults{};
    *update = iclforge_ac4_object_metadata_update_t{
        .object = static_cast<size_t>(defaults.object),
        .sample = defaults.sample,
        .ramp_samples = defaults.ramp_samples,
        .properties = iclforge_c::from_cpp(defaults.properties)};
}

void iclforge_ac4_encoder_config_init(iclforge_ac4_encoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const iclforge::ac4::EncoderConfig defaults{};
    const iclforge::ac4::EncoderConfig::Experimental& experimental = defaults.experimental;
    *config = iclforge_ac4_encoder_config_t{
        .channels = defaults.channels,
        .sample_rate_hz = defaults.sample_rate_hz,
        .frame_rate_index = defaults.frame_rate_index,
        .bitrate_kbps = defaults.bitrate_kbps,
        .rate_mode = iclforge_c::from_cpp(defaults.rate_mode),
        .codec_mode = iclforge_c::from_cpp(defaults.codec_mode),
        .iframe_interval = defaults.iframe_interval,
        .dialnorm_db = defaults.dialnorm_db,
        .iframes = nullptr,
        .iframe_count = 0,
        .fragment_starts = nullptr,
        .fragment_start_count = 0,
        .experimental =
            iclforge_ac4_experimental_t{.aspx_balance = experimental.aspx_balance ? 1 : 0,
                                        .aspx_varvar = experimental.aspx_varvar ? 1 : 0,
                                        .aspx_interleave = experimental.aspx_interleave ? 1 : 0,
                                        .coding_configs = experimental.coding_configs ? 1 : 0,
                                        .seven_x = iclforge_c::from_cpp(experimental.seven_x),
                                        .acpl = experimental.acpl ? 1 : 0,
                                        .back_pair = experimental.back_pair ? 1 : 0,
                                        .ajcc = experimental.ajcc ? 1 : 0,
                                        .objects = experimental.objects ? 1 : 0},
        .objects = nullptr};
}

const char* iclforge_ac4_encoder_refusal_reason(const iclforge_ac4_encoder_config_t* config) {
    if (config == nullptr) {
        return kNoConfiguration;
    }
    try {
        const auto converted = encoder_config_to_cpp(*config);
        if (!converted.has_value()) {
            return kNotAnArgument;
        }
        // A string literal or empty, iclforge::ac4::Encoder::refusal_reason()'s own
        // contract. The empty case is std::string_view{}, whose data() is NULL
        // by the standard, so it is returned as "" rather than as NULL - the
        // same normalization iclforge_ac4_dac4_refusal() makes.
        const std::string_view reason = iclforge::ac4::Encoder::refusal_reason(*converted);
        return reason.empty() ? "" : reason.data();
    } catch (...) {
        return kInternalError;
    }
}

iclforge_status_t iclforge_ac4_encoder_create(const iclforge_ac4_encoder_config_t* config,
                                              iclforge_ac4_encoder_t** out_encoder) {
    if (config == nullptr || out_encoder == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&config, &out_encoder]() -> iclforge_status_t {
        const auto converted = encoder_config_to_cpp(*config);
        if (!converted.has_value()) {
            return converted.error();
        }
        auto result = iclforge::ac4::Encoder::create(*converted);
        if (!result.has_value()) {
            return iclforge_c::from_cpp(result.error());
        }
        *out_encoder = new iclforge_ac4_encoder(std::move(*result));
        return ICLFORGE_OK;
    });
}

void iclforge_ac4_encoder_destroy(iclforge_ac4_encoder_t* encoder) { delete encoder; }

iclforge_ac4_codec_mode_t iclforge_ac4_encoder_codec_mode(const iclforge_ac4_encoder_t* encoder) {
    return encoder == nullptr ? ICLFORGE_AC4_CODEC_AUTO
                              : iclforge_c::from_cpp(encoder->impl.codec_mode());
}

int iclforge_ac4_encoder_delay_samples(const iclforge_ac4_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.delay_samples();
}

int iclforge_ac4_encoder_decoder_delay_samples(const iclforge_ac4_encoder_t* encoder) {
    return encoder == nullptr ? 0 : encoder->impl.decoder_delay_samples();
}

const uint8_t* iclforge_ac4_encoded_frame_data(const iclforge_ac4_encoded_frame_t* frame) {
    if (frame == nullptr || frame->data.raw_ac4_frame.empty()) {
        return nullptr;
    }
    return reinterpret_cast<const uint8_t*>(frame->data.raw_ac4_frame.data());
}

size_t iclforge_ac4_encoded_frame_size(const iclforge_ac4_encoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.raw_ac4_frame.size();
}

int iclforge_ac4_encoded_frame_samples(const iclforge_ac4_encoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.samples;
}

int iclforge_ac4_encoded_frame_iframe(const iclforge_ac4_encoded_frame_t* frame) {
    return frame != nullptr && frame->data.iframe ? 1 : 0;
}

void iclforge_ac4_encoded_frame_destroy(iclforge_ac4_encoded_frame_t* frame) { delete frame; }

void iclforge_ac4_encoded_frame_array_destroy(iclforge_ac4_encoded_frame_t** frames, size_t count) {
    if (frames == nullptr) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        delete frames[i];
    }
    delete[] frames;
}

iclforge_status_t iclforge_ac4_encoder_encode(iclforge_ac4_encoder_t* encoder,
                                              const float* const* channels, size_t channel_count,
                                              size_t samples_per_channel,
                                              iclforge_ac4_encoded_frame_t*** out_frames,
                                              size_t* out_count) {
    if (encoder == nullptr || channels == nullptr || out_frames == nullptr || out_count == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&encoder, &channels, &channel_count, &samples_per_channel, &out_frames,
                  &out_count]() -> iclforge_status_t {
        const auto spans = spans_of(channels, channel_count, samples_per_channel);
        if (!spans.has_value()) {
            return spans.error();
        }
        auto result = encoder->impl.encode(*spans);
        if (!result.has_value()) {
            return iclforge_c::from_cpp(result.error());
        }
        return build_encoded_frame_array(std::move(*result), out_frames, out_count);
    });
}

iclforge_status_t iclforge_ac4_encoder_encode_objects(
    iclforge_ac4_encoder_t* encoder, const float* const* objects, size_t object_count,
    size_t samples_per_object, const iclforge_ac4_object_metadata_update_t* updates,
    size_t update_count, iclforge_ac4_encoded_frame_t*** out_frames, size_t* out_count) {
    if (encoder == nullptr || objects == nullptr || out_frames == nullptr || out_count == nullptr ||
        (update_count > 0 && updates == nullptr)) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&encoder, &objects, &object_count, &samples_per_object, &updates, &update_count,
                  &out_frames, &out_count]() -> iclforge_status_t {
        const auto spans = spans_of(objects, object_count, samples_per_object);
        if (!spans.has_value()) {
            return spans.error();
        }
        std::vector<iclforge::ac4::ObjectMetadataUpdate> converted;
        converted.reserve(update_count);
        for (size_t i = 0; i < update_count; ++i) {
            const iclforge_ac4_object_metadata_update_t& update = updates[i];
            // An object index no int holds is one the configuration lacks.
            if (update.object > static_cast<size_t>(std::numeric_limits<int>::max())) {
                return ICLFORGE_ERROR_AC4_ENCODE_INVALID_INPUT;
            }
            iclforge::ac4::ObjectMetadataUpdate& out = converted.emplace_back();
            out.object = static_cast<int>(update.object);
            out.sample = update.sample;
            out.ramp_samples = update.ramp_samples;
            out.properties = iclforge_c::to_cpp(update.properties);
        }
        auto result = encoder->impl.encode(*spans, converted);
        if (!result.has_value()) {
            return iclforge_c::from_cpp(result.error());
        }
        return build_encoded_frame_array(std::move(*result), out_frames, out_count);
    });
}

iclforge_status_t iclforge_ac4_encoder_flush(iclforge_ac4_encoder_t* encoder,
                                             iclforge_ac4_encoded_frame_t*** out_frames,
                                             size_t* out_count) {
    if (encoder == nullptr || out_frames == nullptr || out_count == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&encoder, &out_frames, &out_count]() -> iclforge_status_t {
        auto result = encoder->impl.flush();
        if (!result.has_value()) {
            return iclforge_c::from_cpp(result.error());
        }
        return build_encoded_frame_array(std::move(*result), out_frames, out_count);
    });
}

iclforge_status_t iclforge_ac4_encoder_toc(const iclforge_ac4_encoder_t* encoder,
                                           iclforge_ac4_toc_t** out_toc) {
    if (encoder == nullptr || out_toc == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&encoder, &out_toc] {
        auto owned = std::make_unique<iclforge_ac4_toc>();
        owned->data = encoder->impl.toc();
        *out_toc = owned.release();
        return ICLFORGE_OK;
    });
}

void iclforge_ac4_toc_destroy(iclforge_ac4_toc_t* toc) { delete toc; }

iclforge_status_t iclforge_ac4_build_dac4(const iclforge_ac4_toc_t* toc,
                                          iclforge_bytes_t** out_box) {
    if (toc == nullptr || out_box == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&toc, &out_box] {
        auto owned = std::make_unique<iclforge_bytes>();
        owned->data = iclforge::ac4::build_dac4(toc->data);
        *out_box = owned.release();
        return ICLFORGE_OK;
    });
}

const char* iclforge_ac4_dac4_refusal(const iclforge_ac4_toc_t* toc) {
    // Library-owned storage valid for the process lifetime: iclforge::ac4::dac4_refusal()
    // always returns a string literal naming what it cannot describe, or an
    // empty view - never a dynamically composed string. The empty case is
    // std::string_view{} (ac4/src/ac4.cpp), whose data() is NULL by the
    // standard, not a zero-length slice of a literal - "" is returned
    // instead so a caller can always treat the result as a NUL-terminated C
    // string without checking for NULL first.
    if (toc == nullptr) {
        return "";
    }
    const std::string_view refusal = iclforge::ac4::dac4_refusal(toc->data);
    return refusal.empty() ? "" : refusal.data();
}

int iclforge_ac4_media_timing(const iclforge_ac4_toc_t* toc, uint32_t* out_timescale,
                              uint32_t* out_sample_delta) {
    if (toc == nullptr) {
        return 0;
    }
    const auto timing = iclforge::ac4::media_timing(toc->data);
    if (!timing.has_value()) {
        return 0;
    }
    if (out_timescale != nullptr) {
        *out_timescale = timing->timescale;
    }
    if (out_sample_delta != nullptr) {
        *out_sample_delta = timing->sample_delta;
    }
    return 1;
}

int iclforge_ac4_samples_per_frame(const iclforge_ac4_toc_t* toc, uint32_t* out_samples) {
    if (toc == nullptr) {
        return 0;
    }
    const auto samples = iclforge::ac4::samples_per_frame(toc->data);
    if (!samples.has_value()) {
        return 0;
    }
    if (out_samples != nullptr) {
        *out_samples = *samples;
    }
    return 1;
}

iclforge_status_t iclforge_ac4_sync_frame(const uint8_t* raw_frame, size_t raw_frame_size, int crc,
                                          iclforge_bytes_t** out_bytes) {
    if (raw_frame == nullptr || out_bytes == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&raw_frame, &raw_frame_size, &crc, &out_bytes] {
        auto owned = std::make_unique<iclforge_bytes>();
        owned->data = iclforge::ac4::sync_frame(
            std::as_bytes(std::span<const uint8_t>(raw_frame, raw_frame_size)), crc != 0);
        *out_bytes = owned.release();
        return ICLFORGE_OK;
    });
}

}  // extern "C"
