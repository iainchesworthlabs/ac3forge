// The "absent" half of ac3forge_c's AC-4 support (src/capi/CMakeLists.txt
// compiles this file instead of ac4.cpp/ac4_encoder.cpp when
// AC3FORGE_BUILD_AC4 is off): every function ac3forge.h's AC-4 section
// declares, given a body that names no ac4:: C++ type. The public header
// declares them unconditionally either way (that header's own comment), so a
// caller sees the same 76 symbols whatever this library was built with; here
// every fallible one returns AC3FORGE_ERROR_UNSUPPORTED, a *_create()
// leaves its out-parameter NULL, and anything else returns a NULL pointer,
// 0, or a zero-initialized struct, as its type allows. An opaque handle
// (ac3forge_ac4_decoder_t and its neighbours) is always an incomplete type
// here: this file never defines struct ac3forge_ac4_decoder or its kin
// (internal_ac4.hpp does, for ac4.cpp/ac4_encoder.cpp alone), and never
// needs to - every handle this file's *_create() hands out is NULL, and a
// NULL handle is all *_destroy() and friends ever see back.

#include <cstdint>

#include "iclforge_c/iclforge.h"

// --- config initializers -----------------------------------------------
// Pure C structs; the defaults below are ac4::OutputConfig{}'s,
// ac4::DecoderConfig{}'s and ac4::EncoderConfig{}'s (src/ac4dec/include/
// ac4dec/decoder.hpp, src/ac4enc/include/iclforge/ac4enc/encoder.hpp) spelled as C
// literals, so a config built by this library is the same whichever way
// AC3FORGE_BUILD_AC4 was set - naming no ac4:: type does not have to mean
// guessing at its defaults.

void ac3forge_ac4_output_config_init(ac3forge_ac4_output_config_t* config) {
    if (config == nullptr) {
        return;
    }
    *config = ac3forge_ac4_output_config_t{.has_output_level_dbfs = 0,
                                           .output_level_dbfs = 0.0,
                                           .drc = AC3FORGE_AC4_DRC_DEFAULT,
                                           .headphones = 0,
                                           .dialogue_enhancement_db = 0.0,
                                           .downmix = AC3FORGE_AC4_DOWNMIX_AS_CODED,
                                           .mix_lfe = 1,
                                           .dialogue_gain_db = 0.0,
                                           .associated_gain_db = 0.0};
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
    ac3forge_ac4_output_config_init(&config->output);
    config->concealment = AC3FORGE_AC4_CONCEALMENT_NONE;
    ac3forge_ac4_presentation_choice_init(&config->presentation);
    config->level = 3;
    config->decoding = AC3FORGE_AC4_DECODING_FULL;
}

// ac4::ObjectProperties{}'s defaults (src/ac4/include/iclforge/ac4/ac4.hpp).
void ac3forge_ac4_object_properties_init(ac3forge_ac4_object_properties_t* properties) {
    if (properties == nullptr) {
        return;
    }
    *properties = ac3forge_ac4_object_properties_t{.active = 1,
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

void ac3forge_ac4_object_config_init(ac3forge_ac4_object_config_t* config) {
    if (config == nullptr) {
        return;
    }
    *config = ac3forge_ac4_object_config_t{};
    config->bed = AC3FORGE_AC4_BED_LEFT;
    ac3forge_ac4_object_properties_init(&config->properties);
}

void ac3forge_ac4_objects_config_init(ac3forge_ac4_objects_config_t* config) {
    if (config == nullptr) {
        return;
    }
    *config = ac3forge_ac4_objects_config_t{};
    config->coding = AC3FORGE_AC4_OBJECT_CODING_AJOC;
    config->downmix = AC3FORGE_AC4_AJOC_DOWNMIX_COMPUTED;
}

void ac3forge_ac4_object_metadata_update_init(ac3forge_ac4_object_metadata_update_t* update) {
    if (update == nullptr) {
        return;
    }
    *update = ac3forge_ac4_object_metadata_update_t{};
    ac3forge_ac4_object_properties_init(&update->properties);
}

void ac3forge_ac4_encoder_config_init(ac3forge_ac4_encoder_config_t* config) {
    if (config == nullptr) {
        return;
    }
    *config = ac3forge_ac4_encoder_config_t{.channels = 2,
                                            .sample_rate_hz = 48000,
                                            .frame_rate_index = 13,
                                            .bitrate_kbps = 192,
                                            .rate_mode = AC3FORGE_AC4_RATE_CONSTANT,
                                            .codec_mode = AC3FORGE_AC4_CODEC_AUTO,
                                            .iframe_interval = 24,
                                            .dialnorm_db = -31.0,
                                            .iframes = nullptr,
                                            .iframe_count = 0,
                                            .fragment_starts = nullptr,
                                            .fragment_start_count = 0,
                                            .experimental = ac3forge_ac4_experimental_t{},
                                            .objects = nullptr};
}

const char* ac3forge_ac4_encoder_refusal_reason(const ac3forge_ac4_encoder_config_t*) {
    return "this library was built without AC-4 support (AC3FORGE_BUILD_AC4 was off)";
}

// --- decoder -------------------------------------------------------------

ac3forge_status_t ac3forge_ac4_decoder_create(const ac3forge_ac4_decoder_config_t*,
                                              ac3forge_ac4_decoder_t** out_decoder) {
    if (out_decoder != nullptr) {
        *out_decoder = nullptr;
    }
    return AC3FORGE_ERROR_UNSUPPORTED;
}

void ac3forge_ac4_decoder_destroy(ac3forge_ac4_decoder_t*) {}

void ac3forge_ac4_decoder_set_output(ac3forge_ac4_decoder_t*, const ac3forge_ac4_output_config_t*) {}

void ac3forge_ac4_decoder_set_presentation(ac3forge_ac4_decoder_t*,
                                           const ac3forge_ac4_presentation_choice_t*) {}

void ac3forge_ac4_decoder_reset(ac3forge_ac4_decoder_t*) {}

int ac3forge_ac4_decoder_latency_samples(const ac3forge_ac4_decoder_t*) { return 0; }

const char* ac3forge_ac4_decoder_refusal_reason(const ac3forge_ac4_decoder_t*) {
    return "this library was built without AC-4 support (AC3FORGE_BUILD_AC4 was off)";
}

ac3forge_status_t ac3forge_ac4_decoder_decode(ac3forge_ac4_decoder_t*, const uint8_t*, size_t,
                                              ac3forge_ac4_decoded_frame_t** out_frame) {
    if (out_frame != nullptr) {
        *out_frame = nullptr;
    }
    return AC3FORGE_ERROR_UNSUPPORTED;
}

int ac3forge_ac4_decoded_frame_sample_rate_hz(const ac3forge_ac4_decoded_frame_t*) { return 0; }
int ac3forge_ac4_decoded_frame_sequence_counter(const ac3forge_ac4_decoded_frame_t*) { return 0; }
size_t ac3forge_ac4_decoded_frame_presentation_index(const ac3forge_ac4_decoded_frame_t*) {
    return 0;
}
int ac3forge_ac4_decoded_frame_has_presentation_id(const ac3forge_ac4_decoded_frame_t*) {
    return 0;
}
int ac3forge_ac4_decoded_frame_presentation_id(const ac3forge_ac4_decoded_frame_t*) { return 0; }
size_t ac3forge_ac4_decoded_frame_channel_count(const ac3forge_ac4_decoded_frame_t*) { return 0; }
size_t ac3forge_ac4_decoded_frame_samples_per_channel(const ac3forge_ac4_decoded_frame_t*) {
    return 0;
}
const float* ac3forge_ac4_decoded_frame_channel_samples(const ac3forge_ac4_decoded_frame_t*,
                                                         size_t) {
    return nullptr;
}
ac3forge_ac4_speaker_t ac3forge_ac4_decoded_frame_speaker(const ac3forge_ac4_decoded_frame_t*,
                                                          size_t) {
    return AC3FORGE_AC4_SPEAKER_LEFT;
}
int ac3forge_ac4_decoded_frame_has_concealed(const ac3forge_ac4_decoded_frame_t*) { return 0; }
ac3forge_ac4_concealment_action_t ac3forge_ac4_decoded_frame_concealment_action(
    const ac3forge_ac4_decoded_frame_t*) {
    return AC3FORGE_AC4_CONCEALMENT_ACTION_REPEAT_FADE;
}
ac3forge_status_t ac3forge_ac4_decoded_frame_concealment_error(const ac3forge_ac4_decoded_frame_t*) {
    return AC3FORGE_ERROR_UNSUPPORTED;
}

size_t ac3forge_ac4_decoded_frame_object_count(const ac3forge_ac4_decoded_frame_t*) { return 0; }
ac3forge_ac4_object_kind_t ac3forge_ac4_decoded_frame_object_kind(
    const ac3forge_ac4_decoded_frame_t*, size_t) {
    return AC3FORGE_AC4_OBJECT_DYN;
}
int ac3forge_ac4_decoded_frame_object_lfe(const ac3forge_ac4_decoded_frame_t*, size_t) {
    return 0;
}
int ac3forge_ac4_decoded_frame_object_has_speaker(const ac3forge_ac4_decoded_frame_t*, size_t) {
    return 0;
}
ac3forge_ac4_speaker_t ac3forge_ac4_decoded_frame_object_speaker(
    const ac3forge_ac4_decoded_frame_t*, size_t) {
    return AC3FORGE_AC4_SPEAKER_LEFT;
}
const float* ac3forge_ac4_decoded_frame_object_samples(const ac3forge_ac4_decoded_frame_t*,
                                                        size_t) {
    return nullptr;
}
ac3forge_ac4_object_properties_t ac3forge_ac4_decoded_frame_object_properties(
    const ac3forge_ac4_decoded_frame_t*, size_t) {
    return ac3forge_ac4_object_properties_t{};
}
size_t ac3forge_ac4_decoded_frame_object_update_count(const ac3forge_ac4_decoded_frame_t*, size_t) {
    return 0;
}
ac3forge_ac4_object_update_t ac3forge_ac4_decoded_frame_object_update(
    const ac3forge_ac4_decoded_frame_t*, size_t, size_t) {
    return ac3forge_ac4_object_update_t{};
}

void ac3forge_ac4_decoded_frame_destroy(ac3forge_ac4_decoded_frame_t*) {}

size_t ac3forge_ac4_decoder_presentation_count(const ac3forge_ac4_decoder_t*) { return 0; }
size_t ac3forge_ac4_decoder_presentation_toc_index(const ac3forge_ac4_decoder_t*, size_t) {
    return 0;
}
int ac3forge_ac4_decoder_presentation_has_id(const ac3forge_ac4_decoder_t*, size_t) { return 0; }
int ac3forge_ac4_decoder_presentation_id(const ac3forge_ac4_decoder_t*, size_t) { return 0; }
int ac3forge_ac4_decoder_presentation_has_md_compat(const ac3forge_ac4_decoder_t*, size_t) {
    return 0;
}
int ac3forge_ac4_decoder_presentation_md_compat(const ac3forge_ac4_decoder_t*, size_t) {
    return 0;
}
int ac3forge_ac4_decoder_presentation_enabled(const ac3forge_ac4_decoder_t*, size_t) { return 0; }
int ac3forge_ac4_decoder_presentation_alternative(const ac3forge_ac4_decoder_t*, size_t) {
    return 0;
}
int ac3forge_ac4_decoder_presentation_pre_virtualized(const ac3forge_ac4_decoder_t*, size_t) {
    return 0;
}
const char* ac3forge_ac4_decoder_presentation_name(const ac3forge_ac4_decoder_t*, size_t) {
    return "";
}
const char* ac3forge_ac4_decoder_presentation_language(const ac3forge_ac4_decoder_t*, size_t) {
    return "";
}
int ac3forge_ac4_decoder_presentation_decodable(const ac3forge_ac4_decoder_t*, size_t) {
    return 0;
}
int ac3forge_ac4_decoder_presentation_selectable(const ac3forge_ac4_decoder_t*, size_t) {
    return 0;
}
size_t ac3forge_ac4_decoder_presentation_speaker_count(const ac3forge_ac4_decoder_t*, size_t) {
    return 0;
}
ac3forge_ac4_speaker_t ac3forge_ac4_decoder_presentation_speaker(const ac3forge_ac4_decoder_t*,
                                                                 size_t, size_t) {
    return AC3FORGE_AC4_SPEAKER_LEFT;
}

ac3forge_ac4_loudness_info_t ac3forge_ac4_decoder_metadata_loudness(const ac3forge_ac4_decoder_t*) {
    return ac3forge_ac4_loudness_info_t{};
}

// --- encoder ---------------------------------------------------------------

ac3forge_status_t ac3forge_ac4_encoder_create(const ac3forge_ac4_encoder_config_t*,
                                              ac3forge_ac4_encoder_t** out_encoder) {
    if (out_encoder != nullptr) {
        *out_encoder = nullptr;
    }
    return AC3FORGE_ERROR_UNSUPPORTED;
}

void ac3forge_ac4_encoder_destroy(ac3forge_ac4_encoder_t*) {}

ac3forge_ac4_codec_mode_t ac3forge_ac4_encoder_codec_mode(const ac3forge_ac4_encoder_t*) {
    return AC3FORGE_AC4_CODEC_AUTO;
}
int ac3forge_ac4_encoder_delay_samples(const ac3forge_ac4_encoder_t*) { return 0; }
int ac3forge_ac4_encoder_decoder_delay_samples(const ac3forge_ac4_encoder_t*) { return 0; }

const uint8_t* ac3forge_ac4_encoded_frame_data(const ac3forge_ac4_encoded_frame_t*) {
    return nullptr;
}
size_t ac3forge_ac4_encoded_frame_size(const ac3forge_ac4_encoded_frame_t*) { return 0; }
int ac3forge_ac4_encoded_frame_samples(const ac3forge_ac4_encoded_frame_t*) { return 0; }
int ac3forge_ac4_encoded_frame_iframe(const ac3forge_ac4_encoded_frame_t*) { return 0; }
void ac3forge_ac4_encoded_frame_destroy(ac3forge_ac4_encoded_frame_t*) {}
void ac3forge_ac4_encoded_frame_array_destroy(ac3forge_ac4_encoded_frame_t**, size_t) {}

ac3forge_status_t ac3forge_ac4_encoder_encode(ac3forge_ac4_encoder_t*, const float* const*,
                                              size_t, size_t,
                                              ac3forge_ac4_encoded_frame_t*** out_frames,
                                              size_t* out_count) {
    if (out_frames != nullptr) {
        *out_frames = nullptr;
    }
    if (out_count != nullptr) {
        *out_count = 0;
    }
    return AC3FORGE_ERROR_UNSUPPORTED;
}

ac3forge_status_t ac3forge_ac4_encoder_encode_objects(ac3forge_ac4_encoder_t*, const float* const*,
                                                      size_t, size_t,
                                                      const ac3forge_ac4_object_metadata_update_t*,
                                                      size_t,
                                                      ac3forge_ac4_encoded_frame_t*** out_frames,
                                                      size_t* out_count) {
    if (out_frames != nullptr) {
        *out_frames = nullptr;
    }
    if (out_count != nullptr) {
        *out_count = 0;
    }
    return AC3FORGE_ERROR_UNSUPPORTED;
}

ac3forge_status_t ac3forge_ac4_encoder_flush(ac3forge_ac4_encoder_t*,
                                             ac3forge_ac4_encoded_frame_t*** out_frames,
                                             size_t* out_count) {
    if (out_frames != nullptr) {
        *out_frames = nullptr;
    }
    if (out_count != nullptr) {
        *out_count = 0;
    }
    return AC3FORGE_ERROR_UNSUPPORTED;
}

ac3forge_status_t ac3forge_ac4_encoder_toc(const ac3forge_ac4_encoder_t*,
                                           ac3forge_ac4_toc_t** out_toc) {
    if (out_toc != nullptr) {
        *out_toc = nullptr;
    }
    return AC3FORGE_ERROR_UNSUPPORTED;
}

void ac3forge_ac4_toc_destroy(ac3forge_ac4_toc_t*) {}

ac3forge_status_t ac3forge_ac4_build_dac4(const ac3forge_ac4_toc_t*, ac3forge_bytes_t** out_box) {
    if (out_box != nullptr) {
        *out_box = nullptr;
    }
    return AC3FORGE_ERROR_UNSUPPORTED;
}

const char* ac3forge_ac4_dac4_refusal(const ac3forge_ac4_toc_t*) {
    return "this library was built without AC-4 support (AC3FORGE_BUILD_AC4 was off)";
}

int ac3forge_ac4_media_timing(const ac3forge_ac4_toc_t*, uint32_t*, uint32_t*) { return 0; }
int ac3forge_ac4_samples_per_frame(const ac3forge_ac4_toc_t*, uint32_t*) { return 0; }

ac3forge_status_t ac3forge_ac4_sync_frame(const uint8_t*, size_t, int,
                                          ac3forge_bytes_t** out_bytes) {
    if (out_bytes != nullptr) {
        *out_bytes = nullptr;
    }
    return AC3FORGE_ERROR_UNSUPPORTED;
}
