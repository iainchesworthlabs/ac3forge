// ac4::Decoder::decode() and the reconstruction behind it (src/ac4dec/src/pcm):
// the noise fill's random number generator against the text's own closed
// form, and the committed DEE streams decoded to PCM - each channel's tone on
// its own channel, and the modes this version refuses refused by name.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"
#include "pcm/snf_random.hpp"
#include "tables/noise_tables.hpp"

namespace {

namespace fs = std::filesystem;

std::vector<std::byte> read_stream(const std::string& leg) {
    const fs::path path = fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg / "dee.ac4";
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

struct Decoded {
    std::vector<ac4::Speaker> speakers;
    std::vector<std::vector<float>> channels;
    int sample_rate_hz = 0;
    std::size_t frames = 0;
};

Decoded decode_all(const std::string& leg) {
    const std::vector<std::byte> stream = read_stream(leg);
    const ac4::ScanResult scan = ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    ac4::Decoder decoder;
    Decoded out;
    for (const ac4::SyncFrame& frame : scan.frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        INFO(decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const ac4::DecodedFrame& pcm = **decoded;
        if (out.frames == 0) {
            out.speakers = pcm.speakers;
            out.sample_rate_hz = pcm.sample_rate_hz;
            out.channels.resize(pcm.channels.size());
        }
        REQUIRE(pcm.channels.size() == out.channels.size());
        for (std::size_t c = 0; c < pcm.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), pcm.channels[c].begin(), pcm.channels[c].end());
        }
        ++out.frames;
    }
    return out;
}

// The power of `samples` at `hz`, by Goertzel's recurrence, normalised so that
// a sine of amplitude A gives A^2 / 4 whatever the length.
double tone_power(std::span<const float> samples, double hz, int sample_rate_hz) {
    const double w = 2.0 * std::numbers::pi * hz / static_cast<double>(sample_rate_hz);
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;
    for (const float x : samples) {
        const double s0 = static_cast<double>(x) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    const auto n = static_cast<double>(samples.size());
    return power / (n * n);
}

// The DEE stream's refusal on the first frame, with its reason.
std::pair<ac4::DecodeError, std::string> first_refusal(const std::string& leg) {
    const std::vector<std::byte> stream = read_stream(leg);
    const ac4::ScanResult scan = ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    ac4::Decoder decoder;
    const auto decoded = decoder.decode(scan.frames.front().raw_ac4_frame);
    REQUIRE_FALSE(decoded.has_value());
    return {decoded.error(), std::string{decoder.refusal_reason()}};
}

}  // namespace

TEST_CASE("Pseudocode 24's reset lands where stepping Pseudocode 57's increments lands", "[ac4dec][pcm]") {
    // Pseudocode 55's state, stepped 255 * (sequence_counter % 256) times
    // with `x = x++` read as an increment, is Pseudocode 24's closed form for
    // every counter; ERRATA.md, "x = x++ in Pseudocode 57".
    ac4::detail::RandGenState stepped;
    std::size_t steps = 0;
    for (int counter = 0; counter < 256; ++counter) {
        CAPTURE(counter);
        const std::size_t target = 255U * static_cast<std::size_t>(counter);
        while (steps < target) {
            ac4::detail::advance(stepped);
            ++steps;
        }
        const ac4::detail::RandGenState reset = ac4::detail::reset_rand_gen_state_snf(counter);
        CHECK(reset.offset_a == stepped.offset_a);
        CHECK(reset.offset_b == stepped.offset_b);
        CHECK(reset.state_idx == stepped.state_idx);
        CHECK(reset.current_idx == stepped.current_idx);
        // sequence_counter is ten bits; the reset repeats every 256.
        const ac4::detail::RandGenState again = ac4::detail::reset_rand_gen_state_snf(counter + 256);
        CHECK(again.state_idx == reset.state_idx);
        CHECK(again.current_idx == reset.current_idx);
    }
}

TEST_CASE("GetRandomNoiseValue adds two table entries and then steps", "[ac4dec][pcm]") {
    ac4::detail::RandGenState state = ac4::detail::reset_rand_gen_state_snf(0);
    // Pseudocode 55's state: current 0, state 1.
    CHECK(ac4::detail::get_random_noise_value(state) ==
          ac4::detail::tables::kRandomNoiseTable[0] + ac4::detail::tables::kRandomNoiseTable[1]);
    // After one step: offset_a 1, state_idx 1 + 1 + 0 + 1 = 3, current_idx 1.
    CHECK(state.offset_a == 1);
    CHECK(state.state_idx == 3);
    CHECK(state.current_idx == 1);
    CHECK(ac4::detail::get_random_noise_value(state) ==
          ac4::detail::tables::kRandomNoiseTable[1] + ac4::detail::tables::kRandomNoiseTable[3]);
}

TEST_CASE("a SIMPLE stereo stream decodes each tone to its own channel", "[ac4dec][pcm]") {
    const Decoded decoded = decode_all("ac4-20-tones-192");
    REQUIRE(decoded.speakers == std::vector<ac4::Speaker>{ac4::Speaker::kLeft, ac4::Speaker::kRight});
    CHECK(decoded.sample_rate_hz == 48000);
    for (const auto& channel : decoded.channels) {
        CHECK(channel.size() == decoded.frames * 2048);
    }
    // tones_20: 331 Hz on L and 457 Hz on R (gen_ac4_baseline.py's TONE_HZ).
    // Skip the first and last half second, where the encoder's and decoder's
    // delays put silence.
    const std::size_t skip = 24000;
    REQUIRE(decoded.channels[0].size() > 4 * skip);
    const auto middle = [&](std::size_t c) {
        return std::span<const float>(decoded.channels[c]).subspan(skip, decoded.channels[c].size() - 2 * skip);
    };
    const double left_331 = tone_power(middle(0), 331.0, 48000);
    const double left_457 = tone_power(middle(0), 457.0, 48000);
    const double right_331 = tone_power(middle(1), 331.0, 48000);
    const double right_457 = tone_power(middle(1), 457.0, 48000);
    CHECK(left_331 > 1e4 * left_457);
    CHECK(right_457 > 1e4 * right_331);
    // Each tone at -20 dBFS, amplitude 0.1 of full scale 1.0, within 0.2 dB:
    // ERRATA.md, "Full scale, and the overlap-add's factor of two".
    const auto level_db = [](double power) { return 20.0 * std::log10(std::sqrt(4.0 * power) / 0.1); };
    CHECK(std::abs(level_db(left_331)) < 0.2);
    CHECK(std::abs(level_db(right_457)) < 0.2);
}

TEST_CASE("a SIMPLE stereo music stream decodes every frame", "[ac4dec][pcm]") {
    const Decoded decoded = decode_all("ac4-20-music-192");
    REQUIRE(decoded.channels.size() == 2);
    float peak = 0.0F;
    for (const auto& channel : decoded.channels) {
        CHECK(channel.size() == decoded.frames * 2048);
        for (const float x : channel) {
            peak = std::max(peak, std::abs(x));
        }
    }
    // Its source, music_20, peaks at 0.21 of full scale (-13.7 dBFS).
    CHECK(peak > 0.15F);
    CHECK(peak < 0.3F);
}

TEST_CASE("decode refuses by name what it does not turn into PCM yet", "[ac4dec][pcm]") {
    {
        const auto [error, reason] = first_refusal("ac4-stereo-64");  // ASPX
        CHECK(error == ac4::DecodeError::kUnsupported);
        CHECK(reason.find("A-SPX") != std::string::npos);
    }
    {
        const auto [error, reason] = first_refusal("ac4-51-music-384");  // SIMPLE 5.1
        CHECK(error == ac4::DecodeError::kUnsupported);
        CHECK(reason.find("mono and stereo") != std::string::npos);
    }
    {
        const auto [error, reason] = first_refusal("ac4-ims-music-128-25");  // frame_rate_index 2
        CHECK(error == ac4::DecodeError::kUnsupported);
        CHECK(reason.find("frame_rate_index 13") != std::string::npos);
    }
}

TEST_CASE("decode reports a table of contents it cannot read", "[ac4dec][pcm]") {
    ac4::Decoder decoder;
    const std::array<std::byte, 3> garbage{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
    const auto decoded = decoder.decode(garbage);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == ac4::DecodeError::kInvalidToc);
    CHECK_FALSE(decoder.refusal_reason().empty());
}
