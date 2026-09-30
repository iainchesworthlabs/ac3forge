// The "absent" half of iclforge_c's AC-4 support (src/capi/CMakeLists.txt
// compiles this file instead of ac4.cpp/ac4_encoder.cpp when
// ICLFORGE_BUILD_AC4 is off): every function iclforge.h's AC-4 section
// declares, given a body that names no iclforge::ac4:: C++ type. The public header
// declares them unconditionally either way (that header's own comment), so a
// caller sees the same 76 symbols whatever this library was built with; here
// every fallible one returns ICLFORGE_ERROR_UNSUPPORTED, a *_create()
// leaves its out-parameter NULL, and anything else returns a NULL pointer,
// 0, or a zero-initialized struct, as its type allows. An opaque handle
// (iclforge_ac4_decoder_t and its neighbours) is always an incomplete type
// here: this file never defines struct iclforge_ac4_decoder or its kin
// (internal_ac4.hpp does, for ac4.cpp/ac4_encoder.cpp alone), and never
// needs to - every handle this file's *_create() hands out is NULL, and a
// NULL handle is all *_destroy() and friends ever see back.

#include <cstdint>

#include "iclforge_c/iclforge.h"

// --- config initializers -----------------------------------------------
// Pure C structs; the defaults below are iclforge::ac4::OutputConfig{}'s,
// iclforge::ac4::DecoderConfig{}'s and iclforge::ac4::EncoderConfig{}'s (src/ac4dec/include/
// ac4dec/decoder.hpp, src/ac4enc/include/iclforge/ac4enc/encoder.hpp) spelled as C
// literals, so a config built by this library is the same whichever way
// ICLFORGE_BUILD_AC4 was set - naming no iclforge::ac4:: type does not have to mean
// guessing at its defaults.

void iclforge_ac4_output_config_init(iclforge_ac4_output_config_t* config) {
    if (config == nullptr) {
        return;
    }
    *config = iclforge_ac4_output_config_t{.has_output_level_dbfs = 0,
                                           .output_level_dbfs = 0.0,
                                           .drc = ICLFORGE_AC4_DRC_DEFAULT,
                                           .headphones = 0,
                                           .dialogue_enhancement_db = 0.0,
                                           .downmix = ICLFORGE_AC4_DOWNMIX_AS_CODED,
                                           .mix_lfe = 1,
                                           .dialogue_gain_db = 0.0,
                                           .associated_gain_db = 0.0};
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
    iclforge_ac4_output_config_init(&config->output);
    config->concealment = ICLFORGE_AC4_CONCEALMENT_NONE;
    iclforge_ac4_presentation_choice_init(&config->presentation);
    config->level = 3;
    config->decoding = ICLFORGE_AC4_DECODING_FULL;
}

// iclforge::ac4::ObjectProperties{}'s defaults (src/ac4/include/iclforge/ac4/ac4.hpp).
void iclforge_ac4_object_properties_init(iclforge_ac4_object_properties_t* properties) {
    if (properties == nullptr) {
        return;
    }
    *properties = iclforge_ac4_object_properties_t{.active = 1,
                                                   .gain_db = 0.0,
                                                   .priority = 1.0,
                                                   .x = 0.5,
                                                   .y = 0.5,
                                                   .z = 0.0,
                                                   .zone_mask = 0,
                                                   .enable_elevation = 1,
                                                   .snap = 0,
                                                   .width_x = 0.0,
                                                   .width_y = 0.0,
                                                   .width_z = 0.0,
                                                   .screen_factor = 0.0,
                                                   .depth_exponent = 1.0,
                                                   .has_distance = 0,
                                                   .distance = 0.0,
                                                   .divergence = 0.0,
                                                   .trim_disabled = 0,
                                                   .has_headphone_render_mode = 0,
                                                   .headphone_render_mode = 0,
                                                   .head_track_disabled = 0};
}

void iclforge_ac4_object_config_init(iclforge_ac4_object_config_t* config) {
    if (config == nullptr) {
        return;
    }
    *config = iclforge_ac4_object_config_t{};
    config->bed = ICLFORGE_AC4_BED_LEFT;
    iclforge_ac4_object_properties_init(&config->properties);
}

void iclforge_ac4_objects_config_init(iclforge_ac4_objects_config_t* config) {
    if (config == nullptr) {
        return;
    }
    *config = iclforge_ac4_objects_config_t{};
    config->coding = ICLFORGE_AC4_OBJECT_CODING_AJOC;
    config->downmix = ICLFORGE_AC4_AJOC_DOWNMIX_COMPUTED;
}

void iclforge_ac4_object_metadata_update_init(iclforge_ac4_object_metadata_update_t* update) {
    if (update == nullptr) {
        return;
    }
    *update = iclforge_ac4_object_metadata_update_t{};
    iclforge_ac4_object_properties_init(&update->properties);
}

void iclforge_ac4_encoder_config_init(iclforge_ac4_encoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    *config = iclforge_ac4_encoder_config_t{.channels = 2,
                                            .sample_rate_hz = 48000,
                                            .frame_rate_index = 13,
                                            .bitrate_kbps = 192,
                                            .rate_mode = ICLFORGE_AC4_RATE_CONSTANT,
                                            .codec_mode = ICLFORGE_AC4_CODEC_AUTO,
                                            .iframe_interval = 24,
                                            .dialnorm_db = -31.0,
                                            .iframes = nullptr,
                                            .iframe_count = 0,
                                            .fragment_starts = nullptr,
                                            .fragment_start_count = 0,
                                            .experimental = iclforge_ac4_experimental_t{},
                                            .objects = nullptr};
}

const char* iclforge_ac4_encoder_refusal_reason(const iclforge_ac4_encoder_config_t*) {
    return "this library was built without AC-4 support (ICLFORGE_BUILD_AC4 was off)";
}

// --- decoder -------------------------------------------------------------

iclforge_status_t iclforge_ac4_decoder_create(const iclforge_ac4_decoder_config_t*,
                                              iclforge_ac4_decoder_t** out_decoder) {
    if (out_decoder != nullptr) {
        *out_decoder = nullptr;
    }
    return ICLFORGE_ERROR_UNSUPPORTED;
}

void iclforge_ac4_decoder_destroy(iclforge_ac4_decoder_t*) {}

void iclforge_ac4_decoder_set_output(iclforge_ac4_decoder_t*, const iclforge_ac4_output_config_t*) {}

void iclforge_ac4_decoder_set_presentation(iclforge_ac4_decoder_t*,
                                           const iclforge_ac4_presentation_choice_t*) {}

void iclforge_ac4_decoder_reset(iclforge_ac4_decoder_t*) {}

int iclforge_ac4_decoder_latency_samples(const iclforge_ac4_decoder_t*) { return 0; }

const char* iclforge_ac4_decoder_refusal_reason(const iclforge_ac4_decoder_t*) {
    return "this library was built without AC-4 support (ICLFORGE_BUILD_AC4 was off)";
}

iclforge_status_t iclforge_ac4_decoder_decode(iclforge_ac4_decoder_t*, const uint8_t*, size_t,
                                              iclforge_ac4_decoded_frame_t** out_frame) {
    if (out_frame != nullptr) {
        *out_frame = nullptr;
    }
    return ICLFORGE_ERROR_UNSUPPORTED;
}

int iclforge_ac4_decoded_frame_sample_rate_hz(const iclforge_ac4_decoded_frame_t*) { return 0; }
int iclforge_ac4_decoded_frame_sequence_counter(const iclforge_ac4_decoded_frame_t*) { return 0; }
size_t iclforge_ac4_decoded_frame_presentation_index(const iclforge_ac4_decoded_frame_t*) {
    return 0;
}
int iclforge_ac4_decoded_frame_has_presentation_id(const iclforge_ac4_decoded_frame_t*) {
    return 0;
}
int iclforge_ac4_decoded_frame_presentation_id(const iclforge_ac4_decoded_frame_t*) { return 0; }
size_t iclforge_ac4_decoded_frame_channel_count(const iclforge_ac4_decoded_frame_t*) { return 0; }
size_t iclforge_ac4_decoded_frame_samples_per_channel(const iclforge_ac4_decoded_frame_t*) {
    return 0;
}
const float* iclforge_ac4_decoded_frame_channel_samples(const iclforge_ac4_decoded_frame_t*,
                                                         size_t) {
    return nullptr;
}
iclforge_ac4_speaker_t iclforge_ac4_decoded_frame_speaker(const iclforge_ac4_decoded_frame_t*,
                                                          size_t) {
    return ICLFORGE_AC4_SPEAKER_LEFT;
}
int iclforge_ac4_decoded_frame_has_concealed(const iclforge_ac4_decoded_frame_t*) { return 0; }
iclforge_ac4_concealment_action_t iclforge_ac4_decoded_frame_concealment_action(
    const iclforge_ac4_decoded_frame_t*) {
    return ICLFORGE_AC4_CONCEALMENT_ACTION_REPEAT_FADE;
}
iclforge_status_t iclforge_ac4_decoded_frame_concealment_error(const iclforge_ac4_decoded_frame_t*) {
    return ICLFORGE_ERROR_UNSUPPORTED;
}

size_t iclforge_ac4_decoded_frame_object_count(const iclforge_ac4_decoded_frame_t*) { return 0; }
iclforge_ac4_object_kind_t iclforge_ac4_decoded_frame_object_kind(
    const iclforge_ac4_decoded_frame_t*, size_t) {
    return ICLFORGE_AC4_OBJECT_DYN;
}
int iclforge_ac4_decoded_frame_object_lfe(const iclforge_ac4_decoded_frame_t*, size_t) {
    return 0;
}
int iclforge_ac4_decoded_frame_object_has_speaker(const iclforge_ac4_decoded_frame_t*, size_t) {
    return 0;
}
iclforge_ac4_speaker_t iclforge_ac4_decoded_frame_object_speaker(
    const iclforge_ac4_decoded_frame_t*, size_t) {
    return ICLFORGE_AC4_SPEAKER_LEFT;
}
const float* iclforge_ac4_decoded_frame_object_samples(const iclforge_ac4_decoded_frame_t*,
                                                        size_t) {
    return nullptr;
}
iclforge_ac4_object_properties_t iclforge_ac4_decoded_frame_object_properties(
    const iclforge_ac4_decoded_frame_t*, size_t) {
    return iclforge_ac4_object_properties_t{};
}
size_t iclforge_ac4_decoded_frame_object_update_count(const iclforge_ac4_decoded_frame_t*, size_t) {
    return 0;
}
iclforge_ac4_object_update_t iclforge_ac4_decoded_frame_object_update(
    const iclforge_ac4_decoded_frame_t*, size_t, size_t) {
    return iclforge_ac4_object_update_t{};
}

void iclforge_ac4_decoded_frame_destroy(iclforge_ac4_decoded_frame_t*) {}

size_t iclforge_ac4_decoder_presentation_count(const iclforge_ac4_decoder_t*) { return 0; }
size_t iclforge_ac4_decoder_presentation_toc_index(const iclforge_ac4_decoder_t*, size_t) {
    return 0;
}
int iclforge_ac4_decoder_presentation_has_id(const iclforge_ac4_decoder_t*, size_t) { return 0; }
int iclforge_ac4_decoder_presentation_id(const iclforge_ac4_decoder_t*, size_t) { return 0; }
int iclforge_ac4_decoder_presentation_has_md_compat(const iclforge_ac4_decoder_t*, size_t) {
    return 0;
}
int iclforge_ac4_decoder_presentation_md_compat(const iclforge_ac4_decoder_t*, size_t) {
    return 0;
}
int iclforge_ac4_decoder_presentation_enabled(const iclforge_ac4_decoder_t*, size_t) { return 0; }
int iclforge_ac4_decoder_presentation_alternative(const iclforge_ac4_decoder_t*, size_t) {
    return 0;
}
int iclforge_ac4_decoder_presentation_pre_virtualized(const iclforge_ac4_decoder_t*, size_t) {
    return 0;
}
const char* iclforge_ac4_decoder_presentation_name(const iclforge_ac4_decoder_t*, size_t) {
    return "";
}
const char* iclforge_ac4_decoder_presentation_language(const iclforge_ac4_decoder_t*, size_t) {
    return "";
}
int iclforge_ac4_decoder_presentation_decodable(const iclforge_ac4_decoder_t*, size_t) {
    return 0;
}
int iclforge_ac4_decoder_presentation_selectable(const iclforge_ac4_decoder_t*, size_t) {
    return 0;
}
size_t iclforge_ac4_decoder_presentation_speaker_count(const iclforge_ac4_decoder_t*, size_t) {
    return 0;
}
iclforge_ac4_speaker_t iclforge_ac4_decoder_presentation_speaker(const iclforge_ac4_decoder_t*,
                                                                 size_t, size_t) {
    return ICLFORGE_AC4_SPEAKER_LEFT;
}

iclforge_ac4_loudness_info_t iclforge_ac4_decoder_metadata_loudness(const iclforge_ac4_decoder_t*) {
    return iclforge_ac4_loudness_info_t{};
}

// --- encoder ---------------------------------------------------------------

iclforge_status_t iclforge_ac4_encoder_create(const iclforge_ac4_encoder_config_t*,
                                              iclforge_ac4_encoder_t** out_encoder) {
    if (out_encoder != nullptr) {
        *out_encoder = nullptr;
    }
    return ICLFORGE_ERROR_UNSUPPORTED;
}

void iclforge_ac4_encoder_destroy(iclforge_ac4_encoder_t*) {}

iclforge_ac4_codec_mode_t iclforge_ac4_encoder_codec_mode(const iclforge_ac4_encoder_t*) {
    return ICLFORGE_AC4_CODEC_AUTO;
}
int iclforge_ac4_encoder_delay_samples(const iclforge_ac4_encoder_t*) { return 0; }
int iclforge_ac4_encoder_decoder_delay_samples(const iclforge_ac4_encoder_t*) { return 0; }

const uint8_t* iclforge_ac4_encoded_frame_data(const iclforge_ac4_encoded_frame_t*) {
    return nullptr;
}
size_t iclforge_ac4_encoded_frame_size(const iclforge_ac4_encoded_frame_t*) { return 0; }
int iclforge_ac4_encoded_frame_samples(const iclforge_ac4_encoded_frame_t*) { return 0; }
int iclforge_ac4_encoded_frame_iframe(const iclforge_ac4_encoded_frame_t*) { return 0; }
void iclforge_ac4_encoded_frame_destroy(iclforge_ac4_encoded_frame_t*) {}
void iclforge_ac4_encoded_frame_array_destroy(iclforge_ac4_encoded_frame_t**, size_t) {}

iclforge_status_t iclforge_ac4_encoder_encode(iclforge_ac4_encoder_t*, const float* const*,
                                              size_t, size_t,
                                              iclforge_ac4_encoded_frame_t*** out_frames,
                                              size_t* out_count) {
    if (out_frames != nullptr) {
        *out_frames = nullptr;
    }
    if (out_count != nullptr) {
        *out_count = 0;
    }
    return ICLFORGE_ERROR_UNSUPPORTED;
}

iclforge_status_t iclforge_ac4_encoder_encode_objects(iclforge_ac4_encoder_t*, const float* const*,
                                                      size_t, size_t,
                                                      const iclforge_ac4_object_metadata_update_t*,
                                                      size_t,
                                                      iclforge_ac4_encoded_frame_t*** out_frames,
                                                      size_t* out_count) {
    if (out_frames != nullptr) {
        *out_frames = nullptr;
    }
    if (out_count != nullptr) {
        *out_count = 0;
    }
    return ICLFORGE_ERROR_UNSUPPORTED;
}

iclforge_status_t iclforge_ac4_encoder_flush(iclforge_ac4_encoder_t*,
                                             iclforge_ac4_encoded_frame_t*** out_frames,
                                             size_t* out_count) {
    if (out_frames != nullptr) {
        *out_frames = nullptr;
    }
    if (out_count != nullptr) {
        *out_count = 0;
    }
    return ICLFORGE_ERROR_UNSUPPORTED;
}

iclforge_status_t iclforge_ac4_encoder_toc(const iclforge_ac4_encoder_t*,
                                           iclforge_ac4_toc_t** out_toc) {
    if (out_toc != nullptr) {
        *out_toc = nullptr;
    }
    return ICLFORGE_ERROR_UNSUPPORTED;
}

void iclforge_ac4_toc_destroy(iclforge_ac4_toc_t*) {}

iclforge_status_t iclforge_ac4_build_dac4(const iclforge_ac4_toc_t*, iclforge_bytes_t** out_box) {
    if (out_box != nullptr) {
        *out_box = nullptr;
    }
    return ICLFORGE_ERROR_UNSUPPORTED;
}

const char* iclforge_ac4_dac4_refusal(const iclforge_ac4_toc_t*) {
    return "this library was built without AC-4 support (ICLFORGE_BUILD_AC4 was off)";
}

int iclforge_ac4_media_timing(const iclforge_ac4_toc_t*, uint32_t*, uint32_t*) { return 0; }
int iclforge_ac4_samples_per_frame(const iclforge_ac4_toc_t*, uint32_t*) { return 0; }

iclforge_status_t iclforge_ac4_sync_frame(const uint8_t*, size_t, int,
                                          iclforge_bytes_t** out_bytes) {
    if (out_bytes != nullptr) {
        *out_bytes = nullptr;
    }
    return ICLFORGE_ERROR_UNSUPPORTED;
}
