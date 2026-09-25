// ac3cli decode's AC-4 options (planning/ac4.md, phase D8), each run against
// the real binary on committed streams: every DRC decoder mode at an output
// level, the presentation by position, associated service and level, the
// associated mix, every downmix with and without the LFE, a 7.X stream folded
// to 5.X, headphones, the syntax trace, and what decode says about options the
// other format reads. tests/cli/test_cli_containers.cpp has the first of them
// (output-level=, presentation-id=, language=, dialogue-gain=,
// dialogue-enhancement=, channels=2 and 1, downmix=loro, conceal=).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

#include "platform/process.hpp"

#include "ac3/io/wav.hpp"

namespace fs = std::filesystem;

namespace {

// Per this project's per-file test-helper convention (see
// tests/cli/test_cli_containers.cpp, whose shapes these copy).
fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} /
               ("cli_ac4_decode_" + ac3::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return ac3::test::platform::run_shell(command);
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::string quoted(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

fs::path leg(const std::string& name, const std::string& file = "dee.ac4") {
    return fs::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / name / file;
}

fs::path multiplexed() {
    return fs::path{AC4DEC_GOLDEN_DIR} / "presentations" / "presentations-5_1.ac4";
}

double energy(const std::vector<float>& x) {
    double sum = 0.0;
    for (const float v : x) {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    return sum;
}

// Decodes `in` with `options` into a WAV and reads it back.
ac3::io::WavData decode(const fs::path& in, const std::string& options, const fs::path& log) {
    const auto wav = scratch_dir() / "ac4_option.wav";
    fs::remove(wav);
    INFO(options);
    REQUIRE(run_cli("decode " + quoted(in) + " " + quoted(wav) + " " + options, log) == 0);
    auto decoded = ac3::io::read_wav(wav.string());
    REQUIRE(decoded.has_value());
    return std::move(*decoded);
}

}  // namespace

TEST_CASE("decode takes AC-4 at an output level in each DRC decoder mode", "[cli][ac4]") {
    const auto log = scratch_dir() / "ac4_drc_modes.log";
    // DEE's 5.1 film leg configures all four modes of Table 161.
    const fs::path stream = leg("ac4-51-film-96");
    const auto off = decode(stream, "output-level=-10 drcmode=off", log);
    CHECK(read_log(log).find("DRC off") != std::string::npos);
    for (const std::string mode :
         {"default", "home-theatre", "flat-panel-tv", "portable-speakers", "portable-headphones"}) {
        CAPTURE(mode);
        const auto compressed = decode(stream, "output-level=-10 drcmode=" + mode, log);
        CHECK(read_log(log).find("dialnorm to -10 dBFS, DRC") != std::string::npos);
        REQUIRE(compressed.channels.size() == off.channels.size());
        // At -10 dBFS every mode's curve cuts the loud passages the level
        // boosts: the output differs from the level alone.
        CHECK(compressed.channels[0] != off.channels[0]);
    }
}

TEST_CASE("decode chooses AC-4's presentation by position associated service and level",
          "[cli][ac4]") {
    const auto log = scratch_dir() / "ac4_presentation_choice.log";
    // tests/ac4dec/test_ac4dec_presentations.cpp's presentations-5_1: index 2
    // is music and effects with English dialogue and audio description (id 3),
    // 4 main with audio description at 0 degrees (id 5), 10 the music and
    // effects alone (id 20, md_compat 1); the rest before 10 have md_compat 2.
    const fs::path stream = multiplexed();
    (void)decode(stream, "associated=audio-description", log);
    CHECK(read_log(log).find("presentation 2 (presentation_id 3)") != std::string::npos);
    (void)decode(stream, "presentation=4", log);
    CHECK(read_log(log).find("presentation 4 (presentation_id 5)") != std::string::npos);
    (void)decode(stream, "md-compat=1", log);
    CHECK(read_log(log).find("presentation 10 (presentation_id 20)") != std::string::npos);
    // At level 0 no presentation of the stream but those of md_compat 0 is
    // selected: the dialogue and associated substreams alone.
    (void)decode(stream, "md-compat=0", log);
    CHECK(read_log(log).find("presentation 11 (presentation_id 21)") != std::string::npos);
}

TEST_CASE("decode mixes AC-4's associated audio at associated-gain=", "[cli][ac4]") {
    const auto log = scratch_dir() / "ac4_associated_gain.log";
    // Presentation 4 puts the audio description at 0 degrees, into C.
    const auto mixed = decode(multiplexed(), "presentation=4", log);
    const auto quiet = decode(multiplexed(), "presentation=4 associated-gain=-130", log);
    CHECK(read_log(log).find("associated audio at -130 dB") != std::string::npos);
    REQUIRE(mixed.channels.size() == 6);
    REQUIRE(quiet.channels.size() == 6);
    CHECK(energy(quiet.channels[2]) < energy(mixed.channels[2]));
    CHECK(energy(quiet.channels[0]) == energy(mixed.channels[0]));
}

TEST_CASE("decode folds AC-4 by each downmix with the LFE and without it", "[cli][ac4]") {
    const auto log = scratch_dir() / "ac4_downmixes.log";
    const fs::path stream = leg("ac4-51-tones-384");
    struct Case {
        const char* options;
        std::size_t channels;
        const char* status;
    };
    constexpr std::array<Case, 4> kCases{{
        {"downmix=ltrt", 2, "downmixed to Lt/Rt"},
        {"downmix=mono", 1, "downmixed to mono"},
        {"downmix=auto", 2, "downmixed to stereo"},
        {"downmix=loro mix-lfe=on", 2, "downmixed to Lo/Ro"},
    }};
    for (const Case& c : kCases) {
        CAPTURE(c.options);
        const auto decoded = decode(stream, c.options, log);
        CHECK(decoded.channels.size() == c.channels);
        CHECK(read_log(log).find(c.status) != std::string::npos);
    }
    // DEE's 5.1 streams send no lfe_mixgain, which leaves the LFE out of a
    // downmix whatever mix-lfe= says. A stream of the encoder's with a 60 Hz
    // tone in the LFE alone, sent at -4.5 dB: the fold takes it into L and R,
    // and mix-lfe=off leaves it out.
    const auto dir = scratch_dir();
    constexpr std::size_t kLength = 96000;
    std::vector<std::vector<float>> input(6, std::vector<float>(kLength, 0.0F));
    for (std::size_t n = 0; n < kLength; ++n) {
        input[3][n] = static_cast<float>(
            0.3 * std::sin(2.0 * std::numbers::pi * 60.0 * static_cast<double>(n) / 48000.0));
    }
    const auto wav_in = dir / "ac4_lfe_in.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_in.string(), input, 48000).has_value());
    const auto lfe_stream = dir / "ac4_lfe.ac4";
    REQUIRE(run_cli("ac4-encode " + quoted(wav_in) + " " + quoted(lfe_stream) + " 384 lfemix=-4.5",
                    log) == 0);
    const auto with = decode(lfe_stream, "downmix=loro", log);
    const auto without = decode(lfe_stream, "downmix=loro mix-lfe=off", log);
    CHECK(read_log(log).find("downmixed to Lo/Ro without the LFE") != std::string::npos);
    REQUIRE(with.channels.size() == 2);
    REQUIRE(without.channels.size() == 2);
    CHECK(energy(with.channels[0]) > 0.0);
    CHECK(energy(without.channels[0]) < energy(with.channels[0]) * 1e-6);
}

TEST_CASE("decode folds an AC-4 7.X stream to 5.X with channels=5.1", "[cli][ac4]") {
    const auto log = scratch_dir() / "ac4_fold_5x.log";
    const fs::path seven_one =
        fs::path{AC4DEC_GOLDEN_DIR} / "constructed" / "7_1-340-simple-config0-2ch1-sap.ac4";
    const auto coded = decode(seven_one, "", log);
    CHECK(coded.channels.size() == 8);
    const auto folded = decode(seven_one, "channels=5.1", log);
    CHECK(folded.channels.size() == 6);
    CHECK(read_log(log).find("(L R C LFE Ls Rs, 48000 Hz)") != std::string::npos);
    CHECK(read_log(log).find("downmixed to 5.X") != std::string::npos);
    const fs::path seven_zero =
        fs::path{AC4DEC_GOLDEN_DIR} / "constructed" / "7_0-322-aspx-config0.ac4";
    CHECK(decode(seven_zero, "channels=5.1", log).channels.size() == 5);
}

TEST_CASE("decode takes headphones for AC-4 and writes its syntax trace", "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_headphones_trace.log";
    (void)decode(leg("ac4-20-music-192"), "output-level=-10 headphones", log);
    CHECK(read_log(log).find("for headphones") != std::string::npos);

    const auto trace = dir / "ac4_trace.tsv";
    fs::remove(trace);
    (void)decode(leg("ac4-stereo-64"), "syntax-trace=" + quoted(trace), log);
    std::ifstream in{trace};
    std::string first;
    REQUIRE(std::getline(in, first));
    // frame, substream, bit offset, width, value and name, tab-separated.
    CHECK(std::count(first.begin(), first.end(), '\t') == 5);
    CHECK(first.starts_with("0\t"));
    std::size_t lines = 1;
    for (std::string line; std::getline(in, line);) {
        ++lines;
    }
    CHECK(lines > 1000);
}

TEST_CASE("decode names the options the other format reads and refuses what it cannot do",
          "[cli][ac4]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_other_format.log";
    const auto wav = dir / "ac4_other_format.wav";
    const fs::path ac4_stream = leg("ac4-stereo-64");
    const fs::path eac3_stream = leg("eac3-stereo-64", "ffmpeg.ec3");
    REQUIRE(
        run_cli("decode " + quoted(ac4_stream) + " " + quoted(wav) + " heavy drc=0.5 programme=1",
                log) == 0);
    CHECK(read_log(log).find("warning:") != std::string::npos);
    CHECK(read_log(log).find("heavy drc=0.5 programme=1 are AC-3's and E-AC-3's, and ignored") !=
          std::string::npos);
    CHECK(run_cli("decode " + quoted(ac4_stream) + " " + quoted(wav) +
                      " bap-census=" + quoted(dir / "census.json"),
                  log) == 1);
    CHECK(read_log(log).find("bap-census= counts the bit allocation") != std::string::npos);
    CHECK(run_cli("decode " + quoted(ac4_stream) + " " + quoted(wav) + " verify-objects", log) ==
          1);

    REQUIRE(run_cli("decode " + quoted(eac3_stream) + " " + quoted(wav) +
                        " output-level=-20 language=en",
                    log) == 0);
    CHECK(read_log(log).find("output-level=-20 language=en are AC-4's, and ignored") !=
          std::string::npos);
    CHECK(run_cli("decode " + quoted(eac3_stream) + " " + quoted(wav) + " channels=5.1", log) == 1);
    CHECK(read_log(log).find("channels=5.1 folds an AC-4 7.X stream") != std::string::npos);
}
