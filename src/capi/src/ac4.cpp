// ac3forge_ac4_decoder_* - see ac3forge.h's AC-4 section and ac4::Decoder
// (src/ac4dec/include/ac4dec/decoder.hpp).

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "internal.hpp"
#include "internal_ac4.hpp"

using ac3forge_c::guard;
using ac3forge_c::to_cpp;

namespace {

ac4::OutputConfig output_config_to_cpp(const ac3forge_ac4_output_config_t& config) {
    return ac4::OutputConfig{
        .output_level_dbfs = config.has_output_level_dbfs
                                  ? std::optional<double>(config.output_level_dbfs)
                                  : std::nullopt,
        .drc = to_cpp(config.drc),
        .headphones = config.headphones != 0,
        .dialogue_enhancement_db = config.dialogue_enhancement_db,
        .downmix = to_cpp(config.downmix),
        .mix_lfe = config.mix_lfe != 0,
        .dialogue_gain_db = config.dialogue_gain_db,
        .associated_gain_db = config.associated_gain_db};
}

ac4::PresentationChoice presentation_choice_to_cpp(const ac3forge_ac4_presentation_choice_t& choice) {
    return ac4::PresentationChoice{
        .presentation_id = choice.has_presentation_id ? std::optional<int>(choice.presentation_id)
                                                        : std::nullopt,
        .index = choice.has_index ? std::optional<std::size_t>(choice.index) : std::nullopt,
        .language = choice.language != nullptr ? std::string(choice.language) : std::string{},
        .associated = choice.has_associated ? std::optional<int>(choice.associated) : std::nullopt,
        .associated_type = to_cpp(choice.associated_type),
        .headphones = choice.headphones != 0};
}

ac4::DecoderConfig decoder_config_to_cpp(const ac3forge_ac4_decoder_config_t& config) {
    return ac4::DecoderConfig{.output = output_config_to_cpp(config.output),
                              .concealment = to_cpp(config.concealment),
                              .presentation = presentation_choice_to_cpp(config.presentation),
                              .level = config.level,
                              .decoding = to_cpp(config.decoding)};
}

// object_index bounds-checked against `objects`; nullptr-safe on every
// out-parameter, matching ac3forge_decoded_substream_dynamic_object()'s own
// convention above.
const ac4::DecodedObject* find_object(const ac3forge_ac4_decoded_frame_t* frame,
                                       size_t object_index) {
    if (frame == nullptr || object_index >= frame->data.objects.size()) {
        return nullptr;
    }
    return &frame->data.objects[object_index];
}

}  // namespace

extern "C" {

void ac3forge_ac4_output_config_init(ac3forge_ac4_output_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const ac4::OutputConfig defaults{};
    *config = ac3forge_ac4_output_config_t{
        .has_output_level_dbfs = defaults.output_level_dbfs.has_value() ? 1 : 0,
        .output_level_dbfs = defaults.output_level_dbfs.value_or(0.0),
        .drc = ac3forge_c::from_cpp(defaults.drc),
        .headphones = defaults.headphones ? 1 : 0,
        .dialogue_enhancement_db = defaults.dialogue_enhancement_db,
        .downmix = ac3forge_c::from_cpp(defaults.downmix),
        .mix_lfe = defaults.mix_lfe ? 1 : 0,
        .dialogue_gain_db = defaults.dialogue_gain_db,
        .associated_gain_db = defaults.associated_gain_db};
}

void ac3forge_ac4_presentation_choice_init(ac3forge_ac4_presentation_choice_t* choice) {
    if (choice == nullptr) {
        return;
    }
    *choice = ac3forge_ac4_presentation_choice_t{.has_presentation_id = 0,
                                                 .presentation_id = 0,
                                                 .has_index = 0,
                                                 .index = 0,
                                                 .language = nullptr,
                                                 .has_associated = 0,
                                                 .associated = 0,
                                                 .associated_type = AC3FORGE_AC4_ASSOCIATED_ANY,
                                                 .headphones = 0};
}

void ac3forge_ac4_decoder_config_init(ac3forge_ac4_decoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const ac4::DecoderConfig defaults{};
    ac3forge_ac4_output_config_init(&config->output);
    config->concealment = ac3forge_c::from_cpp(defaults.concealment);
    ac3forge_ac4_presentation_choice_init(&config->presentation);
    config->level = defaults.level;
    config->decoding = AC3FORGE_AC4_DECODING_FULL;
}

ac3forge_status_t ac3forge_ac4_decoder_create(const ac3forge_ac4_decoder_config_t* config,
                                              ac3forge_ac4_decoder_t** out_decoder) {
    if (config == nullptr || out_decoder == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&config, &out_decoder] {
        *out_decoder = new ac3forge_ac4_decoder(decoder_config_to_cpp(*config));
        return AC3FORGE_OK;
    });
}

void ac3forge_ac4_decoder_destroy(ac3forge_ac4_decoder_t* decoder) { delete decoder; }

void ac3forge_ac4_decoder_set_output(ac3forge_ac4_decoder_t* decoder,
                                     const ac3forge_ac4_output_config_t* output) {
    if (decoder == nullptr || output == nullptr) {
        return;
    }
    decoder->impl.set_output(output_config_to_cpp(*output));
}

void ac3forge_ac4_decoder_set_presentation(ac3forge_ac4_decoder_t* decoder,
                                           const ac3forge_ac4_presentation_choice_t* choice) {
    if (decoder == nullptr || choice == nullptr) {
        return;
    }
    decoder->impl.set_presentation(presentation_choice_to_cpp(*choice));
}

void ac3forge_ac4_decoder_reset(ac3forge_ac4_decoder_t* decoder) {
    if (decoder != nullptr) {
        decoder->impl.reset();
    }
}

int ac3forge_ac4_decoder_latency_samples(const ac3forge_ac4_decoder_t* decoder) {
    return decoder == nullptr ? 0 : decoder->impl.latency_samples();
}

const char* ac3forge_ac4_decoder_refusal_reason(const ac3forge_ac4_decoder_t* decoder) {
    // string_view::data() is not guaranteed NUL-terminated in general, but
    // ac4::Decoder::refusal_reason() is always backed by a string literal
    // when non-empty (ac4::describe(DecodeError) or a literal `reason`
    // passed at the point a substream was refused - see ac4dec/src/decoder.cpp),
    // which is. The empty case (a decode() that decoded normally) is
    // std::string_view{} - data() is NULL by the standard there, not a
    // zero-length slice of a literal, so it is normalized to "" the same way
    // ac3forge_ac4_dac4_refusal() does, rather than handed to the caller as
    // NULL.
    if (decoder == nullptr) {
        return "";
    }
    const std::string_view reason = decoder->impl.refusal_reason();
    return reason.empty() ? "" : reason.data();
}

ac3forge_status_t ac3forge_ac4_decoder_decode(ac3forge_ac4_decoder_t* decoder, const uint8_t* frame,
                                              size_t frame_size,
                                              ac3forge_ac4_decoded_frame_t** out_frame) {
    if (decoder == nullptr || frame == nullptr || out_frame == nullptr) {
        return AC3FORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&decoder, &frame, &frame_size, &out_frame]() -> ac3forge_status_t {
        auto result =
            decoder->impl.decode(std::as_bytes(std::span<const uint8_t>(frame, frame_size)));
        if (!result.has_value()) {
            return ac3forge_c::from_cpp(result.error());
        }
        if (!result->has_value()) {
            *out_frame = nullptr;
            return AC3FORGE_OK;
        }
        auto owned = std::make_unique<ac3forge_ac4_decoded_frame>();
        owned->data = std::move(**result);
        *out_frame = owned.release();
        return AC3FORGE_OK;
    });
}

int ac3forge_ac4_decoded_frame_sample_rate_hz(const ac3forge_ac4_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.sample_rate_hz;
}

int ac3forge_ac4_decoded_frame_sequence_counter(const ac3forge_ac4_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.sequence_counter;
}

size_t ac3forge_ac4_decoded_frame_presentation_index(const ac3forge_ac4_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.presentation;
}

int ac3forge_ac4_decoded_frame_has_presentation_id(const ac3forge_ac4_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.presentation_id.has_value() ? 1 : 0;
}

int ac3forge_ac4_decoded_frame_presentation_id(const ac3forge_ac4_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.presentation_id.has_value() ? *frame->data.presentation_id
                                                                        : 0;
}

size_t ac3forge_ac4_decoded_frame_channel_count(const ac3forge_ac4_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.channels.size();
}

size_t ac3forge_ac4_decoded_frame_samples_per_channel(const ac3forge_ac4_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.samples;
}

const float* ac3forge_ac4_decoded_frame_channel_samples(const ac3forge_ac4_decoded_frame_t* frame,
                                                          size_t channel_index) {
    if (frame == nullptr || channel_index >= frame->data.channels.size()) {
        return nullptr;
    }
    return frame->data.channels[channel_index].data();
}

ac3forge_ac4_speaker_t ac3forge_ac4_decoded_frame_speaker(const ac3forge_ac4_decoded_frame_t* frame,
                                                          size_t channel_index) {
    if (frame == nullptr || channel_index >= frame->data.speakers.size()) {
        return AC3FORGE_AC4_SPEAKER_LEFT;
    }
    return ac3forge_c::from_cpp(frame->data.speakers[channel_index]);
}

int ac3forge_ac4_decoded_frame_has_concealed(const ac3forge_ac4_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.concealed.has_value() ? 1 : 0;
}

ac3forge_ac4_concealment_action_t ac3forge_ac4_decoded_frame_concealment_action(
    const ac3forge_ac4_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.concealed.has_value()
               ? ac3forge_c::from_cpp(frame->data.concealed->action)
               : AC3FORGE_AC4_CONCEALMENT_ACTION_MUTE;
}

ac3forge_status_t ac3forge_ac4_decoded_frame_concealment_error(
    const ac3forge_ac4_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.concealed.has_value()
               ? ac3forge_c::from_cpp(frame->data.concealed->error)
               : AC3FORGE_OK;
}

size_t ac3forge_ac4_decoded_frame_object_count(const ac3forge_ac4_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.objects.size();
}

ac3forge_ac4_object_kind_t ac3forge_ac4_decoded_frame_object_kind(
    const ac3forge_ac4_decoded_frame_t* frame, size_t object_index) {
    const auto* object = find_object(frame, object_index);
    return object == nullptr ? AC3FORGE_AC4_OBJECT_DYN : ac3forge_c::from_cpp(object->kind);
}

int ac3forge_ac4_decoded_frame_object_lfe(const ac3forge_ac4_decoded_frame_t* frame,
                                          size_t object_index) {
    const auto* object = find_object(frame, object_index);
    return object != nullptr && object->lfe ? 1 : 0;
}

int ac3forge_ac4_decoded_frame_object_has_speaker(const ac3forge_ac4_decoded_frame_t* frame,
                                                  size_t object_index) {
    const auto* object = find_object(frame, object_index);
    return object != nullptr && object->speaker.has_value() ? 1 : 0;
}

ac3forge_ac4_speaker_t ac3forge_ac4_decoded_frame_object_speaker(
    const ac3forge_ac4_decoded_frame_t* frame, size_t object_index) {
    const auto* object = find_object(frame, object_index);
    return object != nullptr && object->speaker.has_value() ? ac3forge_c::from_cpp(*object->speaker)
                                                             : AC3FORGE_AC4_SPEAKER_LEFT;
}

const float* ac3forge_ac4_decoded_frame_object_samples(const ac3forge_ac4_decoded_frame_t* frame,
                                                        size_t object_index) {
    const auto* object = find_object(frame, object_index);
    return object == nullptr ? nullptr : object->samples.data();
}

ac3forge_ac4_object_properties_t ac3forge_ac4_decoded_frame_object_properties(
    const ac3forge_ac4_decoded_frame_t* frame, size_t object_index) {
    ac3forge_ac4_object_properties_t out{};
    const auto* object = find_object(frame, object_index);
    if (object == nullptr) {
        // ac4::ObjectProperties' own default member initializers - room
        // centre, unity gain, full width available - the same "safe default"
        // convention as ac3forge_object_placement_init() above.
        const ac4::ObjectProperties defaults{};
        out.active = defaults.active ? 1 : 0;
        out.gain_db = defaults.gain_db;
        out.priority = defaults.priority;
        out.x = defaults.position[0];
        out.y = defaults.position[1];
        out.z = defaults.position[2];
        out.zone_mask = defaults.zone_mask;
        out.enable_elevation = defaults.enable_elevation ? 1 : 0;
        out.snap = defaults.snap ? 1 : 0;
        out.width_x = defaults.width[0];
        out.width_y = defaults.width[1];
        out.width_z = defaults.width[2];
        out.screen_factor = defaults.screen_factor;
        out.depth_exponent = defaults.depth_exponent;
        out.has_distance = defaults.distance.has_value() ? 1 : 0;
        out.distance = defaults.distance.value_or(0.0);
        out.divergence = defaults.divergence;
        out.trim_disabled = defaults.trim_disabled ? 1 : 0;
        out.has_headphone_render_mode = defaults.headphone_render_mode.has_value() ? 1 : 0;
        out.headphone_render_mode = defaults.headphone_render_mode.value_or(0);
        out.head_track_disabled = defaults.head_track_disabled ? 1 : 0;
        return out;
    }
    const auto& properties = object->properties;
    out.active = properties.active ? 1 : 0;
    out.gain_db = properties.gain_db;
    out.priority = properties.priority;
    out.x = properties.position[0];
    out.y = properties.position[1];
    out.z = properties.position[2];
    out.zone_mask = properties.zone_mask;
    out.enable_elevation = properties.enable_elevation ? 1 : 0;
    out.snap = properties.snap ? 1 : 0;
    out.width_x = properties.width[0];
    out.width_y = properties.width[1];
    out.width_z = properties.width[2];
    out.screen_factor = properties.screen_factor;
    out.depth_exponent = properties.depth_exponent;
    out.has_distance = properties.distance.has_value() ? 1 : 0;
    out.distance = properties.distance.value_or(0.0);
    out.divergence = properties.divergence;
    out.trim_disabled = properties.trim_disabled ? 1 : 0;
    out.has_headphone_render_mode = properties.headphone_render_mode.has_value() ? 1 : 0;
    out.headphone_render_mode = properties.headphone_render_mode.value_or(0);
    out.head_track_disabled = properties.head_track_disabled ? 1 : 0;
    return out;
}

void ac3forge_ac4_decoded_frame_destroy(ac3forge_ac4_decoded_frame_t* frame) { delete frame; }

// --- presentations -------------------------------------------------------

size_t ac3forge_ac4_decoder_presentation_count(const ac3forge_ac4_decoder_t* decoder) {
    return decoder == nullptr ? 0 : decoder->impl.presentations().size();
}

namespace {
const ac4::PresentationInfo* find_presentation(const ac3forge_ac4_decoder_t* decoder,
                                                size_t presentation_index) {
    if (decoder == nullptr) {
        return nullptr;
    }
    const auto presentations = decoder->impl.presentations();
    if (presentation_index >= presentations.size()) {
        return nullptr;
    }
    return &presentations[presentation_index];
}
}  // namespace

size_t ac3forge_ac4_decoder_presentation_toc_index(const ac3forge_ac4_decoder_t* decoder,
                                                   size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info == nullptr ? 0 : info->index;
}

int ac3forge_ac4_decoder_presentation_has_id(const ac3forge_ac4_decoder_t* decoder,
                                             size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->presentation_id.has_value() ? 1 : 0;
}

int ac3forge_ac4_decoder_presentation_id(const ac3forge_ac4_decoder_t* decoder,
                                         size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->presentation_id.has_value() ? *info->presentation_id : 0;
}

int ac3forge_ac4_decoder_presentation_has_md_compat(const ac3forge_ac4_decoder_t* decoder,
                                                    size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->md_compat.has_value() ? 1 : 0;
}

int ac3forge_ac4_decoder_presentation_md_compat(const ac3forge_ac4_decoder_t* decoder,
                                                size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->md_compat.has_value() ? *info->md_compat : 0;
}

int ac3forge_ac4_decoder_presentation_enabled(const ac3forge_ac4_decoder_t* decoder,
                                              size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->enabled ? 1 : 0;
}

int ac3forge_ac4_decoder_presentation_alternative(const ac3forge_ac4_decoder_t* decoder,
                                                  size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->alternative ? 1 : 0;
}

int ac3forge_ac4_decoder_presentation_pre_virtualized(const ac3forge_ac4_decoder_t* decoder,
                                                      size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->pre_virtualized ? 1 : 0;
}

const char* ac3forge_ac4_decoder_presentation_name(const ac3forge_ac4_decoder_t* decoder,
                                                   size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info == nullptr ? "" : info->name.c_str();
}

const char* ac3forge_ac4_decoder_presentation_language(const ac3forge_ac4_decoder_t* decoder,
                                                        size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info == nullptr ? "" : info->language.c_str();
}

int ac3forge_ac4_decoder_presentation_decodable(const ac3forge_ac4_decoder_t* decoder,
                                                size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->decodable ? 1 : 0;
}

int ac3forge_ac4_decoder_presentation_selectable(const ac3forge_ac4_decoder_t* decoder,
                                                 size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->selectable ? 1 : 0;
}

size_t ac3forge_ac4_decoder_presentation_speaker_count(const ac3forge_ac4_decoder_t* decoder,
                                                       size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info == nullptr ? 0 : info->speakers.size();
}

ac3forge_ac4_speaker_t ac3forge_ac4_decoder_presentation_speaker(const ac3forge_ac4_decoder_t* decoder,
                                                                 size_t presentation_index,
                                                                 size_t speaker_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    if (info == nullptr || speaker_index >= info->speakers.size()) {
        return AC3FORGE_AC4_SPEAKER_LEFT;
    }
    return ac3forge_c::from_cpp(info->speakers[speaker_index]);
}

ac3forge_ac4_loudness_info_t ac3forge_ac4_decoder_metadata_loudness(
    const ac3forge_ac4_decoder_t* decoder) {
    ac3forge_ac4_loudness_info_t out{};
    if (decoder == nullptr) {
        return out;
    }
    const auto& loudness = decoder->impl.metadata().loudness;
    out.has_dialnorm_dbfs = loudness.dialnorm_dbfs.has_value() ? 1 : 0;
    out.dialnorm_dbfs = loudness.dialnorm_dbfs.value_or(0.0);
    out.has_integrated_lkfs = loudness.integrated_lkfs.has_value() ? 1 : 0;
    out.integrated_lkfs = loudness.integrated_lkfs.value_or(0.0);
    out.has_true_peak_dbtp = loudness.true_peak_dbtp.has_value() ? 1 : 0;
    out.true_peak_dbtp = loudness.true_peak_dbtp.value_or(0.0);
    out.has_loudness_range_lu = loudness.loudness_range_lu.has_value() ? 1 : 0;
    out.loudness_range_lu = loudness.loudness_range_lu.value_or(0.0);
    return out;
}

}  // extern "C"
