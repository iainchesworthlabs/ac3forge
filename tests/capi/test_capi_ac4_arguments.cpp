// The AC-4 C entry points' argument and error arms, and the accessors the round trips in
// test_capi.cpp do not reach: what a NULL argument, an index past the end, an enumerator outside
// its enumeration, a stream that does not decode, a decoder setting, or a change of one while a
// stream plays leaves the call to answer. Each answer is held against the C++ API the entry point
// wraps: the same bytes go to iclforge::ac4::Decoder, the same configuration to
// iclforge::ac4::Encoder, and the two must agree. The cases carry test_capi.cpp's [capi][ac4] tags.

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

#include "iclforge_c/iclforge.h"
#include "iclforge/ac4/ac4.hpp"
#include "iclforge/ac4dec/decoder.hpp"
#include "iclforge/ac4enc/encoder.hpp"

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
Stream encode_with_c_api(const iclforge_ac4_encoder_config_t& config,
                         const std::vector<std::vector<float>>& input) {
    Stream out;
    iclforge_ac4_encoder_t* encoder = nullptr;
    INFO(iclforge_ac4_encoder_refusal_reason(&config));
    REQUIRE(iclforge_ac4_encoder_create(&config, &encoder) == ICLFORGE_OK);
    std::vector<const float*> views;
    for (const auto& channel : input) {
        views.push_back(channel.data());
    }
    const auto take = [&out](iclforge_ac4_encoded_frame_t** frames, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            const std::uint8_t* bytes = iclforge_ac4_encoded_frame_data(frames[i]);
            REQUIRE(bytes != nullptr);
            out.frames.emplace_back(bytes, bytes + iclforge_ac4_encoded_frame_size(frames[i]));
        }
        iclforge_ac4_encoded_frame_array_destroy(frames, count);
    };
    iclforge_ac4_encoded_frame_t** frames = nullptr;
    size_t count = 0;
    const iclforge_status_t status =
        config.objects != nullptr
            ? iclforge_ac4_encoder_encode_objects(encoder, views.data(), views.size(),
                                                  input.front().size(), nullptr, 0, &frames, &count)
            : iclforge_ac4_encoder_encode(encoder, views.data(), views.size(), input.front().size(),
                                          &frames, &count);
    REQUIRE(status == ICLFORGE_OK);
    take(frames, count);
    frames = nullptr;
    count = 0;
    REQUIRE(iclforge_ac4_encoder_flush(encoder, &frames, &count) == ICLFORGE_OK);
    take(frames, count);
    iclforge_ac4_encoder_destroy(encoder);
    return out;
}

// Ten frames of a stereo pair at 96 kbps, the first the only I-frame.
Stream stereo_stream(double first_hz = 1000.0, double second_hz = 700.0) {
    iclforge_ac4_encoder_config_t config;
    iclforge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 96;
    Stream stream = encode_with_c_api(config, tones({first_hz, second_hz}, 10 * kFrameSamples));
    REQUIRE(stream.frames.size() >= 8);
    return stream;
}

// Two dynamic objects and the LFE, direct-coded, six frames of a tone each.
Stream object_stream() {
    std::array<iclforge_ac4_object_config_t, 3> objects{};
    for (auto& object : objects) {
        iclforge_ac4_object_config_init(&object);
    }
    objects[1].lfe = 1;
    objects[2].properties.x = 0.25;
    iclforge_ac4_objects_config_t scene;
    iclforge_ac4_objects_config_init(&scene);
    scene.objects = objects.data();
    scene.object_count = objects.size();
    scene.coding = ICLFORGE_AC4_OBJECT_CODING_DIRECT;
    iclforge_ac4_encoder_config_t config;
    iclforge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 256;
    config.experimental.objects = 1;
    config.objects = &scene;
    // A QMF subband's middle for each dynamic object, and 47 Hz for the LFE.
    return encode_with_c_api(config, tones({562.5, 47.0, 1312.5}, 6 * kFrameSamples));
}

// A decoded frame, destroyed on every way out of the scope that reads it.
struct FrameGuard {
    explicit FrameGuard(iclforge_ac4_decoded_frame_t* f) : frame(f) {}
    FrameGuard(const FrameGuard&) = delete;
    FrameGuard& operator=(const FrameGuard&) = delete;
    ~FrameGuard() { iclforge_ac4_decoded_frame_destroy(frame); }
    iclforge_ac4_decoded_frame_t* frame;
};

// A decoder of the C API, destroyed with the scope.
struct CDecoder {
    explicit CDecoder(const iclforge_ac4_decoder_config_t& config) {
        REQUIRE(iclforge_ac4_decoder_create(&config, &decoder) == ICLFORGE_OK);
    }
    CDecoder(const CDecoder&) = delete;
    CDecoder& operator=(const CDecoder&) = delete;
    ~CDecoder() { iclforge_ac4_decoder_destroy(decoder); }
    iclforge_ac4_decoder_t* decoder = nullptr;
};

iclforge_ac4_decoder_config_t default_decoder_config() {
    iclforge_ac4_decoder_config_t config;
    iclforge_ac4_decoder_config_init(&config);
    return config;
}

// The status a C caller is meant to see for each of the C++ decoder's errors, from the header.
iclforge_status_t status_of(iclforge::ac4::DecodeError error) {
    switch (error) {
        case iclforge::ac4::DecodeError::kTruncated: return ICLFORGE_ERROR_AC4_DECODE_TRUNCATED;
        case iclforge::ac4::DecodeError::kInvalidToc: return ICLFORGE_ERROR_AC4_DECODE_INVALID_TOC;
        case iclforge::ac4::DecodeError::kInvalidStream:
            return ICLFORGE_ERROR_AC4_DECODE_INVALID_STREAM;
        case iclforge::ac4::DecodeError::kUnsupported: return ICLFORGE_ERROR_AC4_DECODE_UNSUPPORTED;
        case iclforge::ac4::DecodeError::kMissingIFrame:
            return ICLFORGE_ERROR_AC4_DECODE_MISSING_IFRAME;
    }
    return ICLFORGE_ERROR_INTERNAL;
}

// Every accessor of a decoded frame against the C++ frame it wraps, and each one past the last
// channel, object and update.
void check_frame(const iclforge_ac4_decoded_frame_t* frame,
                 const iclforge::ac4::DecodedFrame& want) {
    CHECK(iclforge_ac4_decoded_frame_sample_rate_hz(frame) == want.sample_rate_hz);
    CHECK(iclforge_ac4_decoded_frame_sequence_counter(frame) == want.sequence_counter);
    CHECK(iclforge_ac4_decoded_frame_presentation_index(frame) == want.presentation);
    CHECK((iclforge_ac4_decoded_frame_has_presentation_id(frame) != 0) ==
          want.presentation_id.has_value());
    CHECK(iclforge_ac4_decoded_frame_presentation_id(frame) == want.presentation_id.value_or(0));
    CHECK(iclforge_ac4_decoded_frame_samples_per_channel(frame) == want.samples);

    const std::size_t channels = iclforge_ac4_decoded_frame_channel_count(frame);
    REQUIRE(channels == want.channels.size());
    for (std::size_t c = 0; c < channels; ++c) {
        CHECK(static_cast<int>(iclforge_ac4_decoded_frame_speaker(frame, c)) ==
              static_cast<int>(want.speakers[c]));
        const float* pcm = iclforge_ac4_decoded_frame_channel_samples(frame, c);
        REQUIRE(pcm != nullptr);
        CHECK(std::equal(want.channels[c].begin(), want.channels[c].end(), pcm));
    }
    CHECK(iclforge_ac4_decoded_frame_channel_samples(frame, channels) == nullptr);
    CHECK(iclforge_ac4_decoded_frame_speaker(frame, channels) == ICLFORGE_AC4_SPEAKER_LEFT);

    CHECK((iclforge_ac4_decoded_frame_has_concealed(frame) != 0) == want.concealed.has_value());
    if (want.concealed.has_value()) {
        CHECK(static_cast<int>(iclforge_ac4_decoded_frame_concealment_action(frame)) ==
              static_cast<int>(want.concealed->action));
        CHECK(iclforge_ac4_decoded_frame_concealment_error(frame) ==
              status_of(want.concealed->error));
    } else {
        CHECK(iclforge_ac4_decoded_frame_concealment_action(frame) ==
              ICLFORGE_AC4_CONCEALMENT_ACTION_MUTE);
        CHECK(iclforge_ac4_decoded_frame_concealment_error(frame) == ICLFORGE_OK);
    }

    const std::size_t objects = iclforge_ac4_decoded_frame_object_count(frame);
    REQUIRE(objects == want.objects.size());
    for (std::size_t o = 0; o < objects; ++o) {
        const iclforge::ac4::DecodedObject& object = want.objects[o];
        CHECK(static_cast<int>(iclforge_ac4_decoded_frame_object_kind(frame, o)) ==
              static_cast<int>(object.kind));
        CHECK((iclforge_ac4_decoded_frame_object_lfe(frame, o) != 0) == object.lfe);
        CHECK((iclforge_ac4_decoded_frame_object_has_speaker(frame, o) != 0) ==
              object.speaker.has_value());
        CHECK(static_cast<int>(iclforge_ac4_decoded_frame_object_speaker(frame, o)) ==
              static_cast<int>(object.speaker.value_or(iclforge::ac4::Speaker::kLeft)));
        const float* pcm = iclforge_ac4_decoded_frame_object_samples(frame, o);
        REQUIRE(pcm != nullptr);
        CHECK(std::equal(object.samples.begin(), object.samples.end(), pcm));
        const iclforge_ac4_object_properties_t properties =
            iclforge_ac4_decoded_frame_object_properties(frame, o);
        CHECK((properties.active != 0) == object.properties.active);
        CHECK(properties.gain_db == object.properties.gain_db);
        CHECK(properties.priority == object.properties.priority);
        CHECK(properties.x == object.properties.position[0]);
        CHECK(properties.y == object.properties.position[1]);
        CHECK(properties.z == object.properties.position[2]);
        const std::size_t updates = iclforge_ac4_decoded_frame_object_update_count(frame, o);
        REQUIRE(updates == object.updates.size());
        for (std::size_t u = 0; u < updates; ++u) {
            const iclforge_ac4_object_update_t update =
                iclforge_ac4_decoded_frame_object_update(frame, o, u);
            CHECK(update.sample == object.updates[u].sample);
            CHECK(update.ramp_samples == object.updates[u].ramp_samples);
            CHECK(update.properties.gain_db == object.updates[u].properties.gain_db);
        }
        // An update past the last is sample 0, ramp 0 and the default properties.
        const iclforge_ac4_object_update_t past =
            iclforge_ac4_decoded_frame_object_update(frame, o, updates);
        CHECK(past.sample == 0);
        CHECK(past.ramp_samples == 0);
        CHECK(past.properties.depth_exponent == 1.0);
    }
    // An object past the last has the defaults the header names.
    CHECK(iclforge_ac4_decoded_frame_object_kind(frame, objects) == ICLFORGE_AC4_OBJECT_DYN);
    CHECK(iclforge_ac4_decoded_frame_object_lfe(frame, objects) == 0);
    CHECK(iclforge_ac4_decoded_frame_object_has_speaker(frame, objects) == 0);
    CHECK(iclforge_ac4_decoded_frame_object_speaker(frame, objects) == ICLFORGE_AC4_SPEAKER_LEFT);
    CHECK(iclforge_ac4_decoded_frame_object_samples(frame, objects) == nullptr);
    CHECK(iclforge_ac4_decoded_frame_object_properties(frame, objects).depth_exponent == 1.0);
    CHECK(iclforge_ac4_decoded_frame_object_update_count(frame, objects) == 0);
    CHECK(iclforge_ac4_decoded_frame_object_update(frame, objects, 0).ramp_samples == 0);
}

// What one frame did in the two decoders.
struct Outcome {
    iclforge_status_t status = ICLFORGE_OK;
    bool produced = false;
    bool concealed = false;
    iclforge_status_t concealment_error = ICLFORGE_OK;
    iclforge_ac4_concealment_action_t action = ICLFORGE_AC4_CONCEALMENT_ACTION_MUTE;
    std::vector<std::vector<float>> pcm;  // the frame's channels, when it produced one
};

// `frame` decoded by the C API's `decoder` and by `reference`: the two must answer alike, and the
// frame, where there is one, reads alike through every accessor.
Outcome decode_both(iclforge_ac4_decoder_t* decoder, iclforge::ac4::Decoder& reference,
                    std::span<const std::uint8_t> frame) {
    Outcome out;
    const auto want = reference.decode(bytes_of(frame));
    iclforge_ac4_decoded_frame_t* raw = nullptr;
    out.status = iclforge_ac4_decoder_decode(decoder, frame.data(), frame.size(), &raw);
    const FrameGuard guard(raw);
    CHECK(std::string_view(iclforge_ac4_decoder_refusal_reason(decoder)) ==
          reference.refusal_reason());
    if (!want.has_value()) {
        CHECK(out.status == status_of(want.error()));
        CHECK(raw == nullptr);
        return out;
    }
    CHECK(out.status == ICLFORGE_OK);
    CHECK((raw != nullptr) == want->has_value());
    if (raw != nullptr && want->has_value()) {
        out.produced = true;
        out.concealed = iclforge_ac4_decoded_frame_has_concealed(raw) != 0;
        out.concealment_error = iclforge_ac4_decoded_frame_concealment_error(raw);
        out.action = iclforge_ac4_decoded_frame_concealment_action(raw);
        out.pcm = (*want)->channels;
        check_frame(raw, **want);
    }
    return out;
}

// The decoder's presentations, one past the last included, against the C++ decoder's.
void check_presentations(const iclforge_ac4_decoder_t* decoder,
                         const iclforge::ac4::Decoder& reference) {
    const std::span<const iclforge::ac4::PresentationInfo> want = reference.presentations();
    REQUIRE(iclforge_ac4_decoder_presentation_count(decoder) == want.size());
    for (std::size_t p = 0; p < want.size(); ++p) {
        const iclforge::ac4::PresentationInfo& info = want[p];
        CHECK(iclforge_ac4_decoder_presentation_toc_index(decoder, p) == info.index);
        CHECK((iclforge_ac4_decoder_presentation_has_id(decoder, p) != 0) ==
              info.presentation_id.has_value());
        CHECK(iclforge_ac4_decoder_presentation_id(decoder, p) == info.presentation_id.value_or(0));
        CHECK((iclforge_ac4_decoder_presentation_has_md_compat(decoder, p) != 0) ==
              info.md_compat.has_value());
        CHECK(iclforge_ac4_decoder_presentation_md_compat(decoder, p) ==
              info.md_compat.value_or(0));
        CHECK((iclforge_ac4_decoder_presentation_enabled(decoder, p) != 0) == info.enabled);
        CHECK((iclforge_ac4_decoder_presentation_alternative(decoder, p) != 0) == info.alternative);
        CHECK((iclforge_ac4_decoder_presentation_pre_virtualized(decoder, p) != 0) ==
              info.pre_virtualized);
        CHECK(std::string_view(iclforge_ac4_decoder_presentation_name(decoder, p)) == info.name);
        CHECK(std::string_view(iclforge_ac4_decoder_presentation_language(decoder, p)) ==
              info.language);
        CHECK((iclforge_ac4_decoder_presentation_decodable(decoder, p) != 0) == info.decodable);
        CHECK((iclforge_ac4_decoder_presentation_selectable(decoder, p) != 0) == info.selectable);
        REQUIRE(iclforge_ac4_decoder_presentation_speaker_count(decoder, p) ==
                info.speakers.size());
        for (std::size_t s = 0; s < info.speakers.size(); ++s) {
            CHECK(static_cast<int>(iclforge_ac4_decoder_presentation_speaker(decoder, p, s)) ==
                  static_cast<int>(info.speakers[s]));
        }
        CHECK(iclforge_ac4_decoder_presentation_speaker(decoder, p, info.speakers.size()) ==
              ICLFORGE_AC4_SPEAKER_LEFT);
    }
    // A presentation past the last has 0, no name and no language, and is neither enabled nor
    // decodable.
    const std::size_t past = want.size();
    CHECK(iclforge_ac4_decoder_presentation_toc_index(decoder, past) == 0);
    CHECK(iclforge_ac4_decoder_presentation_has_id(decoder, past) == 0);
    CHECK(iclforge_ac4_decoder_presentation_id(decoder, past) == 0);
    CHECK(iclforge_ac4_decoder_presentation_has_md_compat(decoder, past) == 0);
    CHECK(iclforge_ac4_decoder_presentation_md_compat(decoder, past) == 0);
    CHECK(iclforge_ac4_decoder_presentation_enabled(decoder, past) == 0);
    CHECK(iclforge_ac4_decoder_presentation_alternative(decoder, past) == 0);
    CHECK(iclforge_ac4_decoder_presentation_pre_virtualized(decoder, past) == 0);
    CHECK(std::string_view(iclforge_ac4_decoder_presentation_name(decoder, past)).empty());
    CHECK(std::string_view(iclforge_ac4_decoder_presentation_language(decoder, past)).empty());
    CHECK(iclforge_ac4_decoder_presentation_decodable(decoder, past) == 0);
    CHECK(iclforge_ac4_decoder_presentation_selectable(decoder, past) == 0);
    CHECK(iclforge_ac4_decoder_presentation_speaker_count(decoder, past) == 0);
    CHECK(iclforge_ac4_decoder_presentation_speaker(decoder, past, 0) == ICLFORGE_AC4_SPEAKER_LEFT);
}

// The loudness metadata the last frames sent, against the C++ decoder's.
void check_loudness(const iclforge_ac4_decoder_t* decoder,
                    const iclforge::ac4::Decoder& reference) {
    const iclforge_ac4_loudness_info_t got = iclforge_ac4_decoder_metadata_loudness(decoder);
    const iclforge::ac4::LoudnessInfo& want = reference.metadata().loudness;
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

TEST_CASE(
    "AC-4 decoded frame and presentation accessors report what iclforge::ac4::Decoder decodes",
    "[capi][ac4]") {
    const Stream stream = stereo_stream();
    const CDecoder c(default_decoder_config());
    iclforge::ac4::Decoder reference;

    // Before a frame: no delay, no presentation, no loudness, and nothing refused.
    CHECK(iclforge_ac4_decoder_latency_samples(c.decoder) == 0);
    CHECK(iclforge_ac4_decoder_presentation_count(c.decoder) == 0);
    CHECK(std::string_view(iclforge_ac4_decoder_refusal_reason(c.decoder)).empty());
    CHECK(iclforge_ac4_decoder_metadata_loudness(c.decoder).has_dialnorm_dbfs == 0);

    std::size_t produced = 0;
    for (const std::vector<std::uint8_t>& frame : stream.frames) {
        if (decode_both(c.decoder, reference, frame).produced) {
            ++produced;
        }
    }
    REQUIRE(produced >= 6);
    CHECK(iclforge_ac4_decoder_latency_samples(c.decoder) == reference.latency_samples());
    CHECK(iclforge_ac4_decoder_latency_samples(c.decoder) > 0);
    check_presentations(c.decoder, reference);
    check_loudness(c.decoder, reference);
}

TEST_CASE("AC-4 decoded object frames report their objects and the indices past them",
          "[capi][ac4]") {
    const Stream stream = object_stream();
    REQUIRE(stream.frames.size() >= 4);
    const CDecoder c(default_decoder_config());
    iclforge::ac4::Decoder reference;
    std::size_t with_objects = 0;
    for (const std::vector<std::uint8_t>& frame : stream.frames) {
        iclforge_ac4_decoded_frame_t* raw = nullptr;
        const auto want = reference.decode(bytes_of(frame));
        REQUIRE(want.has_value());
        REQUIRE(iclforge_ac4_decoder_decode(c.decoder, frame.data(), frame.size(), &raw) ==
                ICLFORGE_OK);
        const FrameGuard guard(raw);
        if (raw == nullptr || !want->has_value()) {
            continue;
        }
        check_frame(raw, **want);
        if (iclforge_ac4_decoded_frame_object_count(raw) == 3) {
            ++with_objects;
        }
    }
    CHECK(with_objects >= 3);
}

namespace {

// Every field of the output configuration away from its default, and the C++ struct that says the
// same.
iclforge_ac4_output_config_t settled_output() {
    iclforge_ac4_output_config_t output;
    iclforge_ac4_output_config_init(&output);
    output.has_output_level_dbfs = 1;
    output.output_level_dbfs = -24.0;
    output.drc = ICLFORGE_AC4_DRC_PORTABLE_SPEAKERS;
    output.headphones = 1;
    output.dialogue_enhancement_db = 3.0;
    output.downmix = ICLFORGE_AC4_DOWNMIX_MONO;
    output.mix_lfe = 0;
    output.dialogue_gain_db = -3.0;
    output.associated_gain_db = -6.0;
    return output;
}

iclforge::ac4::OutputConfig settled_output_cpp() {
    iclforge::ac4::OutputConfig output;
    output.output_level_dbfs = -24.0;
    output.drc = iclforge::ac4::DrcMode::kPortableSpeakers;
    output.headphones = true;
    output.dialogue_enhancement_db = 3.0;
    output.downmix = iclforge::ac4::DownmixTarget::kMono;
    output.mix_lfe = false;
    output.dialogue_gain_db = -3.0;
    output.associated_gain_db = -6.0;
    return output;
}

// Every field of the presentation choice set. The stream has one presentation, so the choice the
// preferences make is the same whichever of them the decoder takes first.
iclforge_ac4_presentation_choice_t settled_choice() {
    iclforge_ac4_presentation_choice_t choice;
    iclforge_ac4_presentation_choice_init(&choice);
    choice.has_presentation_id = 1;
    choice.presentation_id = 7;
    choice.has_index = 1;
    choice.index = 0;
    choice.language = "eng";
    choice.has_associated = 1;
    choice.associated = 2;
    choice.associated_type = ICLFORGE_AC4_ASSOCIATED_SPOKEN_SUBTITLES;
    choice.headphones = 1;
    return choice;
}

iclforge::ac4::PresentationChoice settled_choice_cpp() {
    iclforge::ac4::PresentationChoice choice;
    choice.presentation_id = 7;
    choice.index = 0;
    choice.language = "eng";
    choice.associated = 2;
    choice.associated_type = iclforge::ac4::AssociatedType::kSpokenSubtitles;
    choice.headphones = true;
    return choice;
}

}  // namespace

TEST_CASE("the AC-4 decoder's configuration and live settings reach the decoder", "[capi][ac4]") {
    const Stream stream = stereo_stream();
    const auto decode_all = [&stream](iclforge_ac4_decoder_t* decoder,
                                      iclforge::ac4::Decoder& reference, std::size_t from,
                                      std::size_t to) {
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
    iclforge::ac4::Decoder plain_reference;
    const std::vector<std::vector<float>> plain_pcm =
        decode_all(plain.decoder, plain_reference, 0, 6);
    REQUIRE(plain_pcm.size() == 2);

    SECTION("the configuration a decoder is created with") {
        iclforge_ac4_decoder_config_t config = default_decoder_config();
        config.output = settled_output();
        config.presentation = settled_choice();
        config.concealment = ICLFORGE_AC4_CONCEALMENT_REPEAT_FADE;
        config.level = 2;
        config.decoding = ICLFORGE_AC4_DECODING_CORE;
        iclforge::ac4::DecoderConfig reference_config;
        reference_config.output = settled_output_cpp();
        reference_config.presentation = settled_choice_cpp();
        reference_config.concealment = iclforge::ac4::ConcealmentPolicy::kRepeatFade;
        reference_config.level = 2;
        reference_config.decoding = iclforge::ac4::DecodingMode::kCore;
        const CDecoder c(config);
        iclforge::ac4::Decoder reference(reference_config);
        const std::vector<std::vector<float>> pcm = decode_all(c.decoder, reference, 0, 6);
        // The mono downmix and the output level are in the decoded audio.
        REQUIRE(pcm.size() == 1);
        CHECK((pcm != plain_pcm));
    }
    SECTION("settings changed while a stream plays") {
        const CDecoder c(default_decoder_config());
        iclforge::ac4::Decoder reference;
        decode_all(c.decoder, reference, 0, 2);

        iclforge_ac4_output_config_t output = settled_output();
        iclforge_ac4_decoder_set_output(c.decoder, &output);
        reference.set_output(settled_output_cpp());
        const std::vector<std::vector<float>> mono = decode_all(c.decoder, reference, 2, 4);
        CHECK(mono.size() == 1);

        iclforge_ac4_presentation_choice_t choice = settled_choice();
        iclforge_ac4_decoder_set_presentation(c.decoder, &choice);
        reference.set_presentation(settled_choice_cpp());
        decode_all(c.decoder, reference, 4, 6);

        // A NULL decoder or a NULL setting changes nothing.
        iclforge_ac4_decoder_set_output(nullptr, &output);
        iclforge_ac4_decoder_set_output(c.decoder, nullptr);
        iclforge_ac4_decoder_set_output(nullptr, nullptr);
        iclforge_ac4_decoder_set_presentation(nullptr, &choice);
        iclforge_ac4_decoder_set_presentation(c.decoder, nullptr);
        iclforge_ac4_decoder_set_presentation(nullptr, nullptr);
        iclforge_ac4_decoder_reset(nullptr);
        decode_all(c.decoder, reference, 6, 8);
    }
    SECTION("reset forgets the stream") {
        const CDecoder c(default_decoder_config());
        iclforge::ac4::Decoder reference;
        const std::vector<std::vector<float>> first = decode_all(c.decoder, reference, 0, 1);
        decode_all(c.decoder, reference, 1, 4);
        REQUIRE(iclforge_ac4_decoder_latency_samples(c.decoder) > 0);
        iclforge_ac4_decoder_reset(c.decoder);
        reference.reset();
        CHECK(iclforge_ac4_decoder_latency_samples(c.decoder) == 0);
        CHECK(iclforge_ac4_decoder_presentation_count(c.decoder) == reference.presentations().size());
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

TEST_CASE(
    "AC-4 decode refuses NULL arguments and answers a damaged frame as iclforge::ac4::Decoder does",
    "[capi][ac4]") {
    const Stream stream = stereo_stream();
    const iclforge_ac4_decoder_config_t config = default_decoder_config();

    SECTION("NULL arguments") {
        const CDecoder c(config);
        iclforge_ac4_decoded_frame_t* out = nullptr;
        const std::vector<std::uint8_t>& frame = stream.frames[0];
        iclforge_ac4_decoder_t* made = nullptr;
        CHECK(iclforge_ac4_decoder_create(nullptr, &made) == ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK(iclforge_ac4_decoder_create(&config, nullptr) == ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK(made == nullptr);
        CHECK(iclforge_ac4_decoder_decode(nullptr, frame.data(), frame.size(), &out) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK(iclforge_ac4_decoder_decode(c.decoder, nullptr, frame.size(), &out) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK(iclforge_ac4_decoder_decode(c.decoder, frame.data(), frame.size(), nullptr) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK(out == nullptr);
    }
    SECTION("the initialisers take NULL") {
        // Documented no-ops, checked by their not crashing.
        iclforge_ac4_output_config_init(nullptr);
        iclforge_ac4_presentation_choice_init(nullptr);
        iclforge_ac4_decoder_config_init(nullptr);
        iclforge_ac4_encoder_config_init(nullptr);
    }
    SECTION("a P-frame with no I-frame before it is held back and not refused") {
        const CDecoder c(config);
        iclforge::ac4::Decoder reference;
        const Outcome outcome = decode_both(c.decoder, reference, stream.frames[3]);
        CHECK(outcome.status == ICLFORGE_OK);
        CHECK_FALSE(outcome.produced);
    }
    SECTION("a frame of no bytes") {
        const CDecoder c(config);
        iclforge::ac4::Decoder reference;
        const Outcome outcome = decode_both(
            c.decoder, reference, std::span<const std::uint8_t>(stream.frames[0].data(), 0));
        CHECK(outcome.status == ICLFORGE_ERROR_AC4_DECODE_INVALID_TOC);
    }
    SECTION("damage to an I-frame and to a P-frame after three good frames") {
        std::size_t refused = 0;
        std::size_t decoded = 0;
        for (const std::size_t position : {std::size_t{0}, std::size_t{3}}) {
            for (const std::vector<std::uint8_t>& bad : damaged(stream.frames[position])) {
                const CDecoder c(config);
                iclforge::ac4::Decoder reference;
                for (std::size_t k = 0; k < position; ++k) {
                    decode_both(c.decoder, reference, stream.frames[k]);
                }
                const Outcome outcome = decode_both(c.decoder, reference, bad);
                if (outcome.status != ICLFORGE_OK) {
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

    const auto run = [&](iclforge_ac4_concealment_policy_t policy,
                         iclforge::ac4::ConcealmentPolicy reference_policy,
                         iclforge_ac4_concealment_action_t action) {
        iclforge_ac4_decoder_config_t config = default_decoder_config();
        config.concealment = policy;
        iclforge::ac4::DecoderConfig reference_config;
        reference_config.concealment = reference_policy;
        const CDecoder c(config);
        iclforge::ac4::Decoder reference(reference_config);
        for (std::size_t k = 0; k < 3; ++k) {
            const Outcome good = decode_both(c.decoder, reference, stream.frames[k]);
            CHECK(good.produced);
            CHECK_FALSE(good.concealed);
        }
        // Bytes that are no frame at all: a frame's worth of audio, said to be concealed.
        const Outcome lost = decode_both(c.decoder, reference, garbage);
        CHECK(lost.status == ICLFORGE_OK);
        CHECK(lost.produced);
        CHECK(lost.concealed);
        CHECK(lost.concealment_error == ICLFORGE_ERROR_AC4_DECODE_INVALID_TOC);
        CHECK(lost.action == action);
        // A P-frame of another stream: its counter does not continue this one, so the frame waits
        // for the new source's I-frame.
        const Outcome switched = decode_both(c.decoder, reference, other.frames[5]);
        CHECK(switched.produced);
        CHECK(switched.concealed);
        CHECK(switched.concealment_error == ICLFORGE_ERROR_AC4_DECODE_MISSING_IFRAME);
    };
    SECTION("mute") {
        run(ICLFORGE_AC4_CONCEALMENT_MUTE, iclforge::ac4::ConcealmentPolicy::kMute,
            ICLFORGE_AC4_CONCEALMENT_ACTION_MUTE);
    }
    SECTION("repeat and fade") {
        run(ICLFORGE_AC4_CONCEALMENT_REPEAT_FADE, iclforge::ac4::ConcealmentPolicy::kRepeatFade,
            ICLFORGE_AC4_CONCEALMENT_ACTION_REPEAT_FADE);
    }
}

TEST_CASE("AC-4 encoder entry points refuse NULL arguments and report their frames",
          "[capi][ac4]") {
    iclforge_ac4_encoder_config_t config;
    iclforge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 96;
    iclforge_ac4_encoder_t* encoder = nullptr;
    REQUIRE(iclforge_ac4_encoder_create(&config, &encoder) == ICLFORGE_OK);

    const std::vector<std::vector<float>> input = tones({1000.0, 700.0}, 3 * kFrameSamples);
    const std::vector<const float*> views = {input[0].data(), input[1].data()};
    const float* const* channels = views.data();
    const std::size_t samples = input[0].size();
    iclforge_ac4_encoded_frame_t** frames = nullptr;
    size_t count = 0;

    CHECK(iclforge_ac4_encoder_encode(nullptr, channels, 2, samples, &frames, &count) ==
          ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_ac4_encoder_encode(encoder, nullptr, 2, samples, &frames, &count) ==
          ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_ac4_encoder_encode(encoder, channels, 2, samples, nullptr, &count) ==
          ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_ac4_encoder_encode(encoder, channels, 2, samples, &frames, nullptr) ==
          ICLFORGE_ERROR_INVALID_ARGUMENT);
    const std::array<const float*, 2> with_null = {views[0], nullptr};
    CHECK(iclforge_ac4_encoder_encode(encoder, with_null.data(), 2, samples, &frames, &count) ==
          ICLFORGE_ERROR_INVALID_ARGUMENT);
    // One channel to a stereo encoder is the encoder's refusal.
    CHECK(iclforge_ac4_encoder_encode(encoder, channels, 1, samples, &frames, &count) ==
          ICLFORGE_ERROR_AC4_ENCODE_INVALID_INPUT);
    CHECK(frames == nullptr);

    CHECK(iclforge_ac4_encoder_encode_objects(encoder, nullptr, 2, samples, nullptr, 0, &frames,
                                              &count) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_ac4_encoder_encode_objects(encoder, channels, 2, samples, nullptr, 0, nullptr,
                                              &count) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_ac4_encoder_encode_objects(encoder, channels, 2, samples, nullptr, 0, &frames,
                                              nullptr) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_ac4_encoder_flush(nullptr, &frames, &count) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_ac4_encoder_flush(encoder, nullptr, &count) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_ac4_encoder_flush(encoder, &frames, nullptr) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(frames == nullptr);

    // The codec mode the encoder settled on is the C++ encoder's.
    iclforge::ac4::EncoderConfig reference_config;
    reference_config.bitrate_kbps = 96;
    const auto reference = iclforge::ac4::Encoder::create(reference_config);
    REQUIRE(reference.has_value());
    CHECK(static_cast<int>(iclforge_ac4_encoder_codec_mode(encoder)) ==
          static_cast<int>(reference->codec_mode()));

    REQUIRE(iclforge_ac4_encoder_encode(encoder, channels, 2, samples, &frames, &count) ==
            ICLFORGE_OK);
    REQUIRE(count > 1);
    // The first frame is an I-frame, a whole frame of samples, and the rest are not I-frames.
    CHECK(iclforge_ac4_encoded_frame_data(frames[0]) != nullptr);
    CHECK(iclforge_ac4_encoded_frame_size(frames[0]) > 0);
    CHECK(iclforge_ac4_encoded_frame_samples(frames[0]) == static_cast<int>(kFrameSamples));
    CHECK(iclforge_ac4_encoded_frame_iframe(frames[0]) == 1);
    CHECK(iclforge_ac4_encoded_frame_iframe(frames[1]) == 0);
    // Each frame destroyed on its own, then the array alone (a count of 0 frees it and no frame).
    for (size_t i = 0; i < count; ++i) {
        iclforge_ac4_encoded_frame_destroy(frames[i]);
    }
    iclforge_ac4_encoded_frame_array_destroy(frames, 0);
    iclforge_ac4_encoder_destroy(encoder);
}

TEST_CASE("the AC-4 carriage helpers take NULL outputs and answer a frame rate with no length",
          "[capi][ac4]") {
    // frame_rate_index 3 is 29.97 fps, whose frames alternate between two lengths: no single
    // samples-per-frame, and Table E.1's time scale of 240 000 for the media timing.
    iclforge_ac4_encoder_config_t config;
    iclforge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 96;
    config.frame_rate_index = 3;
    iclforge_ac4_encoder_t* encoder = nullptr;
    REQUIRE(iclforge_ac4_encoder_create(&config, &encoder) == ICLFORGE_OK);
    const std::vector<std::vector<float>> input = tones({1000.0, 700.0}, 8 * kFrameSamples);
    const std::vector<const float*> views = {input[0].data(), input[1].data()};
    iclforge_ac4_encoded_frame_t** frames = nullptr;
    size_t count = 0;
    REQUIRE(iclforge_ac4_encoder_encode(encoder, views.data(), 2, input[0].size(), &frames,
                                        &count) == ICLFORGE_OK);
    REQUIRE(count > 0);
    iclforge_ac4_encoded_frame_array_destroy(frames, count);

    iclforge_ac4_toc_t* toc = nullptr;
    CHECK(iclforge_ac4_encoder_toc(nullptr, &toc) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_ac4_encoder_toc(encoder, nullptr) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    REQUIRE(iclforge_ac4_encoder_toc(encoder, &toc) == ICLFORGE_OK);

    std::uint32_t samples = 99;
    CHECK(iclforge_ac4_samples_per_frame(toc, &samples) == 0);
    CHECK(samples == 99);
    CHECK(iclforge_ac4_samples_per_frame(toc, nullptr) == 0);
    std::uint32_t timescale = 0;
    std::uint32_t delta = 0;
    CHECK(iclforge_ac4_media_timing(toc, &timescale, &delta) == 1);
    CHECK(timescale == 240000);
    CHECK(delta == 8008);
    // Either output may be left out.
    CHECK(iclforge_ac4_media_timing(toc, nullptr, nullptr) == 1);
    CHECK(iclforge_ac4_media_timing(toc, &timescale, nullptr) == 1);
    CHECK(iclforge_ac4_media_timing(toc, nullptr, &delta) == 1);

    // A frame rate with a whole length answers it.
    iclforge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 96;
    iclforge_ac4_encoder_t* whole = nullptr;
    REQUIRE(iclforge_ac4_encoder_create(&config, &whole) == ICLFORGE_OK);
    REQUIRE(iclforge_ac4_encoder_encode(whole, views.data(), 2, input[0].size(), &frames, &count) ==
            ICLFORGE_OK);
    iclforge_ac4_encoded_frame_array_destroy(frames, count);
    iclforge_ac4_toc_t* whole_toc = nullptr;
    REQUIRE(iclforge_ac4_encoder_toc(whole, &whole_toc) == ICLFORGE_OK);
    CHECK(iclforge_ac4_samples_per_frame(whole_toc, &samples) == 1);
    CHECK(samples == kFrameSamples);
    CHECK(iclforge_ac4_samples_per_frame(whole_toc, nullptr) == 1);

    // The dac4 box and the sync frame refuse a NULL argument.
    iclforge_bytes_t* box = nullptr;
    CHECK(iclforge_ac4_build_dac4(nullptr, &box) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_ac4_build_dac4(toc, nullptr) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(box == nullptr);
    const std::array<std::uint8_t, 4> raw = {1, 2, 3, 4};
    iclforge_bytes_t* wrapped = nullptr;
    CHECK(iclforge_ac4_sync_frame(nullptr, raw.size(), 1, &wrapped) ==
          ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_ac4_sync_frame(raw.data(), raw.size(), 1, nullptr) ==
          ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(wrapped == nullptr);

    iclforge_ac4_toc_destroy(whole_toc);
    iclforge_ac4_toc_destroy(toc);
    iclforge_ac4_encoder_destroy(whole);
    iclforge_ac4_encoder_destroy(encoder);
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

Stream encode_with_cpp(const iclforge::ac4::EncoderConfig& config,
                       const std::vector<std::vector<float>>& input) {
    Stream out;
    auto encoder = iclforge::ac4::Encoder::create(config);
    REQUIRE(encoder.has_value());
    const std::vector<std::span<const float>> views(input.begin(), input.end());
    auto frames = encoder->encode(views);
    REQUIRE(frames.has_value());
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    for (const auto* list : {&*frames, &*rest}) {
        for (const iclforge::ac4::EncodedFrame& frame : *list) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(frame.raw_ac4_frame.data());
            out.frames.emplace_back(bytes, bytes + frame.raw_ac4_frame.size());
        }
    }
    return out;
}

}  // namespace

TEST_CASE("the AC-4 encoder configuration refuses enumerators outside their enumerations",
          "[capi][ac4]") {
    std::array<iclforge_ac4_object_config_t, 2> objects{};
    for (auto& object : objects) {
        iclforge_ac4_object_config_init(&object);
    }
    iclforge_ac4_objects_config_t scene;
    iclforge_ac4_objects_config_init(&scene);
    scene.objects = objects.data();
    scene.object_count = objects.size();
    scene.coding = ICLFORGE_AC4_OBJECT_CODING_DIRECT;
    iclforge_ac4_encoder_config_t config;
    iclforge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 256;
    config.experimental.objects = 1;
    config.objects = &scene;

    const auto create_status = [&config]() {
        iclforge_ac4_encoder_t* encoder = nullptr;
        const iclforge_status_t status = iclforge_ac4_encoder_create(&config, &encoder);
        CHECK((encoder != nullptr) == (status == ICLFORGE_OK));
        iclforge_ac4_encoder_destroy(encoder);
        return status;
    };
    const auto refused_as_argument = [&](const char* what) {
        INFO(what);
        CHECK(create_status() == ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK_FALSE(std::string_view(iclforge_ac4_encoder_refusal_reason(&config)).empty());
    };
    REQUIRE(create_status() == ICLFORGE_OK);
    CHECK(std::string_view(iclforge_ac4_encoder_refusal_reason(&config)).empty());

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
        CHECK(create_status() == ICLFORGE_OK);
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
    std::array<iclforge_ac4_object_config_t, 3> objects{};
    for (auto& object : objects) {
        iclforge_ac4_object_config_init(&object);
    }
    iclforge_ac4_objects_config_t scene;
    iclforge_ac4_objects_config_init(&scene);
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
    iclforge_ac4_encoder_config_t config;
    iclforge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 256;
    config.experimental.objects = 1;
    config.objects = &scene;

    iclforge::ac4::ObjectsConfig reference_objects;
    reference_objects.objects.resize(3);
    reference_objects.downmix_signals = 2;
    reference_objects.parameter_bands = 15;
    reference_objects.coarse = true;
    reference_objects.screen_size_ratio_code = 10;
    iclforge::ac4::SubstreamConfig substream;
    substream.objects = reference_objects;
    iclforge::ac4::EncoderConfig reference;
    reference.bitrate_kbps = 256;
    reference.experimental.objects = true;
    reference.substreams = {substream};
    CHECK(iclforge_ac4_encoder_refusal_reason(&config) ==
          std::string(iclforge::ac4::Encoder::refusal_reason(reference)));

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

TEST_CASE("the AC-4 encoder refuses an object with a depth exponent and no screen factor",
          "[capi][ac4]") {
    // The screen factor and the depth exponent are one group of fields whose factor has no code for
    // 0 (src/ac4enc/ERRATA.md): an exponent other than 1 needs a factor of 1/8 or more. A
    // configuration without one answers ICLFORGE_ERROR_AC4_ENCODE_INVALID_CONFIG and names the
    // reason, and an update ICLFORGE_ERROR_AC4_ENCODE_INVALID_INPUT.
    std::array<iclforge_ac4_object_config_t, 2> objects{};
    for (auto& object : objects) {
        iclforge_ac4_object_config_init(&object);
    }
    iclforge_ac4_objects_config_t scene;
    iclforge_ac4_objects_config_init(&scene);
    scene.objects = objects.data();
    scene.object_count = objects.size();
    scene.coding = ICLFORGE_AC4_OBJECT_CODING_DIRECT;
    iclforge_ac4_encoder_config_t config;
    iclforge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 256;
    config.experimental.objects = 1;
    config.objects = &scene;

    const auto create_status = [&config]() {
        iclforge_ac4_encoder_t* encoder = nullptr;
        const iclforge_status_t status = iclforge_ac4_encoder_create(&config, &encoder);
        CHECK((encoder != nullptr) == (status == ICLFORGE_OK));
        iclforge_ac4_encoder_destroy(encoder);
        return status;
    };
    const auto reason = [&config]() {
        return std::string_view(iclforge_ac4_encoder_refusal_reason(&config));
    };
    REQUIRE(create_status() == ICLFORGE_OK);
    for (const double exponent : {0.25, 0.5, 2.0}) {
        CAPTURE(exponent);
        objects[1].properties.depth_exponent = exponent;
        objects[1].properties.screen_factor = 0.0;
        CHECK(create_status() == ICLFORGE_ERROR_AC4_ENCODE_INVALID_CONFIG);
        CHECK(reason().find("a screen factor of 0") != std::string_view::npos);
        objects[1].properties.screen_factor = 0.125;
        CHECK(create_status() == ICLFORGE_OK);
        CHECK(reason().empty());
    }

    // An update with such properties is invalid input, and the encoder goes on to take one with a
    // factor.
    objects[1].properties.depth_exponent = 0.5;
    objects[1].properties.screen_factor = 0.5;
    iclforge_ac4_encoder_t* encoder = nullptr;
    REQUIRE(iclforge_ac4_encoder_create(&config, &encoder) == ICLFORGE_OK);
    const std::vector<std::vector<float>> input = tones({562.5, 1312.5}, 2 * kFrameSamples);
    std::vector<const float*> views;
    for (const auto& channel : input) {
        views.push_back(channel.data());
    }
    iclforge_ac4_object_metadata_update_t update;
    iclforge_ac4_object_metadata_update_init(&update);
    update.object = 1;
    update.sample = 100;
    update.properties = objects[1].properties;
    update.properties.screen_factor = 0.0;
    iclforge_ac4_encoded_frame_t** frames = nullptr;
    size_t count = 0;
    CHECK(iclforge_ac4_encoder_encode_objects(encoder, views.data(), views.size(),
                                              input.front().size(), &update, 1, &frames,
                                              &count) == ICLFORGE_ERROR_AC4_ENCODE_INVALID_INPUT);
    update.properties.screen_factor = 0.125;
    REQUIRE(iclforge_ac4_encoder_encode_objects(encoder, views.data(), views.size(),
                                                input.front().size(), &update, 1, &frames,
                                                &count) == ICLFORGE_OK);
    iclforge_ac4_encoded_frame_array_destroy(frames, count);
    iclforge_ac4_encoder_destroy(encoder);
}
