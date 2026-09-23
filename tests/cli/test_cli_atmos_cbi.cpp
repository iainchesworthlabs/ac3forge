#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "platform/process.hpp"

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/oba/oamd.hpp"

// ac3cli's 'atmos-cbi' command (apps/cli/commands/atmos.cpp's run_atmos_cbi): a channel-based-
// immersive bed WAV straight to DD+ JOC E-AC-3 with program.bed != 0 and 0 dynamic objects. Same
// subprocess-integration shape as tests/cli/test_cli.cpp's own atmos-encode coverage - see that
// file's own top comment for why (main.cpp compiles everything into one anonymous-namespace
// binary with no library surface run_atmos_cbi's own logic could be linked into this test binary
// and called directly). This file's job is the CLI wiring (arg parsing, layout resolution, WAV
// channel order -> AtmosEncoder::encode_bed_frame) - tests/oba/test_atmos_cbi.cpp already proves
// the reconstruction math itself against every bed channel's own tone.

namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "cli_atmos_cbi";
    fs::create_directories(dir);
    return dir;
}

// Runs `ac3cli <args>` with both streams redirected to `log`. The platform
// differences - cmd.exe's quoting and std::system()'s two return-value
// shapes - live in tests/platform/process.hpp's run_shell, not here.
int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return ac3::test::platform::run_shell(command);
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// DEE's own cbi_wav channel order for 5.1.4 (tools/generators/gen_object_fixture.py) - see
// tests/oba/test_atmos_cbi.cpp's own header comment for the full provenance.
struct BedChannel {
    const char* label;
    double frequency;
};
constexpr std::array<BedChannel, 10> kInput514 = {{
    {"L", 220.0}, {"R", 277.2}, {"C", 330.0}, {"LFE", 55.0}, {"Ls", 554.4},
    {"Rs", 660.0}, {"Tfl", 740.0}, {"Tfr", 831.6}, {"Tbl", 880.0}, {"Tbr", 1108.8},
}};

std::vector<std::vector<float>> make_cbi_channels(std::span<const BedChannel> layout,
                                                   std::size_t frames, std::uint32_t sample_rate) {
    std::vector<std::vector<float>> out(layout.size(), std::vector<float>(frames));
    for (std::size_t c = 0; c < layout.size(); ++c) {
        for (std::size_t n = 0; n < frames; ++n) {
            out[c][n] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * layout[c].frequency *
                              static_cast<double>(n) / static_cast<double>(sample_rate)));
        }
    }
    return out;
}

double tone_magnitude(std::span<const float> signal, double frequency, double sample_rate) {
    double real = 0.0;
    double imag = 0.0;
    for (std::size_t n = 0; n < signal.size(); ++n) {
        const double phase =
            2.0 * std::numbers::pi * frequency * static_cast<double>(n) / sample_rate;
        real += static_cast<double>(signal[n]) * std::cos(phase);
        imag += static_cast<double>(signal[n]) * std::sin(phase);
    }
    return std::hypot(real, imag) / static_cast<double>(signal.size());
}

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(text.size());
    std::ranges::transform(text, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

}  // namespace

TEST_CASE("atmos-cbi encodes a 5.1.4 WAV into a real bed OAMD+JOC stream", "[cli][atmos-cbi]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "cbi_514_in.wav";
    const auto out_path = dir / "cbi_514_out.ec3";
    const auto log = dir / "cbi_514.log";
    fs::remove(out_path);

    // Several frames: the first has no previous JOC matrix to interpolate
    // from (§6.6.5), and this needs enough of them accumulated for 55 Hz (the
    // LFE tone) to resolve past the sine-projection helper's own noise floor.
    constexpr std::uint32_t kSampleRate = 48000;
    const auto channels = make_cbi_channels(kInput514, 8 * static_cast<std::size_t>(ac3::kSamplesPerFrame), kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    const auto rc = run_cli("atmos-cbi \"" + wav_path.string() + "\" \"" + out_path.string() +
                                "\" 448 5.1.4",
                            log);
    CHECK(rc == 0);
    REQUIRE(fs::exists(out_path));

    const auto data = read_bytes(out_path);
    const auto units = ac3::split_access_units(data);
    REQUIRE(units.has_value());
    REQUIRE(units->size() > 3);

    ac3::Eac3Decoder decoder;
    std::vector<std::vector<float>> accumulated;
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value() || (*decoded)->object_audio.empty()) {
            continue;
        }
        REQUIRE((*decoded)->object_metadata.has_value());
        const auto& program = (*decoded)->object_metadata->program;
        CHECK_FALSE(program.dynamic_only);
        CHECK(program.dynamic_objects == 0);
        CHECK(ac3::oba::object_count(program) == 10);

        const auto& audio = (*decoded)->object_audio;
        if (accumulated.empty()) {
            accumulated.assign(audio.size(), {});
        }
        for (std::size_t i = 0; i < audio.size(); ++i) {
            accumulated[i].insert(accumulated[i].end(), audio[i].begin(), audio[i].end());
        }
    }
    // 10 bed channels less the LFE §6.3.2.2 bypasses.
    REQUIRE(accumulated.size() == 9);

    // kInput514 minus the LFE (index 3), in joc_object_indices() order.
    constexpr std::array<const char*, 9> kExpected = {"L",  "R",   "C",   "Ls",  "Rs",
                                                       "Tfl", "Tfr", "Tbl", "Tbr"};
    for (std::size_t object = 0; object < accumulated.size(); ++object) {
        std::size_t best = 0;
        double best_magnitude = -1.0;
        for (std::size_t channel = 0; channel < kInput514.size(); ++channel) {
            const double magnitude =
                tone_magnitude(accumulated[object], kInput514[channel].frequency, kSampleRate);
            if (magnitude > best_magnitude) {
                best_magnitude = magnitude;
                best = channel;
            }
        }
        INFO("object " << object << " strongest tone is " << kInput514[best].label
                       << ", expected " << kExpected[object]);
        CHECK(std::string{kInput514[best].label} == std::string{kExpected[object]});
    }
}

TEST_CASE("atmos-cbi infers the layout from the file's channel count", "[cli][atmos-cbi]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "cbi_infer_in.wav";
    const auto out_path = dir / "cbi_infer_out.ec3";
    const auto log = dir / "cbi_infer.log";
    fs::remove(out_path);

    constexpr std::uint32_t kSampleRate = 48000;
    const auto channels = make_cbi_channels(kInput514, 2 * static_cast<std::size_t>(ac3::kSamplesPerFrame), kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    // No [layout] argument at all - the 10-channel file should resolve to
    // 5.1.4 on its own.
    const auto rc =
        run_cli("atmos-cbi \"" + wav_path.string() + "\" \"" + out_path.string() + "\" 448", log);
    CHECK(rc == 0);
    REQUIRE(fs::exists(out_path));

    const auto data = read_bytes(out_path);
    const auto units = ac3::split_access_units(data);
    REQUIRE(units.has_value());
    REQUIRE_FALSE(units->empty());

    ac3::Eac3Decoder decoder;
    const auto decoded = decoder.decode_access_unit(units->front());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());
    REQUIRE((*decoded)->object_metadata.has_value());
    CHECK((*decoded)->object_metadata->program.dynamic_objects == 0);
    CHECK(ac3::oba::object_count((*decoded)->object_metadata->program) == 10);
}

TEST_CASE("atmos-cbi refuses a layout whose channel count does not match the file",
          "[cli][atmos-cbi]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "cbi_mismatch_in.wav";
    const auto out_path = dir / "cbi_mismatch_out.ec3";
    const auto log = dir / "cbi_mismatch.log";
    fs::remove(out_path);

    constexpr std::uint32_t kSampleRate = 48000;
    const auto channels = make_cbi_channels(kInput514, static_cast<std::size_t>(ac3::kSamplesPerFrame), kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    // 7.1.4 is a 12-channel bed; the file has 10.
    const auto rc = run_cli("atmos-cbi \"" + wav_path.string() + "\" \"" + out_path.string() +
                                "\" 448 7.1.4",
                            log);
    CHECK(rc != 0);
    CHECK_FALSE(fs::exists(out_path));
    CHECK(read_log(log).find("channel(s)") != std::string::npos);
}

TEST_CASE("atmos-cbi refuses an unrecognized channel count with no layout given",
          "[cli][atmos-cbi]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "cbi_unrecognized_in.wav";
    const auto out_path = dir / "cbi_unrecognized_out.ec3";
    const auto log = dir / "cbi_unrecognized.log";
    fs::remove(out_path);

    constexpr std::uint32_t kSampleRate = 48000;
    // 8 channels: none of 10 (5.1.4), 12 (7.1.4) or 16 (9.1.6).
    const auto channels =
        make_cbi_channels(std::span{kInput514}.first(8), static_cast<std::size_t>(ac3::kSamplesPerFrame), kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    const auto rc =
        run_cli("atmos-cbi \"" + wav_path.string() + "\" \"" + out_path.string() + "\" 448", log);
    CHECK(rc != 0);
    CHECK_FALSE(fs::exists(out_path));
    CHECK(read_log(log).find("layout=") != std::string::npos);
}

TEST_CASE("atmos-cbi refuses src=/map=", "[cli][atmos-cbi]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "cbi_srcmap_in.wav";
    const auto out_path = dir / "cbi_srcmap_out.ec3";
    const auto log = dir / "cbi_srcmap.log";
    fs::remove(out_path);

    constexpr std::uint32_t kSampleRate = 48000;
    const auto channels = make_cbi_channels(kInput514, static_cast<std::size_t>(ac3::kSamplesPerFrame), kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    const auto rc = run_cli("atmos-cbi \"" + wav_path.string() + "\" \"" + out_path.string() +
                                "\" 448 5.1.4 src=" + wav_path.string(),
                            log);
    CHECK(rc != 0);
    CHECK_FALSE(fs::exists(out_path));
    CHECK(read_log(log).find("src=") != std::string::npos);
}
