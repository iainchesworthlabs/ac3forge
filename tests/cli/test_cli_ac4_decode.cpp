// ac3cli decode's AC-4 options (planning/ac4.md, phase D8), each run against
// the real binary on committed streams: every DRC decoder mode at an output
// level, the presentation by position, associated service and level, the
// associated mix, every downmix with and without the LFE, a 7.X stream folded
// to 5.X, headphones, the syntax trace, and what decode says about options the
// other format reads; and phase D10's objects, rendered to speakers.
// tests/cli/test_cli_containers.cpp has the first of them (output-level=, presentation-id=,
// language=, dialogue-gain=, dialogue-enhancement=, channels=2 and 1, downmix=loro, conceal=).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "platform/process.hpp"
#include "sanitized.hpp"

#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/ac4/ac4.hpp"

namespace fs = std::filesystem;
using iclforge::test::kSanitized;

namespace {

// Per this project's per-file test-helper convention (see
// tests/cli/test_cli_containers.cpp, whose shapes these copy).
fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_ac4_decode_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::string quoted(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

fs::path leg(const std::string& name, const std::string& file = "dee.ac4") {
    return fs::path{ICLFORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / name / file;
}

fs::path multiplexed() {
    return fs::path{AC4DEC_GOLDEN_DIR} / "presentations" / "presentations-5_1.ac4";
}

// The frames of a stream that a decode takes under the sanitizers, in place of
// the 120 of one of DEE's legs.
constexpr std::size_t kSanitizedFrames = 24;

// A committed AC-4 stream as a decode test takes it (the shape of
// tests/cli/test_cli_containers.cpp's): whole, or under the sanitizers
// (tests/sanitized.hpp) its first `frames` sync frames, written to `prefix`.
fs::path decoded_stream(const fs::path& stream, std::size_t frames, const fs::path& prefix) {
    if (!kSanitized) {
        return stream;
    }
    std::ifstream in{stream, std::ios::binary};
    REQUIRE(in.good());
    const std::vector<char> chars{std::istreambuf_iterator<char>{in},
                                  std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(chars.size());
    std::ranges::transform(chars, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    const iclforge::ac4::ScanResult scan = iclforge::ac4::scan(bytes);
    REQUIRE(scan.frames.size() > frames);
    std::ofstream out{prefix, std::ios::binary};
    REQUIRE(out.is_open());
    out.write(chars.data(), static_cast<std::streamsize>(scan.frames[frames].offset));
    return prefix;
}

double energy(const std::vector<float>& x) {
    double sum = 0.0;
    for (const float v : x) {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    return sum;
}

// Decodes `in` with `options` into a WAV and reads it back.
iclforge::io::WavData decode(const fs::path& in, const std::string& options, const fs::path& log) {
    const auto wav = scratch_dir() / "ac4_option.wav";
    fs::remove(wav);
    INFO(options);
    REQUIRE(run_cli("decode " + quoted(in) + " " + quoted(wav) + " " + options, log) == 0);
    auto decoded = iclforge::io::read_wav(wav.string());
    REQUIRE(decoded.has_value());
    return std::move(*decoded);
}

}  // namespace

TEST_CASE("decode takes AC-4 at an output level in each DRC decoder mode", "[cli][ac4]") {
    const auto log = scratch_dir() / "ac4_drc_modes.log";
    // DEE's 5.1 film leg configures all four modes of Table 161, each on the
    // stream's default profile, so the modes decode alike and each differs from
    // off. Under the sanitizers the leg's first frames show that, for `off`, the
    // automatic choice and one explicit mode, the first and the last of the five.
    const fs::path stream = decoded_stream(leg("ac4-51-film-96"), kSanitizedFrames,
                                           scratch_dir() / "ac4_drc_prefix.ac4");
    const auto off = decode(stream, "output-level=-10 drcmode=off", log);
    CHECK(read_log(log).find("DRC off") != std::string::npos);
    constexpr std::array<const char*, 5> kModes = {"default", "home-theatre", "flat-panel-tv",
                                                   "portable-speakers", "portable-headphones"};
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        if (kSanitized && i != 0 && i + 1 != kModes.size()) {
            continue;
        }
        const std::string mode = kModes[i];
        CAPTURE(mode);
        const auto compressed = decode(stream, "output-level=-10 drcmode=" + mode, log);
        CHECK(read_log(log).find("dialnorm to -10 dBFS, DRC") != std::string::npos);
        REQUIRE(compressed.channels.size() == off.channels.size());
        // At -10 dBFS every mode's curve cuts the loud passages the level
        // boosts: the output differs from the level alone.
        CHECK(compressed.channels[0] != off.channels[0]);
    }
}

TEST_CASE("decode compresses AC-4 alike on one DRC profile and differently on a curve of its own",
          "[cli][ac4]") {
    const auto log = scratch_dir() / "ac4_drc_curves.log";
    // The one committed leg whose modes differ: DEE's ac4-51-drc-ltrt-192 sends
    // home theatre and portable headphones a curve each (music light and
    // speech), and flat panel TV and portable speakers the stream's default
    // profile (tools/generators/gen_ac4_baseline.py). The film leg above sends
    // every mode the default profile, so it cannot show a mode's curve at all.
    // Under the sanitizers the leg's first frames, decoded in `off`, home
    // theatre and portable headphones alone: the curves set the outputs apart
    // within 4 frames.
    constexpr std::size_t kSanitizedCurveFrames = 12;
    const fs::path stream = decoded_stream(leg("ac4-51-drc-ltrt-192"), kSanitizedCurveFrames,
                                           scratch_dir() / "ac4_drc_curves_prefix.ac4");
    const auto in_mode = [&](const std::string& mode) {
        return decode(stream, "output-level=-10 drcmode=" + mode, log);
    };
    const auto off = in_mode("off");
    const auto home_theatre = in_mode("home-theatre");
    const auto headphones = in_mode("portable-headphones");
    REQUIRE(home_theatre.channels.size() == off.channels.size());
    CHECK(home_theatre.channels != off.channels);
    CHECK(headphones.channels != off.channels);
    // Two curves, two outputs.
    CHECK(home_theatre.channels != headphones.channels);
    if (!kSanitized) {
        // Flat panel TV and portable speakers are on the default profile: one
        // output, which differs from off and from each curve of the stream's own.
        const auto flat_panel = in_mode("flat-panel-tv");
        const auto speakers = in_mode("portable-speakers");
        CHECK(flat_panel.channels != off.channels);
        CHECK(flat_panel.channels != home_theatre.channels);
        CHECK(flat_panel.channels != headphones.channels);
        CHECK(speakers.channels == flat_panel.channels);
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
    REQUIRE(iclforge::io::write_wav_f32(wav_in.string(), input, 48000).has_value());
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

TEST_CASE("decode renders AC-4's objects to the speakers the layout options name", "[cli][ac4]") {
    // A presentation of objects comes out through the layout renderer, 7.1.4
    // without a layout option; an intermediate spatial format comes out in the
    // channels the decoder rendered it to.
    const auto dir = scratch_dir();
    const auto log = dir / "ac4_objects.log";
    const fs::path objects = fs::path{AC4DEC_GOLDEN_DIR} / "objects";
    const auto rendered = decode(objects / "direct-dynamic.ac4", "", log);
    CHECK(rendered.channels.size() == 12);
    CHECK(read_log(log).find("(L R C LFE Lb Rb Ls Rs Tfl Tfr Tbl Tbr, 48000 Hz)") !=
          std::string::npos);
    CHECK(read_log(log).find("5 objects, rendered to those speakers by the layout renderer") !=
          std::string::npos);
    CHECK(std::ranges::count_if(rendered.channels,
                                [](const std::vector<float>& x) { return energy(x) > 0.0; }) >= 5);
    CHECK(decode(objects / "direct-dynamic.ac4", "speakers=5.1", log).channels.size() == 6);
    CHECK(decode(objects / "direct-dynamic.ac4", "channels=2", log).channels.size() == 2);
    CHECK(decode(objects / "ajoc-2to4-coarse.ac4", "decoding=core", log).channels.size() == 12);
    CHECK(read_log(log).find("2 objects, rendered") != std::string::npos);
    const auto isf = decode(objects / "direct-isf-sr3100.ac4", "", log);
    CHECK(isf.channels.size() == 11);
    CHECK(read_log(log).find("objects, rendered") == std::string::npos);
    // decode's third argument, the directory D10's own decoded objects are written to
    // (planning/ac4.md, I5: objects_dir is no longer E-AC-3-Atmos-only).
    const auto object_dir = dir / "objects";
    REQUIRE(run_cli("decode " + quoted(objects / "direct-bed-5_1.ac4") + " " +
                        quoted(dir / "bed.wav") + " " + quoted(object_dir),
                    log) == 0);
    CHECK(read_log(log).find("also written to") != std::string::npos);
    REQUIRE(fs::exists(object_dir));
    std::size_t object_files = 0;
    for (const auto& entry : fs::directory_iterator(object_dir)) {
        object_files += entry.path().extension() == ".wav" ? 1U : 0U;
    }
    CHECK(object_files > 0);
}
