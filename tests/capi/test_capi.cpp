// ac3forge_c (C API) round-trips and error paths, exercised from C++ via
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
// encoder - the same tool combinations tests/ac3/encoder/test_eac3.cpp proves stack
// correctly - and hold the C decode surface to the behaviour
// tests/ac3/decoder/test_eac3_decoder.cpp establishes for the C++ one.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/meta/drc.hpp"
#include "iclforge_c/iclforge.h"
#include "iclforge/ac4/ac4.hpp"
#include "iclforge/ac4dec/decoder.hpp"
#include "iclforge/ac4enc/encoder.hpp"

namespace {

void fill_tone(float* out, double hz, int frame, double rate) {
    for (int n = 0; n < AC3FORGE_SAMPLES_PER_FRAME; ++n) {
        const double t = (frame * AC3FORGE_SAMPLES_PER_FRAME + n) / rate;
        out[static_cast<std::size_t>(n)] =
            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * hz * t));
    }
}

// Direct sample SNR with the codec's 256-sample delay, skipping the warm-up
// frame at each end - the same measurement tests/ac3/decoder/test_eac3_decoder.cpp makes,
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
Eac3Stream encode_eac3_stream(iclforge::eac3::FrameEncoder& encoder,
                              const std::vector<double>& tones, int frames) {
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

// AC3FORGE_C_VERSION_* (AP1) is the SDK version this file compiled against;
// ac3forge_version() is what actually got linked. In this test binary - built
// and linked from the same tree in the same invocation - the two must agree.
TEST_CASE("AC3FORGE_C_VERSION macros agree with the linked runtime version", "[capi]") {
    const ac3forge_version_t version = ac3forge_version();
    CHECK(AC3FORGE_C_VERSION_MAJOR == version.major);
    CHECK(AC3FORGE_C_VERSION_MINOR == version.minor);
    CHECK(AC3FORGE_C_VERSION_PATCH == version.patch);
    static_assert(AC3FORGE_C_VERSION ==
                  AC3FORGE_C_VERSION_MAJOR * 1000000 + AC3FORGE_C_VERSION_MINOR * 1000 +
                      AC3FORGE_C_VERSION_PATCH);
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
    // AC3FORGE_ERROR_UNSUPPORTED is what a build without a codec (an
    // AC3FORGE_BUILD_AC4=OFF one, today) returns from that codec's every
    // fallible entry point - see ac3forge.h's own comment on it - so this
    // build, with AC4 on, never produces it through a real call; message()
    // is a pure enum-to-string map, so checking it directly needs no such
    // build to exist.
    CHECK(std::string_view(ac3forge_status_message(AC3FORGE_ERROR_UNSUPPORTED)) ==
          "not built into this library");
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

TEST_CASE("ac3forge_decoder_decode_frame reports the same errors iclforge::FrameDecoder does",
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
    // on OAMD's quantizer grid - see tests/ac3/oba/test_oba.cpp's own comment on why
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
        iclforge::eac3::FrameConfig config;
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
        iclforge::eac3::FrameEncoder encoder{combo.config};
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
                // detector (see tests/ac3/decoder/test_eac3_decoder.cpp's tone-choice
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
    // iclforge::eac3::AccessUnitEncoder's own constructor validates eagerly and
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

    // §E2.3.1.2 allows eight dependents; a larger count is refused up front,
    // before anything is sized from it or read past the caller's array -
    // SIZE_MAX used to reach a reserve() that threw std::length_error and came
    // back as AC3FORGE_ERROR_INTERNAL.
    std::vector<ac3forge_eac3_frame_config_t> nine(9, dependent);
    au_encoder = nullptr;
    CHECK(ac3forge_eac3_access_unit_encoder_create(&independent, nine.data(), 9, &au_encoder) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_eac3_access_unit_encoder_create(&independent, nine.data(), SIZE_MAX,
                                                    &au_encoder) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(au_encoder == nullptr);
    REQUIRE(ac3forge_eac3_access_unit_encoder_create(&independent, nine.data(), 8, &au_encoder) ==
            AC3FORGE_OK);
    ac3forge_eac3_access_unit_encoder_destroy(au_encoder);
}

TEST_CASE("E-AC-3 access units with a dependent substream cross the C API intact",
          "[capi][eac3]") {
    // 5.1.2: a 3/2+LFE bed plus one dependent substream carrying Vhl/Vhr -
    // the same layout family tests/ac3/decoder/test_eac3_decoder.cpp proves against the
    // C++ decoder; here the C access-unit surface is what walks it.
    const iclforge::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = iclforge::Acmod::k3_2, .lfe = true},
        .dependents = {{.bitrate_kbps = 192,
                        .acmod = iclforge::Acmod::k2_0,
                        .chanmap = iclforge::eac3::chanmap::k512Height}}};
    iclforge::eac3::AccessUnitEncoder encoder{config};
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
        // §E3.8.5: the dependent's compre marks the end of the program and
        // brings the program's compr word with it - unity, since nothing asked
        // for heavy compression - and that is the word the unit reports.
        CHECK(ac3forge_decoded_access_unit_has_compr(unit) == 1);
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
    CHECK(ac3forge_decoded_substream_chanmap(dependent) == iclforge::eac3::chanmap::k512Height);
    CHECK(ac3forge_decoded_substream_location_map(dependent) ==
          iclforge::eac3::chanmap::k512Height);
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
    // field the substream accessors expose, Ch2's included. The tones peak at
    // -6 dBFS, which an RF-mode decode lifts by 7 dB for Ch1 (dialnorm 27)
    // and 5 dB for Ch2 (dialnorm 25): past the default -0.5 dBFS ceiling for
    // Ch1, and past the -3 dBFS one Ch2 is given here, so both compressors and
    // both range controllers act rather than idle at unity.
    iclforge::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 192,
         .acmod = iclforge::Acmod::kDualMono,
         .dialnorm = 27,
         .dialnorm2 = 25,
         .drc = iclforge::meta::profile(iclforge::meta::ProfileId::kFilmStandard),
         .heavy = iclforge::meta::HeavyConfig{},
         .drc2 = iclforge::meta::profile(iclforge::meta::ProfileId::kMusicLight),
         .heavy2 = iclforge::meta::HeavyConfig{.peak_ceiling_dbfs = -3.0}}};
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
        REQUIRE(ac3forge_decoded_access_unit_has_dialnorm2(unit) == 1);
        CHECK(ac3forge_decoded_access_unit_dialnorm2(unit) == 25);
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
    // it corrected" (tests/ac3/decoder/test_eac3_decoder.cpp). The silent frames here are
    // the tool's own semantics - frames that never switch a block release
    // immediately - not the test signal; the transient itself is real audio.
    iclforge::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 192, .acmod = iclforge::Acmod::k2_0, .transient_prenoise = true}};

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

    // Every frame fed to `decoder`, replayed below into a second decoder.
    std::vector<std::vector<std::byte>> stream;
    for (int f = 0; f < 2; ++f) {
        const auto frame = encoder.encode_frame(silent_views);
        REQUIRE(frame.has_value());
        stream.push_back(*frame);
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
    stream.push_back(*transient_frame);

    // transproce turns on: AC3FORGE_OK with a NULL substream is the held-back
    // signal, not an error - the header documents exactly this pair.
    ac3forge_decoded_substream_t* held = nullptr;
    REQUIRE(decode_one(*transient_frame, &held) == AC3FORGE_OK);
    CHECK(held == nullptr);

    const auto after = encoder.encode_frame(silent_views);
    REQUIRE(after.has_value());
    stream.push_back(*after);
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
    // array_destroy with the full count destroys the elements too (the header's
    // contract) - the elements are NOT destroyed individually here.
    ac3forge_decoded_substream_array_destroy(flushed, flushed_count);

    // The other documented release shape: take ownership of an element, then
    // free only the array with a count of 0. The kept handle must outlive the
    // array intact (this is the path the Rust binding's flush() uses; it once
    // passed the full count here instead and freed every substream twice).
    ac3forge_eac3_decoder_t* replay = nullptr;
    REQUIRE(ac3forge_eac3_decoder_create(&config, &replay) == AC3FORGE_OK);
    for (const auto& frame : stream) {
        ac3forge_decoded_substream_t* substream = nullptr;
        REQUIRE(ac3forge_eac3_decoder_decode_substream(
                    replay, reinterpret_cast<const uint8_t*>(frame.data()), frame.size(),
                    &substream) == AC3FORGE_OK);
        ac3forge_decoded_substream_destroy(substream);  // NULL for the held-back one: a no-op
    }
    flushed = nullptr;
    flushed_count = 0;
    REQUIRE(ac3forge_eac3_decoder_flush(replay, &flushed, &flushed_count) == AC3FORGE_OK);
    REQUIRE(flushed_count == 1);
    ac3forge_decoded_substream_t* kept = flushed[0];
    ac3forge_decoded_substream_array_destroy(flushed, 0);
    REQUIRE(kept != nullptr);
    CHECK(ac3forge_decoded_substream_channel_count(kept) == 2);
    CHECK(ac3forge_decoded_substream_samples_per_channel(kept) == AC3FORGE_SAMPLES_PER_FRAME);
    ac3forge_decoded_substream_destroy(kept);
    ac3forge_eac3_decoder_destroy(replay);

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
    iclforge::eac3::FrameEncoder encoder{{.bitrate_kbps = 192}};
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
    CHECK(ac3forge_decoded_access_unit_has_dialnorm2(nullptr) == 0);
    CHECK(ac3forge_decoded_access_unit_dialnorm2(nullptr) == 0);
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
    // Each case maps one iclforge::FrameError onto its C status through the
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
    // E-AC-3, which takes it - see tests/ac3/encoder/test_eac3.cpp) must refuse it.
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

TEST_CASE("the C API refuses configs and payloads the codec core would assert on",
          "[capi][eac3][atmos]") {
    // Each of these used to reach an assert() in the codec core (undefined
    // behaviour in a release build) instead of coming back as a status code,
    // so a caller of any binding - the Rust crate's safe API included - could
    // abort the process by filling in one field wrong.
    std::vector<float> samples(AC3FORGE_SAMPLES_PER_FRAME);
    fill_tone(samples.data(), 1000.0, 0, 48000.0);
    const float* stereo[2] = {samples.data(), samples.data()};
    ac3forge_bytes_t* out = nullptr;

    // chbwcod's legal codes stop at 60 (§5.4.3.24; 61-63 fit its six bits
    // but are reserved). Negative is "auto", as documented.
    ac3forge_encoder_config_t ac3_config;
    ac3forge_encoder_config_init(&ac3_config);
    ac3forge_encoder_t* ac3_encoder = nullptr;
    for (const int bad : {61, 63, 1000}) {
        ac3_config.chbwcod = bad;
        CHECK(ac3forge_encoder_create(&ac3_config, &ac3_encoder) ==
              AC3FORGE_ERROR_INVALID_ARGUMENT);
        CHECK(ac3_encoder == nullptr);
    }
    ac3_config.chbwcod = 60;
    REQUIRE(ac3forge_encoder_create(&ac3_config, &ac3_encoder) == AC3FORGE_OK);
    CHECK(ac3forge_encoder_encode_frame(ac3_encoder, stereo, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                        &out) == AC3FORGE_OK);
    ac3forge_bytes_destroy(out);
    out = nullptr;
    ac3forge_encoder_destroy(ac3_encoder);

    // E-AC-3 aux data rides block 0's skip field, whose length (skipl) is 9
    // bits of bytes: 511 fit, 512 do not.
    ac3forge_eac3_frame_config_t eac3_config;
    ac3forge_eac3_frame_config_init(&eac3_config);
    eac3_config.acmod = AC3FORGE_ACMOD_2_0;
    eac3_config.bitrate_kbps = 640;
    ac3forge_eac3_encoder_t* eac3_encoder = nullptr;
    REQUIRE(ac3forge_eac3_encoder_create(&eac3_config, &eac3_encoder) == AC3FORGE_OK);
    const std::vector<std::uint8_t> aux(512, 0x5A);
    CHECK(ac3forge_eac3_encoder_encode_frame(eac3_encoder, stereo, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                              nullptr, aux.data(), aux.size(),
                                              &out) == AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO);
    CHECK(out == nullptr);
    CHECK(ac3forge_eac3_encoder_encode_frame(eac3_encoder, stereo, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                              nullptr, aux.data(), 511, &out) == AC3FORGE_OK);
    ac3forge_bytes_destroy(out);
    out = nullptr;
    ac3forge_eac3_encoder_destroy(eac3_encoder);

    // The access-unit path hands its aux to one substream's frame writer.
    ac3forge_eac3_access_unit_encoder_t* au_encoder = nullptr;
    REQUIRE(ac3forge_eac3_access_unit_encoder_create(&eac3_config, nullptr, 0, &au_encoder) ==
            AC3FORGE_OK);
    ac3forge_eac3_access_unit_t* unit = nullptr;
    CHECK(ac3forge_eac3_access_unit_encoder_encode(
              au_encoder, stereo, 2, AC3FORGE_SAMPLES_PER_FRAME, aux.data(), aux.size(),
              &unit) == AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO);
    CHECK(unit == nullptr);
    ac3forge_eac3_access_unit_encoder_destroy(au_encoder);

    // num_bands_idx indexes Table 50's eight entries.
    ac3forge_atmos_config_t atmos_config;
    ac3forge_atmos_config_init(&atmos_config);
    ac3forge_atmos_encoder_t* atmos = nullptr;
    for (const int bad : {-1, 8, 100}) {
        atmos_config.num_bands_idx = bad;
        CHECK(ac3forge_atmos_encoder_create(&atmos_config, 1, &atmos) ==
              AC3FORGE_ERROR_INVALID_ARGUMENT);
        CHECK(atmos == nullptr);
    }
    ac3forge_atmos_config_init(&atmos_config);

    // With the object container on, the programme needs at least one object
    // to reconstruct and at most sixteen in all (TS 103 420 §8.3.2.2; the
    // bed's LFE counts). A count the container cannot carry comes back as
    // AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO - the code sixteen objects
    // already got - where 0, 17..30 and 31+ each used to hit an assert.
    const ac3forge_object_placement_t placement{.x = 0.5, .y = 0.5, .z = 0.0, .gain = 1.0,
                                                .lfe_send = 0.0};
    for (const int count : {0, 16, 17, 30, 31, 40}) {
        CAPTURE(count);
        atmos = nullptr;
        REQUIRE(ac3forge_atmos_encoder_create(&atmos_config, count, &atmos) == AC3FORGE_OK);
        const std::vector<const float*> objects(static_cast<std::size_t>(count), samples.data());
        const std::vector<ac3forge_object_placement_t> placements(static_cast<std::size_t>(count),
                                                                  placement);
        CHECK(ac3forge_atmos_encoder_encode_frame(
                  atmos, objects.data(), objects.size(), AC3FORGE_SAMPLES_PER_FRAME,
                  placements.data(), placements.size(),
                  &out) == AC3FORGE_ERROR_ENCODE_INVALID_OBJECT_AUDIO);
        CHECK(out == nullptr);
        ac3forge_atmos_encoder_destroy(atmos);

        // With the container off there is nothing to carry the count, and the
        // same objects simply mix into a plain 5.1 bed.
        ac3forge_atmos_config_t bed_only = atmos_config;
        bed_only.emit_object_metadata = 0;
        atmos = nullptr;
        REQUIRE(ac3forge_atmos_encoder_create(&bed_only, count, &atmos) == AC3FORGE_OK);
        CHECK(ac3forge_atmos_encoder_encode_frame(atmos, objects.data(), objects.size(),
                                                  AC3FORGE_SAMPLES_PER_FRAME, placements.data(),
                                                  placements.size(), &out) == AC3FORGE_OK);
        ac3forge_bytes_destroy(out);
        out = nullptr;
        ac3forge_atmos_encoder_destroy(atmos);
    }
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
    // Ch2's -6 dBFS tone at dialnorm 25 lands at -1 dBFS in an RF-mode decode,
    // under the default ceiling, so Ch2 gets a tighter one to make its word
    // move - same reasoning as the E-AC-3 sibling above.
    config.heavy2.peak_ceiling_dbfs = -3.0;

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
    // bare-metal probe harness. The numbers themselves are established empirically in
    // tests/ac3/decoder/test_latency.cpp (an impulse through a real encode ->
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
    // (iclforge::dsp::kQmfDelay = 576, not exposed at the C boundary) on top of the
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

// --- decode_frame_into / decode_access_unit_into (legacy item AP5) -----------

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
    const iclforge::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = iclforge::Acmod::k3_2, .lfe = true},
        .dependents = {{.bitrate_kbps = 192,
                        .acmod = iclforge::Acmod::k2_0,
                        .chanmap = iclforge::eac3::chanmap::k512Height}}};
    iclforge::eac3::AccessUnitEncoder encoder{config};
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
    // Mirrors tests/ac3/decoder/test_eac3_decoder.cpp's C++ test of the same name -
    // see the C API's own version of this table on the AC-3 flush test above
    // for what the silent/transient split is standing in for.
    iclforge::eac3::FrameEncoder encoder{
        {.bitrate_kbps = 192, .acmod = iclforge::Acmod::k2_0, .transient_prenoise = true}};

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
    iclforge::eac3::FrameEncoder encoder{{.bitrate_kbps = 192}};
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

// --- scan / ScannedStream (legacy item AP5) -----------------------------------

TEST_CASE("ac3forge_scan reports the same shape iclforge::io::scan does for an AC-3 stream",
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
    const iclforge::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = iclforge::Acmod::k3_2, .lfe = true},
        .dependents = {{.bitrate_kbps = 192,
                        .acmod = iclforge::Acmod::k2_0,
                        .chanmap = iclforge::eac3::chanmap::k512Height}}};
    iclforge::eac3::AccessUnitEncoder encoder{config};
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
          (iclforge::eac3::chanmap::acmod_map(iclforge::Acmod::k3_2, true) |
           iclforge::eac3::chanmap::k512Height));

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
    iclforge::eac3::FrameEncoder programme0{{.bitrate_kbps = 192, .acmod = iclforge::Acmod::k2_0}};
    iclforge::eac3::FrameEncoder programme1{
        {.bitrate_kbps = 96, .acmod = iclforge::Acmod::k1_0, .substreamid = 1}};

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

TEST_CASE("each C++ error a C caller can provoke reaches it as its own status code",
          "[capi][scan][eac3]") {
    // The tests above deliberately accept any code in a family when the input
    // is arbitrary garbage. This one builds inputs whose failure is fixed by
    // the spec rather than by parser order - a reserved fscod, an unknown
    // bsid, a dependent with no parent, an impossible frmsiz - so the exact
    // code is pinned and every arm of internal.hpp's error translation a C
    // caller can reach is held to the right answer, not merely to a range.
    ac3forge_encoder_config_t config;
    ac3forge_encoder_config_init(&config);
    config.acmod = AC3FORGE_ACMOD_2_0;
    ac3forge_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_encoder_create(&config, &encoder) == AC3FORGE_OK);
    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME);
    fill_tone(left.data(), 1000.0, 0, 48000.0);
    fill_tone(right.data(), 700.0, 0, 48000.0);
    const float* channels[2] = {left.data(), right.data()};
    ac3forge_bytes_t* encoded = nullptr;
    REQUIRE(ac3forge_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                          &encoded) == AC3FORGE_OK);
    ac3forge_encoder_destroy(encoder);
    const std::vector<uint8_t> frame(ac3forge_bytes_data(encoded),
                                     ac3forge_bytes_data(encoded) + ac3forge_bytes_size(encoded));
    ac3forge_bytes_destroy(encoded);
    REQUIRE(frame.size() > 6);

    ac3forge_scanned_stream_t* scanned = nullptr;
    ac3forge_decoder_config_t decoder_config;
    ac3forge_decoder_config_init(&decoder_config);
    ac3forge_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);
    ac3forge_decoded_frame_t* decoded = nullptr;

    SECTION("fscod '11' is reserved (Table 5.6), for the scanner and the decoder alike") {
        auto reserved = frame;
        reserved[4] = static_cast<uint8_t>(reserved[4] | 0xC0U);
        CHECK(ac3forge_scan(reserved.data(), reserved.size(), &scanned) ==
              AC3FORGE_ERROR_SCAN_RESERVED_VALUE);
        CHECK(ac3forge_decoder_decode_frame(decoder, reserved.data(), reserved.size(), &decoded) ==
              AC3FORGE_ERROR_DECODE_RESERVED_VALUE);
    }
    SECTION("a bsid that is neither AC-3 (<= 10) nor E-AC-3 (16) is an unsupported bsid") {
        auto foreign = frame;
        foreign[5] = static_cast<uint8_t>((12U << 3) | (foreign[5] & 0x07U));
        CHECK(ac3forge_scan(foreign.data(), foreign.size(), &scanned) ==
              AC3FORGE_ERROR_SCAN_UNSUPPORTED_BSID);
    }
    SECTION("an E-AC-3 frmsiz too small to hold its own header is an invalid stream") {
        // syncword, frmsiz = 0 (a 2-byte frame), bsid 16 at bit 40.
        const std::vector<uint8_t> tiny = {0x0B, 0x77, 0x00, 0x00, 0x00, 16U << 3};
        ac3forge_spans_t* spans = nullptr;
        CHECK(ac3forge_split_access_units(tiny.data(), tiny.size(), &spans) ==
              AC3FORGE_ERROR_DECODE_INVALID_STREAM);
        CHECK(spans == nullptr);
    }
    CHECK(scanned == nullptr);
    CHECK(decoded == nullptr);
    ac3forge_decoder_destroy(decoder);
}

TEST_CASE("E-AC-3 structure and substream errors reach C as their own codes", "[capi][eac3]") {
    // A dependent substream on its own: legal to encode, but a stream that
    // opens with one has no independent substream for it to extend.
    ac3forge_eac3_frame_config_t dependent;
    ac3forge_eac3_frame_config_init(&dependent);
    dependent.acmod = AC3FORGE_ACMOD_2_0;
    dependent.bitrate_kbps = 192;
    dependent.strmtyp = AC3FORGE_STREAM_TYPE_DEPENDENT;
    ac3forge_eac3_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_eac3_encoder_create(&dependent, &encoder) == AC3FORGE_OK);
    std::vector<float> left(AC3FORGE_SAMPLES_PER_FRAME);
    std::vector<float> right(AC3FORGE_SAMPLES_PER_FRAME);
    fill_tone(left.data(), 1000.0, 0, 48000.0);
    fill_tone(right.data(), 700.0, 0, 48000.0);
    const float* channels[2] = {left.data(), right.data()};
    ac3forge_bytes_t* encoded = nullptr;
    REQUIRE(ac3forge_eac3_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                                nullptr, nullptr, 0, &encoded) == AC3FORGE_OK);
    ac3forge_eac3_encoder_destroy(encoder);
    ac3forge_scanned_stream_t* scanned = nullptr;
    CHECK(ac3forge_scan(ac3forge_bytes_data(encoded), ac3forge_bytes_size(encoded), &scanned) ==
          AC3FORGE_ERROR_SCAN_UNSUPPORTED_STRUCTURE);
    CHECK(scanned == nullptr);
    ac3forge_bytes_destroy(encoded);

    // substreamid is three bits (Table E1.2): 8 cannot be written.
    ac3forge_eac3_frame_config_t out_of_range;
    ac3forge_eac3_frame_config_init(&out_of_range);
    out_of_range.acmod = AC3FORGE_ACMOD_2_0;
    out_of_range.bitrate_kbps = 192;
    out_of_range.substreamid = 8;
    REQUIRE(ac3forge_eac3_encoder_create(&out_of_range, &encoder) == AC3FORGE_OK);
    encoded = nullptr;
    CHECK(ac3forge_eac3_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                              nullptr, nullptr, 0,
                                              &encoded) == AC3FORGE_ERROR_ENCODE_INVALID_SUBSTREAM);
    CHECK(encoded == nullptr);
    ac3forge_eac3_encoder_destroy(encoder);

    // Seventeen distinct rendered locations: the bed's six, two five-channel
    // dependents and a lone Vhc - each substream self-consistent, only the
    // §E3.8.2 sixteen-channel aggregate is broken (the same programme
    // tests/ac3/encoder/test_eac3.cpp refuses through the C++ API).
    ac3forge_eac3_frame_config_t independent;
    ac3forge_eac3_frame_config_init(&independent);
    independent.acmod = AC3FORGE_ACMOD_3_2;
    independent.lfe = 1;
    independent.bitrate_kbps = 448;
    std::array<ac3forge_eac3_frame_config_t, 3> dependents{};
    for (auto& d : dependents) {
        ac3forge_eac3_frame_config_init(&d);
        d.has_chanmap = 1;
    }
    namespace cm = iclforge::eac3::chanmap;
    dependents[0].acmod = AC3FORGE_ACMOD_3_2;
    dependents[0].bitrate_kbps = 448;
    dependents[0].chanmap = static_cast<uint16_t>(cm::kLcRcBit | cm::kLrsRrsBit | cm::kCsBit);
    dependents[1].acmod = AC3FORGE_ACMOD_3_2;
    dependents[1].bitrate_kbps = 448;
    dependents[1].chanmap = static_cast<uint16_t>(cm::kLsdRsdBit | cm::kLwRwBit | cm::kTsBit);
    dependents[2].acmod = AC3FORGE_ACMOD_1_0;
    dependents[2].bitrate_kbps = 32;
    dependents[2].chanmap = cm::kVhcBit;
    ac3forge_eac3_access_unit_encoder_t* au_encoder = nullptr;
    REQUIRE(ac3forge_eac3_access_unit_encoder_create(&independent, dependents.data(),
                                                     dependents.size(), &au_encoder) == AC3FORGE_OK);
    REQUIRE(ac3forge_eac3_access_unit_encoder_channel_count(au_encoder) == 0);
    ac3forge_eac3_access_unit_t* unit = nullptr;
    CHECK(ac3forge_eac3_access_unit_encoder_encode(au_encoder, nullptr, 0,
                                                    AC3FORGE_SAMPLES_PER_FRAME, nullptr, 0,
                                                    &unit) == AC3FORGE_ERROR_ENCODE_TOO_MANY_CHANNELS);
    CHECK(unit == nullptr);
    ac3forge_eac3_access_unit_encoder_destroy(au_encoder);
}

// --- Loudness / level / QC metering (legacy item AP5) -------------------------

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
    // More spans than the meter has channels is an argument error, caught
    // before anything is sized from the count - SIZE_MAX used to reach a
    // reserve() that threw std::length_error and came back as
    // AC3FORGE_ERROR_INTERNAL.
    const std::vector<float> silence(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    const float* three[3] = {silence.data(), silence.data(), silence.data()};
    CHECK(ac3forge_loudness_meter_push(meter, three, 3, AC3FORGE_SAMPLES_PER_FRAME) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_loudness_meter_push(meter, three, SIZE_MAX, AC3FORGE_SAMPLES_PER_FRAME) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_loudness_meter_push(meter, three, 2, AC3FORGE_SAMPLES_PER_FRAME) == AC3FORGE_OK);
    ac3forge_loudness_meter_destroy(meter);
}

TEST_CASE("ac3forge_dialnorm_from_lkfs mirrors iclforge::meta::dialnorm_from_lkfs",
          "[capi][loudness]") {
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
    // Fewer spans than channels is legal (the rest meter as silence); more is
    // an argument error, caught before anything is sized from the count -
    // SIZE_MAX used to reach a reserve() that threw std::length_error and came
    // back as AC3FORGE_ERROR_INTERNAL.
    const std::vector<float> silence(AC3FORGE_SAMPLES_PER_FRAME, 0.0f);
    const float* three[3] = {silence.data(), silence.data(), silence.data()};
    CHECK(ac3forge_level_meter_process(meter, three, 3, AC3FORGE_SAMPLES_PER_FRAME) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_level_meter_process(meter, three, SIZE_MAX, AC3FORGE_SAMPLES_PER_FRAME) ==
          AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_level_meter_process(meter, three, 2, AC3FORGE_SAMPLES_PER_FRAME) == AC3FORGE_OK);
    CHECK(ac3forge_level_meter_process(meter, three, 1, AC3FORGE_SAMPLES_PER_FRAME) == AC3FORGE_OK);
    ac3forge_level_meter_destroy(meter);
}

TEST_CASE("ac3forge_qc_preset/name/parse mirror iclforge::meta::qc's table", "[capi][qc]") {
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

TEST_CASE(
    "ac3forge_evaluate_qc_gate passes/fails the same way iclforge::meta::evaluate_qc_gate does",
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

// --- AC-4 (ac3forge_ac4_*) -------------------------------------------------
//
// This file only builds under AC3FORGE_BUILD_CAPI, which (root CMakeLists.txt)
// requires AC3FORGE_BUILD_AC4 on whenever AC3FORGE_BUILD_TESTS is - so
// ac3forge.h's AC-4 section, always declared either way (that header's own
// comment), is always backed by the real ac4.cpp/ac4_encoder.cpp here, never
// ac4_absent.cpp. The first round trips below are a stereo, 5.1 or 5.1.4
// configuration (channel-based / channel-based-immersive), so
// ac3forge_ac4_decoded_frame_object_count() is exercised at 0 there; the
// object scenes after them (phase I4b) are A-JOC and direct-coded objects,
// encoded through the C API and through iclforge::ac4::Encoder itself, byte for byte.

TEST_CASE("ac3forge_ac4_*_config_init match their C++ struct defaults", "[capi][ac4]") {
    ac3forge_ac4_output_config_t output;
    ac3forge_ac4_output_config_init(&output);
    CHECK(output.has_output_level_dbfs == 0);
    CHECK(output.drc == AC3FORGE_AC4_DRC_DEFAULT);
    CHECK(output.headphones == 0);
    CHECK(output.downmix == AC3FORGE_AC4_DOWNMIX_AS_CODED);
    CHECK(output.mix_lfe == 1);

    ac3forge_ac4_presentation_choice_t choice;
    ac3forge_ac4_presentation_choice_init(&choice);
    CHECK(choice.has_presentation_id == 0);
    CHECK(choice.has_index == 0);
    CHECK(choice.language == nullptr);
    CHECK(choice.has_associated == 0);
    CHECK(choice.associated_type == AC3FORGE_AC4_ASSOCIATED_ANY);

    ac3forge_ac4_decoder_config_t decoder_config;
    ac3forge_ac4_decoder_config_init(&decoder_config);
    CHECK(decoder_config.concealment == AC3FORGE_AC4_CONCEALMENT_NONE);
    CHECK(decoder_config.level == 3);
    CHECK(decoder_config.decoding == AC3FORGE_AC4_DECODING_FULL);

    ac3forge_ac4_encoder_config_t encoder_config;
    ac3forge_ac4_encoder_config_init(&encoder_config);
    CHECK(encoder_config.channels == 2);
    CHECK(encoder_config.sample_rate_hz == 48000);
    CHECK(encoder_config.frame_rate_index == 13);
    CHECK(encoder_config.bitrate_kbps == 192);
    CHECK(encoder_config.rate_mode == AC3FORGE_AC4_RATE_CONSTANT);
    CHECK(encoder_config.codec_mode == AC3FORGE_AC4_CODEC_AUTO);
    CHECK(encoder_config.iframe_interval == 24);
    CHECK(encoder_config.dialnorm_db == -31.0);
    CHECK(encoder_config.iframes == nullptr);
    CHECK(encoder_config.iframe_count == 0);
    CHECK(encoder_config.fragment_starts == nullptr);
    CHECK(encoder_config.fragment_start_count == 0);
    CHECK(encoder_config.experimental.aspx_balance == 0);
    CHECK(encoder_config.experimental.aspx_varvar == 0);
    CHECK(encoder_config.experimental.aspx_interleave == 0);
    CHECK(encoder_config.experimental.coding_configs == 0);
    CHECK(encoder_config.experimental.seven_x == AC3FORGE_AC4_PAIR_NONE);
    CHECK(encoder_config.experimental.acpl == 0);
    CHECK(encoder_config.experimental.back_pair == 0);
    CHECK(encoder_config.experimental.ajcc == 0);
    CHECK(encoder_config.experimental.objects == 0);
    CHECK(encoder_config.objects == nullptr);

    // The object types' defaults are the C++ structs': room centre, unity gain,
    // depth exponent 1 - what a zero-initialised struct is not.
    const iclforge::ac4::ObjectProperties cpp_properties{};
    ac3forge_ac4_object_properties_t properties{};
    ac3forge_ac4_object_properties_init(&properties);
    CHECK(properties.active == 1);
    CHECK(properties.gain_db == cpp_properties.gain_db);
    CHECK(properties.priority == cpp_properties.priority);
    CHECK(properties.x == cpp_properties.position[0]);
    CHECK(properties.y == cpp_properties.position[1]);
    CHECK(properties.z == cpp_properties.position[2]);
    CHECK(properties.zone_mask == cpp_properties.zone_mask);
    CHECK(properties.enable_elevation == 1);
    CHECK(properties.snap == 0);
    CHECK(properties.width_x == 0.0);
    CHECK(properties.screen_factor == cpp_properties.screen_factor);
    CHECK(properties.depth_exponent == cpp_properties.depth_exponent);
    CHECK(properties.has_distance == 0);
    CHECK(properties.divergence == cpp_properties.divergence);
    CHECK(properties.trim_disabled == 0);
    CHECK(properties.has_headphone_render_mode == 0);
    CHECK(properties.head_track_disabled == 0);

    ac3forge_ac4_object_config_t object;
    ac3forge_ac4_object_config_init(&object);
    CHECK(object.has_bed == 0);
    CHECK(object.lfe == 0);
    CHECK(object.properties.priority == cpp_properties.priority);
    CHECK(object.properties.depth_exponent == cpp_properties.depth_exponent);

    ac3forge_ac4_objects_config_t objects;
    ac3forge_ac4_objects_config_init(&objects);
    CHECK(objects.objects == nullptr);
    CHECK(objects.object_count == 0);
    CHECK(objects.coding == AC3FORGE_AC4_OBJECT_CODING_AJOC);
    CHECK(objects.downmix == AC3FORGE_AC4_AJOC_DOWNMIX_COMPUTED);
    CHECK(objects.has_downmix_signals == 0);
    CHECK(objects.decorrelation == 0);
    CHECK(objects.has_parameter_bands == 0);
    CHECK(objects.has_coarse == 0);
    CHECK(objects.has_screen_size_ratio_code == 0);
    CHECK(objects.bed_object_chan_distribute == 0);

    ac3forge_ac4_object_metadata_update_t update;
    ac3forge_ac4_object_metadata_update_init(&update);
    CHECK(update.object == 0);
    CHECK(update.sample == 0);
    CHECK(update.ramp_samples == 0);
    CHECK(update.properties.depth_exponent == cpp_properties.depth_exponent);

    // The documented limits are the encoder's.
    ac3forge_ac4_object_properties_init(nullptr);  // documented no-ops
    ac3forge_ac4_object_config_init(nullptr);
    ac3forge_ac4_objects_config_init(nullptr);
    ac3forge_ac4_object_metadata_update_init(nullptr);
}

namespace {

// Encodes `frames` frames of a phase-continuous tone per channel through the
// AC-4 C API, decodes every produced frame straight back with a fresh
// decoder, and returns the concatenated decoded PCM per channel - the same
// "real signal, not silence" convention as encode_eac3_stream() above
// (CONTRIBUTING.md).
struct Ac4RoundTrip {
    std::vector<std::vector<float>> decoded;  // per decoded channel
    std::vector<ac3forge_ac4_speaker_t> speakers;
    int sample_rate_hz = 0;
};

// ac3forge_ac4_decoder_decode() hands the caller a frame to destroy. This frees
// it on every way out of the loop that reads it, a failed REQUIRE included.
struct DecodedFrameGuard {
    explicit DecodedFrameGuard(ac3forge_ac4_decoded_frame_t* f) : frame(f) {}
    DecodedFrameGuard(const DecodedFrameGuard&) = delete;
    DecodedFrameGuard& operator=(const DecodedFrameGuard&) = delete;
    ~DecodedFrameGuard() { ac3forge_ac4_decoded_frame_destroy(frame); }
    ac3forge_ac4_decoded_frame_t* frame;
};

Ac4RoundTrip round_trip_ac4(const ac3forge_ac4_encoder_config_t& encoder_config,
                            const std::vector<double>& tones, int frames) {
    ac3forge_ac4_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_ac4_encoder_create(&encoder_config, &encoder) == AC3FORGE_OK);
    REQUIRE(encoder != nullptr);

    ac3forge_ac4_decoder_config_t decoder_config;
    ac3forge_ac4_decoder_config_init(&decoder_config);
    ac3forge_ac4_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_ac4_decoder_create(&decoder_config, &decoder) == AC3FORGE_OK);
    REQUIRE(decoder != nullptr);

    const auto nchans = tones.size();
    std::vector<std::vector<float>> block(nchans,
                                          std::vector<float>(AC3FORGE_SAMPLES_PER_FRAME));
    std::vector<const float*> views(nchans);

    Ac4RoundTrip out;

    const auto handle_encoded = [&](ac3forge_ac4_encoded_frame_t* const* encoded, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            REQUIRE(ac3forge_ac4_encoded_frame_data(encoded[i]) != nullptr);
            REQUIRE(ac3forge_ac4_encoded_frame_size(encoded[i]) > 0);

            ac3forge_ac4_decoded_frame_t* decoded = nullptr;
            const auto status =
                ac3forge_ac4_decoder_decode(decoder, ac3forge_ac4_encoded_frame_data(encoded[i]),
                                            ac3forge_ac4_encoded_frame_size(encoded[i]), &decoded);
            const DecodedFrameGuard guard(decoded);
            REQUIRE(status == AC3FORGE_OK);
            if (decoded == nullptr) {
                continue;  // held back pending configuration - not an error
            }
            out.sample_rate_hz = ac3forge_ac4_decoded_frame_sample_rate_hz(decoded);
            const auto channel_count = ac3forge_ac4_decoded_frame_channel_count(decoded);
            if (out.decoded.empty()) {
                out.decoded.resize(channel_count);
                out.speakers.resize(channel_count);
                for (size_t ch = 0; ch < channel_count; ++ch) {
                    out.speakers[ch] = ac3forge_ac4_decoded_frame_speaker(decoded, ch);
                }
            }
            REQUIRE(channel_count == out.decoded.size());
            const auto samples = ac3forge_ac4_decoded_frame_samples_per_channel(decoded);
            for (size_t ch = 0; ch < channel_count; ++ch) {
                const float* samples_ptr = ac3forge_ac4_decoded_frame_channel_samples(decoded, ch);
                REQUIRE(samples_ptr != nullptr);
                out.decoded[ch].insert(out.decoded[ch].end(), samples_ptr, samples_ptr + samples);
            }
            CHECK(ac3forge_ac4_decoded_frame_object_count(decoded) == 0);
            CHECK(ac3forge_ac4_decoded_frame_has_concealed(decoded) == 0);
        }
        ac3forge_ac4_encoded_frame_array_destroy(const_cast<ac3forge_ac4_encoded_frame_t**>(encoded),
                                                  count);
    };

    for (int frame = 0; frame < frames; ++frame) {
        for (size_t ch = 0; ch < nchans; ++ch) {
            fill_tone(block[ch].data(), tones[ch], frame,
                      static_cast<double>(encoder_config.sample_rate_hz));
            views[ch] = block[ch].data();
        }
        ac3forge_ac4_encoded_frame_t** encoded = nullptr;
        size_t count = 0;
        REQUIRE(ac3forge_ac4_encoder_encode(encoder, views.data(), nchans,
                                            AC3FORGE_SAMPLES_PER_FRAME, &encoded,
                                            &count) == AC3FORGE_OK);
        handle_encoded(encoded, count);
    }

    ac3forge_ac4_encoded_frame_t** flushed = nullptr;
    size_t flushed_count = 0;
    REQUIRE(ac3forge_ac4_encoder_flush(encoder, &flushed, &flushed_count) == AC3FORGE_OK);
    handle_encoded(flushed, flushed_count);

    ac3forge_ac4_decoder_destroy(decoder);
    ac3forge_ac4_encoder_destroy(encoder);
    return out;
}

}  // namespace

TEST_CASE("AC-4 stereo encode/decode round-trips through the C API", "[capi][ac4]") {
    ac3forge_ac4_encoder_config_t config;
    ac3forge_ac4_encoder_config_init(&config);
    config.channels = 2;
    config.bitrate_kbps = 96;

    const auto result = round_trip_ac4(config, {1000.0, 800.0}, 12);

    REQUIRE(result.decoded.size() == 2);
    CHECK(result.sample_rate_hz == 48000);
    CHECK(result.speakers[0] == AC3FORGE_AC4_SPEAKER_LEFT);
    CHECK(result.speakers[1] == AC3FORGE_AC4_SPEAKER_RIGHT);
    // Every frame flowed through: at least as many samples as the frames fed
    // in (frame_rate_index 13's 2048-sample frame, minus what decode's own
    // delay is still holding).
    CHECK(result.decoded[0].size() > AC3FORGE_SAMPLES_PER_FRAME);
    // A real decoded signal, not silence (CONTRIBUTING.md) - mean square over
    // the tail, past the encoder/decoder's warm-up.
    double energy = 0.0;
    for (std::size_t i = 4096; i < result.decoded[0].size(); ++i) {
        const double sample = static_cast<double>(result.decoded[0][i]);
        energy += sample * sample;
    }
    CHECK(energy / static_cast<double>(result.decoded[0].size() - 4096) > 1e-6);
}

TEST_CASE("AC-4 5.1 encode/decode round-trips through the C API", "[capi][ac4]") {
    ac3forge_ac4_encoder_config_t config;
    ac3forge_ac4_encoder_config_init(&config);
    config.channels = 6;
    config.bitrate_kbps = 256;

    const auto result = round_trip_ac4(config, {1000.0, 900.0, 700.0, 60.0, 500.0, 600.0}, 12);

    REQUIRE(result.decoded.size() == 6);
    CHECK(result.speakers[0] == AC3FORGE_AC4_SPEAKER_LEFT);
    CHECK(result.speakers[1] == AC3FORGE_AC4_SPEAKER_RIGHT);
    CHECK(result.speakers[2] == AC3FORGE_AC4_SPEAKER_CENTRE);
    CHECK(result.speakers[3] == AC3FORGE_AC4_SPEAKER_LFE);
    CHECK(result.speakers[4] == AC3FORGE_AC4_SPEAKER_LEFT_SURROUND);
    CHECK(result.speakers[5] == AC3FORGE_AC4_SPEAKER_RIGHT_SURROUND);
}

TEST_CASE("AC-4 5.1.4 (channel-based-immersive) encode/decode round-trips", "[capi][ac4]") {
    ac3forge_ac4_encoder_config_t config;
    ac3forge_ac4_encoder_config_init(&config);
    config.channels = 10;
    config.bitrate_kbps = 448;

    const auto result =
        round_trip_ac4(config, {1000.0, 900.0, 700.0, 60.0, 500.0, 600.0, 300.0, 320.0, 340.0, 360.0},
                       12);

    REQUIRE(result.decoded.size() == 10);
    CHECK(result.speakers[3] == AC3FORGE_AC4_SPEAKER_LFE);
    CHECK(result.speakers[6] == AC3FORGE_AC4_SPEAKER_TOP_FRONT_LEFT);
    CHECK(result.speakers[7] == AC3FORGE_AC4_SPEAKER_TOP_FRONT_RIGHT);
    CHECK(result.speakers[8] == AC3FORGE_AC4_SPEAKER_TOP_BACK_LEFT);
    CHECK(result.speakers[9] == AC3FORGE_AC4_SPEAKER_TOP_BACK_RIGHT);
}

TEST_CASE(
    "ac3forge_ac4_encoder_create refuses a configuration iclforge::ac4::Encoder::create() refuses",
    "[capi][ac4]") {
    ac3forge_ac4_encoder_config_t config;
    ac3forge_ac4_encoder_config_init(&config);
    config.channels = 3;  // not one of EncoderConfig::channels' accepted counts

    ac3forge_ac4_encoder_t* encoder = nullptr;
    CHECK(ac3forge_ac4_encoder_create(&config, &encoder) == AC3FORGE_ERROR_AC4_ENCODE_INVALID_CONFIG);
    CHECK(encoder == nullptr);

    CHECK(ac3forge_ac4_encoder_create(nullptr, &encoder) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    ac3forge_ac4_encoder_config_init(&config);
    CHECK(ac3forge_ac4_encoder_create(&config, nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
}

TEST_CASE("ac3forge_ac4_encoder_toc feeds build_dac4/media_timing/samples_per_frame", "[capi][ac4]") {
    ac3forge_ac4_encoder_config_t config;
    ac3forge_ac4_encoder_config_init(&config);
    config.channels = 2;

    ac3forge_ac4_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_ac4_encoder_create(&config, &encoder) == AC3FORGE_OK);

    // A whole frame_rate_index-13 frame (2048 samples), not
    // AC3FORGE_SAMPLES_PER_FRAME (1536, an AC-3ism) - the encoder buffers a
    // partial input frame internally rather than failing (see
    // ac3forge_ac4_encoder_encode()'s own comment), but toc() is only
    // meaningful once at least one frame has actually completed.
    constexpr int kAc4FrameSamples = 2048;
    std::vector<float> left(kAc4FrameSamples, 0.0f);
    std::vector<float> right(kAc4FrameSamples, 0.0f);
    fill_tone(left.data(), 1000.0, 0, 48000.0);
    fill_tone(right.data(), 800.0, 0, 48000.0);
    const float* channels[2] = {left.data(), right.data()};
    ac3forge_ac4_encoded_frame_t** encoded = nullptr;
    size_t count = 0;
    REQUIRE(ac3forge_ac4_encoder_encode(encoder, channels, 2, kAc4FrameSamples, &encoded, &count) ==
            AC3FORGE_OK);
    REQUIRE(count > 0);
    ac3forge_ac4_encoded_frame_array_destroy(encoded, count);

    ac3forge_ac4_toc_t* toc = nullptr;
    REQUIRE(ac3forge_ac4_encoder_toc(encoder, &toc) == AC3FORGE_OK);
    REQUIRE(toc != nullptr);

    ac3forge_bytes_t* box = nullptr;
    REQUIRE(ac3forge_ac4_build_dac4(toc, &box) == AC3FORGE_OK);
    REQUIRE(box != nullptr);
    CHECK(ac3forge_bytes_size(box) > 0);
    CHECK(std::string_view(ac3forge_ac4_dac4_refusal(toc)).empty());

    uint32_t timescale = 0;
    uint32_t sample_delta = 0;
    CHECK(ac3forge_ac4_media_timing(toc, &timescale, &sample_delta) == 1);
    CHECK(timescale > 0);
    CHECK(sample_delta > 0);

    uint32_t samples_per_frame = 0;
    CHECK(ac3forge_ac4_samples_per_frame(toc, &samples_per_frame) == 1);
    CHECK(samples_per_frame == 2048);  // frame_rate_index 13

    ac3forge_bytes_destroy(box);
    ac3forge_ac4_toc_destroy(toc);
    ac3forge_ac4_encoder_destroy(encoder);
}

TEST_CASE("ac3forge_ac4_sync_frame wraps a raw frame", "[capi][ac4]") {
    const std::array<uint8_t, 4> raw{0x01, 0x02, 0x03, 0x04};
    ac3forge_bytes_t* wrapped = nullptr;
    REQUIRE(ac3forge_ac4_sync_frame(raw.data(), raw.size(), /*crc=*/0, &wrapped) == AC3FORGE_OK);
    REQUIRE(wrapped != nullptr);
    // sync word + frame_size + the raw bytes, no crc_word (crc=0 -> 0xAC40).
    CHECK(ac3forge_bytes_size(wrapped) > raw.size());
    REQUIRE(ac3forge_bytes_data(wrapped) != nullptr);
    CHECK(ac3forge_bytes_data(wrapped)[0] == 0xAC);
    CHECK(ac3forge_bytes_data(wrapped)[1] == 0x40);
    const size_t wrapped_size = ac3forge_bytes_size(wrapped);  // read before destroying it below
    ac3forge_bytes_destroy(wrapped);

    ac3forge_bytes_t* wrapped_crc = nullptr;
    REQUIRE(ac3forge_ac4_sync_frame(raw.data(), raw.size(), /*crc=*/1, &wrapped_crc) == AC3FORGE_OK);
    REQUIRE(wrapped_crc != nullptr);
    CHECK(ac3forge_bytes_data(wrapped_crc)[1] == 0x41);
    CHECK(ac3forge_bytes_size(wrapped_crc) == wrapped_size + 2);  // trailing crc_word
    ac3forge_bytes_destroy(wrapped_crc);
}

// --- AC-4 objects (ac3forge_ac4_objects_config_t and its neighbours) ---------
//
// A scene is written once as an iclforge::ac4::EncoderConfig, in the C++ API's own terms:
// the reference. An independent conversion below (not the library's) turns it
// into the C structs, and the C API and iclforge::ac4::Encoder itself then have to write
// the same bytes from the same input; the C API's decoder has to read the
// scene back as it was given, within what each field's code can hold.

namespace {

constexpr double kObjectRate = 48000.0;
constexpr int kObjectFrame = 2048;
// Each object's tone sits at the middle of a QMF subband of its own, each in a
// parameter band of its own in A-JOC's matrices, and the LFE's at 47 Hz, as
// tests/ac4enc/test_ac4enc_objects.cpp has them.
constexpr std::array<int, 8> kObjectSubbands = {1, 3, 5, 7, 9, 12, 16, 22};
constexpr double kObjectLfeHz = 47.0;
// Six frames of input; the metadata update sits at sample 5000, in the third.
constexpr std::size_t kObjectSamples = 6 * kObjectFrame;
constexpr std::int64_t kObjectUpdateSample = 5000;
constexpr int kObjectUpdateRamp = 1024;

struct ObjectScene {
    iclforge::ac4::EncoderConfig config;
    std::vector<iclforge::ac4::ObjectMetadataUpdate> updates;

    [[nodiscard]] const iclforge::ac4::ObjectsConfig& objects() const {
        return *config.substreams.at(0).objects;
    }
};

iclforge::ac4::ObjectConfig dynamic_object(double x, double y, double z, double gain_db) {
    iclforge::ac4::ObjectConfig object;
    object.properties.position = {x, y, z};
    object.properties.gain_db = gain_db;
    return object;
}

iclforge::ac4::ObjectConfig lfe_object() {
    iclforge::ac4::ObjectConfig object;
    object.lfe = true;
    return object;
}

ObjectScene make_scene(iclforge::ac4::ObjectsConfig objects, int kbps) {
    ObjectScene scene;
    scene.config.bitrate_kbps = kbps;
    scene.config.experimental.objects = true;
    iclforge::ac4::SubstreamConfig substream;
    substream.objects = std::move(objects);
    scene.config.substreams = {substream};
    // The first dynamic object moves at the update's sample to a position and
    // a gain of its own, over kObjectUpdateRamp samples.
    iclforge::ac4::ObjectMetadataUpdate update;
    for (std::size_t o = 0; o < scene.objects().objects.size(); ++o) {
        const iclforge::ac4::ObjectConfig& object = scene.objects().objects[o];
        if (!object.lfe && !object.bed) {
            update.object = static_cast<int>(o);
            break;
        }
    }
    update.sample = kObjectUpdateSample;
    update.ramp_samples = kObjectUpdateRamp;
    update.properties.position = {0.75, 0.25, 0.4};
    update.properties.gain_db = -12.0;
    scene.updates = {update};
    return scene;
}

// A-JOC over a computed downmix of two signals: the LFE among the objects, in
// the middle of their list, a bed object and three dynamic objects, whose
// metadata takes every field a dynamic object sends.
ObjectScene ajoc_scene() {
    iclforge::ac4::ObjectsConfig objects;
    iclforge::ac4::ObjectConfig a = dynamic_object(0.1, 0.2, 0.0, -3.0);
    iclforge::ac4::ObjectConfig b = dynamic_object(0.9, 0.5, 7.0 / 15.0, -6.0);
    b.properties.priority = 16.0 / 31.0;
    b.properties.width = {0.2, 0.4, 0.6};
    b.properties.zone_mask = 3;
    b.properties.screen_factor = 0.5;
    iclforge::ac4::ObjectConfig bed;
    bed.bed = iclforge::ac4::BedChannel::kLeft;
    bed.properties.gain_db = -12.0;
    bed.properties.trim_disabled = true;
    bed.properties.headphone_render_mode = 1;
    bed.properties.head_track_disabled = true;
    iclforge::ac4::ObjectConfig c = dynamic_object(0.5, 1.0, -0.6, -4.4);
    c.properties.snap = true;
    c.properties.enable_elevation = false;
    // The screen factor and the depth exponent share a group of fields, which
    // has no code for a factor of 0: a factor is given with the exponent.
    c.properties.screen_factor = 0.25;
    c.properties.depth_exponent = 2.0;
    c.properties.distance = 4.0;
    c.properties.divergence = 0.75;
    objects.objects = {a, lfe_object(), b, bed, c};
    objects.downmix_signals = 2;
    return make_scene(std::move(objects), 256);
}

// Direct-coded: four dynamic objects and the LFE, which rides the first
// substream.
ObjectScene direct_scene() {
    iclforge::ac4::ObjectsConfig objects;
    iclforge::ac4::ObjectConfig b = dynamic_object(0.33, 0.66, 0.2, -6.0);
    b.properties.width = {0.1, 0.3, 0.5};
    b.properties.zone_mask = 5;
    iclforge::ac4::ObjectConfig quiet = dynamic_object(0.67, 0.34, -0.2, 0.0);
    quiet.properties.active = false;
    objects.objects = {dynamic_object(0.0, 0.0, 0.0, -3.0), b, lfe_object(), quiet,
                       dynamic_object(1.0, 1.0, 1.0, -9.0)};
    objects.coding = iclforge::ac4::ObjectCoding::kDirect;
    return make_scene(std::move(objects), 256);
}

std::vector<std::vector<float>> object_input(const iclforge::ac4::ObjectsConfig& objects,
                                             std::size_t samples) {
    std::vector<std::vector<float>> input;
    std::size_t tones = 0;
    for (const iclforge::ac4::ObjectConfig& object : objects.objects) {
        const double hz = object.lfe ? kObjectLfeHz
                                     : (kObjectSubbands[tones++ % kObjectSubbands.size()] + 0.5) *
                                           kObjectRate / 128.0;
        std::vector<float> x(samples);
        for (std::size_t n = 0; n < samples; ++n) {
            x[n] = static_cast<float>(
                0.1 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kObjectRate));
        }
        input.push_back(std::move(x));
    }
    return input;
}

ac3forge_ac4_object_properties_t c_properties(const iclforge::ac4::ObjectProperties& p) {
    ac3forge_ac4_object_properties_t c;
    ac3forge_ac4_object_properties_init(&c);
    c.active = p.active ? 1 : 0;
    c.gain_db = p.gain_db;
    c.priority = p.priority;
    c.x = p.position[0];
    c.y = p.position[1];
    c.z = p.position[2];
    c.zone_mask = p.zone_mask;
    c.enable_elevation = p.enable_elevation ? 1 : 0;
    c.snap = p.snap ? 1 : 0;
    c.width_x = p.width[0];
    c.width_y = p.width[1];
    c.width_z = p.width[2];
    c.screen_factor = p.screen_factor;
    c.depth_exponent = p.depth_exponent;
    c.has_distance = p.distance.has_value() ? 1 : 0;
    c.distance = p.distance.value_or(0.0);
    c.divergence = p.divergence;
    c.trim_disabled = p.trim_disabled ? 1 : 0;
    c.has_headphone_render_mode = p.headphone_render_mode.has_value() ? 1 : 0;
    c.headphone_render_mode = p.headphone_render_mode.value_or(0);
    c.head_track_disabled = p.head_track_disabled ? 1 : 0;
    return c;
}

// The scene as the C structs a caller of the C API would fill in. The arrays
// live here, the pointers inside `config` and `objects_config` point into
// them, so a scene is built in place and never moved.
struct CObjectScene {
    std::vector<ac3forge_ac4_object_config_t> objects;
    ac3forge_ac4_objects_config_t objects_config{};
    ac3forge_ac4_encoder_config_t config{};
    std::vector<ac3forge_ac4_object_metadata_update_t> updates;
};

std::unique_ptr<CObjectScene> c_scene_of(const ObjectScene& scene) {
    auto c = std::make_unique<CObjectScene>();
    const iclforge::ac4::ObjectsConfig& objects = scene.objects();
    for (const iclforge::ac4::ObjectConfig& object : objects.objects) {
        ac3forge_ac4_object_config_t& out = c->objects.emplace_back();
        ac3forge_ac4_object_config_init(&out);
        out.has_bed = object.bed.has_value() ? 1 : 0;
        if (object.bed) {
            out.bed = static_cast<ac3forge_ac4_bed_channel_t>(*object.bed);
        }
        out.lfe = object.lfe ? 1 : 0;
        out.properties = c_properties(object.properties);
    }
    ac3forge_ac4_objects_config_init(&c->objects_config);
    c->objects_config.objects = c->objects.data();
    c->objects_config.object_count = c->objects.size();
    c->objects_config.coding = objects.coding == iclforge::ac4::ObjectCoding::kDirect
                                   ? AC3FORGE_AC4_OBJECT_CODING_DIRECT
                                   : AC3FORGE_AC4_OBJECT_CODING_AJOC;
    c->objects_config.downmix = static_cast<ac3forge_ac4_ajoc_downmix_t>(objects.downmix);
    if (objects.downmix_signals) {
        c->objects_config.has_downmix_signals = 1;
        c->objects_config.downmix_signals = *objects.downmix_signals;
    }
    c->objects_config.decorrelation = objects.decorrelation ? 1 : 0;

    ac3forge_ac4_encoder_config_init(&c->config);
    c->config.bitrate_kbps = scene.config.bitrate_kbps;
    c->config.experimental.objects = scene.config.experimental.objects ? 1 : 0;
    c->config.objects = &c->objects_config;

    for (const iclforge::ac4::ObjectMetadataUpdate& update : scene.updates) {
        ac3forge_ac4_object_metadata_update_t& out = c->updates.emplace_back();
        ac3forge_ac4_object_metadata_update_init(&out);
        out.object = static_cast<std::size_t>(update.object);
        out.sample = update.sample;
        out.ramp_samples = update.ramp_samples;
        out.properties = c_properties(update.properties);
    }
    return c;
}

struct EncodedStream {
    std::vector<std::vector<std::uint8_t>> frames;
    int delay = 0;
    int decoder_delay = 0;
};

EncodedStream encode_with_cpp(const ObjectScene& scene,
                              const std::vector<std::vector<float>>& input) {
    EncodedStream out;
    auto encoder = iclforge::ac4::Encoder::create(scene.config);
    REQUIRE(encoder.has_value());
    out.delay = encoder->delay_samples();
    out.decoder_delay = encoder->decoder_delay_samples();
    const std::vector<std::span<const float>> views(input.begin(), input.end());
    auto frames = encoder->encode(views, scene.updates);
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

EncodedStream encode_with_c_api(const CObjectScene& scene,
                                const std::vector<std::vector<float>>& input) {
    EncodedStream out;
    ac3forge_ac4_encoder_t* encoder = nullptr;
    INFO(ac3forge_ac4_encoder_refusal_reason(&scene.config));
    REQUIRE(ac3forge_ac4_encoder_create(&scene.config, &encoder) == AC3FORGE_OK);
    REQUIRE(encoder != nullptr);
    out.delay = ac3forge_ac4_encoder_delay_samples(encoder);
    out.decoder_delay = ac3forge_ac4_encoder_decoder_delay_samples(encoder);
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
    REQUIRE(ac3forge_ac4_encoder_encode_objects(
                encoder, views.data(), views.size(), input.front().size(), scene.updates.data(),
                scene.updates.size(), &frames, &count) == AC3FORGE_OK);
    take(frames, count);
    frames = nullptr;
    count = 0;
    REQUIRE(ac3forge_ac4_encoder_flush(encoder, &frames, &count) == AC3FORGE_OK);
    take(frames, count);
    ac3forge_ac4_encoder_destroy(encoder);
    return out;
}

struct DecodedObject {
    ac3forge_ac4_object_kind_t kind = AC3FORGE_AC4_OBJECT_DYN;
    bool lfe = false;
    std::optional<ac3forge_ac4_speaker_t> speaker;
    std::vector<float> samples;
    ac3forge_ac4_object_properties_t properties{};  // in force at the last frame's first sample
};

struct DecodedUpdate {
    std::size_t object = 0;
    std::int64_t sample = 0;  // in the decoder's output, from its first sample
    int ramp_samples = 0;
    ac3forge_ac4_object_properties_t properties{};
};

struct DecodedScene {
    std::vector<DecodedObject> objects;
    std::vector<DecodedUpdate> updates;
};

DecodedScene decode_with_c_api(const EncodedStream& stream) {
    DecodedScene out;
    ac3forge_ac4_decoder_config_t config;
    ac3forge_ac4_decoder_config_init(&config);
    ac3forge_ac4_decoder_t* decoder = nullptr;
    REQUIRE(ac3forge_ac4_decoder_create(&config, &decoder) == AC3FORGE_OK);
    std::int64_t start = 0;
    for (const std::vector<std::uint8_t>& bytes : stream.frames) {
        ac3forge_ac4_decoded_frame_t* frame = nullptr;
        const auto status =
            ac3forge_ac4_decoder_decode(decoder, bytes.data(), bytes.size(), &frame);
        const DecodedFrameGuard guard(frame);
        REQUIRE(status == AC3FORGE_OK);
        if (frame == nullptr) {
            continue;  // held back pending configuration - not an error
        }
        const size_t count = ac3forge_ac4_decoded_frame_object_count(frame);
        const size_t samples = ac3forge_ac4_decoded_frame_samples_per_channel(frame);
        out.objects.resize(count);
        for (size_t o = 0; o < count; ++o) {
            DecodedObject& object = out.objects[o];
            object.kind = ac3forge_ac4_decoded_frame_object_kind(frame, o);
            object.lfe = ac3forge_ac4_decoded_frame_object_lfe(frame, o) != 0;
            object.speaker =
                ac3forge_ac4_decoded_frame_object_has_speaker(frame, o) != 0
                    ? std::optional(ac3forge_ac4_decoded_frame_object_speaker(frame, o))
                    : std::nullopt;
            const float* pcm = ac3forge_ac4_decoded_frame_object_samples(frame, o);
            REQUIRE(pcm != nullptr);
            object.samples.insert(object.samples.end(), pcm, pcm + samples);
            object.properties = ac3forge_ac4_decoded_frame_object_properties(frame, o);
            for (size_t u = 0; u < ac3forge_ac4_decoded_frame_object_update_count(frame, o); ++u) {
                const ac3forge_ac4_object_update_t update =
                    ac3forge_ac4_decoded_frame_object_update(frame, o, u);
                out.updates.push_back({o, start + static_cast<std::int64_t>(update.sample),
                                       update.ramp_samples, update.properties});
            }
        }
        start += static_cast<std::int64_t>(samples);
    }
    ac3forge_ac4_decoder_destroy(decoder);
    return out;
}

// The decoded objects' order: the LFE first, then the bed objects and the
// dynamic objects, each in the order the configuration lists them.
std::vector<std::size_t> decoded_order(const iclforge::ac4::ObjectsConfig& objects) {
    std::vector<std::size_t> order;
    for (std::size_t o = 0; o < objects.objects.size(); ++o) {
        if (objects.objects[o].lfe) {
            order.push_back(o);
        }
    }
    for (const bool beds : {true, false}) {
        for (std::size_t o = 0; o < objects.objects.size(); ++o) {
            const iclforge::ac4::ObjectConfig& object = objects.objects[o];
            if (!object.lfe && object.bed.has_value() == beds) {
                order.push_back(o);
            }
        }
    }
    return order;
}

// The normalised correlation of `decoded`, `lag` samples later, with
// `reference`, from two frames in to a frame before its end.
double correlation(const std::vector<float>& reference, const std::vector<float>& decoded,
                   std::size_t lag) {
    double xx = 0.0;
    double yy = 0.0;
    double xy = 0.0;
    for (std::size_t n = 2 * kObjectFrame;
         n + kObjectFrame < reference.size() && n + lag < decoded.size(); ++n) {
        const auto x = static_cast<double>(reference[n]);
        const auto y = static_cast<double>(decoded[n + lag]);
        xx += x * x;
        yy += y * y;
        xy += x * y;
    }
    return xy / std::sqrt(std::max(xx * yy, 1e-300));
}

// What one property's code can hold: X and Y in 62 steps, Z in 15, the gain in
// 1 dB, the priority and each width in 31, the screen factor in 8, the
// divergence and the distance in the tables of Annex F. An inactive object
// sends none of them, only that it is not active.
void check_properties_near(const ac3forge_ac4_object_properties_t& got,
                           const iclforge::ac4::ObjectProperties& want, bool dynamic) {
    CHECK(got.active == (want.active ? 1 : 0));
    if (!want.active) {
        return;
    }
    CHECK(std::abs(got.gain_db - want.gain_db) <= 0.5 + 1e-9);
    CHECK(std::abs(got.priority - want.priority) <= 1.0 / 62.0 + 1e-9);
    CHECK(got.trim_disabled == (want.trim_disabled ? 1 : 0));
    CHECK(got.has_headphone_render_mode == (want.headphone_render_mode.has_value() ? 1 : 0));
    CHECK(got.headphone_render_mode == want.headphone_render_mode.value_or(0));
    CHECK(got.head_track_disabled ==
          (want.headphone_render_mode.has_value() && want.head_track_disabled ? 1 : 0));
    if (!dynamic) {
        return;
    }
    CHECK(std::abs(got.x - want.position[0]) <= 1.0 / 124.0 + 1e-9);
    CHECK(std::abs(got.y - want.position[1]) <= 1.0 / 124.0 + 1e-9);
    CHECK(std::abs(got.z - want.position[2]) <= 1.0 / 30.0 + 1e-9);
    CHECK(got.zone_mask == want.zone_mask);
    CHECK(got.enable_elevation == (want.enable_elevation ? 1 : 0));
    CHECK(got.snap == (want.snap ? 1 : 0));
    CHECK(std::abs(got.width_x - want.width[0]) <= 1.0 / 62.0 + 1e-9);
    CHECK(std::abs(got.width_y - want.width[1]) <= 1.0 / 62.0 + 1e-9);
    CHECK(std::abs(got.width_z - want.width[2]) <= 1.0 / 62.0 + 1e-9);
    CHECK(std::abs(got.screen_factor - want.screen_factor) <= 1.0 / 16.0 + 1e-9);
    CHECK(got.depth_exponent == want.depth_exponent);
    CHECK(got.has_distance == (want.distance.has_value() ? 1 : 0));
    if (want.distance) {
        CHECK(std::abs(got.distance - *want.distance) <= 0.1 * *want.distance);
    }
    CHECK(std::abs(got.divergence - want.divergence) <= 0.02);
}

// Encodes `scene` through the C API and through iclforge::ac4::Encoder, checks the two
// streams are the same bytes, then decodes the C API's and checks each object
// against the configuration, its audio and its metadata update.
void check_object_scene(const ObjectScene& scene) {
    const std::vector<std::vector<float>> input = object_input(scene.objects(), kObjectSamples);
    const std::unique_ptr<CObjectScene> c_scene = c_scene_of(scene);
    const EncodedStream from_c = encode_with_c_api(*c_scene, input);
    const EncodedStream from_cpp = encode_with_cpp(scene, input);

    REQUIRE_FALSE(from_c.frames.empty());
    REQUIRE(from_c.frames.size() == from_cpp.frames.size());
    for (std::size_t f = 0; f < from_c.frames.size(); ++f) {
        CAPTURE(f);
        CHECK(from_c.frames[f] == from_cpp.frames[f]);
    }
    CHECK(from_c.delay == from_cpp.delay);
    CHECK(from_c.decoder_delay == from_cpp.decoder_delay);

    const DecodedScene decoded = decode_with_c_api(from_c);
    const std::vector<std::size_t> order = decoded_order(scene.objects());
    REQUIRE(decoded.objects.size() == order.size());

    const std::int64_t lag = from_c.delay + from_c.decoder_delay;
    const iclforge::ac4::ObjectMetadataUpdate& update = scene.updates.front();
    for (std::size_t d = 0; d < order.size(); ++d) {
        const std::size_t o = order[d];
        const iclforge::ac4::ObjectConfig& configured = scene.objects().objects[o];
        const DecodedObject& object = decoded.objects[d];
        CAPTURE(d, o);
        CHECK(object.lfe == configured.lfe);
        if (configured.bed) {
            CHECK(object.kind == AC3FORGE_AC4_OBJECT_BED);
            REQUIRE(object.speaker.has_value());
            CHECK(*object.speaker == AC3FORGE_AC4_SPEAKER_LEFT);
        } else if (!configured.lfe) {
            CHECK(object.kind == AC3FORGE_AC4_OBJECT_DYN);
        }
        // The last frame's properties are the update's for the object it moved,
        // the configuration's for the rest.
        const bool moved = static_cast<std::size_t>(update.object) == o;
        check_properties_near(object.properties, moved ? update.properties : configured.properties,
                              !configured.lfe && !configured.bed);
        // Its audio is its own tone (each object has one of its own, so an
        // object decoded into the wrong place would not correlate), and no
        // other object's.
        if (configured.properties.active) {
            CHECK(correlation(input[o], object.samples, static_cast<std::size_t>(lag)) > 0.98);
            const std::size_t other = order[(d + 1) % order.size()];
            CHECK(std::abs(correlation(input[other], object.samples,
                                       static_cast<std::size_t>(lag))) < 0.5);
        }
    }

    // The update comes out where its input sample does, to within 32 samples,
    // with the ramp it was given.
    const std::size_t moved_decoded = static_cast<std::size_t>(
        std::find(order.begin(), order.end(), static_cast<std::size_t>(update.object)) -
        order.begin());
    const auto found =
        std::find_if(decoded.updates.begin(), decoded.updates.end(), [&](const DecodedUpdate& u) {
            return u.object == moved_decoded &&
                   std::abs(u.properties.x - update.properties.position[0]) <= 1.0 / 124.0 + 1e-9 &&
                   std::abs(u.properties.y - update.properties.position[1]) <= 1.0 / 124.0 + 1e-9;
        });
    REQUIRE(found != decoded.updates.end());
    CHECK(found->sample <= update.sample + lag);
    CHECK(found->sample > update.sample + lag - 32);
    CHECK(found->ramp_samples == update.ramp_samples);
    check_properties_near(found->properties, update.properties, true);
}

}  // namespace

TEST_CASE(
    "an A-JOC object scene encodes through the C API as iclforge::ac4::Encoder writes it and "
    "decodes back",
    "[capi][ac4]") {
    check_object_scene(ajoc_scene());
}

TEST_CASE(
    "a direct-coded object scene encodes through the C API as iclforge::ac4::Encoder writes it and "
    "decodes "
    "back",
    "[capi][ac4]") {
    check_object_scene(direct_scene());
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

std::string refusal_of(const ac3forge_ac4_encoder_config_t& config) {
    return ac3forge_ac4_encoder_refusal_reason(&config);
}

ac3forge_status_t create_status(const ac3forge_ac4_encoder_config_t& config) {
    ac3forge_ac4_encoder_t* encoder = nullptr;
    const ac3forge_status_t status = ac3forge_ac4_encoder_create(&config, &encoder);
    CHECK((status == AC3FORGE_OK) == (encoder != nullptr));
    ac3forge_ac4_encoder_destroy(encoder);
    return status;
}

// `count` dynamic objects at the room's centre, as the C structs, over a
// computed downmix of `signals` (0 for the encoder's own choice).
std::unique_ptr<CObjectScene> many_objects(std::size_t count, int kbps, int signals = 0) {
    auto scene = std::make_unique<CObjectScene>();
    scene->objects.resize(count);
    for (ac3forge_ac4_object_config_t& object : scene->objects) {
        ac3forge_ac4_object_config_init(&object);
    }
    ac3forge_ac4_objects_config_init(&scene->objects_config);
    scene->objects_config.objects = scene->objects.data();
    scene->objects_config.object_count = count;
    scene->objects_config.has_downmix_signals = signals > 0 ? 1 : 0;
    scene->objects_config.downmix_signals = signals;
    ac3forge_ac4_encoder_config_init(&scene->config);
    scene->config.bitrate_kbps = kbps;
    scene->config.experimental.objects = 1;
    scene->config.objects = &scene->objects_config;
    return scene;
}

}  // namespace

TEST_CASE("the object configuration's limits and refusals are the encoder's", "[capi][ac4]") {
    auto scene = c_scene_of(ajoc_scene());
    CHECK(refusal_of(scene->config).empty());
    CHECK(create_status(scene->config) == AC3FORGE_OK);

    SECTION("objects are experimental") {
        scene->config.experimental.objects = 0;
        CHECK(refusal_of(scene->config) == "objects without experimental.objects");
        CHECK(create_status(scene->config) == AC3FORGE_ERROR_AC4_ENCODE_INVALID_CONFIG);
    }
    SECTION("the object substream is frame_rate_index 13's alone") {
        scene->config.frame_rate_index = 2;
        CHECK(refusal_of(scene->config) == "objects at a frame_rate_index other than 13");
        CHECK(create_status(scene->config) == AC3FORGE_ERROR_AC4_ENCODE_INVALID_CONFIG);
        scene->config.frame_rate_index = 13;
        CHECK(refusal_of(scene->config).empty());
    }
    SECTION("the most objects a substream takes") {
        auto full = many_objects(AC3FORGE_AC4_MAX_OBJECTS, 384);
        CHECK(refusal_of(full->config).empty());
        CHECK(create_status(full->config) == AC3FORGE_OK);
        auto over = many_objects(AC3FORGE_AC4_MAX_OBJECTS + 1, 384);
        CHECK(refusal_of(over->config) == "more than 64 objects");
        CHECK(create_status(over->config) == AC3FORGE_ERROR_AC4_ENCODE_INVALID_CONFIG);
        // A count no caller's array holds is refused by count, not read.
        over->objects_config.object_count = std::numeric_limits<std::size_t>::max() / 2;
        CHECK(refusal_of(over->config) == "more than 64 objects");
        auto none = many_objects(0, 256);
        none->objects_config.objects = nullptr;
        CHECK(refusal_of(none->config) == "an object substream without objects");
    }
    SECTION("the most downmix signals a computed A-JOC downmix takes") {
        auto most =
            many_objects(AC3FORGE_AC4_MAX_DOWNMIX_SIGNALS, 512, AC3FORGE_AC4_MAX_DOWNMIX_SIGNALS);
        CHECK(refusal_of(most->config).empty());
        CHECK(create_status(most->config) == AC3FORGE_OK);
        auto over = many_objects(AC3FORGE_AC4_MAX_DOWNMIX_SIGNALS + 1, 512,
                                 AC3FORGE_AC4_MAX_DOWNMIX_SIGNALS + 1);
        CHECK(refusal_of(over->config) ==
              "a computed downmix of no signal, of more than 11 or of more than its full-band "
              "objects");
        auto more_than_objects = many_objects(3, 256, 4);
        CHECK(refusal_of(more_than_objects->config) ==
              "a computed downmix of no signal, of more than 11 or of more than its full-band "
              "objects");
    }
    SECTION("what the coding allows") {
        scene->objects_config.coding = AC3FORGE_AC4_OBJECT_CODING_DIRECT;  // has a bed object
        CHECK(refusal_of(scene->config) == "bed objects in direct-coded object substreams");
        scene->objects_config.coding = AC3FORGE_AC4_OBJECT_CODING_AJOC;
        scene->objects_config.has_parameter_bands = 1;
        scene->objects_config.parameter_bands = 10;
        CHECK(refusal_of(scene->config) ==
              "A-JOC parameter bands other than Table 78's 23, 15, 12, 9, 7, 5, 3 or 1");
        scene->objects_config.parameter_bands = 9;
        CHECK(refusal_of(scene->config).empty());
        scene->objects_config.downmix = AC3FORGE_AC4_AJOC_DOWNMIX_STATIC_50;  // and an LFE object
        CHECK(refusal_of(scene->config) == "an LFE object with a static 5.0 downmix");
        scene->config.codec_mode = AC3FORGE_AC4_CODEC_ASPX_ACPL2;
        scene->objects_config.downmix = AC3FORGE_AC4_AJOC_DOWNMIX_COMPUTED;
        CHECK(refusal_of(scene->config) ==
              "an object substream's codec mode other than kAuto, kSimple or kAspx");
    }
    SECTION("the metadata's ranges") {
        scene->objects[0].properties.x = 1.5;
        CHECK(refusal_of(scene->config) ==
              "an object's properties off the ranges ObjectProperties gives them");
        CHECK(create_status(scene->config) == AC3FORGE_ERROR_AC4_ENCODE_INVALID_CONFIG);
        // What a zero-initialised properties struct is: a depth exponent no
        // code holds. ac3forge_ac4_object_properties_init() is what fills it.
        scene->objects[0].properties = ac3forge_ac4_object_properties_t{};
        CHECK(refusal_of(scene->config) ==
              "an object's properties off the ranges ObjectProperties gives them");
        ac3forge_ac4_object_properties_init(&scene->objects[0].properties);
        CHECK(refusal_of(scene->config).empty());
        scene->objects[0].properties.gain_db = -std::numeric_limits<double>::infinity();
        CHECK(refusal_of(scene->config).empty());  // -infinity is silence
        scene->objects[0].properties.gain_db = 16.0;
        CHECK_FALSE(refusal_of(scene->config).empty());
    }
    SECTION("arguments that are not a configuration at all") {
        const std::string not_an_argument =
            "an array pointer that is NULL where the array has entries, or an enumerator outside "
            "its "
            "enumeration";
        scene->objects_config.objects = nullptr;
        CHECK(refusal_of(scene->config) == not_an_argument);
        CHECK(create_status(scene->config) == AC3FORGE_ERROR_INVALID_ARGUMENT);
        scene->objects_config.objects = scene->objects.data();
        set_raw(scene->objects[3].bed, 3);  // no loudspeaker has code 3
        CHECK(refusal_of(scene->config) == not_an_argument);
        CHECK(create_status(scene->config) == AC3FORGE_ERROR_INVALID_ARGUMENT);
        scene->objects[3].bed = AC3FORGE_AC4_BED_TOP_BACK_RIGHT;
        CHECK(refusal_of(scene->config).empty());
        set_raw(scene->objects_config.coding, 2);
        CHECK(create_status(scene->config) == AC3FORGE_ERROR_INVALID_ARGUMENT);
        scene->objects_config.coding = AC3FORGE_AC4_OBJECT_CODING_AJOC;
        set_raw(scene->objects_config.downmix, 3);
        CHECK(create_status(scene->config) == AC3FORGE_ERROR_INVALID_ARGUMENT);
        scene->objects_config.downmix = AC3FORGE_AC4_AJOC_DOWNMIX_COMPUTED;
        set_raw(scene->config.experimental.seven_x, 4);
        CHECK(create_status(scene->config) == AC3FORGE_ERROR_INVALID_ARGUMENT);
        scene->config.experimental.seven_x = AC3FORGE_AC4_PAIR_NONE;
        scene->config.iframes = nullptr;
        scene->config.iframe_count = 1;
        CHECK(create_status(scene->config) == AC3FORGE_ERROR_INVALID_ARGUMENT);
        scene->config.iframe_count = 0;
        scene->config.fragment_start_count = 2;
        CHECK(create_status(scene->config) == AC3FORGE_ERROR_INVALID_ARGUMENT);
        scene->config.fragment_start_count = 0;
        CHECK(create_status(scene->config) == AC3FORGE_OK);
    }
}

TEST_CASE("ac3forge_ac4_encoder_encode_objects refuses input the encoder refuses", "[capi][ac4]") {
    const ObjectScene scene = ajoc_scene();
    auto c_scene = c_scene_of(scene);
    const std::vector<std::vector<float>> input = object_input(scene.objects(), 2 * kObjectFrame);
    std::vector<const float*> views;
    for (const auto& channel : input) {
        views.push_back(channel.data());
    }
    ac3forge_ac4_encoder_t* encoder = nullptr;
    REQUIRE(ac3forge_ac4_encoder_create(&c_scene->config, &encoder) == AC3FORGE_OK);

    ac3forge_ac4_encoded_frame_t** frames = nullptr;
    size_t count = 0;
    const auto encode = [&](const std::vector<ac3forge_ac4_object_metadata_update_t>& updates,
                            size_t objects) {
        return ac3forge_ac4_encoder_encode_objects(encoder, views.data(), objects, input[0].size(),
                                                   updates.data(), updates.size(), &frames, &count);
    };
    ac3forge_ac4_object_metadata_update_t update;
    ac3forge_ac4_object_metadata_update_init(&update);

    update.object = views.size();  // one past the last object
    CHECK(encode({update}, views.size()) == AC3FORGE_ERROR_AC4_ENCODE_INVALID_INPUT);
    update.object = std::numeric_limits<std::size_t>::max();
    CHECK(encode({update}, views.size()) == AC3FORGE_ERROR_AC4_ENCODE_INVALID_INPUT);
    update.object = 0;
    update.sample = -1;  // before this input's first sample
    CHECK(encode({update}, views.size()) == AC3FORGE_ERROR_AC4_ENCODE_INVALID_INPUT);
    update.sample = 0;
    update.properties.priority = 2.0;  // off its range
    CHECK(encode({update}, views.size()) == AC3FORGE_ERROR_AC4_ENCODE_INVALID_INPUT);
    ac3forge_ac4_object_properties_init(&update.properties);
    CHECK(encode({}, views.size() - 1) == AC3FORGE_ERROR_AC4_ENCODE_INVALID_INPUT);
    CHECK(frames == nullptr);
    CHECK(count == 0);

    // A NULL array with entries is not an update list at all.
    CHECK(ac3forge_ac4_encoder_encode_objects(encoder, views.data(), views.size(), input[0].size(),
                                              nullptr, 1, &frames,
                                              &count) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    const float* null_channel[1] = {nullptr};
    CHECK(ac3forge_ac4_encoder_encode_objects(encoder, null_channel, 1, 16, nullptr, 0, &frames,
                                              &count) == AC3FORGE_ERROR_INVALID_ARGUMENT);

    // What the refusals left the encoder as it was: the same input encodes,
    // with or without updates (and by encode(), which takes none).
    CHECK(encode({update}, views.size()) == AC3FORGE_OK);
    ac3forge_ac4_encoded_frame_array_destroy(frames, count);
    frames = nullptr;
    CHECK(ac3forge_ac4_encoder_encode(encoder, views.data(), views.size(), input[0].size(), &frames,
                                      &count) == AC3FORGE_OK);
    ac3forge_ac4_encoded_frame_array_destroy(frames, count);
    ac3forge_ac4_encoder_destroy(encoder);

    // An encoder of channels has no object for an update to name.
    ac3forge_ac4_encoder_config_t stereo;
    ac3forge_ac4_encoder_config_init(&stereo);
    REQUIRE(ac3forge_ac4_encoder_create(&stereo, &encoder) == AC3FORGE_OK);
    frames = nullptr;
    count = 0;
    CHECK(ac3forge_ac4_encoder_encode_objects(encoder, views.data(), 2, input[0].size(), &update, 1,
                                              &frames,
                                              &count) == AC3FORGE_ERROR_AC4_ENCODE_INVALID_INPUT);
    CHECK(ac3forge_ac4_encoder_encode_objects(encoder, views.data(), 2, input[0].size(), nullptr, 0,
                                              &frames, &count) == AC3FORGE_OK);
    ac3forge_ac4_encoded_frame_array_destroy(frames, count);
    ac3forge_ac4_encoder_destroy(encoder);
}

namespace {

struct ChannelStream {
    std::vector<std::vector<std::uint8_t>> frames;
    std::vector<bool> iframe;
};

// `input` (planar, all one length) encoded whole and flushed, by the C API
// with `config` and by iclforge::ac4::Encoder with `reference`: what each wrote.
ChannelStream encode_channels_with_c_api(const ac3forge_ac4_encoder_config_t& config,
                                         const std::vector<std::vector<float>>& input) {
    ChannelStream out;
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
            out.frames.emplace_back(bytes, bytes + ac3forge_ac4_encoded_frame_size(frames[i]));
            out.iframe.push_back(ac3forge_ac4_encoded_frame_iframe(frames[i]) != 0);
        }
        ac3forge_ac4_encoded_frame_array_destroy(frames, count);
    };
    ac3forge_ac4_encoded_frame_t** frames = nullptr;
    size_t count = 0;
    REQUIRE(ac3forge_ac4_encoder_encode(encoder, views.data(), views.size(), input.front().size(),
                                        &frames, &count) == AC3FORGE_OK);
    take(frames, count);
    frames = nullptr;
    count = 0;
    REQUIRE(ac3forge_ac4_encoder_flush(encoder, &frames, &count) == AC3FORGE_OK);
    take(frames, count);
    ac3forge_ac4_encoder_destroy(encoder);
    return out;
}

ChannelStream encode_channels_with_cpp(const iclforge::ac4::EncoderConfig& reference,
                                       const std::vector<std::vector<float>>& input) {
    ChannelStream out;
    auto encoder = iclforge::ac4::Encoder::create(reference);
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
            out.iframe.push_back(frame.iframe);
        }
    }
    return out;
}

// One tone per channel, a distinct pitch each, `frames` frames long.
std::vector<std::vector<float>> channel_tones(std::size_t channels, std::size_t frames) {
    std::vector<std::vector<float>> input(channels, std::vector<float>(frames * kObjectFrame));
    for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t n = 0; n < input[c].size(); ++n) {
            input[c][n] = static_cast<float>(
                0.2 * std::sin(2.0 * std::numbers::pi * (500.0 + 170.0 * static_cast<double>(c)) *
                               static_cast<double>(n) / kObjectRate));
        }
    }
    return input;
}

}  // namespace

TEST_CASE("the encoder configuration's I-frame lists reach the encoder", "[capi][ac4]") {
    const std::vector<std::vector<float>> input = channel_tones(2, 8);
    ac3forge_ac4_encoder_config_t config;
    ac3forge_ac4_encoder_config_init(&config);
    config.bitrate_kbps = 96;
    config.iframe_interval = 1000;  // the first frame alone, without the lists
    iclforge::ac4::EncoderConfig reference;
    reference.bitrate_kbps = 96;
    reference.iframe_interval = 1000;

    const auto flagged = [](const ChannelStream& stream) {
        std::vector<std::size_t> frames;
        for (std::size_t f = 0; f < stream.iframe.size(); ++f) {
            if (stream.iframe[f]) {
                frames.push_back(f);
            }
        }
        return frames;
    };

    const ChannelStream plain = encode_channels_with_c_api(config, input);
    REQUIRE(plain.frames.size() > 6);
    CHECK(flagged(plain) == std::vector<std::size_t>{0});

    SECTION("iframes: the frames, counted from 0, that must be I-frames") {
        const std::array<std::int64_t, 2> iframes = {5, 2};  // in any order
        config.iframes = iframes.data();
        config.iframe_count = iframes.size();
        reference.iframes = {5, 2};
        const ChannelStream stream = encode_channels_with_c_api(config, input);
        CHECK(flagged(stream) == std::vector<std::size_t>{0, 2, 5});
        const ChannelStream expected = encode_channels_with_cpp(reference, input);
        CHECK(stream.frames == expected.frames);
        CHECK(stream.iframe == expected.iframe);
        CHECK(stream.frames != plain.frames);
    }
    SECTION("fragment_starts: the first frame to start at or after each is an I-frame") {
        // Frame 2's output starts at sample 4096 exactly; frame 5's is the
        // first to start after 9000 (frame 4's is 8192, frame 5's 10240).
        const std::array<std::int64_t, 2> starts = {4096, 9000};
        config.fragment_starts = starts.data();
        config.fragment_start_count = starts.size();
        reference.fragment_starts = {4096, 9000};
        const ChannelStream stream = encode_channels_with_c_api(config, input);
        CHECK(flagged(stream) == std::vector<std::size_t>{0, 2, 5});
        const ChannelStream expected = encode_channels_with_cpp(reference, input);
        CHECK(stream.frames == expected.frames);
        CHECK(stream.iframe == expected.iframe);
    }
}

TEST_CASE("the encoder configuration's experimental flags reach the encoder", "[capi][ac4]") {
    ac3forge_ac4_encoder_config_t config;
    ac3forge_ac4_encoder_config_init(&config);

    SECTION("acpl: ASPX_ACPL_1 in stereo") {
        config.bitrate_kbps = 64;
        config.codec_mode = AC3FORGE_AC4_CODEC_ASPX_ACPL1;
        CHECK_FALSE(refusal_of(config).empty());
        CHECK(create_status(config) == AC3FORGE_ERROR_AC4_ENCODE_INVALID_CONFIG);
        config.experimental.acpl = 1;
        CHECK(refusal_of(config).empty());
        CHECK(create_status(config) == AC3FORGE_OK);
    }
    SECTION("seven_x: 7.1's additional pair") {
        config.channels = 8;
        config.bitrate_kbps = 448;
        CHECK(refusal_of(config) ==
              "seven or eight channels without experimental.seven_x's additional pair");
        for (const auto pair :
             {AC3FORGE_AC4_PAIR_BACK, AC3FORGE_AC4_PAIR_WIDE, AC3FORGE_AC4_PAIR_TOP_FRONT}) {
            config.experimental.seven_x = pair;
            CHECK(refusal_of(config).empty());
            CHECK(create_status(config) == AC3FORGE_OK);
        }
        config.channels = 6;  // a pair with 5.1
        CHECK(refusal_of(config) ==
              "experimental.seven_x's additional pair without seven or eight channels");
    }
    SECTION("back_pair: 7.1.4's back pair") {
        config.channels = 12;
        config.bitrate_kbps = 768;
        CHECK_FALSE(refusal_of(config).empty());
        CHECK(refusal_of(config).find("experimental.back_pair") != std::string::npos);
        config.experimental.back_pair = 1;
        CHECK(refusal_of(config).empty());
        CHECK(create_status(config) == AC3FORGE_OK);
    }
    SECTION("ajcc: the immersive element's ASPX_AJCC") {
        config.channels = 10;
        config.bitrate_kbps = 448;
        config.codec_mode = AC3FORGE_AC4_CODEC_ASPX_AJCC;
        CHECK(refusal_of(config) == "ASPX_AJCC without experimental.ajcc");
        config.experimental.ajcc = 1;
        CHECK(refusal_of(config).empty());
        CHECK(create_status(config) == AC3FORGE_OK);
    }
    SECTION("coding_configs: refused beside an A-CPL codec mode") {
        config.channels = 6;
        config.bitrate_kbps = 128;
        config.codec_mode = AC3FORGE_AC4_CODEC_ASPX_ACPL2;
        CHECK(refusal_of(config).empty());
        config.experimental.coding_configs = 1;
        CHECK(refusal_of(config) == "an A-CPL codec mode with experimental.coding_configs");
    }
    SECTION("the ASPX mode's options are written as iclforge::ac4::Encoder writes them") {
        // Stereo in the ASPX mode over a steady tone above the crossover, with
        // an attack in both channels every 1 100 samples: each option on its
        // own changes the stream, and the stream is the C++ API's, byte for
        // byte.
        std::vector<std::vector<float>> input = channel_tones(2, 10);
        std::uint32_t state = 12345;
        const auto noise = [&state]() {
            state = state * 1664525U + 1013904223U;
            return static_cast<float>((state >> 8) & 0xFFFFU) / 32768.0F - 1.0F;
        };
        for (std::size_t c = 0; c < 2; ++c) {
            for (std::size_t n = 0; n < input[c].size(); ++n) {
                input[c][n] +=
                    0.05F * static_cast<float>(std::sin(2.0 * std::numbers::pi * 12000.0 *
                                                        static_cast<double>(n) / kObjectRate));
            }
        }
        for (std::size_t at = 3000; at + 200 < input[0].size(); at += 1100) {
            for (std::size_t n = at; n < at + 150; ++n) {
                input[0][n] += 0.5F * noise();
                input[1][n] += (at % 2 == 0 ? 0.5F : 0.05F) * noise();
            }
        }
        config.bitrate_kbps = 48;
        config.codec_mode = AC3FORGE_AC4_CODEC_ASPX;
        iclforge::ac4::EncoderConfig reference;
        reference.bitrate_kbps = 48;
        reference.codec_mode = iclforge::ac4::CodecMode::kAspx;
        const ChannelStream plain = encode_channels_with_c_api(config, input);
        CHECK(plain.frames == encode_channels_with_cpp(reference, input).frames);

        const auto with_option =
            [&](int ac3forge_ac4_experimental_t::* option,
                bool iclforge::ac4::EncoderConfig::Experimental::* cpp_option) {
                ac3forge_ac4_encoder_config_t options = config;
                options.experimental.*option = 1;
                iclforge::ac4::EncoderConfig cpp = reference;
                cpp.experimental.*cpp_option = true;
                const ChannelStream stream = encode_channels_with_c_api(options, input);
                CHECK(stream.frames == encode_channels_with_cpp(cpp, input).frames);
                CHECK(stream.frames != plain.frames);
            };
        with_option(&ac3forge_ac4_experimental_t::aspx_balance,
                    &iclforge::ac4::EncoderConfig::Experimental::aspx_balance);
        with_option(&ac3forge_ac4_experimental_t::aspx_varvar,
                    &iclforge::ac4::EncoderConfig::Experimental::aspx_varvar);
        with_option(&ac3forge_ac4_experimental_t::aspx_interleave,
                    &iclforge::ac4::EncoderConfig::Experimental::aspx_interleave);
    }
}

TEST_CASE("AC-4 accessors are null-safe", "[capi][ac4]") {
    CHECK(ac3forge_ac4_decoder_latency_samples(nullptr) == 0);
    CHECK(std::string_view(ac3forge_ac4_decoder_refusal_reason(nullptr)).empty());
    CHECK(ac3forge_ac4_decoder_presentation_count(nullptr) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_toc_index(nullptr, 0) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_has_id(nullptr, 0) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_id(nullptr, 0) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_has_md_compat(nullptr, 0) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_md_compat(nullptr, 0) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_enabled(nullptr, 0) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_alternative(nullptr, 0) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_pre_virtualized(nullptr, 0) == 0);
    CHECK(std::string_view(ac3forge_ac4_decoder_presentation_name(nullptr, 0)).empty());
    CHECK(std::string_view(ac3forge_ac4_decoder_presentation_language(nullptr, 0)).empty());
    CHECK(ac3forge_ac4_decoder_presentation_decodable(nullptr, 0) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_selectable(nullptr, 0) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_speaker_count(nullptr, 0) == 0);
    CHECK(ac3forge_ac4_decoder_presentation_speaker(nullptr, 0, 0) == AC3FORGE_AC4_SPEAKER_LEFT);
    CHECK(ac3forge_ac4_decoder_metadata_loudness(nullptr).has_dialnorm_dbfs == 0);

    CHECK(ac3forge_ac4_decoded_frame_sample_rate_hz(nullptr) == 0);
    CHECK(ac3forge_ac4_decoded_frame_sequence_counter(nullptr) == 0);
    CHECK(ac3forge_ac4_decoded_frame_presentation_index(nullptr) == 0);
    CHECK(ac3forge_ac4_decoded_frame_has_presentation_id(nullptr) == 0);
    CHECK(ac3forge_ac4_decoded_frame_presentation_id(nullptr) == 0);
    CHECK(ac3forge_ac4_decoded_frame_channel_count(nullptr) == 0);
    CHECK(ac3forge_ac4_decoded_frame_samples_per_channel(nullptr) == 0);
    CHECK(ac3forge_ac4_decoded_frame_channel_samples(nullptr, 0) == nullptr);
    CHECK(ac3forge_ac4_decoded_frame_speaker(nullptr, 0) == AC3FORGE_AC4_SPEAKER_LEFT);
    CHECK(ac3forge_ac4_decoded_frame_has_concealed(nullptr) == 0);
    CHECK(ac3forge_ac4_decoded_frame_concealment_action(nullptr) ==
          AC3FORGE_AC4_CONCEALMENT_ACTION_MUTE);
    CHECK(ac3forge_ac4_decoded_frame_concealment_error(nullptr) == AC3FORGE_OK);
    CHECK(ac3forge_ac4_decoded_frame_object_count(nullptr) == 0);
    CHECK(ac3forge_ac4_decoded_frame_object_kind(nullptr, 0) == AC3FORGE_AC4_OBJECT_DYN);
    CHECK(ac3forge_ac4_decoded_frame_object_lfe(nullptr, 0) == 0);
    CHECK(ac3forge_ac4_decoded_frame_object_has_speaker(nullptr, 0) == 0);
    CHECK(ac3forge_ac4_decoded_frame_object_speaker(nullptr, 0) == AC3FORGE_AC4_SPEAKER_LEFT);
    CHECK(ac3forge_ac4_decoded_frame_object_samples(nullptr, 0) == nullptr);
    CHECK(ac3forge_ac4_decoded_frame_object_properties(nullptr, 0).gain_db == 0.0);
    CHECK(ac3forge_ac4_decoded_frame_object_properties(nullptr, 0).depth_exponent == 1.0);
    CHECK(ac3forge_ac4_decoded_frame_object_update_count(nullptr, 0) == 0);
    CHECK(ac3forge_ac4_decoded_frame_object_update(nullptr, 0, 0).sample == 0);
    CHECK(ac3forge_ac4_decoded_frame_object_update(nullptr, 0, 0).ramp_samples == 0);
    CHECK(ac3forge_ac4_decoded_frame_object_update(nullptr, 0, 0).properties.depth_exponent == 1.0);
    ac3forge_ac4_decoded_frame_destroy(nullptr);

    CHECK(ac3forge_ac4_encoder_codec_mode(nullptr) == AC3FORGE_AC4_CODEC_AUTO);
    CHECK(ac3forge_ac4_encoder_delay_samples(nullptr) == 0);
    CHECK(ac3forge_ac4_encoder_decoder_delay_samples(nullptr) == 0);
    CHECK(ac3forge_ac4_encoded_frame_data(nullptr) == nullptr);
    CHECK(ac3forge_ac4_encoded_frame_size(nullptr) == 0);
    CHECK(ac3forge_ac4_encoded_frame_samples(nullptr) == 0);
    CHECK(ac3forge_ac4_encoded_frame_iframe(nullptr) == 0);
    ac3forge_ac4_encoded_frame_destroy(nullptr);
    ac3forge_ac4_encoded_frame_array_destroy(nullptr, 0);

    CHECK(std::string_view(ac3forge_ac4_encoder_refusal_reason(nullptr)) == "a NULL configuration");
    CHECK(ac3forge_ac4_encoder_encode_objects(nullptr, nullptr, 0, 0, nullptr, 0, nullptr,
                                              nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(ac3forge_ac4_encoder_toc(nullptr, nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    ac3forge_ac4_toc_destroy(nullptr);
    CHECK(ac3forge_ac4_build_dac4(nullptr, nullptr) == AC3FORGE_ERROR_INVALID_ARGUMENT);
    CHECK(std::string_view(ac3forge_ac4_dac4_refusal(nullptr)).empty());
    CHECK(ac3forge_ac4_media_timing(nullptr, nullptr, nullptr) == 0);
    CHECK(ac3forge_ac4_samples_per_frame(nullptr, nullptr) == 0);

    ac3forge_ac4_decoder_destroy(nullptr);
    ac3forge_ac4_encoder_destroy(nullptr);
}
