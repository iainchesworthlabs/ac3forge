// The AC-4 C entry points' argument and error arms, and the accessors the round trips in
// test_capi.cpp do not reach: what a NULL argument, an index past the end, an enumerator outside its
// enumeration, a stream that does not decode, a decoder setting, or a change of one while a stream
// plays leaves the call to answer. Each answer is held against the C++ API the entry point wraps:
// the same bytes go to ac4::Decoder, the same configuration to ac4::Encoder, and the two must
// agree. The cases carry test_capi.cpp's [capi][ac4] tags.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3forge_c/ac3forge.h"
#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "ac4enc/encoder.hpp"

namespace {

constexpr std::size_t kFrameSamples = 2048;  // frame_rate_index 13's samples a frame
constexpr double kRate = 48000.0;

struct Stream {
    std::vector<std::vector<std::uint8_t>> frames;
};

std::span<const std::byte> bytes_of(std::span<const std::uint8_t> frame) {
    return std::as_bytes(frame);
}

// One tone a channel, `samples` long, each at the frequency given.
std::vector<std::vector<float>> tones(const std::vector<double>& hz, std::size_t samples) {
    std::vector<std::vector<float>> input(hz.size(), std::vector<float>(samples));
    for (std::size_t c = 0; c < hz.size(); ++c) {
        for (std::size_t n = 0; n < samples; ++n) {
            input[c][n] = static_cast<float>(
                0.2 * std::sin(2.0 * std::numbers::pi * hz[c] * static_cast<double>(n) / kRate));
        }
    }
    return input;
}

// `input` (planar) encoded whole and flushed through the C API: the frames it wrote. With an
// objects configuration the input is the objects' audio.
Stream encode_with_c_api(const ac3forge_ac4_encoder_config_t& config,
                         const std::vector<std::vector<float>>& input) {
    Stream out;
    ac3forge_ac4_encoder_t* encoder = nullptr;
    INFO(ac3forge_ac4_encoder_refusal_reason(&config));
    REQUIRE(ac3forge_ac4_encoder_create(&config, &encoder) == AC3FORGE_OK);
    std::vector<const float*> views;
    for (const auto& channel : input) {
        views.push_back(channel.data());
    }
    const auto take = [&out](ac3forge_ac4_encoded_frame_t** frames, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            const std::uint8_t* bytes = ac3forge_ac4_encoded_frame_data(frames[i]);
            REQUIRE(bytes != nullptr);
            out.frames.emplace_back(bytes, bytes + ac3forge_ac4_encoded_frame_size(frames[i]));
        }
        ac3forge_ac4_encoded_frame_array_destroy(frames, count);
    };
    ac3forge_ac4_encoded_frame_t** frames = nullptr;
    size_t count = 0;
    const ac3forge_status_t status =
        config.objects != nullptr
            ? ac3forge_ac4_encoder_encode_objects(encoder, views.data(), views.size(),
                                                  input.front().size(), nullptr, 0, &frames, &count)
            : ac3forge_ac4_encoder_encode(encoder, views.data(), views.size(), input.front().size(),
                                          &frames, &count);
    REQUIRE(status == AC3FORGE_OK);
    take(frames, count);
    frames = nullptr;
    count = 0;
    REQUIRE(ac3forge_ac4_encoder_flush(encoder, &frames, &count) == AC3FORGE_OK);
    take(frames, count);
    ac3forge_ac4_encoder_destroy(encoder);
    return out;
}

// Ten frames of a stereo pair at 96 kbps, the first the only I-frame.
Stream stereo_stream(double first_hz = 1000.0, double second_hz = 700.0) {
    ac3forge_ac4_encoder_config_t config;
    ac3forge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 96;
    Stream stream = encode_with_c_api(config, tones({first_hz, second_hz}, 10 * kFrameSamples));
    REQUIRE(stream.frames.size() >= 8);
    return stream;
}

// Two dynamic objects and the LFE, direct-coded, six frames of a tone each.
Stream object_stream() {
    std::array<ac3forge_ac4_object_config_t, 3> objects{};
    for (auto& object : objects) {
        ac3forge_ac4_object_config_init(&object);
    }
    objects[1].lfe = 1;
    objects[2].properties.x = 0.25;
    ac3forge_ac4_objects_config_t scene;
    ac3forge_ac4_objects_config_init(&scene);
    scene.objects = objects.data();
    scene.object_count = objects.size();
    scene.coding = AC3FORGE_AC4_OBJECT_CODING_DIRECT;
    ac3forge_ac4_encoder_config_t config;
    ac3forge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 256;
    config.experimental.objects = 1;
    config.objects = &scene;
    // A QMF subband's middle for each dynamic object, and 47 Hz for the LFE.
    return encode_with_c_api(config, tones({562.5, 47.0, 1312.5}, 6 * kFrameSamples));
}

// A decoded frame, destroyed on every way out of the scope that reads it.
struct FrameGuard {
    explicit FrameGuard(ac3forge_ac4_decoded_frame_t* f) : frame(f) {}
    FrameGuard(const FrameGuard&) = delete;
    FrameGuard& operator=(const FrameGuard&) = delete;
    ~FrameGuard() { ac3forge_ac4_decoded_frame_destroy(frame); }
    ac3forge_ac4_decoded_frame_t* frame;
};

// A decoder of the C API, destroyed with the scope.
struct CDecoder {
    explicit CDecoder(const ac3forge_ac4_decoder_config_t& config) {
        REQUIRE(ac3forge_ac4_decoder_create(&config, &decoder) == AC3FORGE_OK);
    }
    CDecoder(const CDecoder&) = delete;
    CDecoder& operator=(const CDecoder&) = delete;
    ~CDecoder() { ac3forge_ac4_decoder_destroy(decoder); }
    ac3forge_ac4_decoder_t* decoder = nullptr;
};

ac3forge_ac4_decoder_config_t default_decoder_config() {
    ac3forge_ac4_decoder_config_t config;
    ac3forge_ac4_decoder_config_init(&config);
    return config;
}

// The status a C caller is meant to see for each of the C++ decoder's errors, from the header.
ac3forge_status_t status_of(ac4::DecodeError error) {
    switch (error) {
        case ac4::DecodeError::kTruncated: return AC3FORGE_ERROR_AC4_DECODE_TRUNCATED;
        case ac4::DecodeError::kInvalidToc: return AC3FORGE_ERROR_AC4_DECODE_INVALID_TOC;
        case ac4::DecodeError::kInvalidStream: return AC3FORGE_ERROR_AC4_DECODE_INVALID_STREAM;
        case ac4::DecodeError::kUnsupported: return AC3FORGE_ERROR_AC4_DECODE_UNSUPPORTED;
        case ac4::DecodeError::kMissingIFrame: return AC3FORGE_ERROR_AC4_DECODE_MISSING_IFRAME;
    }
    return AC3FORGE_ERROR_INTERNAL;
}

// Every accessor of a decoded frame against the C++ frame it wraps, and each one past the last
// channel, object and update.
void check_frame(const ac3forge_ac4_decoded_frame_t* frame, const ac4::DecodedFrame& want) {
    CHECK(ac3forge_ac4_decoded_frame_sample_rate_hz(frame) == want.sample_rate_hz);
    CHECK(ac3forge_ac4_decoded_frame_sequence_counter(frame) == want.sequence_counter);
    CHECK(ac3forge_ac4_decoded_frame_presentation_index(frame) == want.presentation);
    CHECK((ac3forge_ac4_decoded_frame_has_presentation_id(frame) != 0) ==
          want.presentation_id.has_value());
    CHECK(ac3forge_ac4_decoded_frame_presentation_id(frame) == want.presentation_id.value_or(0));
    CHECK(ac3forge_ac4_decoded_frame_samples_per_channel(frame) == want.samples);

    const std::size_t channels = ac3forge_ac4_decoded_frame_channel_count(frame);
    REQUIRE(channels == want.channels.size());
    for (std::size_t c = 0; c < channels; ++c) {
        CHECK(static_cast<int>(ac3forge_ac4_decoded_frame_speaker(frame, c)) ==
              static_cast<int>(want.speakers[c]));
        const float* pcm = ac3forge_ac4_decoded_frame_channel_samples(frame, c);
        REQUIRE(pcm != nullptr);
        CHECK(std::equal(want.channels[c].begin(), want.channels[c].end(), pcm));
    }
    CHECK(ac3forge_ac4_decoded_frame_channel_samples(frame, channels) == nullptr);
    CHECK(ac3forge_ac4_decoded_frame_speaker(frame, channels) == AC3FORGE_AC4_SPEAKER_LEFT);

    CHECK((ac3forge_ac4_decoded_frame_has_concealed(frame) != 0) == want.concealed.has_value());
    if (want.concealed.has_value()) {
        CHECK(static_cast<int>(ac3forge_ac4_decoded_frame_concealment_action(frame)) ==
              static_cast<int>(want.concealed->action));
        CHECK(ac3forge_ac4_decoded_frame_concealment_error(frame) ==
              status_of(want.concealed->error));
    } else {
        CHECK(ac3forge_ac4_decoded_frame_concealment_action(frame) ==
              AC3FORGE_AC4_CONCEALMENT_ACTION_MUTE);
        CHECK(ac3forge_ac4_decoded_frame_concealment_error(frame) == AC3FORGE_OK);
    }

    const std::size_t objects = ac3forge_ac4_decoded_frame_object_count(frame);
    REQUIRE(objects == want.objects.size());
    for (std::size_t o = 0; o < objects; ++o) {
        const ac4::DecodedObject& object = want.objects[o];
        CHECK(static_cast<int>(ac3forge_ac4_decoded_frame_object_kind(frame, o)) ==
              static_cast<int>(object.kind));
        CHECK((ac3forge_ac4_decoded_frame_object_lfe(frame, o) != 0) == object.lfe);
        CHECK((ac3forge_ac4_decoded_frame_object_has_speaker(frame, o) != 0) ==
              object.speaker.has_value());
        CHECK(static_cast<int>(ac3forge_ac4_decoded_frame_object_speaker(frame, o)) ==
              static_cast<int>(object.speaker.value_or(ac4::Speaker::kLeft)));
        const float* pcm = ac3forge_ac4_decoded_frame_object_samples(frame, o);
        REQUIRE(pcm != nullptr);
        CHECK(std::equal(object.samples.begin(), object.samples.end(), pcm));
        const ac3forge_ac4_object_properties_t properties =
            ac3forge_ac4_decoded_frame_object_properties(frame, o);
        CHECK((properties.active != 0) == object.properties.active);
        CHECK(properties.gain_db == object.properties.gain_db);
        CHECK(properties.priority == object.properties.priority);
        CHECK(properties.x == object.properties.position[0]);
        CHECK(properties.y == object.properties.position[1]);
        CHECK(properties.z == object.properties.position[2]);
        const std::size_t updates = ac3forge_ac4_decoded_frame_object_update_count(frame, o);
        REQUIRE(updates == object.updates.size());
        for (std::size_t u = 0; u < updates; ++u) {
            const ac3forge_ac4_object_update_t update =
                ac3forge_ac4_decoded_frame_object_update(frame, o, u);
            CHECK(update.sample == object.updates[u].sample);
            CHECK(update.ramp_samples == object.updates[u].ramp_samples);
            CHECK(update.properties.gain_db == object.updates[u].properties.gain_db);
        }
        // An update past the last is sample 0, ramp 0 and the default properties.
        const ac3forge_ac4_object_update_t past =
            ac3forge_ac4_decoded_frame_object_update(frame, o, updates);
        CHECK(past.sample == 0);
        CHECK(past.ramp_samples == 0);
        CHECK(past.properties.depth_exponent == 1.0);
    }
    // An object past the last has the defaults the header names.
    CHECK(ac3forge_ac4_decoded_frame_object_kind(frame, objects) == AC3FORGE_AC4_OBJECT_DYN);
    CHECK(ac3forge_ac4_decoded_frame_object_lfe(frame, objects) == 0);
    CHECK(ac3forge_ac4_decoded_frame_object_has_speaker(frame, objects) == 0);
    CHECK(ac3forge_ac4_decoded_frame_object_speaker(frame, objects) == AC3FORGE_AC4_SPEAKER_LEFT);
    CHECK(ac3forge_ac4_decoded_frame_object_samples(frame, objects) == nullptr);
    CHECK(ac3forge_ac4_decoded_frame_object_properties(frame, objects).depth_exponent == 1.0);
    CHECK(ac3forge_ac4_decoded_frame_object_update_count(frame, objects) == 0);
    CHECK(ac3forge_ac4_decoded_frame_object_update(frame, objects, 0).ramp_samples == 0);
}

// What one frame did in the two decoders.
struct Outcome {
    ac3forge_status_t status = AC3FORGE_OK;
    bool produced = false;
    bool concealed = false;
    ac3forge_status_t concealment_error = AC3FORGE_OK;
    ac3forge_ac4_concealment_action_t action = AC3FORGE_AC4_CONCEALMENT_ACTION_MUTE;
    std::vector<std::vector<float>> pcm;  // the frame's channels, when it produced one
};

// `frame` decoded by the C API's `decoder` and by `reference`: the two must answer alike, and the
// frame, where there is one, reads alike through every accessor.
Outcome decode_both(ac3forge_ac4_decoder_t* decoder, ac4::Decoder& reference,
                    std::span<const std::uint8_t> frame) {
    Outcome out;
    const auto want = reference.decode(bytes_of(frame));
    ac3forge_ac4_decoded_frame_t* raw = nullptr;
    out.status = ac3forge_ac4_decoder_decode(decoder, frame.data(), frame.size(), &raw);
    const FrameGuard guard(raw);
    CHECK(std::string_view(ac3forge_ac4_decoder_refusal_reason(decoder)) ==
          reference.refusal_reason());
    if (!want.has_value()) {
        CHECK(out.status == status_of(want.error()));
        CHECK(raw == nullptr);
        return out;
    }
    CHECK(out.status == AC3FORGE_OK);
    CHECK((raw != nullptr) == want->has_value());
    if (raw != nullptr && want->has_value()) {
        out.produced = true;
        out.concealed = ac3forge_ac4_decoded_frame_has_concealed(raw) != 0;
        out.concealment_error = ac3forge_ac4_decoded_frame_concealment_error(raw);
        out.action = ac3forge_ac4_decoded_frame_concealment_action(raw);
        out.pcm = (*want)->channels;
        check_frame(raw, **want);
    }
    return out;
}

// The decoder's presentations, one past the last included, against the C++ decoder's.
void check_presentations(const ac3forge_ac4_decoder_t* decoder, const ac4::Decoder& reference) {
    const std::span<const ac4::PresentationInfo> want = reference.presentations();
    REQUIRE(ac3forge_ac4_decoder_presentation_count(decoder) == want.size());
    for (std::size_t p = 0; p < want.size(); ++p) {
        const ac4::PresentationInfo& info = want[p];
        CHECK(ac3forge_ac4_decoder_presentation_toc_index(decoder, p) == info.index);
        CHECK((ac3forge_ac4_decoder_presentation_has_id(decoder, p) != 0) ==
              info.presentation_id.has_value());
        CHECK(ac3forge_ac4_decoder_presentation_id(decoder, p) == info.presentation_id.value_or(0));
        CHECK((ac3forge_ac4_decoder_presentation_has_md_compat(decoder, p) != 0) ==
              info.md_compat.has_value());
        CHECK(ac3forge_ac4_decoder_presentation_md_compat(decoder, p) ==
              info.md_compat.value_or(0));
        CHECK((ac3forge_ac4_decoder_presentation_enabled(decoder, p) != 0) == info.enabled);
        CHECK((ac3forge_ac4_decoder_presentation_alternative(decoder, p) != 0) == info.alternative);
        CHECK((ac3forge_ac4_decoder_presentation_pre_virtualized(decoder, p) != 0) ==
              info.pre_virtualized);
        CHECK(std::string_view(ac3forge_ac4_decoder_presentation_name(decoder, p)) == info.name);
        CHECK(std::string_view(ac3forge_ac4_decoder_presentation_language(decoder, p)) ==
              info.language);
        CHECK((ac3forge_ac4_decoder_presentation_decodable(decoder, p) != 0) == info.decodable);
        CHECK((ac3forge_ac4_decoder_presentation_selectable(decoder, p) != 0) == info.selectable);
        REQUIRE(ac3forge_ac4_decoder_presentation_speaker_count(decoder, p) ==
                info.speakers.size());
        for (std::size_t s = 0; s < info.speakers.size(); ++s) {
            CHECK(static_cast<int>(ac3forge_ac4_decoder_presentation_speaker(decoder, p, s)) ==
                  static_cast<int>(info.speakers[s]));
        }
        CHECK(ac3forge_ac4_decoder_presentation_speaker(decoder, p, info.speakers.size()) ==
              AC3FORGE_AC4_SPEAKER_LEFT);
    }
    // A presentation past the last has 0, no name and no language, and is neither enabled nor
    // decodable.
    const std::size_t past = want.size();
    CHECK(ac3forge_ac4_decoder_presentation_toc_index(decoder, past) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_has_id(decoder, past) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_id(decoder, past) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_has_md_compat(decoder, past) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_md_compat(decoder, past) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_enabled(decoder, past) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_alternative(decoder, past) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_pre_virtualized(decoder, past) == 0);
    CHECK(std::string_view(ac3forge_ac4_decoder_presentation_name(decoder, past)).empty());
    CHECK(std::string_view(ac3forge_ac4_decoder_presentation_language(decoder, past)).empty());
    CHECK(ac3forge_ac4_decoder_presentation_decodable(decoder, past) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_selectable(decoder, past) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_speaker_count(decoder, past) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_speaker(decoder, past, 0) == AC3FORGE_AC4_SPEAKER_LEFT);
}

// The loudness metadata the last frames sent, against the C++ decoder's.
void check_loudness(const ac3forge_ac4_decoder_t* decoder, const ac4::Decoder& reference) {
    const ac3forge_ac4_loudness_info_t got = ac3forge_ac4_decoder_metadata_loudness(decoder);
    const ac4::LoudnessInfo& want = reference.metadata().loudness;
    CHECK((got.has_dialnorm_dbfs != 0) == want.dialnorm_dbfs.has_value());
    CHECK(got.dialnorm_dbfs == want.dialnorm_dbfs.value_or(0.0));
    CHECK((got.has_integrated_lkfs != 0) == want.integrated_lkfs.has_value());
    CHECK(got.integrated_lkfs == want.integrated_lkfs.value_or(0.0));
    CHECK((got.has_true_peak_dbtp != 0) == want.true_peak_dbtp.has_value());
    CHECK(got.true_peak_dbtp == want.true_peak_dbtp.value_or(0.0));
    CHECK((got.has_loudness_range_lu != 0) == want.loudness_range_lu.has_value());
    CHECK(got.loudness_range_lu == want.loudness_range_lu.value_or(0.0));
}

}  // namespace

TEST_CASE("AC-4 decoded frame and presentation accessors report what ac4::Decoder decodes",
          "[capi][ac4]") {
    const Stream stream = stereo_stream();
    const CDecoder c(default_decoder_config());
    ac4::Decoder reference;

    // Before a frame: no delay, no presentation, no loudness, and nothing refused.
    CHECK(ac3forge_ac4_decoder_latency_samples(c.decoder) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_count(c.decoder) == 0);
    CHECK(std::string_view(ac3forge_ac4_decoder_refusal_reason(c.decoder)).empty());
    CHECK(ac3forge_ac4_decoder_metadata_loudness(c.decoder).has_dialnorm_dbfs == 0);

    std::size_t produced = 0;
    for (const std::vector<std::uint8_t>& frame : stream.frames) {
        if (decode_both(c.decoder, reference, frame).produced) {
            ++produced;
        }
    }
    REQUIRE(produced >= 6);
    CHECK(ac3forge_ac4_decoder_latency_samples(c.decoder) == reference.latency_samples());
    CHECK(ac3forge_ac4_decoder_latency_samples(c.decoder) > 0);
    check_presentations(c.decoder, reference);
    check_loudness(c.decoder, reference);
}

TEST_CASE("AC-4 decoded object frames report their objects and the indices past them",
          "[capi][ac4]") {
    const Stream stream = object_stream();
    REQUIRE(stream.frames.size() >= 4);
    const CDecoder c(default_decoder_config());
    ac4::Decoder reference;
    std::size_t with_objects = 0;
    for (const std::vector<std::uint8_t>& frame : stream.frames) {
        ac3forge_ac4_decoded_frame_t* raw = nullptr;
        const auto want = reference.decode(bytes_of(frame));
        REQUIRE(want.has_value());
        REQUIRE(ac3forge_ac4_decoder_decode(c.decoder, frame.data(), frame.size(), &raw) ==
                AC3FORGE_OK);
        const FrameGuard guard(raw);
        if (raw == nullptr || !want->has_value()) {
            continue;
        }
        check_frame(raw, **want);
        if (ac3forge_ac4_decoded_frame_object_count(raw) == 3) {
            ++with_objects;
        }
    }
    CHECK(with_objects >= 3);
}

namespace {

// Every field of the output configuration away from its default, and the C++ struct that says the
// same.
ac3forge_ac4_output_config_t settled_output() {
    ac3forge_ac4_output_config_t output;
    ac3forge_ac4_output_config_init(&output);
    output.has_output_level_dbfs = 1;
    output.output_level_dbfs = -24.0;
    output.drc = AC3FORGE_AC4_DRC_PORTABLE_SPEAKERS;
    output.headphones = 1;
    output.dialogue_enhancement_db = 3.0;
    output.downmix = AC3FORGE_AC4_DOWNMIX_MONO;
    output.mix_lfe = 0;
    output.dialogue_gain_db = -3.0;
    output.associated_gain_db = -6.0;
    return output;
}

ac4::OutputConfig settled_output_cpp() {
    ac4::OutputConfig output;
    output.output_level_dbfs = -24.0;
    output.drc = ac4::DrcMode::kPortableSpeakers;
    output.headphones = true;
    output.dialogue_enhancement_db = 3.0;
    output.downmix = ac4::DownmixTarget::kMono;
    output.mix_lfe = false;
    output.dialogue_gain_db = -3.0;
    output.associated_gain_db = -6.0;
    return output;
}

// Every field of the presentation choice set. The stream has one presentation, so the choice the
// preferences make is the same whichever of them the decoder takes first.
ac3forge_ac4_presentation_choice_t settled_choice() {
    ac3forge_ac4_presentation_choice_t choice;
    ac3forge_ac4_presentation_choice_init(&choice);
    choice.has_presentation_id = 1;
    choice.presentation_id = 7;
    choice.has_index = 1;
    choice.index = 0;
    choice.language = "eng";
    choice.has_associated = 1;
    choice.associated = 2;
    choice.associated_type = AC3FORGE_AC4_ASSOCIATED_SPOKEN_SUBTITLES;
    choice.headphones = 1;
    return choice;
}

ac4::PresentationChoice settled_choice_cpp() {
    ac4::PresentationChoice choice;
    choice.presentation_id = 7;
    choice.index = 0;
    choice.language = "eng";
    choice.associated = 2;
    choice.associated_type = ac4::AssociatedType::kSpokenSubtitles;
    choice.headphones = true;
    return choice;
}

}  // namespace

TEST_CASE("the AC-4 decoder's configuration and live settings reach the decoder", "[capi][ac4]") {
    const Stream stream = stereo_stream();
    const auto decode_all = [&stream](ac3forge_ac4_decoder_t* decoder, ac4::Decoder& reference,
                                      std::size_t from, std::size_t to) {
        std::vector<std::vector<float>> last;
        for (std::size_t k = from; k < to; ++k) {
            Outcome outcome = decode_both(decoder, reference, stream.frames[k]);
            if (outcome.produced) {
                last = std::move(outcome.pcm);
            }
        }
        return last;
    };
    // What the default decoder puts out for the last frame the sections decode.
    const CDecoder plain(default_decoder_config());
    ac4::Decoder plain_reference;
    const std::vector<std::vector<float>> plain_pcm =
        decode_all(plain.decoder, plain_reference, 0, 6);
    REQUIRE(plain_pcm.size() == 2);

    SECTION("the configuration a decoder is created with") {
        ac3forge_ac4_decoder_config_t config = default_decoder_config();
        config.output = settled_output();
        config.presentation = settled_choice();
        config.concealment = AC3FORGE_AC4_CONCEALMENT_REPEAT_FADE;
        config.level = 2;
        config.decoding = AC3FORGE_AC4_DECODING_CORE;
        ac4::DecoderConfig reference_config;
        reference_config.output = settled_output_cpp();
        reference_config.presentation = settled_choice_cpp();
        reference_config.concealment = ac4::ConcealmentPolicy::kRepeatFade;
        reference_config.level = 2;
        reference_config.decoding = ac4::DecodingMode::kCore;
        const CDecoder c(config);
        ac4::Decoder reference(reference_config);
        const std::vector<std::vector<float>> pcm = decode_all(c.decoder, reference, 0, 6);
        // The mono downmix and the output level are in the decoded audio.
        REQUIRE(pcm.size() == 1);
        CHECK((pcm != plain_pcm));
    }
    SECTION("settings changed while a stream plays") {
        const CDecoder c(default_decoder_config());
        ac4::Decoder reference;
        decode_all(c.decoder, reference, 0, 2);

        ac3forge_ac4_output_config_t output = settled_output();
        ac3forge_ac4_decoder_set_output(c.decoder, &output);
        reference.set_output(settled_output_cpp());
        const std::vector<std::vector<float>> mono = decode_all(c.decoder, reference, 2, 4);
        CHECK(mono.size() == 1);

        ac3forge_ac4_presentation_choice_t choice = settled_choice();
        ac3forge_ac4_decoder_set_presentation(c.decoder, &choice);
        reference.set_presentation(settled_choice_cpp());
        decode_all(c.decoder, reference, 4, 6);

        // A NULL decoder or a NULL setting changes nothing.
        ac3forge_ac4_decoder_set_output(nullptr, &output);
        ac3forge_ac4_decoder_set_output(c.decoder, nullptr);
        ac3forge_ac4_decoder_set_output(nullptr, nullptr);
        ac3forge_ac4_decoder_set_presentation(nullptr, &choice);
        ac3forge_ac4_decoder_set_presentation(c.decoder, nullptr);
        ac3forge_ac4_decoder_set_presentation(nullptr, nullptr);
        ac3forge_ac4_decoder_reset(nullptr);
        decode_all(c.decoder, reference, 6, 8);
    }
    SECTION("reset forgets the stream") {
        const CDecoder c(default_decoder_config());
        ac4::Decoder reference;
        const std::vector<std::vector<float>> first = decode_all(c.decoder, reference, 0, 1);
        decode_all(c.decoder, reference, 1, 4);
        REQUIRE(ac3forge_ac4_decoder_latency_samples(c.decoder) > 0);
        ac3forge_ac4_decoder_reset(c.decoder);
        reference.reset();
        CHECK(ac3forge_ac4_decoder_latency_samples(c.decoder) == 0);
        CHECK(ac3forge_ac4_decoder_presentation_count(c.decoder) == reference.presentations().size());
        // The first frame again decodes to what the first decode gave.
        CHECK((decode_all(c.decoder, reference, 0, 1) == first));
    }
}

namespace {

// A frame damaged in ways a stream meets it: cut short at several lengths, a byte inverted at
// several positions, and all zeros or all ones.
std::vector<std::vector<std::uint8_t>> damaged(const std::vector<std::uint8_t>& frame) {
    std::vector<std::vector<std::uint8_t>> out;
    const std::size_t n = frame.size();
    for (const std::size_t length : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{5},
                                     std::size_t{8}, std::size_t{13}, n / 4, n / 2, n * 3 / 4,
                                     n - 1}) {
        out.emplace_back(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(length));
    }
    for (const std::size_t at :
         {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4},
          std::size_t{5}, std::size_t{6}, std::size_t{7}, std::size_t{8}, std::size_t{10},
          std::size_t{12}, std::size_t{16}, n / 3, n / 2, n - 1}) {
        std::vector<std::uint8_t> copy = frame;
        copy[at] = static_cast<std::uint8_t>(copy[at] ^ 0xFFU);
        out.push_back(std::move(copy));
    }
    out.emplace_back(n, std::uint8_t{0});
    out.emplace_back(n, std::uint8_t{0xFF});
    return out;
}

}  // namespace

TEST_CASE("AC-4 decode refuses NULL arguments and answers a damaged frame as ac4::Decoder does",
          "[capi][ac4]") {
    const Stream stream = stereo_stream();
    const ac3forge_ac4_decoder_config_t config = default_decoder_config();

    SECTION("NULL arguments") {
        const CDecoder c(config);
        ac3forge_ac4_decoded_frame_t* out = nullptr;
        const std::vector<std::uint8_t>& frame = stream.frames[0];
        ac3forge_ac4_decoder_t* made = nullptr;
        CHECK(ac3forge_ac4_decoder_create(nullptr, &made) == AC3FORGE_ERROR_INVALID_ARGUMENT);
        CHECK(ac3forge_ac4_decoder_create(&config, nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
        CHECK(made == nullptr);
        CHECK(ac3forge_ac4_decoder_decode(nullptr, frame.data(), frame.size(), &out) ==
              AC3FORGE_ERROR_INVALID_ARGUMENT);
        CHECK(ac3forge_ac4_decoder_decode(c.decoder, nullptr, frame.size(), &out) ==
              AC3FORGE_ERROR_INVALID_ARGUMENT);
        CHECK(ac3forge_ac4_decoder_decode(c.decoder, frame.data(), frame.size(), nullptr) ==
              AC3FORGE_ERROR_INVALID_ARGUMENT);
        CHECK(out == nullptr);
    }
    SECTION("the initialisers take NULL") {
        // Documented no-ops, checked by their not crashing.
        ac3forge_ac4_output_config_init(nullptr);
        ac3forge_ac4_presentation_choice_init(nullptr);
        ac3forge_ac4_decoder_config_init(nullptr);
        ac3forge_ac4_encoder_config_init(nullptr);
    }
    SECTION("a P-frame with no I-frame before it is held back and not refused") {
        const CDecoder c(config);
        ac4::Decoder reference;
        const Outcome outcome = decode_both(c.decoder, reference, stream.frames[3]);
        CHECK(outcome.status == AC3FORGE_OK);
        CHECK_FALSE(outcome.produced);
    }
    SECTION("a frame of no bytes") {
        const CDecoder c(config);
        ac4::Decoder reference;
        const Outcome outcome = decode_both(
            c.decoder, reference, std::span<const std::uint8_t>(stream.frames[0].data(), 0));
        CHECK(outcome.status == AC3FORGE_ERROR_AC4_DECODE_INVALID_TOC);
    }
    SECTION("damage to an I-frame and to a P-frame after three good frames") {
        std::size_t refused = 0;
        std::size_t decoded = 0;
        for (const std::size_t position : {std::size_t{0}, std::size_t{3}}) {
            for (const std::vector<std::uint8_t>& bad : damaged(stream.frames[position])) {
                const CDecoder c(config);
                ac4::Decoder reference;
                for (std::size_t k = 0; k < position; ++k) {
                    decode_both(c.decoder, reference, stream.frames[k]);
                }
                const Outcome outcome = decode_both(c.decoder, reference, bad);
                if (outcome.status != AC3FORGE_OK) {
                    ++refused;
                } else {
                    ++decoded;
                }
                // Whatever the damage did to the frame, the two decoders stay in step after it.
                decode_both(c.decoder, reference, stream.frames[position + 1]);
            }
        }
        CHECK(refused > 0);
        CHECK(decoded > 0);
    }
}

TEST_CASE("AC-4 decode conceals a frame that does not decode as the configured policy says",
          "[capi][ac4]") {
    const Stream stream = stereo_stream();
    const Stream other = stereo_stream(440.0, 660.0);
    const std::vector<std::uint8_t> garbage(stream.frames[3].size(), 0xFF);

    const auto run = [&](ac3forge_ac4_concealment_policy_t policy,
                         ac4::ConcealmentPolicy reference_policy,
                         ac3forge_ac4_concealment_action_t action) {
        ac3forge_ac4_decoder_config_t config = default_decoder_config();
        config.concealment = policy;
        ac4::DecoderConfig reference_config;
        reference_config.concealment = reference_policy;
        const CDecoder c(config);
        ac4::Decoder reference(reference_config);
        for (std::size_t k = 0; k < 3; ++k) {
            const Outcome good = decode_both(c.decoder, reference, stream.frames[k]);
            CHECK(good.produced);
            CHECK_FALSE(good.concealed);
        }
        // Bytes that are no frame at all: a frame's worth of audio, said to be concealed.
        const Outcome lost = decode_both(c.decoder, reference, garbage);
        CHECK(lost.status == AC3FORGE_OK);
        CHECK(lost.produced);
        CHECK(lost.concealed);
        CHECK(lost.concealment_error == AC3FORGE_ERROR_AC4_DECODE_INVALID_TOC);
        CHECK(lost.action == action);
        // A P-frame of another stream: its counter does not continue this one, so the frame waits
        // for the new source's I-frame.
        const Outcome switched = decode_both(c.decoder, reference, other.frames[5]);
        CHECK(switched.produced);
        CHECK(switched.concealed);
        CHECK(switched.concealment_error == AC3FORGE_ERROR_AC4_DECODE_MISSING_IFRAME);
    };
    SECTION("mute") {
        run(AC3FORGE_AC4_CONCEALMENT_MUTE, ac4::ConcealmentPolicy::kMute,
            AC3FORGE_AC4_CONCEALMENT_ACTION_MUTE);
    }
    SECTION("repeat and fade") {
        run(AC3FORGE_AC4_CONCEALMENT_REPEAT_FADE, ac4::ConcealmentPolicy::kRepeatFade,
            AC3FORGE_AC4_CONCEALMENT_ACTION_REPEAT_FADE);
    }
}

TEST_CASE("AC-4 encoder entry points refuse NULL arguments and report their frames",
          "[capi][ac4]") {
    ac3forge_ac4_encoder_config_t config;
    ac3forge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 96;
    ac3forge_ac4_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_ac4_encoder_create(&config, &encoder) == AC3FORGE_OK);

    const std::vector<std::vector<float>> input = tones({1000.0, 700.0}, 3 * kFrameSamples);
    const std::vector<const float*> views = {input[0].data(), input[1].data()};
    const float* const* channels = views.data();
    const std::size_t samples = input[0].size();
    ac3forge_ac4_encoded_frame_t** frames = nullptr;
    size_t count = 0;

    CHECK(ac3forge_ac4_encoder_encode(nullptr, channels, 2, samples, &frames, &count) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_ac4_encoder_encode(encoder, nullptr, 2, samples, &frames, &count) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_ac4_encoder_encode(encoder, channels, 2, samples, nullptr, &count) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_ac4_encoder_encode(encoder, channels, 2, samples, &frames, nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    const std::array<const float*, 2> with_null = {views[0], nullptr};
    CHECK(ac3forge_ac4_encoder_encode(encoder, with_null.data(), 2, samples, &frames, &count) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    // One channel to a stereo encoder is the encoder's refusal.
    CHECK(ac3forge_ac4_encoder_encode(encoder, channels, 1, samples, &frames, &count) ==
          AC3FORGE_ERROR_AC4_ENCODE_INVALID_INPUT);
    CHECK(frames == nullptr);

    CHECK(ac3forge_ac4_encoder_encode_objects(encoder, nullptr, 2, samples, nullptr, 0, &frames,
                                              &count) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_ac4_encoder_encode_objects(encoder, channels, 2, samples, nullptr, 0, nullptr,
                                              &count) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_ac4_encoder_encode_objects(encoder, channels, 2, samples, nullptr, 0, &frames,
                                              nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_ac4_encoder_flush(nullptr, &frames, &count) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_ac4_encoder_flush(encoder, nullptr, &count) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_ac4_encoder_flush(encoder, &frames, nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(frames == nullptr);

    // The codec mode the encoder settled on is the C++ encoder's.
    ac4::EncoderConfig reference_config;
    reference_config.bitrate_kbps = 96;
    const auto reference = ac4::Encoder::create(reference_config);
    REQUIRE(reference.has_value());
    CHECK(static_cast<int>(ac3forge_ac4_encoder_codec_mode(encoder)) ==
          static_cast<int>(reference->codec_mode()));

    REQUIRE(ac3forge_ac4_encoder_encode(encoder, channels, 2, samples, &frames, &count) ==
            AC3FORGE_OK);
    REQUIRE(count > 1);
    // The first frame is an I-frame, a whole frame of samples, and the rest are not I-frames.
    CHECK(ac3forge_ac4_encoded_frame_data(frames[0]) != nullptr);
    CHECK(ac3forge_ac4_encoded_frame_size(frames[0]) > 0);
    CHECK(ac3forge_ac4_encoded_frame_samples(frames[0]) == static_cast<int>(kFrameSamples));
    CHECK(ac3forge_ac4_encoded_frame_iframe(frames[0]) == 1);
    CHECK(ac3forge_ac4_encoded_frame_iframe(frames[1]) == 0);
    // Each frame destroyed on its own, then the array alone (a count of 0 frees it and no frame).
    for (size_t i = 0; i < count; ++i) {
        ac3forge_ac4_encoded_frame_destroy(frames[i]);
    }
    ac3forge_ac4_encoded_frame_array_destroy(frames, 0);
    ac3forge_ac4_encoder_destroy(encoder);
}

TEST_CASE("the AC-4 carriage helpers take NULL outputs and answer a frame rate with no length",
          "[capi][ac4]") {
    // frame_rate_index 3 is 29.97 fps, whose frames alternate between two lengths: no single
    // samples-per-frame, and Table E.1's time scale of 240 000 for the media timing.
    ac3forge_ac4_encoder_config_t config;
    ac3forge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 96;
    config.frame_rate_index = 3;
    ac3forge_ac4_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_ac4_encoder_create(&config, &encoder) == AC3FORGE_OK);
    const std::vector<std::vector<float>> input = tones({1000.0, 700.0}, 8 * kFrameSamples);
    const std::vector<const float*> views = {input[0].data(), input[1].data()};
    ac3forge_ac4_encoded_frame_t** frames = nullptr;
    size_t count = 0;
    REQUIRE(ac3forge_ac4_encoder_encode(encoder, views.data(), 2, input[0].size(), &frames,
                                        &count) == AC3FORGE_OK);
    REQUIRE(count > 0);
    ac3forge_ac4_encoded_frame_array_destroy(frames, count);

    ac3forge_ac4_toc_t* toc = nullptr;
    CHECK(ac3forge_ac4_encoder_toc(nullptr, &toc) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_ac4_encoder_toc(encoder, nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    REQUIRE(ac3forge_ac4_encoder_toc(encoder, &toc) == AC3FORGE_OK);

    std::uint32_t samples = 99;
    CHECK(ac3forge_ac4_samples_per_frame(toc, &samples) == 0);
    CHECK(samples == 99);
    CHECK(ac3forge_ac4_samples_per_frame(toc, nullptr) == 0);
    std::uint32_t timescale = 0;
    std::uint32_t delta = 0;
    CHECK(ac3forge_ac4_media_timing(toc, &timescale, &delta) == 1);
    CHECK(timescale == 240000);
    CHECK(delta == 8008);
    // Either output may be left out.
    CHECK(ac3forge_ac4_media_timing(toc, nullptr, nullptr) == 1);
    CHECK(ac3forge_ac4_media_timing(toc, &timescale, nullptr) == 1);
    CHECK(ac3forge_ac4_media_timing(toc, nullptr, &delta) == 1);

    // A frame rate with a whole length answers it.
    ac3forge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 96;
    ac3forge_ac4_encoder_t* whole = nullptr;
    REQUIRE(ac3forge_ac4_encoder_create(&config, &whole) == AC3FORGE_OK);
    REQUIRE(ac3forge_ac4_encoder_encode(whole, views.data(), 2, input[0].size(), &frames, &count) ==
            AC3FORGE_OK);
    ac3forge_ac4_encoded_frame_array_destroy(frames, count);
    ac3forge_ac4_toc_t* whole_toc = nullptr;
    REQUIRE(ac3forge_ac4_encoder_toc(whole, &whole_toc) == AC3FORGE_OK);
    CHECK(ac3forge_ac4_samples_per_frame(whole_toc, &samples) == 1);
    CHECK(samples == kFrameSamples);
    CHECK(ac3forge_ac4_samples_per_frame(whole_toc, nullptr) == 1);

    // The dac4 box and the sync frame refuse a NULL argument.
    ac3forge_bytes_t* box = nullptr;
    CHECK(ac3forge_ac4_build_dac4(nullptr, &box) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_ac4_build_dac4(toc, nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(box == nullptr);
    const std::array<std::uint8_t, 4> raw = {1, 2, 3, 4};
    ac3forge_bytes_t* wrapped = nullptr;
    CHECK(ac3forge_ac4_sync_frame(nullptr, raw.size(), 1, &wrapped) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_ac4_sync_frame(raw.data(), raw.size(), 1, nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(wrapped == nullptr);

    ac3forge_ac4_toc_destroy(whole_toc);
    ac3forge_ac4_toc_destroy(toc);
    ac3forge_ac4_encoder_destroy(whole);
    ac3forge_ac4_encoder_destroy(encoder);
}

namespace {

// An enumeration's storage set to a value its enumerators do not name, which a caller's C code can
// store. The bytes are written: converting an int outside the enumeration's range is undefined in
// C++, and so is reading such a value as the enumeration, which the library does not do either
// (internal_ac4.hpp's stored_value()).
template <typename E>
void set_raw(E& target, int value) {
    static_assert(sizeof(E) == sizeof(int));
    std::memcpy(&target, &value, sizeof value);
}

Stream encode_with_cpp(const ac4::EncoderConfig& config,
                       const std::vector<std::vector<float>>& input) {
    Stream out;
    auto encoder = ac4::Encoder::create(config);
    REQUIRE(encoder.has_value());
    const std::vector<std::span<const float>> views(input.begin(), input.end());
    auto frames = encoder->encode(views);
    REQUIRE(frames.has_value());
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    for (const auto* list : {&*frames, &*rest}) {
        for (const ac4::EncodedFrame& frame : *list) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(frame.raw_ac4_frame.data());
            out.frames.emplace_back(bytes, bytes + frame.raw_ac4_frame.size());
        }
    }
    return out;
}

}  // namespace

TEST_CASE("the AC-4 encoder configuration refuses enumerators outside their enumerations",
          "[capi][ac4]") {
    std::array<ac3forge_ac4_object_config_t, 2> objects{};
    for (auto& object : objects) {
        ac3forge_ac4_object_config_init(&object);
    }
    ac3forge_ac4_objects_config_t scene;
    ac3forge_ac4_objects_config_init(&scene);
    scene.objects = objects.data();
    scene.object_count = objects.size();
    scene.coding = AC3FORGE_AC4_OBJECT_CODING_DIRECT;
    ac3forge_ac4_encoder_config_t config;
    ac3forge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 256;
    config.experimental.objects = 1;
    config.objects = &scene;

    const auto create_status = [&config]() {
        ac3forge_ac4_encoder_t* encoder = nullptr;
        const ac3forge_status_t status = ac3forge_ac4_encoder_create(&config, &encoder);
        CHECK((encoder != nullptr) == (status == AC3FORGE_OK));
        ac3forge_ac4_encoder_destroy(encoder);
        return status;
    };
    const auto refused_as_argument = [&](const char* what) {
        INFO(what);
        CHECK(create_status() == AC3FORGE_ERROR_INVALID_ARGUMENT);
        CHECK_FALSE(std::string_view(ac3forge_ac4_encoder_refusal_reason(&config)).empty());
    };
    REQUIRE(create_status() == AC3FORGE_OK);
    CHECK(std::string_view(ac3forge_ac4_encoder_refusal_reason(&config)).empty());

    SECTION("the object coding") {
        set_raw(scene.coding, -1);
        refused_as_argument("coding -1");
        set_raw(scene.coding, 2);
        refused_as_argument("coding 2");
    }
    SECTION("the A-JOC downmix") {
        set_raw(scene.downmix, -1);
        refused_as_argument("downmix -1");
        set_raw(scene.downmix, 3);
        refused_as_argument("downmix 3");
    }
    SECTION("a bed object's channel") {
        objects[0].has_bed = 1;
        set_raw(objects[0].bed, -1);
        refused_as_argument("bed -1");
        // Code 3 of Table 66 is no loudspeaker a bed object can name.
        set_raw(objects[0].bed, 3);
        refused_as_argument("bed 3");
        set_raw(objects[0].bed, 64);
        refused_as_argument("bed 64");
        // Without has_bed the field is not read.
        objects[0].has_bed = 0;
        CHECK(create_status() == AC3FORGE_OK);
    }
    SECTION("the seven-channel experiment's pair") {
        set_raw(config.experimental.seven_x, -1);
        refused_as_argument("seven_x -1");
        set_raw(config.experimental.seven_x, 4);
        refused_as_argument("seven_x 4");
    }
}

TEST_CASE("the AC-4 encoder configuration's optional object fields reach the encoder",
          "[capi][ac4]") {
    // A-JOC over three dynamic objects and a computed downmix of two, with the optional fields
    // set: the parameter bands, their coarse quantisation and the common data's screen size ratio.
    std::array<ac3forge_ac4_object_config_t, 3> objects{};
    for (auto& object : objects) {
        ac3forge_ac4_object_config_init(&object);
    }
    ac3forge_ac4_objects_config_t scene;
    ac3forge_ac4_objects_config_init(&scene);
    scene.objects = objects.data();
    scene.object_count = objects.size();
    scene.has_downmix_signals = 1;
    scene.downmix_signals = 2;
    scene.has_parameter_bands = 1;
    scene.parameter_bands = 15;
    scene.has_coarse = 1;
    scene.coarse = 1;
    scene.has_screen_size_ratio_code = 1;
    scene.screen_size_ratio_code = 10;
    ac3forge_ac4_encoder_config_t config;
    ac3forge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 256;
    config.experimental.objects = 1;
    config.objects = &scene;

    ac4::ObjectsConfig reference_objects;
    reference_objects.objects.resize(3);
    reference_objects.downmix_signals = 2;
    reference_objects.parameter_bands = 15;
    reference_objects.coarse = true;
    reference_objects.screen_size_ratio_code = 10;
    ac4::SubstreamConfig substream;
    substream.objects = reference_objects;
    ac4::EncoderConfig reference;
    reference.bitrate_kbps = 256;
    reference.experimental.objects = true;
    reference.substreams = {substream};
    CHECK(ac3forge_ac4_encoder_refusal_reason(&config) ==
          std::string(ac4::Encoder::refusal_reason(reference)));

    const std::vector<std::vector<float>> input = tones({562.5, 1312.5, 2062.5}, 6 * kFrameSamples);
    const Stream stream = encode_with_c_api(config, input);
    const Stream expected = encode_with_cpp(reference, input);
    REQUIRE_FALSE(expected.frames.empty());
    CHECK((stream.frames == expected.frames));

    // Each optional field is read: with one left unset the stream is not the same.
    scene.has_coarse = 0;
    CHECK((encode_with_c_api(config, input).frames != expected.frames));
    scene.has_coarse = 1;
    scene.has_screen_size_ratio_code = 0;
    CHECK((encode_with_c_api(config, input).frames != expected.frames));
}
