#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"

#include "ac3/io/wav.hpp"

// The WAV-driven object encoders (apps/cli/commands/atmos.cpp: atmos-encode,
// its src=/map= form, and atmos-cbi) on the requests they refuse and the
// report lines only some requests produce. test_cli.cpp and
// test_cli_atmos_cbi.cpp hold the main round trips; this file holds the
// edges.
//
// run_cli and the helpers below are trimmed copies of test_cli.cpp's own,
// duplicated per this project's per-file test-helper convention - including
// the Windows double-quote wrapping explained in test_cli.cpp's run_cli.

namespace fs = std::filesystem;

namespace {

// See tests/cli/test_cli.cpp's own scratch_dir for the reasoning this copy
// shares, including the PID fold; the leaf name below is this file's own.
std::string scratch_pid_suffix() {
    return ac3::test::platform::process_id();
}

fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("cli_atmos_edges_" + scratch_pid_suffix());
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

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

// `channels` channels, one tone each, 0.1 s long.
fs::path tone_wav(const fs::path& path, std::size_t channels) {
    if (!fs::exists(path)) {
        constexpr std::uint32_t kRate = 48000;
        constexpr std::size_t kFrames = 4800;
        std::vector<std::vector<float>> data(channels, std::vector<float>(kFrames));
        for (std::size_t c = 0; c < channels; ++c) {
            for (std::size_t n = 0; n < kFrames; ++n) {
                data[c][n] = static_cast<float>(
                    0.2 * std::sin(2.0 * std::numbers::pi * (301.0 + 97.0 * static_cast<double>(c)) *
                                   static_cast<double>(n) / kRate));
            }
        }
        REQUIRE(ac3::io::write_wav_f32(path.string(), data, kRate).has_value());
    }
    return path;
}

struct Expectation {
    std::string args;
    int exit_code;
    std::string message;
};

void check_rows(std::initializer_list<Expectation> rows, const fs::path& out_path,
                const fs::path& log) {
    for (const auto& row : rows) {
        CAPTURE(row.args);
        fs::remove(out_path);
        const auto rc = run_cli(row.args, log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == row.exit_code);
        CHECK(text.find(row.message) != std::string::npos);
        CHECK_FALSE(fs::exists(out_path));
    }
}

}  // namespace

TEST_CASE("atmos-encode refuses an object count it cannot carry and a loudness it cannot measure",
          "[cli][atmos]") {
    const auto dir = scratch_dir();
    const auto stereo = tone_wav(dir / "enc_stereo.wav", 2);
    const auto wide = tone_wav(dir / "enc_16ch.wav", 16);
    const auto octo = tone_wav(dir / "enc_8ch.wav", 8);
    const auto missing = dir / "enc_missing.wav";
    fs::remove(missing);
    const auto out_path = dir / "enc_refused.ec3";
    const auto log = dir / "enc_refused.log";
    const auto run = [&](const fs::path& in, std::string_view rest) {
        return "atmos-encode " + quoted(in) + " " + quoted(out_path) + " 448 " + std::string{rest};
    };
    check_rows(
        {{run(wide, ""), 1,
          "error: 1 to 15 objects (the bed's LFE is the 16th, and TS 103 420 \xC2\xA7" "8.3.2.2 "
          "caps the total at 16); this file has 16 channels"},
         {run(wide, "src=" + quoted(stereo)), 1,
          "error: 1 to 15 objects (the bed's LFE is the 16th, and TS 103 420 8.3.2.2 caps the "
          "total at 16); this map= resolves 18"},
         // Eight channels have no AC-3 layout to take BS.1770 weights from.
         {run(octo, "dialnorm=auto"), 5,
          "error: cannot measure loudness for this file; pass dialnorm=<1..31> explicitly"},
         {run(missing, "src=" + quoted(stereo)), 2,
          "error: " + missing.string() + ": cannot open file"},
         {run(stereo, "src=" + quoted(stereo) + " map=0.0:L,0.1:R,1.0:none,1.1:none"), 1,
          "error: map= names no obj/objm destination, so this encode would carry no objects at "
          "all - 'eac3-encode' is the command for a purely channel-mapped programme"}},
        out_path, log);
}

TEST_CASE("verbose atmos-encode with map= lists what feeds each object and shows progress",
          "[cli][atmos]") {
    const auto dir = scratch_dir();
    const auto stereo = tone_wav(dir / "verbose_stereo.wav", 2);
    const auto out_path = dir / "verbose.ec3";
    const auto log = dir / "verbose.log";
    // Source 0's two channels fold into one mono object at half each; source
    // 1's first channel is an object of its own 3 dB down.
    REQUIRE(run_cli("atmos-encode " + quoted(stereo) + " " + quoted(out_path) + " 448 src=" +
                        quoted(stereo) + " map=0.0-1:objm,1.0:obj@-3,1.1:none verbose",
                    log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("  2 sources, 4 channels -> 2 objects (map= order)") != std::string::npos);
    CHECK(text.find("    object 0: ch2 x0.708") != std::string::npos);
    CHECK(text.find("    object 1: ch0 x0.500 + ch1 x0.500") != std::string::npos);
    CHECK(text.find("encoding        4 / 4 units (100%)") != std::string::npos);
    CHECK(text.find("  2 objects + the bed's LFE = 3 objects, JOC over a 5.1 downmix") !=
          std::string::npos);
}

TEST_CASE("atmos-encode's single-file map= keeps the scene file's object order",
          "[cli][atmos]") {
    const auto dir = scratch_dir();
    const auto stereo = tone_wav(dir / "scene_map_stereo.wav", 2);
    const auto scene = dir / "scene_map.txt";
    std::ofstream{scene} << "0 0 0.1 0.2 0 1 0\n1 0 0.9 0.2 0 1 0\n";
    const auto out_path = dir / "scene_map.ec3";
    const auto log = dir / "scene_map.log";
    REQUIRE(run_cli("atmos-encode " + quoted(stereo) + " " + quoted(out_path) + " 448 0 " +
                        quoted(scene) + " map=0.0:obj,0.1:obj",
                    log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("  1 sources, 2 channels -> 2 objects (map= order)") != std::string::npos);
    CHECK(text.find("  2 objects + the bed's LFE = 3 objects") != std::string::npos);
}

TEST_CASE("atmos-cbi reads a bed from stdin and refuses a rate or output it cannot use",
          "[cli][atmos][cbi]") {
    const auto dir = scratch_dir();
    const auto bed = tone_wav(dir / "cbi_10ch.wav", 10);
    const auto wider = tone_wav(dir / "cbi_12ch.wav", 12);
    const auto log = dir / "cbi_edges.log";

    const auto piped = dir / "cbi_stdin.ec3";
    REQUIRE(run_cli("atmos-cbi - " + quoted(piped) + " 768 5.1.4 < " + quoted(bed), log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("encoded 4 E-AC-3 access units (768 kbps, 48000 Hz) from - to " +
                    piped.string()) != std::string::npos);
    CHECK(text.find("10-channel channel-based-immersive bed, 0 dynamic objects -> 10 objects "
                    "total") != std::string::npos);

    const auto out_path = dir / "cbi_refused.ec3";
    const auto nowhere = dir / "no_such_directory" / "cbi.ec3";
    check_rows({{"atmos-cbi " + quoted(wider) + " " + quoted(out_path) + " 32", 1,
                 "error: cannot encode a 12-channel bed at 32 kbps - the metadata and the "
                 "mantissas share one frame, so try a higher bit rate"}},
               out_path, log);
    check_rows({{"atmos-cbi " + quoted(bed) + " " + quoted(nowhere) + " 768", 3,
                 "error: cannot open " + nowhere.string() + " for writing"}},
               nowhere, log);
}
