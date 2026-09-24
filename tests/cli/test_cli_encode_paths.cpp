#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

#include "ac3/decoder/decoder.hpp"
#include "ac3/io/wav.hpp"

// The encode front ends (apps/cli/commands/encode.cpp: encode, eac3-encode,
// their src=/map= multi-source twins in apps/cli/multi_source.cpp, and
// programmeN='s extra programmes) along the paths a user takes when
// something about the input or the request is wrong: every refusal's exit
// code and reason, and - for the requests that should work - what the
// encoder reports and writes. test_cli.cpp holds the main encode contracts;
// this file holds the edges of the same commands.
//
// src=/map= and the single-file path are separate implementations
// (run_encode vs run_encode_multi, and the same pair for E-AC-3), so a
// refusal that both share is asked of both.
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
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("cli_encode_paths_" + scratch_pid_suffix());
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

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i] = static_cast<std::byte>(raw[i]);
    }
    return out;
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

// A tone per channel (a different pitch on each, 0.3 amplitude), except the
// channels listed in `silent`, which carry digital silence. 0.5 s by default:
// longer than BS.1770's 400 ms gating block, so dialnorm=auto has something
// to measure, and still only 16 AC-3 frames to encode.
fs::path write_wav(const fs::path& path, std::size_t channels, std::uint32_t rate = 48000,
                   std::initializer_list<std::size_t> silent = {}, double seconds = 0.5) {
    const auto frames = static_cast<std::size_t>(seconds * rate);
    std::vector<std::vector<float>> data(channels, std::vector<float>(frames));
    for (std::size_t c = 0; c < channels; ++c) {
        if (std::find(silent.begin(), silent.end(), c) != silent.end()) {
            continue;
        }
        const double hz = 301.0 + 97.0 * static_cast<double>(c);
        for (std::size_t n = 0; n < frames; ++n) {
            data[c][n] = static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * hz *
                                                           static_cast<double>(n) / rate));
        }
    }
    REQUIRE(ac3::io::write_wav_f32(path.string(), data, rate).has_value());
    return path;
}

struct Expectation {
    std::string args;
    int exit_code;
    std::string message;
};

// Runs each row, holding it to its exit code and message and to leaving no
// output behind at `out_path` (which every row's own args name).
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

// The number `ac3cli` printed straight after `label`, or -1.
long number_after(const std::string& text, std::string_view label) {
    const auto at = text.find(label);
    if (at == std::string::npos) {
        return -1;
    }
    return std::strtol(text.c_str() + at + label.size(), nullptr, 10);
}

// A two-source map= that keeps source 0 as stereo and drops source 1.
constexpr std::string_view kStereoMap = "map=0.0:L,0.1:R,1.0:none,1.1:none";

}  // namespace

TEST_CASE("eac3-encode refuses an unknown tool set and vbr setting on both input paths",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "tools_stereo.wav", 2);
    const auto out_path = dir / "tools.ec3";
    const auto log = dir / "tools.log";
    const std::string head = "eac3-encode " + quoted(wav) + " " + quoted(out_path) + " 192 ";
    const std::string multi = " src=" + quoted(wav) + " " + std::string{kStereoMap};
    check_rows({{head + "bogus stereo", 1, "error: unknown tool set 'bogus' (none | auto | cpl"},
                {head + "bogus stereo" + multi, 1, "error: unknown tool set 'bogus'"},
                {head + "none stereo wild", 1,
                 "error: unrecognised vbr setting 'wild' (off | q:0..1[,min:kbps][,max:kbps] | "
                 "avg:kbps"},
                {head + "none stereo wild" + multi, 1, "error: unrecognised vbr setting 'wild'"},
                {head + "none bogus" + multi, 1,
                 "error: unknown layout 'bogus' (mono | stereo | 1+1 | 51 | 71 | 512 | 514 | 714)"}},
               out_path, log);
}

TEST_CASE("encode and eac3-encode refuse an unwritable output on both input paths",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "unwritable_stereo.wav", 2);
    const auto nowhere = dir / "no_such_directory" / "out.bin";
    const auto log = dir / "unwritable.log";
    const std::string multi = " src=" + quoted(wav) + " " + std::string{kStereoMap};
    const std::string message = "error: cannot open " + nowhere.string() + " for writing";
    check_rows({{"encode " + quoted(wav) + " " + quoted(nowhere) + " 192", 3, message},
                {"encode " + quoted(wav) + " " + quoted(nowhere) + " 192 stereo" + multi, 3,
                 message},
                {"eac3-encode " + quoted(wav) + " " + quoted(nowhere) + " 192", 3, message},
                {"eac3-encode " + quoted(wav) + " " + quoted(nowhere) + " 192 none stereo" + multi,
                 3, message}},
               nowhere, log);
}

TEST_CASE("a VBR E-AC-3 encode reports the access-unit sizes it really wrote, on both input paths",
          "[cli][encode][vbr]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "vbr_stereo.wav", 2);
    const auto log = dir / "vbr.log";
    for (const std::string& extra : {std::string{}, " src=" + quoted(wav) + " " +
                                                       std::string{kStereoMap}}) {
        CAPTURE(extra);
        const auto out_path = dir / "vbr.ec3";
        fs::remove(out_path);
        REQUIRE(run_cli("eac3-encode " + quoted(wav) + " " + quoted(out_path) +
                            " 192 none stereo q:0.5" + extra,
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("encoded 16 E-AC-3 access units (vbr q:0.500000, 48000 Hz, 2/0 stereo, "
                        "2 coded channels, tools: none)") != std::string::npos);
        const auto min_bytes = number_after(text, "access unit size: ");
        const auto max_bytes = number_after(text, std::to_string(min_bytes) + "-");
        const auto bytes = read_bytes(out_path);
        const auto frames = ac3::split_frames(bytes);
        REQUIRE(frames.has_value());
        REQUIRE(frames->size() == 16u);
        const auto [smallest, largest] = std::ranges::minmax_element(
            *frames, {}, [](const auto& frame) { return frame.size(); });
        CHECK(min_bytes == static_cast<long>(smallest->size()));
        CHECK(max_bytes == static_cast<long>(largest->size()));
        // Variable rate means the units really do differ in size.
        CHECK(min_bytes < max_bytes);
    }
}

TEST_CASE("verify on a src=/map= E-AC-3 encode checks every access unit against the decoder",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "verify_multi.wav", 2);
    const auto out_path = dir / "verify_multi.ec3";
    const auto log = dir / "verify_multi.log";
    REQUIRE(run_cli("eac3-encode " + quoted(wav) + " " + quoted(out_path) +
                        " 192 none stereo off verify src=" + quoted(wav) + " " +
                        std::string{kStereoMap},
                    log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("  verify: encoder and decoder agree on all 16 access units") !=
          std::string::npos);
    CHECK(text.find("  4 source channels rendered onto 2/0 stereo") != std::string::npos);
}

TEST_CASE("a src=/map= encode with no layout named follows the total source channel count",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "auto_layout.wav", 2);
    const auto log = dir / "auto_layout.log";
    // Two stereo files are four channels, which no layout carries exactly -
    // layout_for_source picks the smallest that fits, 5.1, and the two
    // dropped channels leave the rest of it silent.
    for (const std::string_view command : {"encode", "eac3-encode"}) {
        CAPTURE(command);
        const auto out_path = dir / "auto_layout.bin";
        fs::remove(out_path);
        const std::string tools = command == "eac3-encode" ? " none" : "";
        REQUIRE(run_cli(std::string{command} + " " + quoted(wav) + " " + quoted(out_path) + " 192" +
                            tools + " src=" + quoted(wav) + " " + std::string{kStereoMap},
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("  4 source channels rendered onto 5.1") != std::string::npos);
        CHECK(text.find("  silent (the source carries nothing that belongs there): C Ls Rs LFE") !=
              std::string::npos);
    }
}

TEST_CASE("the src=/map= input path refuses sources it cannot load", "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto stereo = write_wav(dir / "multi_stereo.wav", 2);
    const auto slow = write_wav(dir / "multi_22k.wav", 2, 22050);
    const auto other_rate = write_wav(dir / "multi_44k.wav", 2, 44100);
    const auto missing = dir / "multi_missing.wav";
    fs::remove(missing);
    const auto out_path = dir / "multi_refused.bin";
    const auto log = dir / "multi_refused.log";
    const std::string map{kStereoMap};
    const auto encode = [&](const fs::path& in, std::string_view rest) {
        return "encode " + quoted(in) + " " + quoted(out_path) + " " + std::string{rest};
    };
    const auto eac3 = [&](const fs::path& in, std::string_view rest) {
        return "eac3-encode " + quoted(in) + " " + quoted(out_path) + " " + std::string{rest};
    };
    check_rows(
        {{encode(missing, "192 stereo src=" + quoted(stereo) + " " + map), 2,
          "error: " + missing.string() + ": cannot open file"},
         {eac3(stereo, "192 none stereo src=" + quoted(missing) + " " + map), 2,
          "error: " + missing.string() + ": cannot open file"},
         {encode(stereo, "192 stereo src=" + quoted(other_rate) + " " + map), 2,
          "error: " + other_rate.string() + " is 44100 Hz, but " + stereo.string() +
              " is 48000 Hz - every source must share a sample rate"},
         {encode(slow, "192 stereo src=" + quoted(slow) + " " + map), 2,
          "error: sample rate 22050 is not legal for AC-3 (need 32/44.1/48 kHz)"}},
        out_path, log);
}

TEST_CASE("the src=/map= input path refuses a map, layout or request it cannot route",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto stereo = write_wav(dir / "route_stereo.wav", 2);
    const auto wide = write_wav(dir / "route_7ch.wav", 7);
    const auto mono = write_wav(dir / "route_mono.wav", 1);
    const auto out_path = dir / "route_refused.bin";
    const auto log = dir / "route_refused.log";
    const std::string map{kStereoMap};
    const auto encode = [&](const fs::path& in, std::string_view rest) {
        return "encode " + quoted(in) + " " + quoted(out_path) + " " + std::string{rest};
    };
    const auto eac3 = [&](const fs::path& in, std::string_view rest) {
        return "eac3-encode " + quoted(in) + " " + quoted(out_path) + " " + std::string{rest};
    };
    check_rows(
        {{eac3(stereo, "192 none stereo src=" + quoted(stereo)), 1,
          "error: more than one source needs map= to say where each channel goes"},
         {encode(stereo, "192 stereo src=" + quoted(stereo) + " map=0.0:L"), 1,
          "error: bad map= spec (<source>.<channel>"},
         // Ls has nowhere to go in a stereo programme.
         {eac3(stereo, "192 none stereo src=" + quoted(stereo) +
                           " map=0.0:Ls,0.1:R,1.0:none,1.1:none"),
          1, "error: map= does not resolve to a valid routing for this format"},
         {encode(stereo, "192 bogus src=" + quoted(stereo) + " " + map), 1,
          "error: unknown layout 'bogus' (mono | stereo | 1+1 | 51)"},
         {encode(stereo, "191 stereo src=" + quoted(stereo) + " " + map), 1,
          "error: AC-3 takes only the 19 nominal rates of Table 5.18"},
         {encode(wide, "192 src=" + quoted(stereo) +
                           " map=0.0:L,0.1:R,0.2:none,0.3:none,0.4:none,0.5:none,0.6:none,"
                           "1.0:none,1.1:none"),
          1,
          "error: encode handles 1 to 6 channels (9 given); no AC-3 coding mode is wider than "
          "3/2 + LFE"},
         {encode(stereo, "192 stereo " + quoted(mono) + " src=" + quoted(stereo)), 1,
          "error: use either a second positional file or src=/map=, not both"},
         {eac3(stereo, "192 none stereo off " + quoted(mono) + " src=" + quoted(stereo)), 1,
          "error: use either a second positional file or src=/map=, not both"},
         {eac3(stereo, "192 none stereo src=" + quoted(stereo) + " programme2=" + quoted(mono)), 1,
          "error: programmeN= and src=/map= cannot be combined yet - the multi-source router "
          "assigns channels to one programme"}},
        out_path, log);
}

TEST_CASE("an AC-3 src=/map= destination the command cannot carry is warned about, not dropped "
          "silently",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "obj_warning.wav", 2);
    const auto out_path = dir / "obj_warning.ac3";
    const auto log = dir / "obj_warning.log";
    REQUIRE(run_cli("encode " + quoted(wav) + " " + quoted(out_path) + " 192 stereo src=" +
                        quoted(wav) + " map=0.0:L,0.1:R,1.0:obj,1.1:none",
                    log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("warning: 1.0 maps to 'obj', which this command has no way to carry - that "
                    "channel contributes nothing to the output") != std::string::npos);
}

TEST_CASE("offset= on a src=/map= encode delays its source and lengthens the programme",
          "[cli][encode][offset]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "offset_multi.wav", 2);
    const auto log = dir / "offset_multi.log";
    // 0.5 s is 24000 samples, 16 frames once rounded up; shifting source 1
    // by 0.05 s pushes the programme's end to 26400 samples, which is 18.
    for (const std::string_view command : {"encode", "eac3-encode"}) {
        CAPTURE(command);
        const auto out_path = dir / "offset_multi.bin";
        const std::string tools = command == "eac3-encode" ? " none" : "";
        REQUIRE(run_cli(std::string{command} + " " + quoted(wav) + " " + quoted(out_path) + " 192" +
                            tools + " stereo src=" + quoted(wav) + " " + std::string{kStereoMap} +
                            " offset=1:0.05",
                        log) == 0);
        const auto bytes = read_bytes(out_path);
        const auto frames = ac3::split_frames(bytes);
        REQUIRE(frames.has_value());
        CHECK(frames->size() == 18u);
    }
}

TEST_CASE("the single-file encode paths refuse a rate, width or configuration they cannot code",
          "[cli][encode]") {
    const auto dir = scratch_dir();
    const auto slow = write_wav(dir / "single_22k.wav", 2, 22050);
    const auto wide = write_wav(dir / "single_7ch.wav", 7);
    const auto six = write_wav(dir / "single_51.wav", 6);
    const auto mono = write_wav(dir / "single_mono.wav", 1);
    const auto out_path = dir / "single_refused.bin";
    const auto log = dir / "single_refused.log";
    const auto run = [&](std::string_view command, const fs::path& in, std::string_view rest) {
        return std::string{command} + " " + quoted(in) + " " + quoted(out_path) + " " +
               std::string{rest};
    };
    check_rows({{run("encode", slow, "192"), 2,
                 "error: sample rate 22050 is not legal for AC-3 (need 32/44.1/48 kHz)"},
                {run("encode", wide, "192"), 1,
                 "error: encode handles 1 to 6 channels (7 given); no AC-3 coding mode is wider "
                 "than 3/2 + LFE"},
                {run("eac3-encode", wide, "192"), 1,
                 "error: 7 channels - no standard speaker layout has that many channels"},
                {run("eac3-encode", wide, "192 none 51"), 1,
                 "error: 7 channels - no standard speaker layout has that many channels"},
                {run("eac3-encode", six, "32 none 51"), 1,
                 "error: the encoder cannot express this configuration"},
                // One mono file is half of a 1+1 programme, on the streaming
                // path as much as the whole-file one.
                {run("eac3-encode", mono, "192 none 1+1"), 1,
                 "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or two mono "
                 "files; the source has 1 channel(s) and no second file was given"},
                {run("encode", mono, "192 1+1"), 1,
                 "error: layout 1+1 needs either one two-channel file (Ch1, Ch2) or two mono "
                 "files; the source has 1 channel(s)"}},
               out_path, log);
}

TEST_CASE("a second positional file is refused unless it can be layout 1+1's mono Ch2",
          "[cli][encode][dual-mono]") {
    const auto dir = scratch_dir();
    const auto mono = write_wav(dir / "ch2_mono.wav", 1);
    const auto stereo = write_wav(dir / "ch2_stereo.wav", 2);
    const auto other_rate = write_wav(dir / "ch2_44k.wav", 1, 44100);
    const auto missing = dir / "ch2_missing.wav";
    fs::remove(missing);
    const auto out_path = dir / "ch2_refused.ac3";
    const auto log = dir / "ch2_refused.log";
    const auto run = [&](const fs::path& first, std::string_view layout, const fs::path& second) {
        return "encode " + quoted(first) + " " + quoted(out_path) + " 192 " + std::string{layout} +
               " " + quoted(second);
    };
    check_rows({{run(stereo, "1+1", mono), 2,
                 "error: layout 1+1 with a second input file needs the first file to be mono "
                 "(Ch1); it has 2 channels"},
                {run(mono, "1+1", missing), 2, "error: " + missing.string() + ": cannot open file"},
                {run(mono, "1+1", stereo), 2,
                 "error: " + stereo.string() + " must be mono (Ch2); it has 2 channels"},
                {run(mono, "1+1", other_rate), 2,
                 "error: " + other_rate.string() +
                     " is 44100 Hz, but the first file is 48000 Hz - both programmes must share "
                     "a sample rate"},
                {run(stereo, "stereo", mono), 2,
                 "error: a second input file is only meaningful with layout 1+1 (got layout "
                 "'stereo')"}},
               out_path, log);
}

namespace {

// dialnorm=auto against a programme with nothing to measure, on one input
// path: every combination of codec and which programme is silent, since
// each of the four encode implementations measures on its own.
void check_silent_dialnorm_refusals(bool multi, std::string_view leaf) {
    const auto dir = scratch_dir();
    const auto silent = write_wav(dir / "auto_silent.wav", 2, 48000, {0, 1});
    const auto ch2_silent = write_wav(dir / "auto_ch2_silent.wav", 2, 48000, {1});
    const auto out_path = dir / (std::string{leaf} + ".bin");
    const auto log = dir / (std::string{leaf} + ".log");
    const std::string whole = "error: no audio above the -70 LKFS absolute gate; pass "
                              "dialnorm=<1..31> explicitly";
    const std::string ch1 = "error: Ch1 has no audio above the -70 LKFS absolute gate; pass "
                            "dialnorm=<1..31> explicitly";
    const std::string ch2 = "error: Ch2 has no audio above the -70 LKFS absolute gate; pass "
                            "dialnorm2=<1..31> explicitly";
    const auto run = [&](std::string_view command, const fs::path& in, std::string_view rest) {
        std::string args = std::string{command} + " " + quoted(in) + " " + quoted(out_path) +
                           " 192 " + std::string{rest};
        if (multi) {
            args += " src=" + quoted(in) + " map=0.0:" +
                    std::string{rest.find("1+1") != std::string_view::npos ? "p1,0.1:p2"
                                                                           : "L,0.1:R"} +
                    ",1.0:none,1.1:none";
        }
        return args;
    };
    check_rows(
            {{run("encode", silent, "stereo dialnorm=auto"), 5, whole},
             {run("encode", silent, "1+1 dialnorm=auto"), 5, ch1},
             {run("encode", ch2_silent, "1+1 dialnorm2=auto"), 5, ch2},
             {run("eac3-encode", silent, "none stereo dialnorm=auto"), 5, whole},
             {run("eac3-encode", silent, "none 1+1 dialnorm=auto"), 5, ch1},
             {run("eac3-encode", ch2_silent, "none 1+1 dialnorm2=auto"), 5, ch2}},
            out_path, log);
}

}  // namespace

TEST_CASE("dialnorm=auto on a single-file programme with nothing to measure is refused, not "
          "guessed",
          "[cli][encode][dialnorm]") {
    check_silent_dialnorm_refusals(false, "auto_refused_single");
}

TEST_CASE("dialnorm=auto on a src=/map= programme with nothing to measure is refused, not guessed",
          "[cli][encode][dialnorm]") {
    check_silent_dialnorm_refusals(true, "auto_refused_multi");
}

TEST_CASE("dialnorm=auto measures each 1+1 programme on its own, from one file or from src=/map=",
          "[cli][encode][dialnorm]") {
    const auto dir = scratch_dir();
    const auto wav = write_wav(dir / "auto_dual.wav", 2);
    const auto log = dir / "auto_dual.log";
    for (const bool multi : {false, true}) {
        for (const std::string_view command : {"encode", "eac3-encode"}) {
            CAPTURE(multi, command);
            const auto out_path = dir / "auto_dual.bin";
            std::string args = std::string{command} + " " + quoted(wav) + " " + quoted(out_path) +
                               " 192" + (command == "eac3-encode" ? " none" : "") +
                               " 1+1 dialnorm=auto dialnorm2=auto";
            if (multi) {
                args += " src=" + quoted(wav) + " map=0.0:p1,0.1:p2,1.0:none,1.1:none";
            }
            REQUIRE(run_cli(args, log) == 0);
            const auto text = read_log(log);
            INFO(text);
            const auto dialnorm = number_after(text, "-> dialnorm ");
            const auto dialnorm2 = number_after(text, "-> dialnorm2 ");
            CHECK(text.find("Ch1 measured ") != std::string::npos);
            CHECK(text.find("Ch2 measured ") != std::string::npos);
            // Two 0.3-amplitude tones measure about -14 LKFS apiece; what
            // matters is that each channel got its own figure in range.
            CHECK(dialnorm >= 13);
            CHECK(dialnorm <= 15);
            CHECK(dialnorm2 >= 13);
            CHECK(dialnorm2 <= 15);
        }
    }
}

TEST_CASE("an extra programme is planned from its own file: its own layout, rate and dialnorm",
          "[cli][encode][programme2]") {
    const auto dir = scratch_dir();
    const auto primary = write_wav(dir / "extra_primary.wav", 6);
    const auto mono = write_wav(dir / "extra_mono.wav", 1);
    const auto log = dir / "extra.log";

    SECTION("no programme2-layout= follows the file, at half the primary's rate") {
        const auto out_path = dir / "extra_default.ec3";
        REQUIRE(run_cli("eac3-encode " + quoted(primary) + " " + quoted(out_path) +
                            " 256 none 51 programme2=" + quoted(mono),
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("  programme 1 (\xC2\xA7" "E2.3.1.2 I1): 128 kbps, 1/0 mono, 1 coded "
                        "channels, dialnorm 31 from " +
                        mono.string()) != std::string::npos);
    }

    SECTION("programme2-dialnorm=auto measures that programme alone") {
        const auto out_path = dir / "extra_auto.ec3";
        REQUIRE(run_cli("eac3-encode " + quoted(primary) + " " + quoted(out_path) +
                            " 256 none 51 programme2=" + quoted(mono) +
                            " programme2-dialnorm=auto",
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        const auto measured = number_after(text, "programme2 measured ");
        CHECK(measured == -14);
        const auto dialnorm = number_after(text, "(BS.1770-4, gated) -> dialnorm ");
        CHECK(dialnorm == 14);
        CHECK(text.find("dialnorm 14 from " + mono.string()) != std::string::npos);
    }
}

TEST_CASE("an extra programme's own file is refused when it cannot ride the primary's access units",
          "[cli][encode][programme2]") {
    const auto dir = scratch_dir();
    const auto primary = write_wav(dir / "extra_refused_primary.wav", 6);
    const auto other_rate = write_wav(dir / "extra_44k.wav", 1, 44100);
    const auto silent = write_wav(dir / "extra_silent.wav", 1, 48000, {0});
    const auto wide = write_wav(dir / "extra_7ch.wav", 7);
    const auto mono = write_wav(dir / "extra_refused_mono.wav", 1);
    const auto out_path = dir / "extra_refused.ec3";
    const auto log = dir / "extra_refused.log";
    const std::string head =
        "eac3-encode " + quoted(primary) + " " + quoted(out_path) + " 256 none 51 programme2=";
    check_rows({{head + quoted(other_rate), 1,
                 "error: programme2= is 44100 Hz but the primary programme is 48000 Hz - every "
                 "substream of an access unit codes the same frame period"},
                // Nothing above the gate is a runtime outcome (exit 5), as it
                // is for the primary programme's own dialnorm=auto.
                {head + quoted(silent) + " programme2-dialnorm=auto", 5,
                 "error: programme2 has no audio above the -70 LKFS absolute gate; pass "
                 "programme2-dialnorm=<1..31> explicitly"},
                {head + quoted(mono) + " programme2-layout=bogus", 1,
                 "error: unknown layout 'bogus' (mono | stereo | 1+1 | 51 | 71 | 512 | 514 | 714)"},
                {head + quoted(wide) + " programme2-layout=51", 1,
                 "error: 7 channels - no standard speaker layout has that many channels"}},
               out_path, log);
}
