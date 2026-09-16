#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/sendspin/codec.hpp"
#include "ac3/sendspin/messages.hpp"

// player@v1's codecs: PCM and FLAC decoding to exactly what was encoded at each bit depth,
// whatever sizes the input arrives in; FLAC's header, frames and positions; Opus's 20 ms packets,
// its look-ahead and a faithful decode; and the formats each refuses.

namespace {

namespace codec = ac3::sendspin::codec;
namespace m = ac3::sendspin::messages;

// Two tones and a little noise at `bit_depth`, interleaved.
std::vector<std::int32_t> signal(std::int32_t channels, std::int32_t sample_rate, std::int32_t bit_depth,
                                 std::int32_t frames) {
    std::vector<std::int32_t> out;
    out.reserve(static_cast<std::size_t>(channels) * static_cast<std::size_t>(frames));
    const double full = std::ldexp(1.0, bit_depth - 1) - 1.0;
    std::uint32_t noise = 12345;
    for (std::int32_t frame = 0; frame < frames; ++frame) {
        const double t = static_cast<double>(frame) / sample_rate;
        for (std::int32_t channel = 0; channel < channels; ++channel) {
            noise = (noise * 1664525U) + 1013904223U;
            const double dither = (static_cast<double>(noise >> 8U) / 16777216.0) - 0.5;
            const double value = (0.4 * std::sin(2.0 * std::numbers::pi * 440.0 * (1.0 + (0.5 * channel)) * t)) +
                                 (0.2 * std::sin(2.0 * std::numbers::pi * 3000.0 * t)) + (0.01 * dither);
            out.push_back(static_cast<std::int32_t>(std::lround(value * full)));
        }
    }
    return out;
}

// Encodes `samples` in pieces of uneven sizes, then finishes.
std::vector<codec::Unit> encode_all(codec::Encoder& encoder, const std::vector<std::int32_t>& samples,
                                    std::int32_t channels) {
    std::vector<codec::Unit> units;
    const std::array<std::size_t, 4> pieces{317, 1024, 55, 4100};
    const auto width = static_cast<std::size_t>(channels);
    const std::size_t total = samples.size() / width;
    std::size_t offset = 0;
    for (std::size_t k = 0; offset < total; ++k) {
        const std::size_t frames = std::min(pieces[k % pieces.size()], total - offset);
        std::optional<std::vector<codec::Unit>> produced =
            encoder.encode(std::span<const std::int32_t>(samples).subspan(offset * width, frames * width));
        REQUIRE(produced.has_value());
        units.insert(units.end(), produced->begin(), produced->end());
        offset += frames;
    }
    std::optional<std::vector<codec::Unit>> rest = encoder.finish();
    REQUIRE(rest.has_value());
    units.insert(units.end(), rest->begin(), rest->end());
    return units;
}

}  // namespace

TEST_CASE("codec: PCM and FLAC decode to exactly what was encoded", "[sendspin][codec]") {
    const m::Codec kind = GENERATE(m::Codec::kPcm, m::Codec::kFlac);
    const std::int32_t bit_depth = GENERATE(16, 24, 32);
    const std::int32_t channels = GENERATE(1, 2);
    const m::AudioFormat format{.codec = kind, .channels = channels, .sample_rate = 48000, .bit_depth = bit_depth};
    const std::vector<std::int32_t> input = signal(channels, 48000, bit_depth, 48000);

    const std::unique_ptr<codec::Encoder> encoder = codec::make_encoder(format);
    REQUIRE(encoder != nullptr);
    CHECK(encoder->delay_frames() == 0);
    CHECK(encoder->codec_header().empty() == (kind == m::Codec::kPcm));
    const std::vector<codec::Unit> units = encode_all(*encoder, input, channels);
    REQUIRE_FALSE(units.empty());

    const std::unique_ptr<codec::Decoder> decoder =
        codec::make_decoder({.format = format, .codec_header = encoder->codec_header()});
    REQUIRE(decoder != nullptr);
    CHECK(decoder->bit_depth() == bit_depth);
    std::vector<std::int32_t> output;
    std::int64_t position = 0;
    for (const codec::Unit& unit : units) {
        CHECK(unit.first_frame == position);
        const std::optional<std::vector<std::int32_t>> decoded = decoder->decode(unit.bytes);
        REQUIRE(decoded.has_value());
        output.insert(output.end(), decoded->begin(), decoded->end());
        position += static_cast<std::int64_t>(decoded->size()) / channels;
    }
    REQUIRE(output.size() == input.size());
    CHECK(std::equal(output.begin(), output.end(), input.begin()));
}

TEST_CASE("codec: FLAC's header is the marker and STREAMINFO, and each unit one frame", "[sendspin][codec]") {
    const m::AudioFormat format{.codec = m::Codec::kFlac, .channels = 2, .sample_rate = 44100, .bit_depth = 16};
    const std::unique_ptr<codec::Encoder> encoder = codec::make_encoder(format);
    REQUIRE(encoder != nullptr);
    const std::vector<std::uint8_t>& header = encoder->codec_header();
    REQUIRE(header.size() == 42);
    CHECK(std::string(header.begin(), header.begin() + 4) == "fLaC");
    // STREAMINFO, flagged as the last metadata block.
    CHECK((header[4] & 0x7FU) == 0);
    CHECK((header[4] & 0x80U) != 0);

    const std::vector<codec::Unit> units = encode_all(*encoder, signal(2, 44100, 16, 44100), 2);
    // Blocks of 4,096 frames, the subset's largest at 44.1 kHz and under 150 ms.
    REQUIRE(units.size() == 11);
    for (std::size_t k = 0; k < units.size(); ++k) {
        CHECK(units[k].first_frame == static_cast<std::int64_t>(k) * 4096);
        REQUIRE(units[k].bytes.size() > 2);
        // A frame begins with its sync code.
        CHECK(units[k].bytes[0] == 0xFF);
        CHECK((units[k].bytes[1] & 0xFCU) == 0xF8);
    }

    const std::unique_ptr<codec::Decoder> decoder = codec::make_decoder({.format = format, .codec_header = header});
    REQUIRE(decoder != nullptr);
    // Bytes that hold no frame give no samples, and the next unit still decodes.
    const std::optional<std::vector<std::int32_t>> garbage = decoder->decode(std::vector<std::uint8_t>(64, 0x55));
    CHECK((!garbage || garbage->empty()));
    const std::optional<std::vector<std::int32_t>> first = decoder->decode(units[0].bytes);
    REQUIRE(first.has_value());
    CHECK(first->size() == 2U * 4096U);

    // A stream whose header is not FLAC's has no decoder.
    CHECK(codec::make_decoder({.format = format, .codec_header = {1, 2, 3}}) == nullptr);
}

TEST_CASE("codec: Opus packets of 20 ms, the look-ahead, and a faithful decode", "[sendspin][codec]") {
    const m::AudioFormat format{.codec = m::Codec::kOpus, .channels = 2, .sample_rate = 48000, .bit_depth = 16};
    const std::vector<std::int32_t> input = signal(2, 48000, 16, 48000);
    const std::unique_ptr<codec::Encoder> encoder = codec::make_encoder(format);
    REQUIRE(encoder != nullptr);
    CHECK(encoder->codec_header().empty());
    const std::int32_t delay = encoder->delay_frames();
    CHECK(delay > 0);
    CHECK(delay < 960);

    const std::vector<codec::Unit> units = encode_all(*encoder, input, 2);
    REQUIRE(units.size() == 50);
    const std::unique_ptr<codec::Decoder> decoder = codec::make_decoder({.format = format, .codec_header = {}});
    REQUIRE(decoder != nullptr);
    CHECK(decoder->bit_depth() == 16);
    std::vector<std::int32_t> output;
    for (std::size_t k = 0; k < units.size(); ++k) {
        CHECK(units[k].first_frame == static_cast<std::int64_t>(k) * 960);
        const std::optional<std::vector<std::int32_t>> decoded = decoder->decode(units[k].bytes);
        REQUIRE(decoded.has_value());
        CHECK(decoded->size() == 2U * 960U);
        output.insert(output.end(), decoded->begin(), decoded->end());
    }

    // Shifted back by the look-ahead, the decoded audio follows the input closely.
    double signal_power = 0.0;
    double error_power = 0.0;
    const auto shift = static_cast<std::size_t>(delay) * 2U;
    for (std::size_t i = 0; i + shift < output.size(); ++i) {
        const auto wanted = static_cast<double>(input[i]);
        const double difference = static_cast<double>(output[i + shift]) - wanted;
        signal_power += wanted * wanted;
        error_power += difference * difference;
    }
    CHECK(10.0 * std::log10(signal_power / error_power) > 12.0);

    // Rates Opus does not code, and more channels than a plain Opus stream carries.
    CHECK(codec::make_encoder({.codec = m::Codec::kOpus, .channels = 2, .sample_rate = 44100, .bit_depth = 16}) == nullptr);
    CHECK(codec::make_encoder({.codec = m::Codec::kOpus, .channels = 6, .sample_rate = 48000, .bit_depth = 16}) == nullptr);
    CHECK(codec::make_encoder({.codec = m::Codec::kPcm, .channels = 2, .sample_rate = 48000, .bit_depth = 20}) == nullptr);
}
