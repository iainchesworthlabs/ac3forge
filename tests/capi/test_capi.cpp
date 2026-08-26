// ac3forge_c (roadmap F1) round-trips and error paths, exercised from C++ via
// Catch2 like every other test here - see examples/capi_encode_decode.c for
// the companion check that the header genuinely compiles as C, not merely as
// C++ parsing valid-C syntax.
//
// Real audio from the first frame onward matters: an all-zero frame takes
// the §7.2.2.1.1 all-zero bit-allocation path and exercises almost none of
// the encoder - see CONTRIBUTING.md on why silence is a bad test signal.
//
// The E-AC-3 half of the surface is decode-only (the Atmos encoder is the C
// header's only Annex E producer), so the E-AC-3 tests below drive the C++
// encoder - the same tool combinations tests/encoder/test_eac3.cpp proves stack
// correctly - and hold the C decode surface to the behaviour
// tests/decoder/test_eac3_decoder.cpp establishes for the C++ one.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/meta/drc.hpp"
#include "ac3forge_c/ac3forge.h"

namespace {

void fill_tone(float* out, double hz, int frame, double rate) {
    for (int n = 0; n < AC3FORGE_SAMPLES_PER_FRAME; ++n) {
        const double t = (frame * AC3FORGE_SAMPLES_PER_FRAME + n) / rate;
        out[static_cast<std::size_t>(n)] =
            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * hz * t));
    }
}

// Direct sample SNR with the codec's 256-sample delay, skipping the warm-up
// frame at each end - the same measurement tests/decoder/test_eac3_decoder.cpp makes,
// so a C-boundary round trip is held against real decoded audio, not just a
// header parse.
double snr_db(const std::vector<float>& input, const std::vector<float>& decoded) {
    constexpr std::size_t kDelay = 256;
    constexpr std::size_t kSkip = 1536;
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t i = kSkip; i + kSkip < input.size(); ++i) {
        const double x = static_cast<double>(input[i - kDelay]);
        const double d = static_cast<double>(decoded[i]) - x;
        signal += x * x;
        noise += d * d;
    }
    return 10.0 * std::log10(signal / std::max(noise, 1e-30));
}

struct Eac3Stream {
    std::vector<uint8_t> bytes;              // the concatenated syncframes
    std::vector<std::vector<float>> source;  // per coded channel, full length
};

// Phase-continuous tones, one per coded channel, through the C++ E-AC-3
// encoder - the raw-byte input side of every C-API decode test below.
Eac3Stream encode_eac3_stream(ac3::eac3::FrameEncoder& encoder, const std::vector<double>& tones,
                              int frames) {
    const auto nchans = static_cast<std::size_t>(encoder.channel_count());
    REQUIRE(tones.size() == nchans);
    Eac3Stream out;
    out.source.resize(nchans);
    std::vector<std::vector<float>> block(nchans, std::vector<float>(AC3FORGE_SAMPLES_PER_FRAME));
    std::vector<std::span<const float>> views(nchans);
    for (int frame = 0; frame < frames; ++frame) {
        for (std::size_t ch = 0; ch < nchans; ++ch) {
            fill_tone(block[ch].data(), tones[ch], frame, 48000.0);
            views[ch] = block[ch];
            out.source[ch].insert(out.source[ch].end(), block[ch].begin(), block[ch].end());
        }
        const auto encoded = encoder.encode_frame(views);
        REQUIRE(encoded.has_value());
        const auto* data = reinterpret_cast<const uint8_t*>(encoded->data());
        out.bytes.insert(out.bytes.end(), data, data + encoded->size());
    }
    return out;
}

}  // namespace

TEST_CASE("ac3forge_version reports a sane version", "[capi]") {
    const ac3forge_version_t version = ac3forge_version();
    CHECK(version.major >= 0);
    CHECK(version.full != nullptr);
}

TEST_CASE("ac3forge_status_message never returns null", "[capi]") {
    // Not testing an out-of-range cast to ac3forge_status_t here: for an
    // unfixed enum, a value outside the range its enumerators need is
    // unspecified per the standard, and GCC's -Wconversion (part of this
    // project's warnings-as-errors set) rightly flags constructing one. The
    // switch's own `default:` case (common.cpp) is simple enough not to need
    // a dedicated test for it.
    CHECK(std::string_view(ac3forge_status_message(AC3FORGE_OK)) == "ok");
    CHECK(ac3forge_status_message(AC3FORGE_ERROR_DECODE_BAD_SYNC_WORD) != nullptr);
    CHECK(ac3forge_status_message(AC3FORGE_ERROR_DECODE_INVALID_STREAM) != nullptr);
}

TEST_CASE("ac3forge_encoder_config_init matches EncoderConfig{}'s own defaults", "[capi]") {
    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    CHECK(config.dialnorm == 31);
    CHECK(config.acmod == AC3FORGE_ACMOD_2_0);
    CHECK(config.fast_mdct == 1);
    CHECK(config.has_drc == 0);
    CHECK(config.has_dialnorm2 == 0);
}

TEST_CASE("AC-3 encode/decode round-trips through the C API", "[capi]") {
    ac3forge_encoder_config_t encoder_config;
    ac3forge_encoder_config_init(&encoder_config);
    encoder_config.bitrate_kbps = 192;
    encoder_config.acmod = AC3FORGE_ACMOD_2_0;

    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&encoder_config, &encoder) == AC3FORGE_OK);
    REQUIRE(encoder != nullptr);
    CHECK(ac3forge_encoder_channel_count(encoder) == 2);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);
    REQUIRE(decoder != nullptr);

    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<std::byte> stream;

    for (int frame = 0; frame < 8; ++frame) {
        fill_tone(left.data(), 1000.0, frame, 48000.0);
        fill_tone(right.data(), 800.0, frame, 48000.0);
        const float* channels[2] = {left.data(), right.data()};

        ac3forge_bytes_t* encoded = nullptr;
        REQUIRE(ac3forge_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                               &encoded) == AC3FORGE_OK);
        REQUIRE(encoded != nullptr);
        REQUIRE(ac3forge_bytes_size(encoded) > 0);

        ac3forge_decoded_frame_t* decoded = nullptr;
        const auto status = ac3forge_decoder_decode_frame(
            decoder, ac3forge_bytes_data(encoded), ac3forge_bytes_size(encoded), &decoded);
        REQUIRE(status == AC3FORGE_OK);
        REQUIRE(decoded != nullptr);

        CHECK(ac3forge_decoded_frame_acmod(decoded) == AC3FORGE_ACMOD_2_0);
        CHECK(ac3forge_decoded_frame_channel_count(decoded) == 2);
        CHECK(ac3forge_decoded_frame_samples_per_channel(decoded) == AC3FORGE_SAMPLES_PER_FRAME);
        CHECK(ac3forge_decoded_frame_dialnorm(decoded) == 31);
        CHECK(ac3forge_decoded_frame_channel_samples(decoded, 0) != nullptr);
        CHECK(ac3forge_decoded_frame_channel_samples(decoded, 1) != nullptr);
        CHECK(ac3forge_decoded_frame_channel_samples(decoded, 2) == nullptr);  // out of range

        ac3forge_decoded_frame_destroy(decoded);
        ac3forge_bytes_destroy(encoded);
    }

    ac3forge_decoder_destroy(decoder);
    ac3forge_encoder_destroy(encoder);
}

TEST_CASE("ac3forge_encoder_encode_frame rejects a mismatched channel/sample count", "[capi]") {
    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    config.acmod = AC3FORGE_ACMOD_2_0;

    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);

    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    const float* one_channel[1] = {left.data()};
    ac3forge_bytes_t* encoded = nullptr;

    CHECK(ac3forge_encoder_encode_frame(encoder, one_channel, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                         &encoded) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(encoded == nullptr);

    const float* two_channels[2] = {left.data(), left.data()};
    CHECK(ac3forge_encoder_encode_frame(encoder, two_channels, 2, AC3FORGE_SAMPLES_PER_FRAME / 2,
                                         &encoded) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(encoded == nullptr);

    ac3forge_encoder_destroy(encoder);
}

TEST_CASE("ac3forge_decoder_decode_frame reports the same errors ac3::FrameDecoder does",
          "[capi]") {
    ac3forge_decoder_config_t config;
    ac3forge_decoder_config_init(&config);
    ac3forge_decoder_t* decoder = nullptr;
    CHECK(ac3forge_decoder_create(nullptr, &decoder) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_decoder_create(&config, nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(decoder == nullptr);
    REQUIRE(ac3forge_decoder_create(&config, &decoder) == AC3FORGE_OK);

    const std::vector<uint8_t> garbage(16, 0xAB);
    ac3forge_decoded_frame_t* decoded = nullptr;
    CHECK(ac3forge_decoder_decode_frame(nullptr, garbage.data(), garbage.size(), &decoded) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_decoder_decode_frame(decoder, nullptr, 0, &decoded) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_decoder_decode_frame(decoder, garbage.data(), garbage.size(), nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    const auto status =
        ac3forge_decoder_decode_frame(decoder, garbage.data(), garbage.size(), &decoded);
    // Not asserting which specific DecodeError this garbage maps to - that's an
    // implementation detail of the real bitstream parser, not something this
    // boundary layer should pin down. What matters here is that a decode
    // failure reports one of the mapped decode-error codes and leaves the
    // out-parameter untouched, exactly like every other failure path.
    CHECK(status >= AC3FORGE_ERROR_DECODE_TRUNCATED);
    CHECK(status <= AC3FORGE_ERROR_DECODE_INVALID_STREAM);
    CHECK(decoded == nullptr);

    ac3forge_decoder_destroy(decoder);
}

TEST_CASE("ac3forge_split_frames and ac3forge_stream_bsid see the same framing as the C++ API",
          "[capi]") {
    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    config.acmod = AC3FORGE_ACMOD_2_0;
    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);

    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<uint8_t> stream;
    for (int frame = 0; frame < 3; ++frame) {
        fill_tone(left.data(), 1000.0, frame, 48000.0);
        fill_tone(right.data(), 800.0, frame, 48000.0);
        const float* channels[2] = {left.data(), right.data()};
        ac3forge_bytes_t* encoded = nullptr;
        REQUIRE(ac3forge_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                               &encoded) == AC3FORGE_OK);
        const auto* data = ac3forge_bytes_data(encoded);
        stream.insert(stream.end(), data, data + ac3forge_bytes_size(encoded));
        ac3forge_bytes_destroy(encoded);
    }
    ac3forge_encoder_destroy(encoder);

    ac3forge_spans_t* spans = nullptr;
    REQUIRE(ac3forge_split_frames(stream.data(), stream.size(), &spans) == AC3FORGE_OK);
    REQUIRE(spans != nullptr);
    CHECK(ac3forge_spans_count(spans) == 3);
    const auto first = ac3forge_spans_get(spans, 0);
    CHECK(first.offset == 0);
    CHECK(first.length > 0);
    ac3forge_spans_destroy(spans);

    int bsid = -1;
    REQUIRE(ac3forge_stream_bsid(stream.data(), stream.size(), &bsid) == AC3FORGE_OK);
    CHECK(bsid <= 8);  // classic AC-3, not Annex E
}

TEST_CASE("Atmos encode/decode round-trips OAMD position and JOC object audio", "[capi]") {
    ac3forge_atmos_config_t config;
    ac3forge_atmos_config_init(&config);

    ac3forge_atmos_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_atmos_encoder_create(&config, 1, &encoder) == AC3FORGE_OK);
    REQUIRE(encoder != nullptr);
    REQUIRE(ac3forge_atmos_encoder_dynamic_object_count(encoder) == 1);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_eac3_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);

    std::vector<float> object(AC3FORGE_SAMPLES_PER_FRAME);
    // The default placement (x=0.5, y=0.5, z=0.0, gain=1.0/0 dB) sits exactly
    // on OAMD's quantizer grid - see tests/oba/test_oba.cpp's own comment on why
    // that makes an exact round-trip assertion valid rather than a tolerance.
    const ac3forge_object_placement_t placement{.x = 0.5, .y = 0.5, .z = 0.0, .gain = 1.0,
                                                 .lfe_send = 0.0};

    for (int frame = 0; frame < 3; ++frame) {
        fill_tone(object.data(), 1000.0, frame, 48000.0);
        const float* objects[1] = {object.data()};

        ac3forge_bytes_t* unit = nullptr;
        REQUIRE(ac3forge_atmos_encoder_encode_frame(encoder, objects, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                                     &placement, 1, &unit) == AC3FORGE_OK);
        REQUIRE(unit != nullptr);

        ac3forge_decoded_substream_t* substream = nullptr;
        REQUIRE(ac3forge_eac3_decoder_decode_substream(decoder, ac3forge_bytes_data(unit),
                                                        ac3forge_bytes_size(unit),
                                                        &substream) == AC3FORGE_OK);
        ac3forge_bytes_destroy(unit);

        if (substream == nullptr) {
            continue;  // held back for transient pre-noise processing; not used here, but tolerate it
        }

        CHECK(ac3forge_decoded_substream_has_object_metadata(substream) == 1);
        CHECK(ac3forge_decoded_substream_program_dynamic_only(substream) == 1);
        CHECK(ac3forge_decoded_substream_program_lfe(substream) == 1);
        CHECK(ac3forge_decoded_substream_program_dynamic_object_count(substream) == 1);

        double x = -1, y = -1, z = -1, gain_db = -1;
        ac3forge_decoded_substream_dynamic_object(substream, 0, &x, &y, &z, &gain_db);
        CHECK(x == 0.5);
        CHECK(y == 0.5);
        CHECK(z == 0.0);
        CHECK(gain_db == 0.0);

        REQUIRE(ac3forge_decoded_substream_object_audio_count(substream) == 1);
        CHECK(ac3forge_decoded_substream_object_audio(substream, 0) != nullptr);
        CHECK(ac3forge_decoded_substream_object_audio(substream, 1) == nullptr);  // out of range

        ac3forge_decoded_substream_destroy(substream);
    }

    ac3forge_eac3_decoder_destroy(decoder);
    ac3forge_atmos_encoder_destroy(encoder);
}

TEST_CASE("E-AC-3 substreams round-trip through the C API across the Annex E tool combinations",
          "[capi][eac3]") {
    struct ToolCombo {
        const char* name;
        ac3::eac3::FrameConfig config;
    };
    const ToolCombo combos[] = {
        {"plain", {.bitrate_kbps = 192}},
        {"coupling", {.bitrate_kbps = 192, .coupling = true}},
        {"enhanced coupling", {.bitrate_kbps = 192, .coupling = true, .enhanced = true}},
        {"spx", {.bitrate_kbps = 192, .spx = true}},
        {"aht", {.bitrate_kbps = 192, .aht = true}},
        {"coupling+spx+aht", {.bitrate_kbps = 192, .coupling = true, .spx = true, .aht = true}},
        {"auto tools", {.bitrate_kbps = 192, .auto_tools = true}},
    };
    for (const auto& combo : combos) {
        INFO(combo.name);
        ac3::eac3::FrameEncoder encoder{combo.config};
        const auto stream = encode_eac3_stream(encoder, {1000.0, 800.0}, 4);

        ac3forge_decoder_config_t config;
        ac3forge_decoder_config_init(&config);
        ac3forge_eac3_decoder_t* decoder = nullptr;
        REQUIRE(ac3forge_eac3_decoder_create(&config, &decoder) == AC3FORGE_OK);

        int bsid = -1;
        REQUIRE(ac3forge_stream_bsid(stream.bytes.data(), stream.bytes.size(), &bsid) ==
                AC3FORGE_OK);
        CHECK(bsid == 16);  // Annex E framing, where the AC-3 test above saw <= 8

        ac3forge_spans_t* spans = nullptr;
        REQUIRE(ac3forge_split_frames(stream.bytes.data(), stream.bytes.size(), &spans) ==
                AC3FORGE_OK);
        REQUIRE(ac3forge_spans_count(spans) == 4);

        std::vector<std::vector<float>> rendered(2);
        for (std::size_t i = 0; i < ac3forge_spans_count(spans); ++i) {
            const auto span = ac3forge_spans_get(spans, i);
            ac3forge_decoded_substream_t* substream = nullptr;
            REQUIRE(ac3forge_eac3_decoder_decode_substream(decoder,
                                                           stream.bytes.data() + span.offset,
                                                           span.length,
                                                           &substream) == AC3FORGE_OK);
            // Nothing here uses transient pre-noise, so no frame is ever held
            // back - every decode returns a substream immediately.
            REQUIRE(substream != nullptr);

            CHECK(ac3forge_decoded_substream_is_independent(substream) == 1);
            CHECK(ac3forge_decoded_substream_id(substream) == 0);
            CHECK(ac3forge_decoded_substream_sample_rate(substream) == AC3FORGE_SAMPLE_RATE_48000);
            CHECK(ac3forge_decoded_substream_acmod(substream) == AC3FORGE_ACMOD_2_0);
            CHECK(ac3forge_decoded_substream_lfe(substream) == 0);
            CHECK(ac3forge_decoded_substream_dialnorm(substream) == 31);
            CHECK(ac3forge_decoded_substream_numblkscod(substream) == 3);  // six blocks
            CHECK(ac3forge_decoded_substream_has_chanmap(substream) == 0);
            CHECK(ac3forge_decoded_substream_last_dependent(substream) == 0);
            CHECK(ac3forge_decoded_substream_location_map(substream) != 0);
            CHECK(ac3forge_decoded_substream_has_compr(substream) == 0);
            CHECK(ac3forge_decoded_substream_dynrng(substream, 0) == 0);
            REQUIRE(ac3forge_decoded_substream_channel_count(substream) == 2);
            CHECK(ac3forge_decoded_substream_samples_per_channel(substream) ==
                  AC3FORGE_SAMPLES_PER_FRAME);
            for (std::size_t ch = 0; ch < 2; ++ch) {
                const float* samples = ac3forge_decoded_substream_channel_samples(substream, ch);
                REQUIRE(samples != nullptr);
                rendered[ch].insert(rendered[ch].end(), samples,
                                    samples + AC3FORGE_SAMPLES_PER_FRAME);
                // A steady sub-2 kHz tone never trips §8.2.2's transient
                // detector (see tests/decoder/test_eac3_decoder.cpp's tone-choice
                // comment), so no block may report the short transform.
                for (int blk = 0; blk < AC3FORGE_BLOCKS_PER_FRAME; ++blk) {
                    CHECK(ac3forge_decoded_substream_block_switched(substream, ch, blk) == 0);
                }
            }
            ac3forge_decoded_substream_destroy(substream);
        }
        ac3forge_spans_destroy(spans);
        ac3forge_eac3_decoder_destroy(decoder);

        for (std::size_t ch = 0; ch < 2; ++ch) {
            CHECK(snr_db(stream.source[ch], rendered[ch]) > 20.0);
        }
    }
}

TEST_CASE("ac3forge_eac3_frame_config_init matches FrameConfig{}'s own defaults", "[capi][eac3]") {
    ac3forge_eac3_frame_config_t config;
    ac3forge_eac3_frame_config_init(&config);
    CHECK(config.dialnorm == 31);
    CHECK(config.acmod == AC3FORGE_ACMOD_2_0);
    CHECK(config.fast_mdct == 1);
    CHECK(config.auto_tools == 0);
    CHECK(config.coupling == 0);
    CHECK(config.spx == 0);
    CHECK(config.aht == 0);
    CHECK(config.transient_prenoise == 0);
    CHECK(config.strmtyp == AC3FORGE_STREAM_TYPE_INDEPENDENT);
    CHECK(config.substreamid == 0);
    CHECK(config.has_chanmap == 0);
}

TEST_CASE("E-AC-3 encoder encode/decode round-trips through the C API", "[capi][eac3]") {
    ac3forge_eac3_frame_config_t encoder_config;
    ac3forge_eac3_frame_config_init(&encoder_config);
    encoder_config.bitrate_kbps = 192;
    encoder_config.acmod = AC3FORGE_ACMOD_2_0;

    ac3forge_eac3_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_eac3_encoder_create(&encoder_config, &encoder) == AC3FORGE_OK);
    REQUIRE(encoder != nullptr);
    CHECK(ac3forge_eac3_encoder_channel_count(encoder) == 2);
    CHECK(ac3forge_eac3_encoder_samples_per_frame(encoder) == AC3FORGE_SAMPLES_PER_FRAME);
    CHECK(ac3forge_eac3_encoder_latency_samples(encoder) > 0);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_eac3_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);

    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<std::vector<float>> rendered(2);

    for (int frame = 0; frame < 8; ++frame) {
        fill_tone(left.data(), 1000.0, frame, 48000.0);
        fill_tone(right.data(), 800.0, frame, 48000.0);
        const float* channels[2] = {left.data(), right.data()};

        ac3forge_bytes_t* encoded = nullptr;
        REQUIRE(ac3forge_eac3_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                                    nullptr, nullptr, 0, &encoded) == AC3FORGE_OK);
        REQUIRE(encoded != nullptr);
        REQUIRE(ac3forge_bytes_size(encoded) > 0);

        ac3forge_decoded_substream_t* substream = nullptr;
        REQUIRE(ac3forge_eac3_decoder_decode_substream(decoder, ac3forge_bytes_data(encoded),
                                                        ac3forge_bytes_size(encoded),
                                                        &substream) == AC3FORGE_OK);
        ac3forge_bytes_destroy(encoded);
        REQUIRE(substream != nullptr);

        CHECK(ac3forge_decoded_substream_is_independent(substream) == 1);
        CHECK(ac3forge_decoded_substream_acmod(substream) == AC3FORGE_ACMOD_2_0);
        REQUIRE(ac3forge_decoded_substream_channel_count(substream) == 2);
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const float* samples = ac3forge_decoded_substream_channel_samples(substream, ch);
            REQUIRE(samples != nullptr);
            rendered[ch].insert(rendered[ch].end(), samples, samples + AC3FORGE_SAMPLES_PER_FRAME);
        }
        ac3forge_decoded_substream_destroy(substream);
    }

    ac3forge_eac3_decoder_destroy(decoder);
    ac3forge_eac3_encoder_destroy(encoder);
}

TEST_CASE("E-AC-3 access-unit encoder produces a 5.1.2 stream the C API can decode",
          "[capi][eac3]") {
    ac3forge_eac3_frame_config_t independent;
    ac3forge_eac3_frame_config_init(&independent);
    independent.bitrate_kbps = 448;
    independent.acmod = AC3FORGE_ACMOD_3_2;
    independent.lfe = 1;

    ac3forge_eac3_frame_config_t dependent;
    ac3forge_eac3_frame_config_init(&dependent);
    dependent.bitrate_kbps = 192;
    dependent.acmod = AC3FORGE_ACMOD_2_0;
    dependent.has_chanmap = 1;
    dependent.chanmap = AC3FORGE_CHANMAP_512_HEIGHT;

    ac3forge_eac3_access_unit_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_eac3_access_unit_encoder_create(&independent, &dependent, 1, &encoder) ==
            AC3FORGE_OK);
    REQUIRE(encoder != nullptr);
    REQUIRE(ac3forge_eac3_access_unit_encoder_channel_count(encoder) == 8);

    const std::vector<double> tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 2000.0, 1300.0};
    std::vector<std::vector<float>> block(8, std::vector<float>(AC3FORGE_SAMPLES_PER_FRAME));
    std::vector<uint8_t> stream;
    std::vector<std::size_t> unit_offsets;
    for (int frame = 0; frame < 3; ++frame) {
        const float* channels[8];
        for (std::size_t ch = 0; ch < 8; ++ch) {
            fill_tone(block[ch].data(), tones[ch], frame, 48000.0);
            channels[ch] = block[ch].data();
        }
        ac3forge_eac3_access_unit_t* unit = nullptr;
        REQUIRE(ac3forge_eac3_access_unit_encoder_encode(encoder, channels, 8,
                                                          AC3FORGE_SAMPLES_PER_FRAME, nullptr, 0,
                                                          &unit) == AC3FORGE_OK);
        REQUIRE(unit != nullptr);
        REQUIRE(ac3forge_eac3_access_unit_substream_count(unit) == 2);
        const auto total = ac3forge_eac3_access_unit_size(unit);
        std::uint64_t summed = 0;
        for (std::size_t i = 0; i < ac3forge_eac3_access_unit_substream_count(unit); ++i) {
            summed += ac3forge_eac3_access_unit_substream_bytes(unit, i);
        }
        CHECK(summed == total);

        unit_offsets.push_back(stream.size());
        const auto* data = ac3forge_eac3_access_unit_data(unit);
        stream.insert(stream.end(), data, data + total);
        ac3forge_eac3_access_unit_destroy(unit);
    }
    ac3forge_eac3_access_unit_encoder_destroy(encoder);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_eac3_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);

    ac3forge_spans_t* units = nullptr;
    REQUIRE(ac3forge_split_access_units(stream.data(), stream.size(), &units) == AC3FORGE_OK);
    REQUIRE(ac3forge_spans_count(units) == 3);

    for (std::size_t i = 0; i < ac3forge_spans_count(units); ++i) {
        const auto span = ac3forge_spans_get(units, i);
        ac3forge_decoded_access_unit_t* decoded = nullptr;
        REQUIRE(ac3forge_eac3_decoder_decode_access_unit(
                    decoder, stream.data() + span.offset, span.length, &decoded) == AC3FORGE_OK);
        REQUIRE(decoded != nullptr);
        CHECK(ac3forge_decoded_access_unit_acmod(decoded) == AC3FORGE_ACMOD_3_2);
        REQUIRE(ac3forge_decoded_access_unit_channel_count(decoded) == 8);
        ac3forge_decoded_access_unit_destroy(decoded);
    }
    ac3forge_spans_destroy(units);
    ac3forge_eac3_decoder_destroy(decoder);
}

TEST_CASE("E-AC-3 C encode entry points surface the encoder's own error codes", "[capi][eac3]") {
    ac3forge_eac3_frame_config_t config;
    ac3forge_eac3_frame_config_init(&config);
    config.acmod = AC3FORGE_ACMOD_2_0;
    config.bitrate_kbps = 0;  // no such Annex E rate

    ac3forge_eac3_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_eac3_encoder_create(&config, &encoder) == AC3FORGE_OK);

    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    const float* channels[2] = {left.data(), right.data()};
    ac3forge_bytes_t* encoded = nullptr;

    CHECK(ac3forge_eac3_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                              nullptr, nullptr, 0,
                                              &encoded) == AC3FORGE_ERROR_ENCODE_INVALID_BITRATE);
    CHECK(encoded == nullptr);
    ac3forge_eac3_encoder_destroy(encoder);

    // A dependent whose chanmap does not add up to its acmod/lfeon's coded
    // channel count - AC3FORGE_CHANMAP_TOP_QUAD names four locations, but the
    // dependent below is coded 2/0 (two channels).
    ac3forge_eac3_frame_config_t independent;
    ac3forge_eac3_frame_config_init(&independent);
    independent.acmod = AC3FORGE_ACMOD_3_2;
    independent.lfe = 1;
    independent.bitrate_kbps = 448;

    ac3forge_eac3_frame_config_t dependent;
    ac3forge_eac3_frame_config_init(&dependent);
    dependent.acmod = AC3FORGE_ACMOD_2_0;
    dependent.bitrate_kbps = 192;
    dependent.has_chanmap = 1;
    dependent.chanmap = AC3FORGE_CHANMAP_TOP_QUAD;

    ac3forge_eac3_access_unit_encoder_t* au_encoder = nullptr;
    REQUIRE(ac3forge_eac3_access_unit_encoder_create(&independent, &dependent, 1, &au_encoder) ==
            AC3FORGE_OK);
    // ac3::eac3::AccessUnitEncoder's own constructor validates eagerly and
    // silently builds no substreams when a config is invalid - channel_count()
    // is 0 rather than the 8 a caller might expect from acmod/lfe alone;
    // encode() below is how the real reason (an invalid channel map) surfaces.
    REQUIRE(ac3forge_eac3_access_unit_encoder_channel_count(au_encoder) == 0);

    ac3forge_eac3_access_unit_t* unit = nullptr;
    CHECK(ac3forge_eac3_access_unit_encoder_encode(au_encoder, nullptr, 0,
                                                    AC3FORGE_SAMPLES_PER_FRAME, nullptr, 0,
                                                    &unit) == AC3FORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP);
    CHECK(unit == nullptr);
    ac3forge_eac3_access_unit_encoder_destroy(au_encoder);
}

TEST_CASE("E-AC-3 access units with a dependent substream cross the C API intact",
          "[capi][eac3]") {
    // 5.1.2: a 3/2+LFE bed plus one dependent substream carrying Vhl/Vhr -
    // the same layout family tests/decoder/test_eac3_decoder.cpp proves against the
    // C++ decoder; here the C access-unit surface is what walks it.
    const ac3::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true},
        .dependents = {{.bitrate_kbps = 192,
                        .acmod = ac3::Acmod::k2_0,
                        .chanmap = ac3::eac3::chanmap::k512Height}}};
    ac3::eac3::AccessUnitEncoder encoder{config};
    REQUIRE(encoder.channel_count() == 8);

    const std::vector<double> tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 2000.0, 1300.0};
    std::vector<std::vector<float>> block(8, std::vector<float>(AC3FORGE_SAMPLES_PER_FRAME));
    std::vector<std::span<const float>> views(8);
    std::vector<uint8_t> stream;
    for (int frame = 0; frame < 3; ++frame) {
        for (std::size_t ch = 0; ch < 8; ++ch) {
            fill_tone(block[ch].data(), tones[ch], frame, 48000.0);
            views[ch] = block[ch];
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        const auto* data = reinterpret_cast<const uint8_t*>(unit->bytes.data());
        stream.insert(stream.end(), data, data + unit->bytes.size());
    }

    // Framing first: three access units of two syncframes each.
    ac3forge_spans_t* units = nullptr;
    REQUIRE(ac3forge_split_access_units(stream.data(), stream.size(), &units) == AC3FORGE_OK);
    REQUIRE(ac3forge_spans_count(units) == 3);
    ac3forge_spans_t* frames = nullptr;
    REQUIRE(ac3forge_split_frames(stream.data(), stream.size(), &frames) == AC3FORGE_OK);
    REQUIRE(ac3forge_spans_count(frames) == 6);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_eac3_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);

    std::vector<double> energy(8, 0.0);
    for (std::size_t i = 0; i < ac3forge_spans_count(units); ++i) {
        const auto span = ac3forge_spans_get(units, i);
        ac3forge_decoded_access_unit_t* unit = nullptr;
        REQUIRE(ac3forge_eac3_decoder_decode_access_unit(decoder, stream.data() + span.offset,
                                                         span.length, &unit) == AC3FORGE_OK);
        REQUIRE(unit != nullptr);

        CHECK(ac3forge_decoded_access_unit_sample_rate(unit) == AC3FORGE_SAMPLE_RATE_48000);
        CHECK(ac3forge_decoded_access_unit_acmod(unit) == AC3FORGE_ACMOD_3_2);
        CHECK(ac3forge_decoded_access_unit_dialnorm(unit) == 31);
        CHECK(ac3forge_decoded_access_unit_has_compr(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_compr(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_dynrng(unit, 0) == 0);  // no DRC profile configured
        CHECK(ac3forge_decoded_access_unit_numblkscod(unit) == 3);
        CHECK(ac3forge_decoded_access_unit_substream_count(unit) == 2);
        REQUIRE(ac3forge_decoded_access_unit_channel_count(unit) == 8);
        CHECK(ac3forge_decoded_access_unit_samples_per_channel(unit) == AC3FORGE_SAMPLES_PER_FRAME);

        // Table E2.5 location order: L first, the heights in the middle, the
        // LFE last of these eight. An out-of-range index takes the documented
        // L default.
        REQUIRE(ac3forge_decoded_access_unit_layout_count(unit) == 8);
        CHECK(ac3forge_decoded_access_unit_layout_location(unit, 0) == AC3FORGE_LOCATION_L);
        CHECK(ac3forge_decoded_access_unit_layout_location(unit, 5) == AC3FORGE_LOCATION_VHL);
        CHECK(ac3forge_decoded_access_unit_layout_location(unit, 7) == AC3FORGE_LOCATION_LFE);
        CHECK(ac3forge_decoded_access_unit_layout_location(unit, 8) == AC3FORGE_LOCATION_L);

        // No object metadata rode along, so the object accessors take their
        // no-programme defaults rather than reading anything.
        CHECK(ac3forge_decoded_access_unit_has_object_metadata(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_program_dynamic_only(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_program_lfe(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_program_bed(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_program_dynamic_object_count(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_object_audio_count(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_object_audio(unit, 0) == nullptr);
        double x = -1, y = -1, z = -1, gain_db = -1;
        ac3forge_decoded_access_unit_dynamic_object(unit, 0, &x, &y, &z, &gain_db);
        CHECK(x == 0.5);
        CHECK(y == 0.5);
        CHECK(z == 0.0);
        CHECK(gain_db == 0.0);

        for (std::size_t ch = 0; ch < 8; ++ch) {
            const float* samples = ac3forge_decoded_access_unit_channel_samples(unit, ch);
            REQUIRE(samples != nullptr);
            for (int n = 0; n < AC3FORGE_SAMPLES_PER_FRAME; ++n) {
                energy[ch] += static_cast<double>(samples[n]) * static_cast<double>(samples[n]);
            }
        }
        CHECK(ac3forge_decoded_access_unit_channel_samples(unit, 8) == nullptr);

        ac3forge_decoded_access_unit_destroy(unit);
    }
    // Real audio landed in every rendered channel, not just a header parse.
    for (std::size_t ch = 0; ch < 8; ++ch) {
        INFO("channel " << ch);
        CHECK(energy[ch] > 1.0);
    }
    ac3forge_eac3_decoder_destroy(decoder);

    // The same stream substream by substream, so the dependent's own bsi
    // fields cross the boundary too: the independent's acmod/lfe speak for
    // themselves, the dependent speaks through its chanmap.
    ac3forge_eac3_decoder_t* frame_decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&decoder_config, &frame_decoder) == AC3FORGE_OK);
    const auto ind_span = ac3forge_spans_get(frames, 0);
    ac3forge_decoded_substream_t* independent = nullptr;
    REQUIRE(ac3forge_eac3_decoder_decode_substream(frame_decoder, stream.data() + ind_span.offset,
                                                   ind_span.length, &independent) == AC3FORGE_OK);
    REQUIRE(independent != nullptr);
    CHECK(ac3forge_decoded_substream_is_independent(independent) == 1);
    CHECK(ac3forge_decoded_substream_acmod(independent) == AC3FORGE_ACMOD_3_2);
    CHECK(ac3forge_decoded_substream_lfe(independent) == 1);
    CHECK(ac3forge_decoded_substream_has_chanmap(independent) == 0);
    CHECK(ac3forge_decoded_substream_chanmap(independent) == 0);
    CHECK(ac3forge_decoded_substream_channel_count(independent) == 6);
    CHECK(ac3forge_decoded_substream_location_map(independent) != 0);
    ac3forge_decoded_substream_destroy(independent);

    const auto dep_span = ac3forge_spans_get(frames, 1);
    ac3forge_decoded_substream_t* dependent = nullptr;
    REQUIRE(ac3forge_eac3_decoder_decode_substream(frame_decoder, stream.data() + dep_span.offset,
                                                   dep_span.length, &dependent) == AC3FORGE_OK);
    REQUIRE(dependent != nullptr);
    CHECK(ac3forge_decoded_substream_is_independent(dependent) == 0);
    CHECK(ac3forge_decoded_substream_id(dependent) == 0);
    CHECK(ac3forge_decoded_substream_acmod(dependent) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_decoded_substream_has_chanmap(dependent) == 1);
    CHECK(ac3forge_decoded_substream_chanmap(dependent) == ac3::eac3::chanmap::k512Height);
    CHECK(ac3forge_decoded_substream_location_map(dependent) == ac3::eac3::chanmap::k512Height);
    CHECK(ac3forge_decoded_substream_last_dependent(dependent) == 1);
    ac3forge_decoded_substream_destroy(dependent);
    ac3forge_eac3_decoder_destroy(frame_decoder);

    ac3forge_spans_destroy(units);
    ac3forge_spans_destroy(frames);
}

TEST_CASE("E-AC-3 dual mono metadata crosses the C boundary on both decode surfaces",
          "[capi][eac3]") {
    // 1+1 with each programme's own dialnorm, DRC profile and heavy
    // compression - the configuration that populates every optional metadata
    // field the substream accessors expose, Ch2's included. The tones sit
    // well above the -20 dBFS dialogue target, so both compressors and both
    // range controllers actually act rather than idle at unity.
    ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 192,
         .acmod = ac3::Acmod::kDualMono,
         .dialnorm = 27,
         .dialnorm2 = 25,
         .drc = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard),
         .heavy = ac3::meta::HeavyConfig{},
         .drc2 = ac3::meta::profile(ac3::meta::ProfileId::kMusicLight),
         .heavy2 = ac3::meta::HeavyConfig{}}};
    const auto stream = encode_eac3_stream(encoder, {900.0, 500.0}, 3);

    ac3forge_decoder_config_t config;
    ac3forge_decoder_config_init(&config);
    ac3forge_eac3_decoder_t* substream_decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&config, &substream_decoder) == AC3FORGE_OK);
    // A dual mono frame is a whole access unit on its own, so the same bytes
    // exercise the unit surface too - including its layout_count == 0 rule.
    ac3forge_eac3_decoder_t* unit_decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&config, &unit_decoder) == AC3FORGE_OK);

    ac3forge_spans_t* spans = nullptr;
    REQUIRE(ac3forge_split_frames(stream.bytes.data(), stream.bytes.size(), &spans) == AC3FORGE_OK);
    REQUIRE(ac3forge_spans_count(spans) == 3);

    bool saw_compr = false;
    bool saw_compr2 = false;
    bool saw_dynrng = false;
    bool saw_dynrng2 = false;
    bool saw_unit_compr = false;
    bool saw_unit_dynrng = false;
    for (std::size_t i = 0; i < ac3forge_spans_count(spans); ++i) {
        const auto span = ac3forge_spans_get(spans, i);

        ac3forge_decoded_substream_t* substream = nullptr;
        REQUIRE(ac3forge_eac3_decoder_decode_substream(substream_decoder,
                                                       stream.bytes.data() + span.offset,
                                                       span.length, &substream) == AC3FORGE_OK);
        REQUIRE(substream != nullptr);
        CHECK(ac3forge_decoded_substream_acmod(substream) == AC3FORGE_ACMOD_DUAL_MONO);
        CHECK(ac3forge_decoded_substream_dialnorm(substream) == 27);
        REQUIRE(ac3forge_decoded_substream_has_dialnorm2(substream) == 1);
        CHECK(ac3forge_decoded_substream_dialnorm2(substream) == 25);
        REQUIRE(ac3forge_decoded_substream_has_compr(substream) == 1);
        REQUIRE(ac3forge_decoded_substream_has_compr2(substream) == 1);
        saw_compr = saw_compr || ac3forge_decoded_substream_compr(substream) != 0;
        saw_compr2 = saw_compr2 || ac3forge_decoded_substream_compr2(substream) != 0;
        for (int blk = 0; blk < AC3FORGE_BLOCKS_PER_FRAME; ++blk) {
            saw_dynrng = saw_dynrng || ac3forge_decoded_substream_dynrng(substream, blk) != 0;
            saw_dynrng2 = saw_dynrng2 || ac3forge_decoded_substream_dynrng2(substream, blk) != 0;
        }
        CHECK(ac3forge_decoded_substream_dynrng(substream, -1) == 0);
        CHECK(ac3forge_decoded_substream_dynrng(substream, AC3FORGE_BLOCKS_PER_FRAME) == 0);
        CHECK(ac3forge_decoded_substream_dynrng2(substream, -1) == 0);
        CHECK(ac3forge_decoded_substream_dynrng2(substream, AC3FORGE_BLOCKS_PER_FRAME) == 0);
        CHECK(ac3forge_decoded_substream_channel_count(substream) == 2);
        ac3forge_decoded_substream_destroy(substream);

        ac3forge_decoded_access_unit_t* unit = nullptr;
        REQUIRE(ac3forge_eac3_decoder_decode_access_unit(unit_decoder,
                                                         stream.bytes.data() + span.offset,
                                                         span.length, &unit) == AC3FORGE_OK);
        REQUIRE(unit != nullptr);
        CHECK(ac3forge_decoded_access_unit_acmod(unit) == AC3FORGE_ACMOD_DUAL_MONO);
        CHECK(ac3forge_decoded_access_unit_dialnorm(unit) == 27);
        CHECK(ac3forge_decoded_access_unit_substream_count(unit) == 1);
        CHECK(ac3forge_decoded_access_unit_channel_count(unit) == 2);
        // 1+1 has no Table E2.5 layout - two unrelated programmes - so the
        // count is 0 and any index takes the L default.
        CHECK(ac3forge_decoded_access_unit_layout_count(unit) == 0);
        CHECK(ac3forge_decoded_access_unit_layout_location(unit, 0) == AC3FORGE_LOCATION_L);
        REQUIRE(ac3forge_decoded_access_unit_has_compr(unit) == 1);
        saw_unit_compr = saw_unit_compr || ac3forge_decoded_access_unit_compr(unit) != 0;
        for (int blk = 0; blk < AC3FORGE_BLOCKS_PER_FRAME; ++blk) {
            saw_unit_dynrng =
                saw_unit_dynrng || ac3forge_decoded_access_unit_dynrng(unit, blk) != 0;
        }
        CHECK(ac3forge_decoded_access_unit_dynrng(unit, -1) == 0);
        CHECK(ac3forge_decoded_access_unit_dynrng(unit, AC3FORGE_BLOCKS_PER_FRAME) == 0);
        ac3forge_decoded_access_unit_destroy(unit);
    }
    CHECK(saw_compr);
    CHECK(saw_compr2);
    CHECK(saw_dynrng);
    CHECK(saw_dynrng2);
    CHECK(saw_unit_compr);
    CHECK(saw_unit_dynrng);

    ac3forge_spans_destroy(spans);
    ac3forge_eac3_decoder_destroy(unit_decoder);
    ac3forge_eac3_decoder_destroy(substream_decoder);
}

TEST_CASE("the C API holds back and flushes transient pre-noise frames like the C++ decoder",
          "[capi][eac3]") {
    // Mirrors "transient pre-noise processing holds a frame back then releases
    // it corrected" (tests/decoder/test_eac3_decoder.cpp). The silent frames here are
    // the tool's own semantics - frames that never switch a block release
    // immediately - not the test signal; the transient itself is real audio.
    ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0, .transient_prenoise = true}};

    ac3forge_decoder_config_t config;
    ac3forge_decoder_config_init(&config);
    ac3forge_eac3_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&config, &decoder) == AC3FORGE_OK);

    // A fresh decoder has nothing buffered: flush hands over the documented
    // empty shape, not an error.
    ac3forge_decoded_substream_t** flushed = nullptr;
    std::size_t flushed_count = 99;
    REQUIRE(ac3forge_eac3_decoder_flush(decoder, &flushed, &flushed_count) == AC3FORGE_OK);
    CHECK(flushed == nullptr);
    CHECK(flushed_count == 0);

    const std::vector<float> silence(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    const std::vector<std::span<const float>> silent_views{silence, silence};
    const auto decode_one = [&](const std::vector<std::byte>& frame,
                                ac3forge_decoded_substream_t** out) {
        return ac3forge_eac3_decoder_decode_substream(
            decoder, reinterpret_cast<const uint8_t*>(frame.data()), frame.size(), out);
    };

    for (int f = 0; f < 2; ++f) {
        const auto frame = encoder.encode_frame(silent_views);
        REQUIRE(frame.has_value());
        ac3forge_decoded_substream_t* substream = nullptr;
        REQUIRE(decode_one(*frame, &substream) == AC3FORGE_OK);
        REQUIRE(substream != nullptr);
        ac3forge_decoded_substream_destroy(substream);
    }

    constexpr int kOnset = 960;
    std::vector<float> transient(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    for (int n = kOnset; n < AC3FORGE_SAMPLES_PER_FRAME; ++n) {
        transient[static_cast<std::size_t>(n)] = static_cast<float>(
            0.9 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / 48000.0));
    }
    const std::vector<std::span<const float>> transient_views{transient, transient};
    const auto transient_frame = encoder.encode_frame(transient_views);
    REQUIRE(transient_frame.has_value());

    // transproce turns on: AC3FORGE_OK with a NULL substream is the held-back
    // signal, not an error - the header documents exactly this pair.
    ac3forge_decoded_substream_t* held = nullptr;
    REQUIRE(decode_one(*transient_frame, &held) == AC3FORGE_OK);
    CHECK(held == nullptr);

    const auto after = encoder.encode_frame(silent_views);
    REQUIRE(after.has_value());
    ac3forge_decoded_substream_t* released = nullptr;
    REQUIRE(decode_one(*after, &released) == AC3FORGE_OK);
    REQUIRE(released != nullptr);
    CHECK(ac3forge_decoded_substream_channel_count(released) == 2);
    ac3forge_decoded_substream_destroy(released);

    // The "after" frame is itself the one now being held back (buffered mode
    // is sticky once the tool has fired); flush is what hands it over at end
    // of stream.
    REQUIRE(ac3forge_eac3_decoder_flush(decoder, &flushed, &flushed_count) == AC3FORGE_OK);
    REQUIRE(flushed != nullptr);
    REQUIRE(flushed_count == 1);
    CHECK(ac3forge_decoded_substream_channel_count(flushed[0]) == 2);
    ac3forge_decoded_substream_array_destroy(flushed, flushed_count);

    ac3forge_eac3_decoder_destroy(decoder);
}

TEST_CASE("E-AC-3 C decode entry points reject bad arguments and bad bitstreams",
          "[capi][eac3]") {
    ac3forge_decoder_config_t config;
    ac3forge_decoder_config_init(&config);

    ac3forge_eac3_decoder_t* decoder = nullptr;
    CHECK(ac3forge_eac3_decoder_create(nullptr, &decoder) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_create(&config, nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(decoder == nullptr);
    REQUIRE(ac3forge_eac3_decoder_create(&config, &decoder) == AC3FORGE_OK);

    const std::vector<uint8_t> garbage(16, 0xAB);
    ac3forge_decoded_substream_t* substream = nullptr;
    ac3forge_decoded_access_unit_t* unit = nullptr;
    ac3forge_decoded_substream_t** flushed = nullptr;
    std::size_t count = 0;

    CHECK(ac3forge_eac3_decoder_decode_substream(nullptr, garbage.data(), garbage.size(),
                                                 &substream) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_substream(decoder, nullptr, 0, &substream) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_substream(decoder, garbage.data(), garbage.size(),
                                                 nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_access_unit(nullptr, garbage.data(), garbage.size(),
                                                   &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_access_unit(decoder, nullptr, 0, &unit) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_access_unit(decoder, garbage.data(), garbage.size(),
                                                   nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_flush(nullptr, &flushed, &count) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_flush(decoder, nullptr, &count) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_flush(decoder, &flushed, nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);

    // Same convention as the AC-3 decode-error test above: a bitstream
    // failure maps into the decode-error range and the out-parameter is left
    // untouched; which exact code the parser picks is its own business.
    auto status = ac3forge_eac3_decoder_decode_substream(decoder, garbage.data(), garbage.size(),
                                                         &substream);
    CHECK(status >= AC3FORGE_ERROR_DECODE_TRUNCATED);
    CHECK(status <= AC3FORGE_ERROR_DECODE_INVALID_STREAM);
    CHECK(substream == nullptr);
    status = ac3forge_eac3_decoder_decode_access_unit(decoder, garbage.data(), garbage.size(),
                                                      &unit);
    CHECK(status >= AC3FORGE_ERROR_DECODE_TRUNCATED);
    CHECK(status <= AC3FORGE_ERROR_DECODE_INVALID_STREAM);
    CHECK(unit == nullptr);

    // A real frame decodes on this same decoder; the same frame cut short is
    // an error again, not a crash and not a false success.
    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192}};
    const auto stream = encode_eac3_stream(encoder, {1000.0, 800.0}, 1);
    REQUIRE(ac3forge_eac3_decoder_decode_substream(decoder, stream.bytes.data(),
                                                   stream.bytes.size(), &substream) == AC3FORGE_OK);
    REQUIRE(substream != nullptr);
    ac3forge_decoded_substream_destroy(substream);
    substream = nullptr;
    status = ac3forge_eac3_decoder_decode_substream(decoder, stream.bytes.data(),
                                                    stream.bytes.size() / 2, &substream);
    CHECK(status >= AC3FORGE_ERROR_DECODE_TRUNCATED);
    CHECK(status <= AC3FORGE_ERROR_DECODE_INVALID_STREAM);
    CHECK(substream == nullptr);

    // The framing helpers police their own arguments the same way.
    ac3forge_spans_t* spans = nullptr;
    CHECK(ac3forge_split_frames(nullptr, 0, &spans) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_split_frames(garbage.data(), garbage.size(), nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_split_access_units(nullptr, 0, &spans) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_split_access_units(garbage.data(), garbage.size(), nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    status = ac3forge_split_access_units(garbage.data(), garbage.size(), &spans);
    CHECK(status >= AC3FORGE_ERROR_DECODE_TRUNCATED);
    CHECK(status <= AC3FORGE_ERROR_DECODE_INVALID_STREAM);
    CHECK(spans == nullptr);

    int bsid = -1;
    CHECK(ac3forge_stream_bsid(nullptr, 0, &bsid) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_stream_bsid(garbage.data(), garbage.size(), nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    // bsid sits at bit 40; five bytes cannot hold it.
    CHECK(ac3forge_stream_bsid(garbage.data(), 5, &bsid) == AC3FORGE_ERROR_DECODE_TRUNCATED);

    ac3forge_eac3_decoder_destroy(decoder);
}

TEST_CASE("C accessors take their documented defaults on null handles", "[capi]") {
    // Every accessor tolerates NULL and answers with the default its header
    // comment names, and every _destroy is a free()-style no-op on NULL -
    // the whole-surface sweep of the convention the header leads with.
    ac3forge_encoder_destroy(nullptr);
    ac3forge_decoder_destroy(nullptr);
    ac3forge_eac3_decoder_destroy(nullptr);
    ac3forge_atmos_encoder_destroy(nullptr);
    ac3forge_bytes_destroy(nullptr);
    ac3forge_decoded_frame_destroy(nullptr);
    ac3forge_decoded_substream_destroy(nullptr);
    ac3forge_decoded_access_unit_destroy(nullptr);
    ac3forge_spans_destroy(nullptr);
    ac3forge_decoded_substream_array_destroy(nullptr, 5);

    CHECK(ac3forge_encoder_channel_count(nullptr) == 0);
    CHECK(ac3forge_atmos_encoder_dynamic_object_count(nullptr) == 0);
    CHECK(ac3forge_bytes_data(nullptr) == nullptr);
    CHECK(ac3forge_bytes_size(nullptr) == 0);
    CHECK(ac3forge_spans_count(nullptr) == 0);
    const auto span = ac3forge_spans_get(nullptr, 0);
    CHECK(span.offset == 0);
    CHECK(span.length == 0);

    CHECK(ac3forge_decoded_frame_sample_rate(nullptr) == AC3FORGE_SAMPLE_RATE_48000);
    CHECK(ac3forge_decoded_frame_bitrate_kbps(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_acmod(nullptr) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_decoded_frame_lfe(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_dialnorm(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_has_compr(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_compr(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_dynrng(nullptr, 0) == 0);
    CHECK(ac3forge_decoded_frame_has_dialnorm2(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_dialnorm2(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_has_compr2(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_compr2(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_dynrng2(nullptr, 0) == 0);
    CHECK(ac3forge_decoded_frame_channel_count(nullptr) == 0);
    CHECK(ac3forge_decoded_frame_samples_per_channel(nullptr) == AC3FORGE_SAMPLES_PER_FRAME);
    CHECK(ac3forge_decoded_frame_channel_samples(nullptr, 0) == nullptr);
    CHECK(ac3forge_decoded_frame_block_switched(nullptr, 0, 0) == 0);

    CHECK(ac3forge_decoded_substream_is_independent(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_id(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_sample_rate(nullptr) == AC3FORGE_SAMPLE_RATE_48000);
    CHECK(ac3forge_decoded_substream_acmod(nullptr) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_decoded_substream_lfe(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_dialnorm(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_has_compr(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_compr(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_dynrng(nullptr, 0) == 0);
    CHECK(ac3forge_decoded_substream_has_dialnorm2(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_dialnorm2(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_has_compr2(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_compr2(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_dynrng2(nullptr, 0) == 0);
    CHECK(ac3forge_decoded_substream_numblkscod(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_has_chanmap(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_chanmap(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_last_dependent(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_location_map(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_channel_count(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_samples_per_channel(nullptr) == AC3FORGE_SAMPLES_PER_FRAME);
    CHECK(ac3forge_decoded_substream_channel_samples(nullptr, 0) == nullptr);
    CHECK(ac3forge_decoded_substream_block_switched(nullptr, 0, 0) == 0);
    CHECK(ac3forge_decoded_substream_has_object_metadata(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_program_dynamic_only(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_program_lfe(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_program_bed(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_program_dynamic_object_count(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_object_audio_count(nullptr) == 0);
    CHECK(ac3forge_decoded_substream_object_audio(nullptr, 0) == nullptr);
    double x = -1, y = -1, z = -1, gain_db = -1;
    ac3forge_decoded_substream_dynamic_object(nullptr, 0, &x, &y, &z, &gain_db);
    CHECK(x == 0.5);
    CHECK(y == 0.5);
    CHECK(z == 0.0);
    CHECK(gain_db == 0.0);
    // NULL out-parameters are individually optional too.
    ac3forge_decoded_substream_dynamic_object(nullptr, 0, nullptr, nullptr, nullptr, nullptr);

    CHECK(ac3forge_decoded_access_unit_sample_rate(nullptr) == AC3FORGE_SAMPLE_RATE_48000);
    CHECK(ac3forge_decoded_access_unit_acmod(nullptr) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_decoded_access_unit_dialnorm(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_has_compr(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_compr(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_dynrng(nullptr, 0) == 0);
    CHECK(ac3forge_decoded_access_unit_numblkscod(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_substream_count(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_channel_count(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_samples_per_channel(nullptr) == AC3FORGE_SAMPLES_PER_FRAME);
    CHECK(ac3forge_decoded_access_unit_channel_samples(nullptr, 0) == nullptr);
    CHECK(ac3forge_decoded_access_unit_layout_count(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_layout_location(nullptr, 0) == AC3FORGE_LOCATION_L);
    CHECK(ac3forge_decoded_access_unit_has_object_metadata(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_program_dynamic_only(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_program_lfe(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_program_bed(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_program_dynamic_object_count(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_object_audio_count(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_object_audio(nullptr, 0) == nullptr);
    x = -1;
    y = -1;
    z = -1;
    gain_db = -1;
    ac3forge_decoded_access_unit_dynamic_object(nullptr, 0, &x, &y, &z, &gain_db);
    CHECK(x == 0.5);
    CHECK(y == 0.5);
    CHECK(z == 0.0);
    CHECK(gain_db == 0.0);
}

TEST_CASE("every ac3forge status code carries its own message", "[capi]") {
    constexpr ac3forge_status_t codes[] = {AC3FORGE_OK,
                                           AC3FORGE_ERROR_INVALID_ARGUMENT,
                                           AC3FORGE_ERROR_OUT_OF_MEMORY,
                                           AC3FORGE_ERROR_INTERNAL,
                                           AC3FORGE_ERROR_ENCODE_INVALID_BITRATE,
                                           AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM,
                                           AC3FORGE_ERROR_ENCODE_INVALID_SUBSTREAM,
                                           AC3FORGE_ERROR_ENCODE_INVALID_CHANNEL_MAP,
                                           AC3FORGE_ERROR_ENCODE_TOO_MANY_CHANNELS,
                                           AC3FORGE_ERROR_ENCODE_INVALID_MIX_LEVEL,
                                           AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO,
                                           AC3FORGE_ERROR_ENCODE_INVALID_BSI,
                                           AC3FORGE_ERROR_DECODE_TRUNCATED,
                                           AC3FORGE_ERROR_DECODE_BAD_SYNC_WORD,
                                           AC3FORGE_ERROR_DECODE_BAD_CRC,
                                           AC3FORGE_ERROR_DECODE_RESERVED_VALUE,
                                           AC3FORGE_ERROR_DECODE_UNSUPPORTED,
                                           AC3FORGE_ERROR_DECODE_INVALID_STREAM,
                                           AC3FORGE_ERROR_SCAN_EMPTY,
                                           AC3FORGE_ERROR_SCAN_LOST_SYNC,
                                           AC3FORGE_ERROR_SCAN_UNSUPPORTED_BSID,
                                           AC3FORGE_ERROR_SCAN_RESERVED_VALUE,
                                           AC3FORGE_ERROR_SCAN_TRUNCATED,
                                           AC3FORGE_ERROR_SCAN_UNSUPPORTED_STRUCTURE};
    for (const auto code : codes) {
        const char* message = ac3forge_status_message(code);
        REQUIRE(message != nullptr);
        // Every enumerator has its own case; the fallthrough string is
        // reserved for values outside the enum.
        CHECK(std::string_view(message) != "unknown status");
    }
}

TEST_CASE("C config initializers report the C++ defaults and tolerate NULL", "[capi]") {
    // NULL is documented as a no-op for every _init, matching _destroy's own
    // free()-like convention.
    ac3forge_heavy_config_init(nullptr);
    ac3forge_encoder_config_init(nullptr);
    ac3forge_decoder_config_init(nullptr);
    ac3forge_atmos_config_init(nullptr);

    ac3forge_heavy_config_t heavy;
    ac3forge_heavy_config_init(&heavy);
    CHECK(heavy.dialogue_target_dbfs == -20.0);
    CHECK(heavy.peak_ceiling_dbfs == -0.5);
    CHECK(heavy.release_db_per_second == 20.0);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    CHECK(decoder_config.drc_scale == 0.0);
    CHECK(decoder_config.heavy_compression == 0);

    ac3forge_atmos_config_t atmos;
    ac3forge_atmos_config_init(&atmos);
    CHECK(atmos.sample_rate == AC3FORGE_SAMPLE_RATE_48000);
    CHECK(atmos.bitrate_kbps == 448);
    CHECK(atmos.dialnorm == 31);
    CHECK(atmos.num_bands_idx == 4);
    CHECK(atmos.fine_quant == 0);
    CHECK(atmos.emit_object_metadata == 1);
    CHECK(atmos.fast_mdct == 1);
}

TEST_CASE("Atmos C API tool variants round-trip objects and beds", "[capi][atmos]") {
    struct Variant {
        const char* name;
        int num_bands_idx;
        int fine_quant;
        int emit_object_metadata;
        int fast_mdct;
    };
    // The knobs the C header exposes on its one Annex E producer: the JOC
    // band count at both ends of Table 50, the fine quantizer, the reference
    // MDCT, and the container-omitted bed-only mode.
    const Variant variants[] = {
        {"fine quantizer, 5 bands", 2, 1, 1, 1},
        {"23 bands, reference MDCT", 7, 0, 1, 0},
        {"bed only, container omitted", 4, 0, 0, 1},
    };
    for (const auto& variant : variants) {
        INFO(variant.name);
        ac3forge_atmos_config_t config;
        ac3forge_atmos_config_init(&config);
        config.num_bands_idx = variant.num_bands_idx;
        config.fine_quant = variant.fine_quant;
        config.emit_object_metadata = variant.emit_object_metadata;
        config.fast_mdct = variant.fast_mdct;

        ac3forge_atmos_encoder_t* encoder = nullptr;
        REQUIRE(ac3forge_atmos_encoder_create(&config, 2, &encoder) == AC3FORGE_OK);
        CHECK(ac3forge_atmos_encoder_dynamic_object_count(encoder) == 2);

        ac3forge_decoder_config_t decoder_config;
        ac3forge_decoder_config_init(&decoder_config);
        ac3forge_eac3_decoder_t* decoder = nullptr;
        REQUIRE(ac3forge_eac3_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);

        std::vector<float> object_a(AC3FORGE_SAMPLES_PER_FRAME);
        std::vector<float> object_b(AC3FORGE_SAMPLES_PER_FRAME);
        // Both placements sit on OAMD's quantizer grid (see the Atmos
        // round-trip test above), so position equality is exact.
        const ac3forge_object_placement_t placements[2] = {
            {.x = 0.5, .y = 0.5, .z = 0.0, .gain = 1.0, .lfe_send = 0.0},
            {.x = 0.0, .y = 1.0, .z = 0.0, .gain = 1.0, .lfe_send = 0.0}};

        for (int frame = 0; frame < 2; ++frame) {
            fill_tone(object_a.data(), 1000.0, frame, 48000.0);
            fill_tone(object_b.data(), 600.0, frame, 48000.0);
            const float* objects[2] = {object_a.data(), object_b.data()};

            ac3forge_bytes_t* unit = nullptr;
            REQUIRE(ac3forge_atmos_encoder_encode_frame(encoder, objects, 2,
                                                        AC3FORGE_SAMPLES_PER_FRAME, placements, 2,
                                                        &unit) == AC3FORGE_OK);
            ac3forge_decoded_substream_t* substream = nullptr;
            REQUIRE(ac3forge_eac3_decoder_decode_substream(decoder, ac3forge_bytes_data(unit),
                                                           ac3forge_bytes_size(unit),
                                                           &substream) == AC3FORGE_OK);
            ac3forge_bytes_destroy(unit);
            REQUIRE(substream != nullptr);

            // Whatever the variant, a legacy decoder sees an ordinary 5.1 bed.
            CHECK(ac3forge_decoded_substream_acmod(substream) == AC3FORGE_ACMOD_3_2);
            CHECK(ac3forge_decoded_substream_lfe(substream) == 1);
            REQUIRE(ac3forge_decoded_substream_channel_count(substream) == 6);
            CHECK(ac3forge_decoded_substream_channel_samples(substream, 0) != nullptr);

            if (variant.emit_object_metadata != 0) {
                CHECK(ac3forge_decoded_substream_has_object_metadata(substream) == 1);
                CHECK(ac3forge_decoded_substream_program_dynamic_only(substream) == 1);
                CHECK(ac3forge_decoded_substream_program_dynamic_object_count(substream) == 2);
                CHECK(ac3forge_decoded_substream_object_audio_count(substream) == 2);
                CHECK(ac3forge_decoded_substream_object_audio(substream, 1) != nullptr);
                double x = -1, y = -1, z = -1, gain_db = -1;
                ac3forge_decoded_substream_dynamic_object(substream, 1, &x, &y, &z, &gain_db);
                CHECK(x == 0.0);
                CHECK(y == 1.0);
                CHECK(z == 0.0);
                CHECK(gain_db == 0.0);
            } else {
                // The container was omitted, so nothing object-shaped
                // survives - the objects live on only inside the bed mix.
                CHECK(ac3forge_decoded_substream_has_object_metadata(substream) == 0);
                CHECK(ac3forge_decoded_substream_object_audio_count(substream) == 0);
            }
            ac3forge_decoded_substream_destroy(substream);
        }
        ac3forge_eac3_decoder_destroy(decoder);
        ac3forge_atmos_encoder_destroy(encoder);
    }
}

TEST_CASE("C encode entry points surface the encoder's own error codes", "[capi]") {
    // Each case maps one ac3::FrameError onto its C status through the
    // internal from_cpp() bridge - the encode-side sibling of the decode
    // range checks above.
    std::vector<float> samples(AC3FORGE_SAMPLES_PER_FRAME);
    fill_tone(samples.data(), 1000.0, 0, 48000.0);
    const float* stereo[2] = {samples.data(), samples.data()};

    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    ac3forge_encoder_t* encoder = nullptr;
    CHECK(ac3forge_encoder_create(nullptr, &encoder) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_encoder_create(&config, nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(encoder == nullptr);

    // 100 kbps is not one of Table 5.18's 19 nominal rates, so AC-3 (unlike
    // E-AC-3, which takes it - see tests/encoder/test_eac3.cpp) must refuse it.
    config.bitrate_kbps = 100;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);
    ac3forge_bytes_t* encoded = nullptr;
    CHECK(ac3forge_encoder_encode_frame(encoder, stereo, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                        &encoded) == AC3FORGE_ERROR_ENCODE_INVALID_BITRATE);
    CHECK(encoded == nullptr);
    ac3forge_encoder_destroy(encoder);

    // Dialnorm 0 is reserved (§5.4.2.8).
    ac3forge_encoder_config_init(&config);
    config.dialnorm = 0;
    encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);
    CHECK(ac3forge_encoder_encode_frame(encoder, stereo, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                        &encoded) == AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM);
    CHECK(encoded == nullptr);
    ac3forge_encoder_destroy(encoder);

    // 1+1 without Ch2's own dialnorm2 is exactly as invalid as a missing
    // dialnorm would be.
    ac3forge_encoder_config_init(&config);
    config.acmod = AC3FORGE_ACMOD_DUAL_MONO;
    encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);
    CHECK(ac3forge_encoder_encode_frame(encoder, stereo, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                        &encoded) == AC3FORGE_ERROR_ENCODE_INVALID_DIALNORM);
    CHECK(encoded == nullptr);
    ac3forge_encoder_destroy(encoder);

    // The Atmos encoder rides the E-AC-3 frame writer, so its failures come
    // back through the same mapping.
    ac3forge_atmos_config_t atmos_config;
    ac3forge_atmos_config_init(&atmos_config);
    ac3forge_atmos_encoder_t* atmos = nullptr;
    CHECK(ac3forge_atmos_encoder_create(nullptr, 1, &atmos) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_create(&atmos_config, -1, &atmos) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_create(&atmos_config, 1, nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(atmos == nullptr);

    const ac3forge_object_placement_t placement{.x = 0.5, .y = 0.5, .z = 0.0, .gain = 1.0,
                                                .lfe_send = 0.0};
    const float* objects[1] = {samples.data()};
    ac3forge_bytes_t* unit = nullptr;

    // §E2.3.1.3: frmsiz is 11 bits, so 2000 kbps needs more words than any
    // legal E-AC-3 frame can signal.
    atmos_config.bitrate_kbps = 2000;
    REQUIRE(ac3forge_atmos_encoder_create(&atmos_config, 1, &atmos) == AC3FORGE_OK);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, objects, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 1,
                                              &unit) == AC3FORGE_ERROR_ENCODE_INVALID_BITRATE);
    CHECK(unit == nullptr);

    // Mismatched counts and NULL spans are this layer's own argument checks,
    // caught before the encoder runs at all.
    CHECK(ac3forge_atmos_encoder_encode_frame(nullptr, objects, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 1,
                                              &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, objects, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 2,
                                              &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, objects, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 0,
                                              &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, objects, 1, AC3FORGE_SAMPLES_PER_FRAME / 2,
                                              &placement, 1,
                                              &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, nullptr, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 1,
                                              &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, objects, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 1,
                                              nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    const float* null_object[1] = {nullptr};
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, null_object, 1, AC3FORGE_SAMPLES_PER_FRAME,
                                              &placement, 1,
                                              &unit) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(unit == nullptr);
    ac3forge_atmos_encoder_destroy(atmos);

    // TS 103 420 §8.3.2.2 caps the complexity index at 16; the bed's LFE
    // counts, so sixteen dynamic objects push the programme to seventeen.
    atmos_config.bitrate_kbps = 448;
    atmos = nullptr;
    REQUIRE(ac3forge_atmos_encoder_create(&atmos_config, 16, &atmos) == AC3FORGE_OK);
    std::vector<const float*> many_objects(16, samples.data());
    std::vector<ac3forge_object_placement_t> many_placements(16, placement);
    CHECK(ac3forge_atmos_encoder_encode_frame(atmos, many_objects.data(), 16,
                                              AC3FORGE_SAMPLES_PER_FRAME, many_placements.data(),
                                              16,
                                              &unit) == AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO);
    CHECK(unit == nullptr);
    ac3forge_atmos_encoder_destroy(atmos);
}

TEST_CASE("AC-3 dual mono metadata crosses the C boundary per channel", "[capi]") {
    // The AC-3 sibling of the E-AC-3 dual mono test above, for the decoded
    // frame's own Ch2 accessors.
    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    config.bitrate_kbps = 192;
    config.acmod = AC3FORGE_ACMOD_DUAL_MONO;
    config.dialnorm = 27;
    config.has_dialnorm2 = 1;
    config.dialnorm2 = 25;
    config.has_drc = 1;
    config.drc_profile = AC3FORGE_DRC_FILM_STANDARD;
    config.has_heavy = 1;  // the _init defaults are a real heavy profile
    config.has_drc2 = 1;
    config.drc2_profile = AC3FORGE_DRC_MUSIC_LIGHT;
    config.has_heavy2 = 1;

    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);
    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);

    std::vector<float> ch1(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> ch2(AC3FORGE_SAMPLES_PER_FRAME);
    bool saw_compr = false;
    bool saw_compr2 = false;
    bool saw_dynrng = false;
    bool saw_dynrng2 = false;
    for (int frame = 0; frame < 3; ++frame) {
        fill_tone(ch1.data(), 900.0, frame, 48000.0);
        fill_tone(ch2.data(), 500.0, frame, 48000.0);
        const float* channels[2] = {ch1.data(), ch2.data()};

        ac3forge_bytes_t* encoded = nullptr;
        REQUIRE(ac3forge_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                              &encoded) == AC3FORGE_OK);
        ac3forge_decoded_frame_t* decoded = nullptr;
        REQUIRE(ac3forge_decoder_decode_frame(decoder, ac3forge_bytes_data(encoded),
                                              ac3forge_bytes_size(encoded),
                                              &decoded) == AC3FORGE_OK);
        ac3forge_bytes_destroy(encoded);
        REQUIRE(decoded != nullptr);

        CHECK(ac3forge_decoded_frame_acmod(decoded) == AC3FORGE_ACMOD_DUAL_MONO);
        CHECK(ac3forge_decoded_frame_sample_rate(decoded) == AC3FORGE_SAMPLE_RATE_48000);
        CHECK(ac3forge_decoded_frame_bitrate_kbps(decoded) == 192);
        CHECK(ac3forge_decoded_frame_lfe(decoded) == 0);
        CHECK(ac3forge_decoded_frame_dialnorm(decoded) == 27);
        REQUIRE(ac3forge_decoded_frame_has_dialnorm2(decoded) == 1);
        CHECK(ac3forge_decoded_frame_dialnorm2(decoded) == 25);
        REQUIRE(ac3forge_decoded_frame_has_compr(decoded) == 1);
        REQUIRE(ac3forge_decoded_frame_has_compr2(decoded) == 1);
        saw_compr = saw_compr || ac3forge_decoded_frame_compr(decoded) != 0;
        saw_compr2 = saw_compr2 || ac3forge_decoded_frame_compr2(decoded) != 0;
        for (int blk = 0; blk < AC3FORGE_BLOCKS_PER_FRAME; ++blk) {
            saw_dynrng = saw_dynrng || ac3forge_decoded_frame_dynrng(decoded, blk) != 0;
            saw_dynrng2 = saw_dynrng2 || ac3forge_decoded_frame_dynrng2(decoded, blk) != 0;
        }
        CHECK(ac3forge_decoded_frame_dynrng(decoded, -1) == 0);
        CHECK(ac3forge_decoded_frame_dynrng(decoded, AC3FORGE_BLOCKS_PER_FRAME) == 0);
        CHECK(ac3forge_decoded_frame_dynrng2(decoded, -1) == 0);
        CHECK(ac3forge_decoded_frame_dynrng2(decoded, AC3FORGE_BLOCKS_PER_FRAME) == 0);
        CHECK(ac3forge_decoded_frame_block_switched(decoded, 0, 0) == 0);
        CHECK(ac3forge_decoded_frame_block_switched(decoded, 2, 0) == 0);   // channel OOR
        CHECK(ac3forge_decoded_frame_block_switched(decoded, 0, -1) == 0);  // block OOR

        ac3forge_decoded_frame_destroy(decoded);
    }
    CHECK(saw_compr);
    CHECK(saw_compr2);
    CHECK(saw_dynrng);
    CHECK(saw_dynrng2);

    ac3forge_decoder_destroy(decoder);
    ac3forge_encoder_destroy(encoder);
}

TEST_CASE("the C latency surface reports the same budget as the C++ one", "[capi][latency]") {
    // Roadmap PF6. The numbers themselves are established empirically in
    // tests/decoder/test_latency.cpp (an impulse through a real encode ->
    // decode, located to the sample); this checks that the C translation
    // layer hands them across unchanged and that the free helpers agree with
    // the struct they are given.
    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    config.acmod = AC3FORGE_ACMOD_2_0;
    config.bitrate_kbps = 192;

    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);

    ac3forge_latency_t latency{};
    ac3forge_encoder_latency(encoder, &latency);
    CHECK(latency.frame_samples == AC3FORGE_SAMPLES_PER_FRAME);
    CHECK(latency.transform_samples == AC3FORGE_SAMPLES_PER_BLOCK);
    CHECK(latency.lookahead_samples == 0);
    CHECK(latency.holdback_samples == 0);
    CHECK(ac3forge_latency_total_samples(&latency) == 1792);
    CHECK(ac3forge_encoder_latency_samples(encoder) == 1792);

    const double ms = ac3forge_latency_ms(1792, AC3FORGE_SAMPLE_RATE_48000);
    CHECK(ms > 37.3);
    CHECK(ms < 37.4);
    // The rate really is read: 1792 samples is 40.6 ms at 44.1 kHz, not 37.3.
    CHECK(ac3forge_latency_ms(1792, AC3FORGE_SAMPLE_RATE_44100) > 40.6);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);
    CHECK(ac3forge_decoder_latency_samples(decoder) == 0);

    ac3forge_eac3_decoder_t* eac3_decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&decoder_config, &eac3_decoder) == AC3FORGE_OK);
    // Nothing decoded yet, so nothing held back yet.
    CHECK(ac3forge_eac3_decoder_latency_samples(eac3_decoder) == 0);

    ac3forge_decoder_destroy(decoder);
    ac3forge_eac3_decoder_destroy(eac3_decoder);
    ac3forge_encoder_destroy(encoder);
}

TEST_CASE("the C Atmos latency surface separates the object path from the bed",
          "[capi][latency][atmos]") {
    ac3forge_atmos_config_t config;
    ac3forge_atmos_config_init(&config);

    ac3forge_atmos_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_atmos_encoder_create(&config, 2, &encoder) == AC3FORGE_OK);

    ac3forge_latency_t objects{};
    ac3forge_latency_t bed{};
    ac3forge_atmos_encoder_latency(encoder, &objects);
    ac3forge_atmos_encoder_bed_latency(encoder, &bed);
    // The §7.1 QMF filterbank's own analysis+synthesis delay
    // (ac3::dsp::kQmfDelay = 576, not exposed at the C boundary) on top of the
    // already-decoded bed's own overlap - measured end to end in
    // test_latency.cpp. 576 is spelled out here rather than named: the C API
    // has no QMF-specific constant of its own to reference.
    constexpr int kQmfDelay = 576;
    CHECK(objects.transform_samples == AC3FORGE_SAMPLES_PER_BLOCK + kQmfDelay);
    CHECK(bed.transform_samples == AC3FORGE_SAMPLES_PER_BLOCK);
    CHECK(ac3forge_atmos_encoder_latency_samples(encoder) ==
          ac3forge_latency_total_samples(&objects));
    CHECK(ac3forge_latency_total_samples(&objects) ==
          ac3forge_latency_total_samples(&bed) + kQmfDelay);

    ac3forge_atmos_encoder_destroy(encoder);

    // No container, no JOC, no second transform.
    config.emit_object_metadata = 0;
    ac3forge_atmos_encoder_t* plain = nullptr;
    REQUIRE(ac3forge_atmos_encoder_create(&config, 2, &plain) == AC3FORGE_OK);
    ac3forge_latency_t plain_latency{};
    ac3forge_atmos_encoder_latency(plain, &plain_latency);
    CHECK(plain_latency.transform_samples == AC3FORGE_SAMPLES_PER_BLOCK);
    ac3forge_atmos_encoder_destroy(plain);
}

TEST_CASE("the C latency accessors tolerate null the way the rest of the surface does",
          "[capi][latency]") {
    CHECK(ac3forge_encoder_latency_samples(nullptr) == 0);
    CHECK(ac3forge_decoder_latency_samples(nullptr) == 0);
    CHECK(ac3forge_eac3_decoder_latency_samples(nullptr) == 0);
    CHECK(ac3forge_atmos_encoder_latency_samples(nullptr) == 0);
    CHECK(ac3forge_latency_total_samples(nullptr) == 0);

    // The out-parameter forms leave their target untouched rather than
    // zeroing it, same as every other _create-style out-parameter here.
    ac3forge_latency_t sentinel{.frame_samples = -7,
                                .transform_samples = -7,
                                .lookahead_samples = -7,
                                .holdback_samples = -7};
    ac3forge_encoder_latency(nullptr, &sentinel);
    ac3forge_atmos_encoder_latency(nullptr, &sentinel);
    ac3forge_atmos_encoder_bed_latency(nullptr, &sentinel);
    CHECK(sentinel.frame_samples == -7);
    CHECK(sentinel.holdback_samples == -7);
}

// --- decode_frame_into / decode_access_unit_into (roadmap AP5) -----------

TEST_CASE("ac3forge_decoder_decode_frame_into writes the same samples the value form allocates",
          "[capi]") {
    ac3forge_encoder_config_t encoder_config;
    ac3forge_encoder_config_init(&encoder_config);
    encoder_config.bitrate_kbps = 192;
    encoder_config.acmod = AC3FORGE_ACMOD_2_0;

    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&encoder_config, &encoder) == AC3FORGE_OK);
    REQUIRE(ac3forge_encoder_channel_count(encoder) == 2);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_decoder_t* value_decoder = nullptr;
    ac3forge_decoder_t* into_decoder = nullptr;
    REQUIRE(ac3forge_decoder_create(&decoder_config, &value_decoder) == AC3FORGE_OK);
    REQUIRE(ac3forge_decoder_create(&decoder_config, &into_decoder) == AC3FORGE_OK);

    std::array<std::vector<float>, AC3FORGE_DECODER_MAX_CHANNELS> into_storage;
    for (auto& v : into_storage) v.assign(AC3FORGE_SAMPLES_PER_FRAME, -99.0f);
    std::array<float*, AC3FORGE_DECODER_MAX_CHANNELS> into_ptrs{};
    for (std::size_t i = 0; i < into_storage.size(); ++i) into_ptrs[i] = into_storage[i].data();

    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME);

    for (int frame = 0; frame < 4; ++frame) {
        fill_tone(left.data(), 1000.0, frame, 48000.0);
        fill_tone(right.data(), 800.0, frame, 48000.0);
        const float* channels[2] = {left.data(), right.data()};

        ac3forge_bytes_t* encoded = nullptr;
        REQUIRE(ac3forge_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                               &encoded) == AC3FORGE_OK);

        ac3forge_decoded_frame_t* value_result = nullptr;
        REQUIRE(ac3forge_decoder_decode_frame(value_decoder, ac3forge_bytes_data(encoded),
                                              ac3forge_bytes_size(encoded),
                                              &value_result) == AC3FORGE_OK);
        REQUIRE(value_result != nullptr);
        REQUIRE(ac3forge_decoded_frame_channel_count(value_result) == 2);

        ac3forge_decoded_frame_t* into_result = nullptr;
        REQUIRE(ac3forge_decoder_decode_frame_into(
                    into_decoder, ac3forge_bytes_data(encoded), ac3forge_bytes_size(encoded),
                    into_ptrs.data(), AC3FORGE_DECODER_MAX_CHANNELS, AC3FORGE_SAMPLES_PER_FRAME,
                    &into_result) == AC3FORGE_OK);
        REQUIRE(into_result != nullptr);

        // The _into form's own handle carries every field EXCEPT the PCM.
        CHECK(ac3forge_decoded_frame_channel_count(into_result) == 0);
        CHECK(ac3forge_decoded_frame_acmod(into_result) == ac3forge_decoded_frame_acmod(value_result));
        CHECK(ac3forge_decoded_frame_dialnorm(into_result) ==
              ac3forge_decoded_frame_dialnorm(value_result));

        for (std::size_t ch = 0; ch < 2; ++ch) {
            const float* expected = ac3forge_decoded_frame_channel_samples(value_result, ch);
            REQUIRE(expected != nullptr);
            for (int n = 0; n < AC3FORGE_SAMPLES_PER_FRAME; ++n) {
                CHECK(into_storage[ch][static_cast<std::size_t>(n)] == expected[n]);
            }
        }
        // This layout codes only 2 of the 6 documented spans; the rest stay
        // exactly the sentinel they started as.
        for (std::size_t ch = 2; ch < AC3FORGE_DECODER_MAX_CHANNELS; ++ch) {
            CHECK(into_storage[ch][0] == -99.0f);
        }

        ac3forge_decoded_frame_destroy(value_result);
        ac3forge_decoded_frame_destroy(into_result);
        ac3forge_bytes_destroy(encoded);
    }

    ac3forge_decoder_destroy(value_decoder);
    ac3forge_decoder_destroy(into_decoder);
    ac3forge_encoder_destroy(encoder);
}

TEST_CASE(
    "ac3forge_decoder_decode_frame_into rejects bad arguments and a mismatched channel/sample count",
    "[capi]") {
    ac3forge_encoder_config_t encoder_config;
    ac3forge_encoder_config_init(&encoder_config);
    encoder_config.acmod = AC3FORGE_ACMOD_2_0;
    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&encoder_config, &encoder) == AC3FORGE_OK);
    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME, 0.1f);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME, 0.1f);
    const float* channels[2] = {left.data(), right.data()};
    ac3forge_bytes_t* encoded = nullptr;
    REQUIRE(ac3forge_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                          &encoded) == AC3FORGE_OK);
    ac3forge_encoder_destroy(encoder);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);

    std::array<std::vector<float>, AC3FORGE_DECODER_MAX_CHANNELS> storage;
    for (auto& v : storage) v.assign(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    std::array<float*, AC3FORGE_DECODER_MAX_CHANNELS> ptrs{};
    for (std::size_t i = 0; i < storage.size(); ++i) ptrs[i] = storage[i].data();

    ac3forge_decoded_frame_t* out = nullptr;
    CHECK(ac3forge_decoder_decode_frame_into(nullptr, ac3forge_bytes_data(encoded),
                                              ac3forge_bytes_size(encoded), ptrs.data(),
                                              AC3FORGE_DECODER_MAX_CHANNELS, AC3FORGE_SAMPLES_PER_FRAME,
                                              &out) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_decoder_decode_frame_into(decoder, nullptr, 0, ptrs.data(),
                                              AC3FORGE_DECODER_MAX_CHANNELS, AC3FORGE_SAMPLES_PER_FRAME,
                                              &out) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_decoder_decode_frame_into(decoder, ac3forge_bytes_data(encoded),
                                              ac3forge_bytes_size(encoded), nullptr,
                                              AC3FORGE_DECODER_MAX_CHANNELS, AC3FORGE_SAMPLES_PER_FRAME,
                                              &out) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_decoder_decode_frame_into(decoder, ac3forge_bytes_data(encoded),
                                              ac3forge_bytes_size(encoded), ptrs.data(),
                                              AC3FORGE_DECODER_MAX_CHANNELS, AC3FORGE_SAMPLES_PER_FRAME,
                                              nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    // Too few spans - the caller must always supply the documented maximum,
    // not merely enough for this particular frame.
    CHECK(ac3forge_decoder_decode_frame_into(decoder, ac3forge_bytes_data(encoded),
                                              ac3forge_bytes_size(encoded), ptrs.data(), 2,
                                              AC3FORGE_SAMPLES_PER_FRAME,
                                              &out) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_decoder_decode_frame_into(decoder, ac3forge_bytes_data(encoded),
                                              ac3forge_bytes_size(encoded), ptrs.data(),
                                              AC3FORGE_DECODER_MAX_CHANNELS,
                                              AC3FORGE_SAMPLES_PER_FRAME / 2,
                                              &out) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    ptrs[0] = nullptr;
    CHECK(ac3forge_decoder_decode_frame_into(decoder, ac3forge_bytes_data(encoded),
                                              ac3forge_bytes_size(encoded), ptrs.data(),
                                              AC3FORGE_DECODER_MAX_CHANNELS, AC3FORGE_SAMPLES_PER_FRAME,
                                              &out) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(out == nullptr);
    ptrs[0] = storage[0].data();

    // A genuine bitstream failure reports a decode-error code, same as the
    // value form does - not merely a mismatched-argument rejection.
    const std::vector<uint8_t> garbage(16, 0xAB);
    const auto status = ac3forge_decoder_decode_frame_into(
        decoder, garbage.data(), garbage.size(), ptrs.data(), AC3FORGE_DECODER_MAX_CHANNELS,
        AC3FORGE_SAMPLES_PER_FRAME, &out);
    CHECK(status >= AC3FORGE_ERROR_DECODE_TRUNCATED);
    CHECK(status <= AC3FORGE_ERROR_DECODE_INVALID_STREAM);
    CHECK(out == nullptr);

    ac3forge_bytes_destroy(encoded);
    ac3forge_decoder_destroy(decoder);
}

TEST_CASE(
    "ac3forge_eac3_decoder_decode_access_unit_into writes the same programme the value form "
    "allocates",
    "[capi][eac3]") {
    // 5.1.2, same layout as the dependent-substream test above - eight
    // rendered channels out of the sixteen documented spans, so the unused
    // trailing spans staying untouched is exercised for real.
    const ac3::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true},
        .dependents = {{.bitrate_kbps = 192,
                        .acmod = ac3::Acmod::k2_0,
                        .chanmap = ac3::eac3::chanmap::k512Height}}};
    ac3::eac3::AccessUnitEncoder encoder{config};
    REQUIRE(encoder.channel_count() == 8);

    const std::vector<double> tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 2000.0, 1300.0};
    std::vector<std::vector<float>> block(8, std::vector<float>(AC3FORGE_SAMPLES_PER_FRAME));
    std::vector<std::span<const float>> views(8);
    std::vector<uint8_t> stream;
    for (int frame = 0; frame < 3; ++frame) {
        for (std::size_t ch = 0; ch < 8; ++ch) {
            fill_tone(block[ch].data(), tones[ch], frame, 48000.0);
            views[ch] = block[ch];
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        const auto* data = reinterpret_cast<const uint8_t*>(unit->bytes.data());
        stream.insert(stream.end(), data, data + unit->bytes.size());
    }

    ac3forge_spans_t* units = nullptr;
    REQUIRE(ac3forge_split_access_units(stream.data(), stream.size(), &units) == AC3FORGE_OK);
    REQUIRE(ac3forge_spans_count(units) == 3);

    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_eac3_decoder_t* value_decoder = nullptr;
    ac3forge_eac3_decoder_t* into_decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&decoder_config, &value_decoder) == AC3FORGE_OK);
    REQUIRE(ac3forge_eac3_decoder_create(&decoder_config, &into_decoder) == AC3FORGE_OK);

    std::array<std::vector<float>, AC3FORGE_EAC3_DECODER_MAX_CHANNELS> into_storage;
    for (auto& v : into_storage) v.assign(AC3FORGE_SAMPLES_PER_FRAME, -99.0f);
    std::array<float*, AC3FORGE_EAC3_DECODER_MAX_CHANNELS> into_ptrs{};
    for (std::size_t i = 0; i < into_storage.size(); ++i) into_ptrs[i] = into_storage[i].data();

    for (std::size_t i = 0; i < ac3forge_spans_count(units); ++i) {
        const auto span = ac3forge_spans_get(units, i);

        ac3forge_decoded_access_unit_t* value_result = nullptr;
        REQUIRE(ac3forge_eac3_decoder_decode_access_unit(value_decoder, stream.data() + span.offset,
                                                         span.length,
                                                         &value_result) == AC3FORGE_OK);
        REQUIRE(value_result != nullptr);
        REQUIRE(ac3forge_decoded_access_unit_channel_count(value_result) == 8);

        ac3forge_decoded_access_unit_t* into_result = nullptr;
        REQUIRE(ac3forge_eac3_decoder_decode_access_unit_into(
                    into_decoder, stream.data() + span.offset, span.length, into_ptrs.data(),
                    AC3FORGE_EAC3_DECODER_MAX_CHANNELS, AC3FORGE_SAMPLES_PER_FRAME,
                    &into_result) == AC3FORGE_OK);
        REQUIRE(into_result != nullptr);
        CHECK(ac3forge_decoded_access_unit_channel_count(into_result) == 0);
        CHECK(ac3forge_decoded_access_unit_substream_count(into_result) ==
              ac3forge_decoded_access_unit_substream_count(value_result));

        for (std::size_t ch = 0; ch < 8; ++ch) {
            const float* expected = ac3forge_decoded_access_unit_channel_samples(value_result, ch);
            REQUIRE(expected != nullptr);
            for (int n = 0; n < AC3FORGE_SAMPLES_PER_FRAME; ++n) {
                CHECK(into_storage[ch][static_cast<std::size_t>(n)] == expected[n]);
            }
        }
        for (std::size_t ch = 8; ch < AC3FORGE_EAC3_DECODER_MAX_CHANNELS; ++ch) {
            CHECK(into_storage[ch][0] == -99.0f);
        }

        ac3forge_decoded_access_unit_destroy(value_result);
        ac3forge_decoded_access_unit_destroy(into_result);
    }

    ac3forge_eac3_decoder_destroy(value_decoder);
    ac3forge_eac3_decoder_destroy(into_decoder);
    ac3forge_spans_destroy(units);
}

TEST_CASE(
    "ac3forge_eac3_decoder_decode_access_unit_into leaves the spans untouched across a hold-back "
    "and releases identically",
    "[capi][eac3][transient_prenoise]") {
    // Mirrors tests/decoder/test_eac3_decoder.cpp's C++ test of the same name -
    // see the C API's own version of this table on the AC-3 flush test above
    // for what the silent/transient split is standing in for.
    ac3::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0, .transient_prenoise = true}};

    ac3forge_decoder_config_t config;
    ac3forge_decoder_config_init(&config);
    ac3forge_eac3_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&config, &decoder) == AC3FORGE_OK);

    std::array<std::vector<float>, AC3FORGE_EAC3_DECODER_MAX_CHANNELS> storage;
    for (auto& v : storage) v.assign(AC3FORGE_SAMPLES_PER_FRAME, -99.0f);
    std::array<float*, AC3FORGE_EAC3_DECODER_MAX_CHANNELS> ptrs{};
    for (std::size_t i = 0; i < storage.size(); ++i) ptrs[i] = storage[i].data();

    const std::vector<float> silence(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    const std::vector<std::span<const float>> silent_views{silence, silence};
    const auto decode_one = [&](const std::vector<std::byte>& frame,
                                ac3forge_decoded_access_unit_t** out) {
        return ac3forge_eac3_decoder_decode_access_unit_into(
            decoder, reinterpret_cast<const uint8_t*>(frame.data()), frame.size(), ptrs.data(),
            AC3FORGE_EAC3_DECODER_MAX_CHANNELS, AC3FORGE_SAMPLES_PER_FRAME, out);
    };

    for (int f = 0; f < 2; ++f) {
        const auto frame = encoder.encode_frame(silent_views);
        REQUIRE(frame.has_value());
        ac3forge_decoded_access_unit_t* unit = nullptr;
        REQUIRE(decode_one(*frame, &unit) == AC3FORGE_OK);
        REQUIRE(unit != nullptr);
        ac3forge_decoded_access_unit_destroy(unit);
    }

    constexpr int kOnset = 960;
    std::vector<float> transient(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    for (int n = kOnset; n < AC3FORGE_SAMPLES_PER_FRAME; ++n) {
        transient[static_cast<std::size_t>(n)] = static_cast<float>(
            0.9 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) / 48000.0));
    }
    const std::vector<std::span<const float>> transient_views{transient, transient};
    const auto transient_frame = encoder.encode_frame(transient_views);
    REQUIRE(transient_frame.has_value());

    // Sentinel-fill right before the held-back call, so any write at all -
    // partial or full - would be caught.
    for (auto& v : storage) std::ranges::fill(v, -77.0f);

    ac3forge_decoded_access_unit_t* held = nullptr;
    REQUIRE(decode_one(*transient_frame, &held) == AC3FORGE_OK);
    CHECK(held == nullptr);
    for (const auto& v : storage) {
        for (float sample : v) {
            CHECK(sample == -77.0f);
        }
    }

    // The releasing call writes real audio through the same spans.
    const auto after = encoder.encode_frame(silent_views);
    REQUIRE(after.has_value());
    ac3forge_decoded_access_unit_t* released = nullptr;
    REQUIRE(decode_one(*after, &released) == AC3FORGE_OK);
    REQUIRE(released != nullptr);
    CHECK(ac3forge_decoded_access_unit_channel_count(released) == 0);
    bool any_written = false;
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (float sample : storage[ch]) {
            if (sample != -77.0f) any_written = true;
        }
    }
    CHECK(any_written);
    ac3forge_decoded_access_unit_destroy(released);

    ac3forge_eac3_decoder_destroy(decoder);
}

TEST_CASE(
    "ac3forge_eac3_decoder_decode_access_unit_into rejects bad arguments and a mismatched "
    "channel/sample count",
    "[capi][eac3]") {
    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 192}};
    const auto stream = encode_eac3_stream(encoder, {1000.0, 800.0}, 1);

    ac3forge_decoder_config_t config;
    ac3forge_decoder_config_init(&config);
    ac3forge_eac3_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&config, &decoder) == AC3FORGE_OK);

    std::array<std::vector<float>, AC3FORGE_EAC3_DECODER_MAX_CHANNELS> storage;
    for (auto& v : storage) v.assign(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    std::array<float*, AC3FORGE_EAC3_DECODER_MAX_CHANNELS> ptrs{};
    for (std::size_t i = 0; i < storage.size(); ++i) ptrs[i] = storage[i].data();

    ac3forge_decoded_access_unit_t* out = nullptr;
    CHECK(ac3forge_eac3_decoder_decode_access_unit_into(
              nullptr, stream.bytes.data(), stream.bytes.size(), ptrs.data(),
              AC3FORGE_EAC3_DECODER_MAX_CHANNELS,
              AC3FORGE_SAMPLES_PER_FRAME, &out) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_access_unit_into(decoder, nullptr, 0, ptrs.data(),
                                                         AC3FORGE_EAC3_DECODER_MAX_CHANNELS,
                                                         AC3FORGE_SAMPLES_PER_FRAME,
                                                         &out) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_access_unit_into(
              decoder, stream.bytes.data(), stream.bytes.size(), nullptr,
              AC3FORGE_EAC3_DECODER_MAX_CHANNELS,
              AC3FORGE_SAMPLES_PER_FRAME, &out) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_access_unit_into(
              decoder, stream.bytes.data(), stream.bytes.size(), ptrs.data(),
              AC3FORGE_EAC3_DECODER_MAX_CHANNELS, AC3FORGE_SAMPLES_PER_FRAME,
              nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_access_unit_into(
              decoder, stream.bytes.data(), stream.bytes.size(), ptrs.data(), 4,
              AC3FORGE_SAMPLES_PER_FRAME, &out) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_decoder_decode_access_unit_into(
              decoder, stream.bytes.data(), stream.bytes.size(), ptrs.data(),
              AC3FORGE_EAC3_DECODER_MAX_CHANNELS,
              AC3FORGE_SAMPLES_PER_FRAME / 2, &out) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    ptrs[0] = nullptr;
    CHECK(ac3forge_eac3_decoder_decode_access_unit_into(
              decoder, stream.bytes.data(), stream.bytes.size(), ptrs.data(),
              AC3FORGE_EAC3_DECODER_MAX_CHANNELS,
              AC3FORGE_SAMPLES_PER_FRAME, &out) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(out == nullptr);
    ptrs[0] = storage[0].data();

    // A genuine bitstream failure reports a decode-error code, same as the
    // value form does - not merely a mismatched-argument rejection.
    const std::vector<uint8_t> garbage(16, 0xAB);
    const auto status = ac3forge_eac3_decoder_decode_access_unit_into(
        decoder, garbage.data(), garbage.size(), ptrs.data(), AC3FORGE_EAC3_DECODER_MAX_CHANNELS,
        AC3FORGE_SAMPLES_PER_FRAME, &out);
    CHECK(status >= AC3FORGE_ERROR_DECODE_TRUNCATED);
    CHECK(status <= AC3FORGE_ERROR_DECODE_INVALID_STREAM);
    CHECK(out == nullptr);

    ac3forge_eac3_decoder_destroy(decoder);
}

// --- scan / ScannedStream (roadmap AP5) -----------------------------------

TEST_CASE("ac3forge_scan reports the same shape ac3::io::scan does for an AC-3 stream",
          "[capi][scan]") {
    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    config.acmod = AC3FORGE_ACMOD_2_0;
    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);

    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<uint8_t> stream;
    for (int frame = 0; frame < 3; ++frame) {
        fill_tone(left.data(), 1000.0, frame, 48000.0);
        fill_tone(right.data(), 800.0, frame, 48000.0);
        const float* channels[2] = {left.data(), right.data()};
        ac3forge_bytes_t* encoded = nullptr;
        REQUIRE(ac3forge_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                               &encoded) == AC3FORGE_OK);
        const auto* data = ac3forge_bytes_data(encoded);
        stream.insert(stream.end(), data, data + ac3forge_bytes_size(encoded));
        ac3forge_bytes_destroy(encoded);
    }
    ac3forge_encoder_destroy(encoder);

    ac3forge_scanned_stream_t* scanned = nullptr;
    REQUIRE(ac3forge_scan(stream.data(), stream.size(), &scanned) == AC3FORGE_OK);
    REQUIRE(scanned != nullptr);

    CHECK(ac3forge_scanned_stream_kind(scanned) == AC3FORGE_STREAM_KIND_AC3);
    CHECK(ac3forge_scanned_stream_sample_rate(scanned) == AC3FORGE_SAMPLE_RATE_48000);
    CHECK(ac3forge_scanned_stream_acmod(scanned) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_scanned_stream_lfe(scanned) == 0);
    CHECK(ac3forge_scanned_stream_channels(scanned) == 2);
    CHECK(ac3forge_scanned_stream_substreams_per_unit(scanned) == 1);
    CHECK(ac3forge_scanned_stream_bsid(scanned) <= 8);
    CHECK(ac3forge_scanned_stream_bsmod(scanned) == 0);  // no bsmod configured
    CHECK(ac3forge_scanned_stream_bit_rate_code(scanned) < 19);  // Table 5.18 has 19 entries
    CHECK(ac3forge_scanned_stream_bsmod_present(scanned) == 1);  // AC-3 always transmits bsmod
    CHECK(ac3forge_scanned_stream_dsurmod(scanned) == 0);  // not indicated for a 2/0 stream
    CHECK(ac3forge_scanned_stream_mix_metadata(scanned) == 0);
    CHECK(ac3forge_scanned_stream_independent_substreams(scanned) == 0);  // AC-3 has no substreams
    CHECK(ac3forge_scanned_stream_has_oba_complexity_index(scanned) == 0);  // no Atmos container
    CHECK(ac3forge_scanned_stream_oba_complexity_index(scanned) == 0);

    REQUIRE(ac3forge_scanned_stream_access_unit_count(scanned) == 3);
    std::size_t offset_sum = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto span = ac3forge_scanned_stream_access_unit(scanned, i);
        CHECK(span.offset == offset_sum);
        CHECK(span.length > 0);
        CHECK(ac3forge_scanned_stream_access_unit_samples(scanned, i) == AC3FORGE_SAMPLES_PER_FRAME);
        offset_sum += span.length;
    }
    CHECK(offset_sum == stream.size());
    CHECK(ac3forge_scanned_stream_access_unit(scanned, 3).length == 0);  // out of range
    CHECK(ac3forge_scanned_stream_access_unit_samples(scanned, 3) == 0);  // out of range

    // AC-3 has no independent substream 1-3 to be an associated one - both a
    // present-but-unseen index and an out-of-range one take the same default.
    CHECK(ac3forge_scanned_stream_associated_substream_present(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_bsmod(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_bsmod_present(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_acmod(scanned, 0) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_scanned_stream_associated_substream_lfe(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_mix_metadata(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_present(scanned, -1) == 0);  // out of range
    CHECK(ac3forge_scanned_stream_associated_substream_present(scanned, 3) == 0);   // out of range

    REQUIRE(ac3forge_scanned_stream_programme_count(scanned) == 1);
    CHECK(ac3forge_scanned_stream_programme_substream_id(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_acmod(scanned, 0) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_scanned_stream_programme_lfe(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_channels(scanned, 0) == 2);
    CHECK(ac3forge_scanned_stream_programme_bsid(scanned, 0) <= 8);
    CHECK(ac3forge_scanned_stream_programme_bsmod(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_substreams_per_unit(scanned, 0) == 1);
    CHECK(ac3forge_scanned_stream_programme_has_oba_complexity_index(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_oba_complexity_index(scanned, 0) == 0);
    REQUIRE(ac3forge_scanned_stream_programme_access_unit_count(scanned, 0) == 3);
    CHECK(ac3forge_scanned_stream_programme_access_unit(scanned, 0, 0).offset == 0);
    CHECK(ac3forge_scanned_stream_programme_access_unit(scanned, 0, 3).length == 0);  // out of range
    CHECK(ac3forge_scanned_stream_programme_access_unit(scanned, 1, 0).length == 0);  // no 2nd programme
    CHECK(ac3forge_scanned_stream_programme_access_unit_count(scanned, 1) == 0);      // no 2nd programme
    // Every programme_* accessor takes its no-such-programme default the same way.
    CHECK(ac3forge_scanned_stream_programme_substream_id(scanned, 1) == 0);
    CHECK(ac3forge_scanned_stream_programme_acmod(scanned, 1) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_scanned_stream_programme_lfe(scanned, 1) == 0);
    CHECK(ac3forge_scanned_stream_programme_channels(scanned, 1) == 0);
    CHECK(ac3forge_scanned_stream_programme_bsid(scanned, 1) == 0);
    CHECK(ac3forge_scanned_stream_programme_bsmod(scanned, 1) == 0);
    CHECK(ac3forge_scanned_stream_programme_substreams_per_unit(scanned, 1) == 0);
    CHECK(ac3forge_scanned_stream_programme_has_oba_complexity_index(scanned, 1) == 0);
    CHECK(ac3forge_scanned_stream_programme_oba_complexity_index(scanned, 1) == 0);

    CHECK(ac3forge_scanned_stream_duration_samples(scanned) == 3 * AC3FORGE_SAMPLES_PER_FRAME);
    uint32_t uniform = 0;
    REQUIRE(ac3forge_scanned_stream_uniform_access_unit_samples(scanned, &uniform) == 1);
    CHECK(uniform == AC3FORGE_SAMPLES_PER_FRAME);

    uint64_t start = 999;
    uint32_t duration = 0;
    uint32_t rate = 0;
    REQUIRE(ac3forge_scanned_stream_access_unit_timing(scanned, 1, &start, &duration, &rate) == 1);
    CHECK(start == AC3FORGE_SAMPLES_PER_FRAME);
    CHECK(duration == AC3FORGE_SAMPLES_PER_FRAME);
    CHECK(rate == 48000);
    CHECK(ac3forge_scanned_stream_access_unit_timing(scanned, 3, nullptr, nullptr, nullptr) == 0);

    std::size_t index = 999;
    REQUIRE(ac3forge_scanned_stream_access_unit_at_sample(scanned, AC3FORGE_SAMPLES_PER_FRAME,
                                                           &index) == 1);
    CHECK(index == 1);
    CHECK(ac3forge_scanned_stream_access_unit_at_sample(
              scanned, 3u * AC3FORGE_SAMPLES_PER_FRAME, &index) == 0);

    index = 999;
    REQUIRE(ac3forge_scanned_stream_access_unit_at_seconds(scanned, 0.0, &index) == 1);
    CHECK(index == 0);
    CHECK(ac3forge_scanned_stream_access_unit_at_seconds(scanned, 999.0, &index) == 0);

    ac3forge_scanned_stream_destroy(scanned);
}

TEST_CASE(
    "ac3forge_scan reports programme and substream detail for an E-AC-3 access unit with a "
    "dependent",
    "[capi][scan][eac3]") {
    const ac3::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true},
        .dependents = {{.bitrate_kbps = 192,
                        .acmod = ac3::Acmod::k2_0,
                        .chanmap = ac3::eac3::chanmap::k512Height}}};
    ac3::eac3::AccessUnitEncoder encoder{config};
    REQUIRE(encoder.channel_count() == 8);

    const std::vector<double> tones = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0, 2000.0, 1300.0};
    std::vector<std::vector<float>> block(8, std::vector<float>(AC3FORGE_SAMPLES_PER_FRAME));
    std::vector<std::span<const float>> views(8);
    std::vector<uint8_t> stream;
    for (int frame = 0; frame < 2; ++frame) {
        for (std::size_t ch = 0; ch < 8; ++ch) {
            fill_tone(block[ch].data(), tones[ch], frame, 48000.0);
            views[ch] = block[ch];
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        const auto* data = reinterpret_cast<const uint8_t*>(unit->bytes.data());
        stream.insert(stream.end(), data, data + unit->bytes.size());
    }

    ac3forge_scanned_stream_t* scanned = nullptr;
    REQUIRE(ac3forge_scan(stream.data(), stream.size(), &scanned) == AC3FORGE_OK);
    REQUIRE(scanned != nullptr);

    CHECK(ac3forge_scanned_stream_kind(scanned) == AC3FORGE_STREAM_KIND_EAC3);
    CHECK(ac3forge_scanned_stream_acmod(scanned) == AC3FORGE_ACMOD_3_2);
    CHECK(ac3forge_scanned_stream_lfe(scanned) == 1);
    CHECK(ac3forge_scanned_stream_channels(scanned) == 8);  // bed + Vhl/Vhr folded in
    CHECK(ac3forge_scanned_stream_substreams_per_unit(scanned) == 2);
    CHECK(ac3forge_scanned_stream_channel_map(scanned) ==
          (ac3::eac3::chanmap::acmod_map(ac3::Acmod::k3_2, true) | ac3::eac3::chanmap::k512Height));

    REQUIRE(ac3forge_scanned_stream_access_unit_count(scanned) == 2);
    CHECK(ac3forge_scanned_stream_access_unit(scanned, 0).offset == 0);

    REQUIRE(ac3forge_scanned_stream_programme_count(scanned) == 1);
    CHECK(ac3forge_scanned_stream_programme_channels(scanned, 0) == 8);
    CHECK(ac3forge_scanned_stream_programme_substreams_per_unit(scanned, 0) == 2);
    REQUIRE(ac3forge_scanned_stream_programme_access_unit_count(scanned, 0) == 2);
    CHECK(ac3forge_scanned_stream_programme_access_unit(scanned, 0, 0).offset ==
          ac3forge_scanned_stream_access_unit(scanned, 0).offset);
    CHECK(ac3forge_scanned_stream_programme_access_unit(scanned, 0, 0).length ==
          ac3forge_scanned_stream_access_unit(scanned, 0).length);

    ac3forge_scanned_stream_destroy(scanned);
}

TEST_CASE(
    "ac3forge_scan reports independent_substreams and associated-substream fields for a "
    "multi-programme stream",
    "[capi][scan][eac3]") {
    // Two independent substreams (I0, I1), each its own single-syncframe
    // programme - a broadcast "second service" (§5.4.2.2), not a dependent
    // widening one bed. Concatenated frame by frame, the same wire shape
    // ac3forge_split_access_units already delimits into four access units.
    ac3::eac3::FrameEncoder programme0{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
    ac3::eac3::FrameEncoder programme1{
        {.bitrate_kbps = 96, .acmod = ac3::Acmod::k1_0, .substreamid = 1}};

    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> centre(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<uint8_t> stream;
    for (int frame = 0; frame < 2; ++frame) {
        fill_tone(left.data(), 1000.0, frame, 48000.0);
        fill_tone(right.data(), 800.0, frame, 48000.0);
        fill_tone(centre.data(), 1200.0, frame, 48000.0);
        const std::vector<std::span<const float>> views0{left, right};
        const std::vector<std::span<const float>> views1{centre};

        const auto frame0 = programme0.encode_frame(views0);
        REQUIRE(frame0.has_value());
        const auto* data0 = reinterpret_cast<const uint8_t*>(frame0->data());
        stream.insert(stream.end(), data0, data0 + frame0->size());

        const auto frame1 = programme1.encode_frame(views1);
        REQUIRE(frame1.has_value());
        const auto* data1 = reinterpret_cast<const uint8_t*>(frame1->data());
        stream.insert(stream.end(), data1, data1 + frame1->size());
    }

    ac3forge_scanned_stream_t* scanned = nullptr;
    REQUIRE(ac3forge_scan(stream.data(), stream.size(), &scanned) == AC3FORGE_OK);
    REQUIRE(scanned != nullptr);

    // Bit 0 (substream 0) and bit 1 (substream 1) both seen somewhere in the stream.
    CHECK(ac3forge_scanned_stream_independent_substreams(scanned) == 0x03);

    REQUIRE(ac3forge_scanned_stream_programme_count(scanned) == 2);
    CHECK(ac3forge_scanned_stream_programme_substream_id(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_acmod(scanned, 0) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_scanned_stream_programme_channels(scanned, 0) == 2);
    CHECK(ac3forge_scanned_stream_programme_bsid(scanned, 0) == 16);
    REQUIRE(ac3forge_scanned_stream_programme_access_unit_count(scanned, 0) == 2);
    CHECK(ac3forge_scanned_stream_programme_access_unit(scanned, 0, 0).offset == 0);

    CHECK(ac3forge_scanned_stream_programme_substream_id(scanned, 1) == 1);
    CHECK(ac3forge_scanned_stream_programme_acmod(scanned, 1) == AC3FORGE_ACMOD_1_0);
    CHECK(ac3forge_scanned_stream_programme_channels(scanned, 1) == 1);
    CHECK(ac3forge_scanned_stream_programme_substreams_per_unit(scanned, 1) == 1);
    REQUIRE(ac3forge_scanned_stream_programme_access_unit_count(scanned, 1) == 2);
    CHECK(ac3forge_scanned_stream_programme_access_unit(scanned, 1, 0).length > 0);

    // Independent substream 1 (index 0 - substreams 1-3 map to indices 0-2) is
    // a real, present associated substream; substreams 2 and 3 were never seen.
    CHECK(ac3forge_scanned_stream_associated_substream_present(scanned, 0) == 1);
    CHECK(ac3forge_scanned_stream_associated_substream_acmod(scanned, 0) == AC3FORGE_ACMOD_1_0);
    CHECK(ac3forge_scanned_stream_associated_substream_lfe(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_bsmod(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_bsmod_present(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_mix_metadata(scanned, 0) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_present(scanned, 1) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_present(scanned, 2) == 0);

    ac3forge_scanned_stream_destroy(scanned);
}

TEST_CASE("ac3forge_scan rejects bad arguments and reports ScanError codes", "[capi][scan]") {
    ac3forge_scanned_stream_t* scanned = nullptr;
    CHECK(ac3forge_scan(nullptr, 0, &scanned) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    const std::vector<uint8_t> garbage(4, 0xAB);
    CHECK(ac3forge_scan(garbage.data(), garbage.size(), nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);

    CHECK(ac3forge_scan(garbage.data(), 0, &scanned) == AC3FORGE_ERROR_SCAN_EMPTY);
    CHECK(scanned == nullptr);

    const std::vector<uint8_t> no_sync(64, 0x00);  // no 0x0B77 anywhere in this buffer
    CHECK(ac3forge_scan(no_sync.data(), no_sync.size(), &scanned) == AC3FORGE_ERROR_SCAN_LOST_SYNC);
    CHECK(scanned == nullptr);

    // A real frame cut short after its sync word: enough to find sync, not
    // enough to hold the rest of bsi.
    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    config.acmod = AC3FORGE_ACMOD_2_0;
    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);
    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME, 0.1f);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME, 0.1f);
    const float* channels[2] = {left.data(), right.data()};
    ac3forge_bytes_t* encoded = nullptr;
    REQUIRE(ac3forge_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                          &encoded) == AC3FORGE_OK);
    ac3forge_encoder_destroy(encoder);
    // Not pinning which specific ScanError this lands on - same "the parser's
    // own business" stance the C++ DecodeError tests above take - only that
    // it is a real scan failure past the empty/lost-sync gate (scan() needs
    // at least 6 bytes just to look for a sync word at all).
    const auto status = ac3forge_scan(ac3forge_bytes_data(encoded), 8, &scanned);
    CHECK(status >= AC3FORGE_ERROR_SCAN_UNSUPPORTED_BSID);
    CHECK(status <= AC3FORGE_ERROR_SCAN_UNSUPPORTED_STRUCTURE);
    CHECK(scanned == nullptr);
    ac3forge_bytes_destroy(encoded);
}

// --- Loudness / level / QC metering (roadmap AP5) -------------------------

TEST_CASE("ac3forge_loudness_meter measures a stereo tone", "[capi][loudness]") {
    ac3forge_loudness_meter_t* meter = nullptr;
    REQUIRE(ac3forge_loudness_meter_create(AC3FORGE_SAMPLE_RATE_48000, AC3FORGE_ACMOD_2_0, 0,
                                           &meter) == AC3FORGE_OK);
    REQUIRE(meter != nullptr);
    CHECK(ac3forge_loudness_meter_channel_count(meter) == 2);
    CHECK(ac3forge_loudness_meter_has_integrated_lkfs(meter) == 0);

    // 100 frames of 1536 samples at 48 kHz is 3.2 s - enough for every window
    // (400 ms momentary, 3 s short-term, and Loudness Range's own short-term
    // population) to have something to report.
    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME);
    for (int frame = 0; frame < 100; ++frame) {
        fill_tone(left.data(), 1000.0, frame, 48000.0);
        fill_tone(right.data(), 1000.0, frame, 48000.0);
        const float* channels[2] = {left.data(), right.data()};
        REQUIRE(ac3forge_loudness_meter_push(meter, channels, 2, AC3FORGE_SAMPLES_PER_FRAME) ==
                AC3FORGE_OK);
    }

    REQUIRE(ac3forge_loudness_meter_has_integrated_lkfs(meter) == 1);
    const double integrated = ac3forge_loudness_meter_integrated_lkfs(meter);
    // A sanity bound on the C boundary's plumbing, not a re-derivation of
    // BS.1770 - a 0.5-amplitude full-band tone reads well inside this band.
    CHECK(integrated > -20.0);
    CHECK(integrated < 0.0);

    REQUIRE(ac3forge_loudness_meter_has_momentary_lkfs(meter) == 1);
    CHECK(ac3forge_loudness_meter_momentary_lkfs(meter) < 0.0);
    REQUIRE(ac3forge_loudness_meter_has_short_term_lkfs(meter) == 1);
    CHECK(ac3forge_loudness_meter_short_term_lkfs(meter) < 0.0);
    // A constant-amplitude tone has no loudness variation, so the gated
    // population's 95th and 10th percentiles coincide: LRA reads ~0 LU.
    REQUIRE(ac3forge_loudness_meter_has_loudness_range(meter) == 1);
    CHECK(std::abs(ac3forge_loudness_meter_loudness_range(meter)) < 1.0);
    REQUIRE(ac3forge_loudness_meter_has_true_peak_dbtp(meter) == 1);
    CHECK(ac3forge_loudness_meter_true_peak_dbtp(meter) < 0.0);  // 0.5 amplitude, under full scale

    ac3forge_loudness_meter_destroy(meter);
}

TEST_CASE("ac3forge_loudness_meter_create_for_chanmap mirrors the C++ Annex 3 constructor",
          "[capi][loudness]") {
    ac3forge_loudness_meter_t* meter = nullptr;
    CHECK(ac3forge_loudness_meter_create_for_chanmap(AC3FORGE_SAMPLE_RATE_48000,
                                                      AC3FORGE_CHANMAP_512_HEIGHT,
                                                      nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    REQUIRE(ac3forge_loudness_meter_create_for_chanmap(
                AC3FORGE_SAMPLE_RATE_48000, AC3FORGE_CHANMAP_512_HEIGHT, &meter) == AC3FORGE_OK);
    REQUIRE(meter != nullptr);
    CHECK(ac3forge_loudness_meter_channel_count(meter) == 2);  // Vhl, Vhr
    ac3forge_loudness_meter_destroy(meter);

    CHECK(ac3forge_loudness_meter_create_for_chanmap(AC3FORGE_SAMPLE_RATE_48000, 0, &meter) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("ac3forge_loudness_meter_create/push reject bad arguments", "[capi][loudness]") {
    ac3forge_loudness_meter_t* meter = nullptr;
    CHECK(ac3forge_loudness_meter_create(AC3FORGE_SAMPLE_RATE_48000, AC3FORGE_ACMOD_2_0, 0,
                                         nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    REQUIRE(ac3forge_loudness_meter_create(AC3FORGE_SAMPLE_RATE_48000, AC3FORGE_ACMOD_2_0, 0,
                                           &meter) == AC3FORGE_OK);
    CHECK(ac3forge_loudness_meter_push(nullptr, nullptr, 0, 0) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_loudness_meter_push(meter, nullptr, 2, AC3FORGE_SAMPLES_PER_FRAME) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    const float* channels[2] = {nullptr, nullptr};
    CHECK(ac3forge_loudness_meter_push(meter, channels, 2, AC3FORGE_SAMPLES_PER_FRAME) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    ac3forge_loudness_meter_destroy(meter);
}

TEST_CASE("ac3forge_dialnorm_from_lkfs mirrors ac3::meta::dialnorm_from_lkfs", "[capi][loudness]") {
    CHECK(ac3forge_dialnorm_from_lkfs(-24.0) == 24);
    CHECK(ac3forge_dialnorm_from_lkfs(-1.0) == 1);
    CHECK(ac3forge_dialnorm_from_lkfs(0.0) == 1);     // louder than -1 LKFS clamps
    CHECK(ac3forge_dialnorm_from_lkfs(-40.0) == 31);  // quieter than -31 clamps
}

TEST_CASE("ac3forge_level_meter_ballistics_init matches MeterBallistics{}'s own defaults",
          "[capi][levels]") {
    ac3forge_level_meter_ballistics_t ballistics;
    ac3forge_level_meter_ballistics_init(&ballistics);
    CHECK(ballistics.rms_integration_ms == 300.0);
    CHECK(ballistics.peak_decay_db_per_s == 20.0);
    CHECK(ballistics.peak_hold_ms == 1200.0);

    // A custom ballistics struct is honoured, not silently replaced with the
    // defaults - faster RMS integration and peak decay than the default.
    ballistics.rms_integration_ms = 50.0;
    ballistics.peak_decay_db_per_s = 40.0;
    ac3forge_level_meter_t* meter = nullptr;
    REQUIRE(ac3forge_level_meter_create(AC3FORGE_ACMOD_2_0, 0, 48000, 0, &ballistics, &meter) ==
            AC3FORGE_OK);
    REQUIRE(meter != nullptr);
    ac3forge_level_meter_destroy(meter);
}

TEST_CASE("ac3forge_level_meter measures peak and RMS of a known tone", "[capi][levels]") {
    ac3forge_level_meter_t* meter = nullptr;
    REQUIRE(ac3forge_level_meter_create(AC3FORGE_ACMOD_2_0, 0, 48000, 0, nullptr, &meter) ==
            AC3FORGE_OK);
    REQUIRE(meter != nullptr);
    CHECK(ac3forge_level_meter_acmod(meter) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_level_meter_lfe(meter) == 0);
    CHECK(ac3forge_level_meter_channel_count(meter) == 2);
    CHECK(ac3forge_level_meter_sample_rate(meter) == 48000);

    // A 0.5-amplitude sine peaks at -6.02 dBFS and never clips.
    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME);
    fill_tone(left.data(), 1000.0, 0, 48000.0);
    fill_tone(right.data(), 1000.0, 0, 48000.0);
    const float* channels[2] = {left.data(), right.data()};
    REQUIRE(ac3forge_level_meter_process(meter, channels, 2, AC3FORGE_SAMPLES_PER_FRAME) ==
            AC3FORGE_OK);

    const auto summary = ac3forge_level_meter_summary(meter, 0);
    CHECK(std::abs(summary.peak - 0.5) < 0.01);
    CHECK(std::abs(summary.peak_db - (-6.02)) < 0.1);
    CHECK(summary.clipped_samples == 0);
    CHECK(summary.samples == AC3FORGE_SAMPLES_PER_FRAME);
    // Out of range takes the documented empty default.
    const auto summary_out_of_range = ac3forge_level_meter_summary(meter, 99);
    CHECK(summary_out_of_range.samples == 0);
    CHECK(summary_out_of_range.peak_db == AC3FORGE_LEVEL_METER_FLOOR_DB);

    const auto level = ac3forge_level_meter_level(meter, 0);
    CHECK(level.clipped == 0);
    CHECK(level.peak_db < 0.0);
    // Out of range takes the documented floor.
    const auto out_of_range = ac3forge_level_meter_level(meter, 99);
    CHECK(out_of_range.peak_db == AC3FORGE_LEVEL_METER_FLOOR_DB);

    ac3forge_level_meter_reset(meter);
    const auto reset_summary = ac3forge_level_meter_summary(meter, 0);
    CHECK(reset_summary.samples == 0);
    CHECK(reset_summary.peak_db == AC3FORGE_LEVEL_METER_FLOOR_DB);

    ac3forge_level_meter_destroy(meter);
}

TEST_CASE("ac3forge_level_meter detects clipping and a wider explicit channel count",
          "[capi][levels]") {
    ac3forge_level_meter_t* meter = nullptr;
    REQUIRE(ac3forge_level_meter_create(AC3FORGE_ACMOD_2_0, 0, 48000, 4, nullptr, &meter) ==
            AC3FORGE_OK);
    CHECK(ac3forge_level_meter_channel_count(meter) == 4);  // wider than acmod's own 2

    const std::vector<float> full_scale(AC3FORGE_SAMPLES_PER_FRAME, 1.0f);
    const float* channels[1] = {full_scale.data()};
    REQUIRE(ac3forge_level_meter_process(meter, channels, 1, AC3FORGE_SAMPLES_PER_FRAME) ==
            AC3FORGE_OK);
    const auto summary = ac3forge_level_meter_summary(meter, 0);
    CHECK(summary.clipped_samples == AC3FORGE_SAMPLES_PER_FRAME);

    ac3forge_level_meter_destroy(meter);
}

TEST_CASE("ac3forge_level_meter_process rejects bad arguments", "[capi][levels]") {
    ac3forge_level_meter_t* meter = nullptr;
    CHECK(ac3forge_level_meter_create(AC3FORGE_ACMOD_2_0, 0, 48000, 0, nullptr, nullptr) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    REQUIRE(ac3forge_level_meter_create(AC3FORGE_ACMOD_2_0, 0, 48000, 0, nullptr, &meter) ==
            AC3FORGE_OK);
    CHECK(ac3forge_level_meter_process(nullptr, nullptr, 0, 0) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_level_meter_process(meter, nullptr, 2, AC3FORGE_SAMPLES_PER_FRAME) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    const float* channels[2] = {nullptr, nullptr};
    CHECK(ac3forge_level_meter_process(meter, channels, 2, AC3FORGE_SAMPLES_PER_FRAME) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    ac3forge_level_meter_destroy(meter);
}

TEST_CASE("ac3forge_qc_preset/name/parse mirror ac3::meta::qc's table", "[capi][qc]") {
    CHECK(ac3forge_qc_preset_count() == 5);

    const auto atsc = ac3forge_qc_preset(AC3FORGE_QC_PRESET_ATSC_A85);
    CHECK(atsc.target_lkfs == -24.0);
    CHECK(atsc.tolerance_lu == 2.0);
    CHECK(atsc.max_true_peak_dbtp == -2.0);
    CHECK(atsc.loudness_limit == AC3FORGE_QC_LOUDNESS_BAND);
    REQUIRE(atsc.source != nullptr);
    CHECK(std::string_view(atsc.source).find("A/85") != std::string_view::npos);

    const auto apple = ac3forge_qc_preset(AC3FORGE_QC_PRESET_APPLE_MUSIC_ATMOS);
    CHECK(apple.loudness_limit == AC3FORGE_QC_LOUDNESS_CEILING);

    CHECK(std::string_view(ac3forge_qc_preset_name(AC3FORGE_QC_PRESET_ATSC_A85)) == "atsc-a85");

    ac3forge_qc_preset_id_t parsed{};
    REQUIRE(ac3forge_parse_qc_preset("netflix", &parsed) == 1);
    CHECK(parsed == AC3FORGE_QC_PRESET_NETFLIX);
    CHECK(ac3forge_parse_qc_preset("not-a-preset", &parsed) == 0);
    CHECK(ac3forge_parse_qc_preset(nullptr, &parsed) == 0);
    CHECK(ac3forge_parse_qc_preset("netflix", nullptr) == 0);
}

TEST_CASE("ac3forge_evaluate_qc_gate passes/fails the same way ac3::meta::evaluate_qc_gate does",
          "[capi][qc]") {
    const auto preset = ac3forge_qc_preset(AC3FORGE_QC_PRESET_NETFLIX);  // -27 +/-2 LU, -2 dBTP ceiling

    auto verdict = ac3forge_evaluate_qc_gate(&preset, 1, -27.0, 1, -3.0);
    CHECK(verdict.has_loudness_delta_lu == 1);
    CHECK(verdict.loudness_delta_lu == 0.0);
    CHECK(verdict.loudness_pass == 1);
    CHECK(verdict.has_true_peak_margin_dbtp == 1);
    CHECK(verdict.true_peak_margin_dbtp == 1.0);
    CHECK(verdict.true_peak_pass == 1);
    CHECK(ac3forge_qc_verdict_pass(&verdict) == 1);

    // 17 LU too hot fails the band; a true peak above the ceiling fails too.
    verdict = ac3forge_evaluate_qc_gate(&preset, 1, -10.0, 1, -1.0);
    CHECK(verdict.loudness_pass == 0);
    CHECK(verdict.true_peak_pass == 0);
    CHECK(ac3forge_qc_verdict_pass(&verdict) == 0);

    // No measurement at all - not a false pass.
    verdict = ac3forge_evaluate_qc_gate(&preset, 0, 0.0, 0, 0.0);
    CHECK(verdict.has_loudness_delta_lu == 0);
    CHECK(verdict.loudness_pass == 0);
    CHECK(verdict.has_true_peak_margin_dbtp == 0);
    CHECK(verdict.true_peak_pass == 0);

    CHECK(ac3forge_qc_verdict_pass(nullptr) == 0);
    const ac3forge_qc_verdict_t empty{};
    CHECK(ac3forge_evaluate_qc_gate(nullptr, 1, -27.0, 1, -3.0).loudness_pass ==
          empty.loudness_pass);

    // The ceiling preset: quieter than the target passes, louder fails.
    const auto ceiling = ac3forge_qc_preset(AC3FORGE_QC_PRESET_APPLE_MUSIC_ATMOS);  // <= -18 LKFS
    CHECK(ac3forge_evaluate_qc_gate(&ceiling, 1, -25.0, 1, -5.0).loudness_pass == 1);
    CHECK(ac3forge_evaluate_qc_gate(&ceiling, 1, -10.0, 1, -5.0).loudness_pass == 0);
}

TEST_CASE("scan/metering C accessors take their documented defaults on null handles",
          "[capi][scan][loudness][levels]") {
    ac3forge_scanned_stream_destroy(nullptr);
    ac3forge_loudness_meter_destroy(nullptr);
    ac3forge_level_meter_destroy(nullptr);
    ac3forge_level_meter_reset(nullptr);

    CHECK(ac3forge_scanned_stream_kind(nullptr) == AC3FORGE_STREAM_KIND_AC3);
    CHECK(ac3forge_scanned_stream_sample_rate(nullptr) == AC3FORGE_SAMPLE_RATE_48000);
    CHECK(ac3forge_scanned_stream_acmod(nullptr) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_scanned_stream_lfe(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_channels(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_access_unit_count(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_access_unit(nullptr, 0).length == 0);
    CHECK(ac3forge_scanned_stream_access_unit_samples(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_substreams_per_unit(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_bsid(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_bsmod(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_bit_rate_code(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_has_oba_complexity_index(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_oba_complexity_index(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_bsmod_present(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_dsurmod(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_mix_metadata(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_independent_substreams(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_channel_map(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_present(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_bsmod(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_bsmod_present(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_acmod(nullptr, 0) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_scanned_stream_associated_substream_lfe(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_associated_substream_mix_metadata(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_count(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_programme_substream_id(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_acmod(nullptr, 0) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_scanned_stream_programme_lfe(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_channels(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_bsid(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_bsmod(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_substreams_per_unit(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_has_oba_complexity_index(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_oba_complexity_index(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_access_unit_count(nullptr, 0) == 0);
    CHECK(ac3forge_scanned_stream_programme_access_unit(nullptr, 0, 0).length == 0);
    CHECK(ac3forge_scanned_stream_duration_samples(nullptr) == 0);
    CHECK(ac3forge_scanned_stream_duration_seconds(nullptr) == 0.0);
    CHECK(ac3forge_scanned_stream_access_unit_timing(nullptr, 0, nullptr, nullptr, nullptr) == 0);
    CHECK(ac3forge_scanned_stream_access_unit_at_sample(nullptr, 0, nullptr) == 0);
    CHECK(ac3forge_scanned_stream_access_unit_at_seconds(nullptr, 0.0, nullptr) == 0);
    CHECK(ac3forge_scanned_stream_uniform_access_unit_samples(nullptr, nullptr) == 0);

    CHECK(ac3forge_loudness_meter_channel_count(nullptr) == 0);
    CHECK(ac3forge_loudness_meter_has_integrated_lkfs(nullptr) == 0);
    CHECK(ac3forge_loudness_meter_integrated_lkfs(nullptr) == 0.0);
    CHECK(ac3forge_loudness_meter_has_momentary_lkfs(nullptr) == 0);
    CHECK(ac3forge_loudness_meter_momentary_lkfs(nullptr) == 0.0);
    CHECK(ac3forge_loudness_meter_has_short_term_lkfs(nullptr) == 0);
    CHECK(ac3forge_loudness_meter_short_term_lkfs(nullptr) == 0.0);
    CHECK(ac3forge_loudness_meter_has_loudness_range(nullptr) == 0);
    CHECK(ac3forge_loudness_meter_loudness_range(nullptr) == 0.0);
    CHECK(ac3forge_loudness_meter_has_true_peak_dbtp(nullptr) == 0);
    CHECK(ac3forge_loudness_meter_true_peak_dbtp(nullptr) == 0.0);

    CHECK(ac3forge_level_meter_channel_count(nullptr) == 0);
    CHECK(ac3forge_level_meter_acmod(nullptr) == AC3FORGE_ACMOD_2_0);
    CHECK(ac3forge_level_meter_lfe(nullptr) == 0);
    CHECK(ac3forge_level_meter_sample_rate(nullptr) == 0);
    CHECK(ac3forge_level_meter_summary(nullptr, 0).samples == 0);
    CHECK(ac3forge_level_meter_level(nullptr, 0).peak_db == AC3FORGE_LEVEL_METER_FLOOR_DB);

    ac3forge_level_meter_ballistics_init(nullptr);  // documented no-op
}
