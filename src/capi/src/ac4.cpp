// iclforge_ac4_decoder_* - see iclforge.h's AC-4 section and iclforge::ac4::Decoder
// (src/ac4dec/include/iclforge/ac4dec/decoder.hpp).

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "internal.hpp"
#include "internal_ac4.hpp"

using iclforge_c::guard;
using iclforge_c::to_cpp;

namespace {

iclforge::ac4::OutputConfig output_config_to_cpp(const iclforge_ac4_output_config_t& config) {
    return iclforge::ac4::OutputConfig{
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

iclforge::ac4::PresentationChoice presentation_choice_to_cpp(const iclforge_ac4_presentation_choice_t& choice) {
    return iclforge::ac4::PresentationChoice{
        .presentation_id = choice.has_presentation_id ? std::optional<int>(choice.presentation_id)
                                                        : std::nullopt,
        .index = choice.has_index ? std::optional<std::size_t>(choice.index) : std::nullopt,
        .language = choice.language != nullptr ? std::string(choice.language) : std::string{},
        .associated = choice.has_associated ? std::optional<int>(choice.associated) : std::nullopt,
        .associated_type = to_cpp(choice.associated_type),
        .headphones = choice.headphones != 0};
}

iclforge::ac4::DecoderConfig decoder_config_to_cpp(const iclforge_ac4_decoder_config_t& config) {
    return iclforge::ac4::DecoderConfig{.output = output_config_to_cpp(config.output),
                              .concealment = to_cpp(config.concealment),
                              .presentation = presentation_choice_to_cpp(config.presentation),
                              .level = config.level,
                              .decoding = to_cpp(config.decoding)};
}

// object_index bounds-checked against `objects`; nullptr-safe on every
// out-parameter, matching iclforge_decoded_substream_dynamic_object()'s own
// convention above.
const iclforge::ac4::DecodedObject* find_object(const iclforge_ac4_decoded_frame_t* frame,
                                       size_t object_index) {
    if (frame == nullptr || object_index >= frame->data.objects.size()) {
        return nullptr;
    }
    return &frame->data.objects[object_index];
}

}  // namespace

extern "C" {

void iclforge_ac4_output_config_init(iclforge_ac4_output_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const iclforge::ac4::OutputConfig defaults{};
    *config = iclforge_ac4_output_config_t{
        .has_output_level_dbfs = defaults.output_level_dbfs.has_value() ? 1 : 0,
        .output_level_dbfs = defaults.output_level_dbfs.value_or(0.0),
        .drc = iclforge_c::from_cpp(defaults.drc),
        .headphones = defaults.headphones ? 1 : 0,
        .dialogue_enhancement_db = defaults.dialogue_enhancement_db,
        .downmix = iclforge_c::from_cpp(defaults.downmix),
        .mix_lfe = defaults.mix_lfe ? 1 : 0,
        .dialogue_gain_db = defaults.dialogue_gain_db,
        .associated_gain_db = defaults.associated_gain_db};
}

void iclforge_ac4_presentation_choice_init(iclforge_ac4_presentation_choice_t* choice) {
    if (choice == nullptr) {
        return;
    }
    *choice = iclforge_ac4_presentation_choice_t{.has_presentation_id = 0,
                                                 .presentation_id = 0,
                                                 .has_index = 0,
                                                 .index = 0,
                                                 .language = nullptr,
                                                 .has_associated = 0,
                                                 .associated = 0,
                                                 .associated_type = ICLFORGE_AC4_ASSOCIATED_ANY,
                                                 .headphones = 0};
}

void iclforge_ac4_decoder_config_init(iclforge_ac4_decoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    const iclforge::ac4::DecoderConfig defaults{};
    iclforge_ac4_output_config_init(&config->output);
    config->concealment = iclforge_c::from_cpp(defaults.concealment);
    iclforge_ac4_presentation_choice_init(&config->presentation);
    config->level = defaults.level;
    config->decoding = ICLFORGE_AC4_DECODING_FULL;
}

iclforge_status_t iclforge_ac4_decoder_create(const iclforge_ac4_decoder_config_t* config,
                                              iclforge_ac4_decoder_t** out_decoder) {
    if (config == nullptr || out_decoder == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&config, &out_decoder] {
        *out_decoder = new iclforge_ac4_decoder(decoder_config_to_cpp(*config));
        return ICLFORGE_OK;
    });
}

void iclforge_ac4_decoder_destroy(iclforge_ac4_decoder_t* decoder) { delete decoder; }

void iclforge_ac4_decoder_set_output(iclforge_ac4_decoder_t* decoder,
                                     const iclforge_ac4_output_config_t* output) {
    if (decoder == nullptr || output == nullptr) {
        return;
    }
    decoder->impl.set_output(output_config_to_cpp(*output));
}

void iclforge_ac4_decoder_set_presentation(iclforge_ac4_decoder_t* decoder,
                                           const iclforge_ac4_presentation_choice_t* choice) {
    if (decoder == nullptr || choice == nullptr) {
        return;
    }
    decoder->impl.set_presentation(presentation_choice_to_cpp(*choice));
}

void iclforge_ac4_decoder_reset(iclforge_ac4_decoder_t* decoder) {
    if (decoder != nullptr) {
        decoder->impl.reset();
    }
}

int iclforge_ac4_decoder_latency_samples(const iclforge_ac4_decoder_t* decoder) {
    return decoder == nullptr ? 0 : decoder->impl.latency_samples();
}

const char* iclforge_ac4_decoder_refusal_reason(const iclforge_ac4_decoder_t* decoder) {
    // string_view::data() is not guaranteed NUL-terminated in general, but
    // iclforge::ac4::Decoder::refusal_reason() is always backed by a string literal
    // when non-empty (iclforge::ac4::describe(DecodeError) or a literal `reason`
    // passed at the point a substream was refused - see ac4dec/src/decoder.cpp),
    // which is. The empty case (a decode() that decoded normally) is
    // std::string_view{} - data() is NULL by the standard there, not a
    // zero-length slice of a literal, so it is normalized to "" the same way
    // iclforge_ac4_dac4_refusal() does, rather than handed to the caller as
    // NULL.
    if (decoder == nullptr) {
        return "";
    }
    const std::string_view reason = decoder->impl.refusal_reason();
    return reason.empty() ? "" : reason.data();
}

iclforge_status_t iclforge_ac4_decoder_decode(iclforge_ac4_decoder_t* decoder, const uint8_t* frame,
                                              size_t frame_size,
                                              iclforge_ac4_decoded_frame_t** out_frame) {
    if (decoder == nullptr || frame == nullptr || out_frame == nullptr) {
        return ICLFORGE_ERROR_INVALID_ARGUMENT;
    }
    return guard([&decoder, &frame, &frame_size, &out_frame]() -> iclforge_status_t {
        auto result =
            decoder->impl.decode(std::as_bytes(std::span<const uint8_t>(frame, frame_size)));
        if (!result.has_value()) {
            return iclforge_c::from_cpp(result.error());
        }
        if (!result->has_value()) {
            *out_frame = nullptr;
            return ICLFORGE_OK;
        }
        auto owned = std::make_unique<iclforge_ac4_decoded_frame>();
        owned->data = std::move(**result);
        *out_frame = owned.release();
        return ICLFORGE_OK;
    });
}

int iclforge_ac4_decoded_frame_sample_rate_hz(const iclforge_ac4_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.sample_rate_hz;
}

int iclforge_ac4_decoded_frame_sequence_counter(const iclforge_ac4_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.sequence_counter;
}

size_t iclforge_ac4_decoded_frame_presentation_index(const iclforge_ac4_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.presentation;
}

int iclforge_ac4_decoded_frame_has_presentation_id(const iclforge_ac4_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.presentation_id.has_value() ? 1 : 0;
}

int iclforge_ac4_decoded_frame_presentation_id(const iclforge_ac4_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.presentation_id.has_value() ? *frame->data.presentation_id
                                                                        : 0;
}

size_t iclforge_ac4_decoded_frame_channel_count(const iclforge_ac4_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.channels.size();
}

size_t iclforge_ac4_decoded_frame_samples_per_channel(const iclforge_ac4_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.samples;
}

const float* iclforge_ac4_decoded_frame_channel_samples(const iclforge_ac4_decoded_frame_t* frame,
                                                          size_t channel_index) {
    if (frame == nullptr || channel_index >= frame->data.channels.size()) {
        return nullptr;
    }
    return frame->data.channels[channel_index].data();
}

iclforge_ac4_speaker_t iclforge_ac4_decoded_frame_speaker(const iclforge_ac4_decoded_frame_t* frame,
                                                          size_t channel_index) {
    if (frame == nullptr || channel_index >= frame->data.speakers.size()) {
        return ICLFORGE_AC4_SPEAKER_LEFT;
    }
    return iclforge_c::from_cpp(frame->data.speakers[channel_index]);
}

int iclforge_ac4_decoded_frame_has_concealed(const iclforge_ac4_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.concealed.has_value() ? 1 : 0;
}

iclforge_ac4_concealment_action_t iclforge_ac4_decoded_frame_concealment_action(
    const iclforge_ac4_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.concealed.has_value()
               ? iclforge_c::from_cpp(frame->data.concealed->action)
               : ICLFORGE_AC4_CONCEALMENT_ACTION_MUTE;
}

iclforge_status_t iclforge_ac4_decoded_frame_concealment_error(
    const iclforge_ac4_decoded_frame_t* frame) {
    return frame != nullptr && frame->data.concealed.has_value()
               ? iclforge_c::from_cpp(frame->data.concealed->error)
               : ICLFORGE_OK;
}

size_t iclforge_ac4_decoded_frame_object_count(const iclforge_ac4_decoded_frame_t* frame) {
    return frame == nullptr ? 0 : frame->data.objects.size();
}

iclforge_ac4_object_kind_t iclforge_ac4_decoded_frame_object_kind(
    const iclforge_ac4_decoded_frame_t* frame, size_t object_index) {
    const auto* object = find_object(frame, object_index);
    return object == nullptr ? ICLFORGE_AC4_OBJECT_DYN : iclforge_c::from_cpp(object->kind);
}

int iclforge_ac4_decoded_frame_object_lfe(const iclforge_ac4_decoded_frame_t* frame,
                                          size_t object_index) {
    const auto* object = find_object(frame, object_index);
    return object != nullptr && object->lfe ? 1 : 0;
}

int iclforge_ac4_decoded_frame_object_has_speaker(const iclforge_ac4_decoded_frame_t* frame,
                                                  size_t object_index) {
    const auto* object = find_object(frame, object_index);
    return object != nullptr && object->speaker.has_value() ? 1 : 0;
}

iclforge_ac4_speaker_t iclforge_ac4_decoded_frame_object_speaker(
    const iclforge_ac4_decoded_frame_t* frame, size_t object_index) {
    const auto* object = find_object(frame, object_index);
    return object != nullptr && object->speaker.has_value() ? iclforge_c::from_cpp(*object->speaker)
                                                             : ICLFORGE_AC4_SPEAKER_LEFT;
}

const float* iclforge_ac4_decoded_frame_object_samples(const iclforge_ac4_decoded_frame_t* frame,
                                                        size_t object_index) {
    const auto* object = find_object(frame, object_index);
    return object == nullptr ? nullptr : object->samples.data();
}

void iclforge_ac4_object_properties_init(iclforge_ac4_object_properties_t* properties) {
    if (properties == nullptr) {
        return;
    }
    // iclforge::ac4::ObjectProperties' own default member initializers - room centre,
    // unity gain, no width - the same "safe default" convention as
    // iclforge_object_placement_init() above.
    *properties = iclforge_c::from_cpp(iclforge::ac4::ObjectProperties{});
}

iclforge_ac4_object_properties_t iclforge_ac4_decoded_frame_object_properties(
    const iclforge_ac4_decoded_frame_t* frame, size_t object_index) {
    const auto* object = find_object(frame, object_index);
    return iclforge_c::from_cpp(object == nullptr ? iclforge::ac4::ObjectProperties{}
                                                  : object->properties);
}

size_t iclforge_ac4_decoded_frame_object_update_count(const iclforge_ac4_decoded_frame_t* frame,
                                                      size_t object_index) {
    const auto* object = find_object(frame, object_index);
    return object == nullptr ? 0 : object->updates.size();
}

iclforge_ac4_object_update_t iclforge_ac4_decoded_frame_object_update(
    const iclforge_ac4_decoded_frame_t* frame, size_t object_index, size_t update_index) {
    iclforge_ac4_object_update_t out{};
    const auto* object = find_object(frame, object_index);
    if (object == nullptr || update_index >= object->updates.size()) {
        out.properties = iclforge_c::from_cpp(iclforge::ac4::ObjectProperties{});
        return out;
    }
    const iclforge::ac4::ObjectUpdate& update = object->updates[update_index];
    out.sample = update.sample;
    out.ramp_samples = update.ramp_samples;
    out.properties = iclforge_c::from_cpp(update.properties);
    return out;
}

void iclforge_ac4_decoded_frame_destroy(iclforge_ac4_decoded_frame_t* frame) { delete frame; }

// --- presentations -------------------------------------------------------

size_t iclforge_ac4_decoder_presentation_count(const iclforge_ac4_decoder_t* decoder) {
    return decoder == nullptr ? 0 : decoder->impl.presentations().size();
}

namespace {
const iclforge::ac4::PresentationInfo* find_presentation(const iclforge_ac4_decoder_t* decoder,
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

size_t iclforge_ac4_decoder_presentation_toc_index(const iclforge_ac4_decoder_t* decoder,
                                                   size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info == nullptr ? 0 : info->index;
}

int iclforge_ac4_decoder_presentation_has_id(const iclforge_ac4_decoder_t* decoder,
                                             size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->presentation_id.has_value() ? 1 : 0;
}

int iclforge_ac4_decoder_presentation_id(const iclforge_ac4_decoder_t* decoder,
                                         size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->presentation_id.has_value() ? *info->presentation_id : 0;
}

int iclforge_ac4_decoder_presentation_has_md_compat(const iclforge_ac4_decoder_t* decoder,
                                                    size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->md_compat.has_value() ? 1 : 0;
}

int iclforge_ac4_decoder_presentation_md_compat(const iclforge_ac4_decoder_t* decoder,
                                                size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->md_compat.has_value() ? *info->md_compat : 0;
}

int iclforge_ac4_decoder_presentation_enabled(const iclforge_ac4_decoder_t* decoder,
                                              size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->enabled ? 1 : 0;
}

int iclforge_ac4_decoder_presentation_alternative(const iclforge_ac4_decoder_t* decoder,
                                                  size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->alternative ? 1 : 0;
}

int iclforge_ac4_decoder_presentation_pre_virtualized(const iclforge_ac4_decoder_t* decoder,
                                                      size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->pre_virtualized ? 1 : 0;
}

const char* iclforge_ac4_decoder_presentation_name(const iclforge_ac4_decoder_t* decoder,
                                                   size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info == nullptr ? "" : info->name.c_str();
}

const char* iclforge_ac4_decoder_presentation_language(const iclforge_ac4_decoder_t* decoder,
                                                        size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info == nullptr ? "" : info->language.c_str();
}

int iclforge_ac4_decoder_presentation_decodable(const iclforge_ac4_decoder_t* decoder,
                                                size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->decodable ? 1 : 0;
}

int iclforge_ac4_decoder_presentation_selectable(const iclforge_ac4_decoder_t* decoder,
                                                 size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info != nullptr && info->selectable ? 1 : 0;
}

size_t iclforge_ac4_decoder_presentation_speaker_count(const iclforge_ac4_decoder_t* decoder,
                                                       size_t presentation_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    return info == nullptr ? 0 : info->speakers.size();
}

iclforge_ac4_speaker_t iclforge_ac4_decoder_presentation_speaker(const iclforge_ac4_decoder_t* decoder,
                                                                 size_t presentation_index,
                                                                 size_t speaker_index) {
    const auto* info = find_presentation(decoder, presentation_index);
    if (info == nullptr || speaker_index >= info->speakers.size()) {
        return ICLFORGE_AC4_SPEAKER_LEFT;
    }
    return iclforge_c::from_cpp(info->speakers[speaker_index]);
}

iclforge_ac4_loudness_info_t iclforge_ac4_decoder_metadata_loudness(
    const iclforge_ac4_decoder_t* decoder) {
    iclforge_ac4_loudness_info_t out{};
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
