#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"

#include "ac3/decoder/decoder.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/mixing.hpp"

// parse_options (apps/cli/support.cpp) at the level a user meets it: the real
// ac3cli binary, run as a subprocess, and what it says about a key=value
// token it was handed.
//
// Every command parses its whole option tail BEFORE it opens a single file,
// which is what makes most of this file cheap: a malformed token is refused
// with exit code 1 (kExitUsage) and its own reason while the input path does
// not even exist, and a well-formed one gets exactly as far as that missing
// input - exit code 2 (kExitInput) and "cannot open file". The second half is
// a genuine claim, not a formality: a value the parser rejected would have
// stopped at 1, and a value it silently mistook for an unknown option would
// have printed "unknown option" instead. Where the value has somewhere to be
// SEEN once accepted (the bsi fields a decoder reports back), the cases at
// the bottom encode for real and read it back off the stream.
//
// run_cli and the helpers below are trimmed copies of test_cli.cpp's own,
// duplicated per this project's per-file test-helper convention (see
// test_cli_probe.cpp, which does the same) - including the Windows
// double-quote wrapping explained in test_cli.cpp's run_cli.

namespace fs = std::filesystem;

namespace {

// See tests/cli/test_cli.cpp's own scratch_dir for the reasoning this copy
// shares, including the PID fold; the leaf name below is this file's own.
std::string scratch_pid_suffix() {
    return ac3::test::platform::process_id();
}

fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("cli_options_" + scratch_pid_suffix());
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

// A short, non-silent WAV: one tone per channel, a different pitch on each.
// 0.1 s is three AC-3 frames' worth - enough for a decoder to read a bsi
// back, and short enough that the encode costs nothing next to the process
// start-up around it.
fs::path write_tone_wav(const fs::path& path, std::size_t channels) {
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 4800;
    std::vector<std::vector<float>> data(channels, std::vector<float>(kFrames));
    for (std::size_t c = 0; c < channels; ++c) {
        const double hz = 330.0 + 110.0 * static_cast<double>(c);
        for (std::size_t n = 0; n < kFrames; ++n) {
            data[c][n] = static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * hz *
                                                           static_cast<double>(n) / kRate));
        }
    }
    REQUIRE(ac3::io::write_wav_f32(path.string(), data, kRate).has_value());
    return path;
}

// The first syncframe of an AC-3 file, decoded - its bsi is what the
// metadata tokens under test were meant to reach.
ac3::DecodedFrame first_frame(const fs::path& path) {
    const auto bytes = read_bytes(path);
    const auto frames = ac3::split_frames(bytes);
    REQUIRE(frames.has_value());
    REQUIRE_FALSE(frames->empty());
    ac3::FrameDecoder decoder;
    auto decoded = decoder.decode_frame(frames->front());
    REQUIRE(decoded.has_value());
    return std::move(*decoded);
}

struct Refusal {
    std::string_view token;
    // The whole of what follows "error: " on the first line, or a distinctive
    // leading part of it where the rest is a long enumerated list.
    std::string_view message;
};

// Runs `<command> <missing input> <output> <token>` once per row, and holds
// each to the refusal contract: usage exit code, its own reason on stderr,
// and no output file left behind.
void check_refusals(std::string_view command, std::span<const Refusal> rows,
                    std::string_view leaf) {
    const auto dir = scratch_dir();
    const auto missing = dir / (std::string{leaf} + "_missing.wav");
    const auto out_path = dir / (std::string{leaf} + "_out.bin");
    const auto log = dir / (std::string{leaf} + ".log");
    for (const auto& row : rows) {
        CAPTURE(row.token);
        fs::remove(out_path);
        const auto rc = run_cli(std::string{command} + " \"" + missing.string() + "\" \"" +
                                    out_path.string() + "\" \"" + std::string{row.token} + "\"",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 1);
        CHECK(text.find("error: " + std::string{row.message}) != std::string::npos);
        CHECK_FALSE(fs::exists(out_path));
    }
}

// The acceptance half: every token parses, so the run gets as far as the
// missing input and stops there with the input exit code, not the usage one.
// All of a table's tokens go on ONE command line - parse_options walks the
// whole tail before the command looks at its input, so a single refused or
// unrecognised token anywhere in it would stop the run at exit code 1 and
// name itself, and one process per table keeps these cases cheap.
void check_accepted(std::string_view command, std::span<const std::string_view> tokens,
                    std::string_view leaf) {
    const auto dir = scratch_dir();
    const auto missing = dir / (std::string{leaf} + "_missing.wav");
    const auto out_path = dir / (std::string{leaf} + "_out.bin");
    const auto log = dir / (std::string{leaf} + ".log");
    std::string args = std::string{command} + " \"" + missing.string() + "\" \"" +
                       out_path.string() + "\"";
    for (const auto token : tokens) {
        args += " \"" + std::string{token} + "\"";
    }
    const auto rc = run_cli(args, log);
    const auto text = read_log(log);
    INFO(text);
    CHECK(rc == 2);
    CHECK(text.find("error: " + missing.string() + ": cannot open file") != std::string::npos);
    CHECK(text.find("unknown option") == std::string::npos);
    CHECK_FALSE(fs::exists(out_path));
}

}  // namespace

TEST_CASE("a malformed metadata option is refused with its own reason before any input is read",
          "[cli][options]") {
    static constexpr Refusal kRows[] = {
        {"dialnorm=0", "dialnorm must be auto or 1..31 (\xC2\xA7" "5.4.2.8)"},
        {"dialnorm2=32", "dialnorm2 must be auto or 1..31 (\xC2\xA7" "5.4.2.16)"},
        {"compr2=abc", "compr2 takes a gain in dB (got 'abc')"},
        {"dsurmod=9", "dsurmod must be 0..3 (Table 5.11's Dolby Surround mode) or one of: "},
        {"cmixlev=-2", "cmixlev must be -3, -4.5 or -6 (Table 5.9)"},
        {"surmixlev=-4", "surmixlev must be -3, -6 or off (Table 5.10)"},
        {"lfemix=40", "lfemix must be off or 0..31 (\xC2\xA7" "E2.3.1.11)"},
        {"dmixmod=bogus", "dmixmod must be ltrt, loro or none (Table D2.2)"},
        {"ltrtcmixlev=-2",
         "ltrtcmixlev must be +3, +1.5, 0, -1.5, -3, -4.5, -6 or off (Tables D2.3-D2.6)"},
        // Tables D2.4/D2.6 reserve the three loudest surround codes, so a
        // level that is legal for a centre field is refused for a surround.
        {"ltrtsurmixlev=+3", "ltrtsurmixlev must be -1.5, -3, -4.5, -6 or off - Tables "
                             "D2.4/D2.6 reserve the three louder codes"},
        {"lorosurmixlev=0", "lorosurmixlev must be -1.5, -3, -4.5, -6 or off"},
        {"dheadphonmod=bogus", "dheadphonmod must be one of: none | off | on"},
        {"dsurexmod=bogus", "dsurexmod must be one of: none | off | ex | pliiz"},
        {"adconvtyp=bogus", "adconvtyp must be one of: standard | hdcd"},
        {"origbs=maybe", "origbs must be on or off (\xC2\xA7" "5.4.2.25)"},
        {"mixdef=bogus", "mixdef must be none, premix, reserved or ext (Table E2.6)"},
        {"mixdata=5000",
         "mixdata is the twelve bits mixdef=reserved reserves, 0..4095 (\xC2\xA7" "E2.3.1.23)"},
        {"extmix=1,2,3", "extmix takes 6 Table E2.8 codes (0..15 or 'off'), optionally a "
                         "seventh for the downmix scale"},
        {"paninfo=240", "paninfo is <0..239>[:<0..63>] - 1.5 degree steps clockwise from "
                        "centre (\xC2\xA7" "E2.3.1.54)"},
        {"drc=bogus", "unknown DRC profile 'bogus' (film-standard | film-light | "
                      "music-standard | music-light | speech)"},
        {"drc2=film", "unknown DRC profile 'film' (film-standard | film-light | "
                      "music-standard | music-light | speech)"},
        {"ceiling=loud", "ceiling needs a level in dBFS"},
        {"ceiling2=x", "ceiling2 needs a level in dBFS"},
        {"dialogue2=", "dialogue2 needs a level in dBFS"},
    };
    check_refusals("eac3-encode", kRows, "refuse_meta");
}

TEST_CASE("a malformed coding, decoding or routing option is refused with its own reason",
          "[cli][options]") {
    static constexpr Refusal kRows[] = {
        {"fast-imdct=on",
         "the fast IMDCT is the default; 'fast-imdct=off' forces the direct "
         "\xC2\xA7"
         "7.9.4 step-3 evaluation (got 'fast-imdct=on')"},
        {"numblkscod=4",
         "numblkscod is 0-3 (1/2/3/6 blocks per syncframe, section E2.3.1.4) "
         "(got 'numblkscod=4')"},
        {"numblkscod=x", "numblkscod is 0-3"},
        {"joc-domain=fft",
         "joc-domain is 'qmf' (the default, \xC2\xA7"
         "7.1's complex "
         "filterbank) or 'mdct' (the 256-bin approximation) "
         "(got 'joc-domain=fft')"},
        {"search=fast",
         "search is 'off' (the default), 'distortion' or 'perceptual' (got 'search=fast')"},
        {"delta=on",
         "delta bit allocation is on by default; 'delta=off' skips the "
         "corrections and the second fit that weighs them (got 'delta=on')"},
        {"channels=3",
         "channels is '2' (\xC2\xA7"
         "7.8 stereo), '1' (mono) or 'as-coded' "
         "(the default - no downmix at all), and for AC-4 '5.1' (a 7.X stream folded to 5.X) "
         "(got 'channels=3')"},
        {"ltrt-phase=on",
         "the Lt/Rt surround phase shift is the default; 'ltrt-phase=off' "
         "selects the sign-only matrix (got 'ltrt-phase=on')"},
        {"drcmode=film",
         "drcmode is 'line' (\xC2\xA7"
         "7.7.1), 'rf' (\xC2\xA7"
         "7.7.2, with "
         "downmix overload protection) or 'none' (the default) for AC-3 and "
         "E-AC-3, and 'off', 'default', 'home-theatre', 'flat-panel-tv', "
         "'portable-speakers' or 'portable-headphones' for AC-4 "
         "(got 'drcmode=film')"},
        {"output-level=5", "output-level is a level in dBFS from -60 to 0 (got 'output-level=5')"},
        {"output-level=loud", "output-level is a level in dBFS from -60 to 0"},
        {"dialogue-enhancement=13",
         "dialogue-enhancement is a gain in dB from 0 to 12 (got 'dialogue-enhancement=13')"},
        {"dialogue-enhancement=-3", "dialogue-enhancement is a gain in dB from 0 to 12"},
        {"presentation=2000", "presentation is a number from 0 to 1023 (got 'presentation=2000')"},
        {"presentation-id=first",
         "presentation-id is a number from 0 to 1023 (got 'presentation-id=first')"},
        {"language=en_GB", "language is a BCP 47 tag such as en or pt-BR (got 'language=en_GB')"},
        {"associated=subtitles", "associated is visually-impaired, audio-description, "},
        {"dialogue-gain=13",
         "dialogue-gain is a gain in dB from -130 to 12 (got 'dialogue-gain=13')"},
        {"associated-gain=1",
         "associated-gain is a gain in dB from -130 to 0 (got 'associated-gain=1')"},
        {"conceal=hide",
         "conceal is 'repeat' (repeat-and-fade), 'mute' (window-ramped "
         "silence) or 'off' (the default) (got 'conceal=hide')"},
        {"codec=mp3", "codec must be ac3 or eac3 (got 'codec=mp3')"},
        {"src=", "src= needs a file path"},
        {"map=", "map= needs a spec (<source>.<channel>"},
        {"fmp4-window=x", "fmp4-window= needs a segment count (0 keeps every segment)"},
        {"signing-key=", "signing-key= needs a key file path"},
        {"programme=9", "programme= needs a substream id 0..7 (got 'programme=9')"},
        {"mainid=9", "mainid must be 0-7 (got 'mainid=9')"},
        {"asvc=1,9", "asvc main-service list must be comma-separated 0-7 (got 'asvc=1,9')"},
        {"asvc=0x100", "asvc must be 0-255 or 0x00-0xFF (got 'asvc=0x100')"},
        {"capture2=-1", "capture2= needs a non-negative device index"},
        {"container=avi", "container must be raw, mkv, ts, spdif or fmp4 (got 'container=avi')"},
        {"positions=osc", "positions= needs a scheme (positions=osc:<port>)"},
        {"positions=midi:1",
         "positions= scheme must be 'osc' (got 'midi'; MIDI and a game "
         "controller are not implemented yet)"},
        {"positions=osc:local:x",
         "positions=osc:[local|any|<ipv4>:]<port> needs a port from 1 to 65535"},
    };
    check_refusals("eac3-encode", kRows, "refuse_tools");
}

TEST_CASE("qc's own layout= and objects= are refused with their own reasons", "[cli][options]") {
    // layout= and objects= mean something else to every other command, so
    // these two refusals are qc's alone - parse_options is told the command.
    static constexpr Refusal kRows[] = {
        {"layout=wide", "layout must be bed or rendered (got 'layout=wide')"},
        {"objects=sideways", "objects layout 'sideways' not recognised (mono | stereo"},
    };
    const auto dir = scratch_dir();
    const auto missing = dir / "qc_missing.ec3";
    const auto log = dir / "qc_refuse.log";
    for (const auto& row : kRows) {
        CAPTURE(row.token);
        const auto rc =
            run_cli("qc \"" + missing.string() + "\" \"" + std::string{row.token} + "\"", log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 1);
        CHECK(text.find("error: " + std::string{row.message}) != std::string::npos);
    }
}

TEST_CASE("an unknown option names itself and prints the option summary", "[cli][options]") {
    const auto dir = scratch_dir();
    const auto missing = dir / "unknown_missing.wav";
    const auto out_path = dir / "unknown_out.ec3";
    const auto log = dir / "unknown.log";
    // programme9= is past the eight independent substreams §E2.3.1.2 allows,
    // and programme2-bogus= is a real programme with a field nobody defined:
    // neither may be swallowed as an extra programme's option.
    for (const std::string_view token : {"volume=11", "programme9=x", "programme2-bogus=1",
                                         "programme2x=1"}) {
        CAPTURE(token);
        fs::remove(out_path);
        const auto rc = run_cli("eac3-encode \"" + missing.string() + "\" \"" +
                                    out_path.string() + "\" \"" + std::string{token} + "\"",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 1);
        CHECK(text.find("error: unknown option '" + std::string{token} + "'") !=
              std::string::npos);
        CHECK(text.find("metadata options (any order, after the positional arguments):") !=
              std::string::npos);
        CHECK_FALSE(fs::exists(out_path));
    }
}

TEST_CASE("every documented spelling of a valued option gets past parsing to the input file",
          "[cli][options]") {
    static constexpr std::string_view kTokens[] = {
        "search=off",
        "search=distortion",
        "search=perceptual",
        "delta=off",
        "channels=2",
        "channels=1",
        "channels=as-coded",
        "mix-lfe",
        "ltrt-phase=off",
        "drcmode=line",
        "drcmode=rf",
        "drcmode=none",
        "drcmode=default",
        "drcmode=home-theatre",
        "drcmode=portable-headphones",
        "output-level=-31",
        "output-level=-14.5",
        "dialogue-enhancement=9",
        "dialogue-enhancement=0",
        "presentation=0",
        "presentation-id=7",
        "language=pt-BR",
        "associated=audio-description",
        "associated=commentary",
        "dialogue-gain=-6",
        "dialogue-gain=6",
        "associated-gain=-10",
        "conceal=repeat",
        "conceal=mute",
        "conceal=off",
        "drc=0.5",
        "drc=film-light",
        "drc2=speech",
        "ceiling=-1",
        "dialogue=-20",
        "ceiling2=-1.5",
        "dialogue2=-24",
        "dialnorm2=5",
        "dialnorm2=auto",
        "compr2=-3",
        "dsurmod=3",
        "dsurmod=on",
        "dsurmod=1",
        "cmixlev=-3",
        "cmixlev=-4.5",
        "cmixlev=-6",
        "surmixlev=-3",
        "surmixlev=-6",
        "surmixlev=off",
        "lfemix=off",
        "lfemix=10",
        "dmixmod=none",
        "dmixmod=ltrt",
        "dmixmod=loro",
        "lorocmixlev=+1.5",
        "ltrtsurmixlev=-3",
        "lorosurmixlev=off",
        "dheadphonmod=on",
        "dsurexmod=ex",
        "adconvtyp=hdcd",
        "codec=ec3",
        "codec=eac3",
        "codec=ac3",
    };
    check_accepted("eac3-encode", kTokens, "accept_a");
}

TEST_CASE("the remaining valued and bare option spellings get past parsing to the input file",
          "[cli][options]") {
    static constexpr std::string_view kTokens[] = {
        "origbs=on", "origbs=off", "pgmscl=+3", "pgmscl2=mute",
        "extpgmscl=-50", "mixdef=none", "mixdef=premix", "mixdef=reserved",
        "mixdef=ext", "mixdata=100", "premixcmp=compr:local:7", "extmix=0,1,2,3,4,off",
        "extmix=0,1,2,3,4,5,6", "auxmix=off,15", "speechmix=3,4:1,5:7", "paninfo=10",
        "paninfo2=10:5", "blkmixcfg=1,-,3,-,5,31", "capture2=1", "fmp4-window=4",
        "bed-only", "encinfo", "infomdat", "positions=osc:9000",
        "positions=osc:any:9000", "positions=osc:10.0.0.1:9000", "numblkscod=1",
        "joc-domain=qmf", "joc-domain=mdct", "fast-imdct", "fast-imdct=off",
        "container=raw", "container=matroska", "container=mpegts", "container=spdif",
        "container=cmaf", "asvc=0,2", "asvc=0x05", "mainid=3", "langcod2",
    };
    check_accepted("eac3-encode", kTokens, "accept_b");
}

TEST_CASE("decode's AC-4 options are refused with their own reasons", "[cli][options][ac4]") {
    static constexpr Refusal kRows[] = {
        {"md-compat=8", "md-compat is a level from 0 to 7 (got 'md-compat=8')"},
        {"md-compat=high", "md-compat is a level from 0 to 7 (got 'md-compat=high')"},
        {"mix-lfe=maybe", "mix-lfe is 'on' or 'off' (got 'mix-lfe=maybe')"},
        {"channels=6",
         "channels is '2' (\xC2\xA7"
         "7.8 stereo), '1' (mono) or 'as-coded' (the default - no downmix at all), and for "
         "AC-4 '5.1' (a 7.X stream folded to 5.X) (got 'channels=6')"},
    };
    check_refusals("decode", kRows, "refuse_ac4_decode");
}

TEST_CASE("every spelling of decode's AC-4 options gets past parsing to the input file",
          "[cli][options][ac4]") {
    // decode reports a missing input in its own words, so check_accepted's
    // encoder message does not fit; the check is the same.
    static constexpr std::string_view kTokens[] = {
        "channels=5.1",
        "mix-lfe=on",
        "mix-lfe=off",
        "headphones",
        "md-compat=0",
        "md-compat=7",
        "drcmode=off",
        "drcmode=flat-panel-tv",
        "drcmode=portable-speakers",
        "presentation-id=0",
        "associated=visually-impaired",
        "associated=audio-description-subtitles",
        "associated=spoken-subtitles",
        "associated=emergency-information",
        "associated=hearing-impaired",
        "downmix=auto",
        "downmix=ltrt",
        "downmix=mono",
        "syntax-trace=trace.tsv",
        "conceal=mute",
    };
    const auto dir = scratch_dir();
    const auto missing = dir / "accept_ac4_decode_missing.ac4";
    const auto log = dir / "accept_ac4_decode.log";
    std::string args =
        "decode \"" + missing.string() + "\" \"" + (dir / "accept_ac4_decode.wav").string() + "\"";
    for (const auto token : kTokens) {
        args += " \"" + std::string{token} + "\"";
    }
    const auto rc = run_cli(args, log);
    const auto text = read_log(log);
    INFO(text);
    CHECK(rc == 2);
    CHECK(text.find("error: cannot read " + missing.string()) != std::string::npos);
    CHECK(text.find("unknown option") == std::string::npos);
    // md-compat= is decode's alone.
    CHECK(run_cli("eac3-encode \"" + missing.string() + "\" \"" + (dir / "x.ec3").string() +
                      "\" md-compat=3",
                  log) == 1);
    CHECK(read_log(log).find("error: unknown option 'md-compat=3'") != std::string::npos);
}

TEST_CASE("ac4-encode's own options are refused with their own reasons", "[cli][options][ac4]") {
    static constexpr Refusal kRows[] = {
        {"frame-rate=31",
         "frame-rate is 23.976, 24, 25, 29.97, 30, 47.95, 48, 50, 59.94, 60, 100, "
         "119.88 or 120 fps, or native, the 2 048-sample frame (got 'frame-rate=31')"},
        {"rate-mode=cbr", "rate-mode is constant, average or variable (got 'rate-mode=cbr')"},
        {"iframe-interval=0", "iframe-interval is a number of frames from 1"},
        {"iframes=3,,4", "iframes is a comma-separated list of frame numbers from 0"},
        {"iframes=-1", "iframes is a comma-separated list of frame numbers from 0"},
        {"fragment=0", "fragment is a fragment's duration in seconds"},
        // AC-4's dialnorm is in quarters of a dB, from 0 to 31.75.
        {"dialnorm=32", "dialnorm is auto, or the dialogue level in dB below full scale"},
        {"dialnorm=24.1", "dialnorm is auto, or the dialogue level in dB below full scale"},
        {"loudness=r128", "loudness is the practice the programme is measured to"},
        {"drc=0.5",
         "unknown DRC profile '0.5' (film-standard | film-light | music-standard | "
         "music-light | speech | none)"},
        {"drc-portable-speakers=loud", "unknown DRC profile 'loud'"},
        {"cmixlev=-2", "an AC-4 centre mix level is +3, +1.5, 0, -1.5, -3, -4.5, -6 or off"},
        // Table 149a has 0 dB for the surrounds, and nothing louder.
        {"lorosurmixlev=+1.5", "an AC-4 surround mix level is 0, -1.5, -3, -4.5, -6 or off"},
        {"lfemix=10", "AC-4's lfemix is the LFE's gain into the stereo downmix, +5.5 to -25.5 dB"},
        {"lfemix=-3", "AC-4's lfemix is the LFE's gain into the stereo downmix"},
        {"dmixmod=reserved", "AC-4's dmixmod is loro, ltrt, pl2 (Lt/Rt for Pro Logic II) or none"},
        {"loro-correction=8", "a downmix loudness correction is -7.5 to +7.5 dB in steps of 0.5"},
        {"ltrt-correction=0.3", "a downmix loudness correction is -7.5 to +7.5 dB in steps of 0.5"},
        {"dialogue-channels=l,x", "dialogue-channels is any of l, r and c, comma-separated"},
        {"dialogue-stem=", "dialogue-stem needs a WAV file"},
        {"dialogue-method=hybrid", "dialogue-method is independent, mid or cross"},
        {"dialogue-max-gain=10", "dialogue-max-gain is 3, 6, 9 or 12 dB"},
        {"dialogue-hybrid=1.5",
         "dialogue-hybrid is the waveform's share of the enhancement, 0 to 1"},
        {"experimental=drc-gains-4", "experimental takes aspx-balance, aspx-varvar"},
        {"experimental=three-one", "experimental takes aspx-balance, aspx-varvar"},
        {"crc=yes",
         "crc is on, a raw stream's sync frames with Part 2 Annex G's CRC (the default), or off"},
    };
    check_refusals("ac4-encode", kRows, "refuse_ac4");
}

TEST_CASE("ac4-encode's substreamN and presentationN options are refused with their own reasons",
          "[cli][options][ac4]") {
    static constexpr Refusal kRows[] = {
        {"substream1=a.wav", "substream 1 is the positional input; substream2= names the next one"},
        {"substream2=", "substreamN= needs a WAV file"},
        {"substream2-bitrate=0", "a substream's bitrate is its share of the rate in kbps, from 1"},
        {"substream2-codec-mode=aspx-acpl-4", "a substream's codec-mode is auto, simple, aspx"},
        {"substream2-content=news", "a substream's content is main, music-and-effects"},
        {"substream2-language=", "a substream's language is an IETF BCP 47 tag"},
        {"substream2-enhances=0", "enhances names the substream, from 1"},
        {"substream2-max-dialogue-gain=4", "max-dialogue-gain is 3, 6, 9 or 12 dB"},
        {"substream2-pan=360",
         "pan is a dialogue channel's direction in degrees clockwise from the front"},
        {"substream2-pan=0,30,330", "pan is a dialogue channel's direction in degrees clockwise"},
        {"substream2-emdf=1:e", "emdf is <id>:<hex bytes>, the id from 1"},
        {"substream2-emdf=e606", "emdf is <id>:<hex bytes>, the id from 1"},
        {"substream2-dialogue-method=hybrid", "dialogue-method is independent, mid or cross"},
        {"substream2-colour=red", "unknown substreamN option; see ac3cli help ac4-encode"},
        {"presentation1=0",
         "presentationN= lists the substreams it plays, from 1, comma-separated"},
        {"presentation1=1,,2",
         "presentationN= lists the substreams it plays, from 1, comma-separated"},
        {"presentation1-config=7", "a presentation's config is Part 2 Table 53's 0 to 6"},
        {"presentation1-id=-1", "a presentation's id is its presentation_id, from 0"},
        {"presentation1-md-compat=5",
         "a presentation's md-compat is Part 2 Table 55's 0 to 3, or 7"},
        {"presentation1-enabled=yes", "a presentation's enabled and pre-virtualized are on or off"},
        {"presentation1-pre-virtualized=1",
         "a presentation's enabled and pre-virtualized are on or off"},
        {"presentation1-name=", "an alternative presentation's name is UTF-8 text"},
        {"presentation1-dialnorm=24.1",
         "a presentation's dialnorm is dB below full scale, 0 to 31.75"},
        {"presentation1-gains=0,+3",
         "gains are each substream's group gain in dB, 0 or below, or off"},
        {"presentation1-main-gain=3",
         "the main audio's scaling beside associated audio is dB, 0 or below"},
        {"presentation1-main-centre-gain=x",
         "the main audio's scaling beside associated audio is dB"},
        {"presentation1-main-front-gain=+1",
         "the main audio's scaling beside associated audio is dB"},
        {"presentation1-associated-pan=360", "associated-pan is mono associated audio's direction"},
        {"presentation1-emdf=0:00", "emdf is <id>:<hex bytes>, the id from 1"},
        {"presentation1-colour=red", "unknown presentationN option; see ac3cli help ac4-encode"},
        // N runs to 32 substreams and 64 presentations, without a leading 0.
        {"substream33=a.wav", "unknown option 'substream33=a.wav'"},
        {"substream02=a.wav", "unknown option 'substream02=a.wav'"},
        {"presentation65=1", "unknown option 'presentation65=1'"},
    };
    check_refusals("ac4-encode", kRows, "refuse_ac4_numbered");
}

TEST_CASE(
    "every spelling of ac4-encode's substreamN and presentationN options gets past parsing "
    "to the input file",
    "[cli][options][ac4]") {
    // One stream the checks before the inputs are read take: substream 2 an
    // input, substream 3 the waveform of substream 1's hybrid enhancement,
    // and presentations of configurations 1, 0 and 6.
    static constexpr std::string_view kTokens[] = {
        "crc=on",
        "crc=off",
        "dialogue-channels=c",
        "dialogue-hybrid=0.25",
        "substream1-codec-mode=simple",
        "substream1-bitrate=96",
        "substream1-content=main",
        "substream1-language=en",
        "substream2=dialogue.wav",
        "substream2-bitrate=64",
        "substream2-codec-mode=aspx",
        "substream2-content=dialogue",
        "substream2-language=qad",
        "substream2-max-dialogue-gain=6",
        "substream2-pan=330,30",
        "substream2-emdf=1:e606",
        "substream2-emdf=20:",
        "substream2-dialogue-channels=l,r",
        "substream2-dialogue-method=cross",
        "substream2-dialogue-max-gain=12",
        "substream2-dialogue-stem=stem.wav",
        "substream3-enhances=1",
        "presentation1=1,3",
        "presentation1-config=1",
        "presentation1-id=5",
        "presentation1-md-compat=7",
        "presentation1-enabled=on",
        "presentation1-pre-virtualized=off",
        "presentation1-dialnorm=23.25",
        "presentation1-gains=0",
        "presentation2=1,2",
        "presentation2-config=0",
        "presentation2-name=Commentary",
        "presentation2-gains=0,off",
        "presentation2-main-gain=-6",
        "presentation2-main-centre-gain=off",
        "presentation2-main-front-gain=-1.5",
        "presentation2-associated-pan=30",
        "presentation2-emdf=20:",
        "presentation3-config=6",
        "presentation3-emdf=1:00ff",
    };
    check_accepted("ac4-encode", kTokens, "accept_ac4_numbered");
}

TEST_CASE("every spelling of ac4-encode's own options gets past parsing to the input file",
          "[cli][options][ac4]") {
    static constexpr std::string_view kTokens[] = {
        "frame-rate=23.976",
        "frame-rate=29.97",
        "frame-rate=47.95",
        "frame-rate=119.88",
        "frame-rate=native",
        "rate-mode=constant",
        "rate-mode=average",
        "rate-mode=variable",
        "iframe-interval=1",
        "iframes=0,5,100",
        "fragment=2.002",
        "dialnorm=0",
        "dialnorm=31.75",
        "dialnorm=auto",
        "dialnorm=23.25",
        "loudness=atsc-a85",
        "loudness=ebu-r128",
        "loudness=arib-tr-b32",
        "loudness=freetv-op59",
        "loudness=manual",
        "loudness=consumer-leveller",
        "loudness=not-indicated",
        "drc=none",
        "drc=music-light",
        "drc-home-theatre=film-standard",
        "drc-flat-panel-tv=speech",
        "drc-portable-speakers=none",
        "drc-portable-headphones=music-standard",
        "cmixlev=+3",
        "lorocmixlev=off",
        "ltrtcmixlev=-4.5",
        "surmixlev=0",
        "lorosurmixlev=-1.5",
        "ltrtsurmixlev=off",
        "lfemix=5.5",
        "lfemix=+4.5",
        "lfemix=-25.5",
        "lfemix=off",
        "dmixmod=pl2",
        "dmixmod=ltrt",
        "dmixmod=none",
        "loro-correction=-7.5",
        "ltrt-correction=+2.5",
        "dialogue-channels=l,r,c",
        "dialogue-channels=c",
        "dialogue-stem=stem.wav",
        "dialogue-method=independent",
        "dialogue-method=mid",
        "dialogue-method=cross",
        "dialogue-max-gain=12",
        "experimental=drc-gains-0,acpl",
    };
    check_accepted("ac4-encode", kTokens, "accept_ac4");
}

TEST_CASE("a programmeN= token without its value, or naming a field no extra programme has, is "
          "refused naming that programme's own key",
          "[cli][options][programme2]") {
    // Every message carries the ORIGINAL programmeN- spelling, so a user with
    // three extra programmes can tell whose field was wrong.
    static constexpr Refusal kRows[] = {
        {"programme2=", "programme2= needs an input file path"},
        {"programme3-layout=", "programme3-layout= needs a layout name (mono | stereo"},
        {"programme2-bitrate=0",
         "programme2-bitrate= needs a rate in kbit/s (got 'programme2-bitrate=0')"},
        {"programme2-langcod", "programme2-langcod is an AC-3 Annex D field"},
        {"programme2-timecode", "programme2-timecode is an AC-3 Annex D field"},
        {"programme2-drc2=film-light", "programme2-drc2 is 1+1 dual-mono only"},
        {"programme4-roomtyp2=large", "programme4-roomtyp2 is 1+1 dual-mono only"},
    };
    check_refusals("eac3-encode", kRows, "refuse_programme");
}

TEST_CASE("a malformed programmeN- metadata value is refused naming that programme's own key",
          "[cli][options][programme2]") {
    static constexpr Refusal kRows[] = {
        {"programme2-drc=film", "unknown DRC profile 'film'"},
        {"programme2-ceiling=x", "programme2-ceiling needs a level in dBFS"},
        {"programme2-dialnorm=0", "programme2-dialnorm must be auto or 1..31"},
        {"programme2-bsmod=9", "programme2-bsmod must be 0..7 (Table 5.5's service type)"},
        {"programme2-dsurmod=loud", "programme2-dsurmod must be 0..3 (Table 5.11's Dolby "
                                    "Surround mode)"},
        {"programme2-cmixlev=-2", "programme2-cmixlev must be -3, -4.5 or -6 (Table 5.9)"},
        {"programme2-surmixlev=-4", "programme2-surmixlev must be -3, -6 or off (Table 5.10)"},
        {"programme2-lfemix=40", "programme2-lfemix must be off or 0..31"},
        {"programme2-dmixmod=x", "programme2-dmixmod must be ltrt, loro or none (Table D2.2)"},
        {"programme2-lorocmixlev=-2", "programme2-lorocmixlev must be +3, +1.5, 0, -1.5, -3, "
                                      "-4.5, -6 or off (Tables D2.3-D2.6)"},
        {"programme2-lorosurmixlev=+1.5", "programme2-lorosurmixlev must be -1.5, -3, -4.5, -6 "
                                          "or off - Tables D2.4/D2.6 reserve the three louder "
                                          "codes"},
        {"programme2-dsurexmod=x", "programme2-dsurexmod must be one of: none | off | ex | pliiz"},
        {"programme2-dheadphonmod=x", "programme2-dheadphonmod must be one of: none | off | on"},
        {"programme2-adconvtyp=x", "programme2-adconvtyp must be one of: standard | hdcd"},
        {"programme2-mixlevel=79", "programme2-mixlevel is a peak mixing level of 80..111 dB "
                                   "SPL"},
        {"programme2-roomtyp=cave", "programme2-roomtyp must be one of: "},
        {"programme2-origbs=x", "programme2-origbs must be on or off"},
        {"programme2-pgmscl=+13", "programme2-pgmscl is mute or a level in -50..+12 dB"},
        {"programme2-extpgmscl=-51", "programme2-extpgmscl is mute or a level in -50..+12 dB"},
        {"programme2-mixdef=x", "programme2-mixdef must be none, premix, reserved or ext"},
        {"programme2-premixcmp=compr:far:1",
         "programme2-premixcmp is <dynrng|compr>:<external|local>:<0..7>"},
        {"programme2-mixdata=4096", "programme2-mixdata is the twelve bits mixdef=reserved "
                                    "reserves, 0..4095"},
        {"programme2-auxmix=1,2,3", "programme2-auxmix takes 2 Table E2.8 codes (0..15 or "
                                    "'off')"},
        {"programme2-speechmix=32", "programme2-speechmix is <0..31>[,<0..31>:<0..3>"},
        {"programme2-paninfo=1:2:3", "programme2-paninfo is <0..239>[:<0..63>]"},
        {"programme2-blkmixcfg=1,2", "programme2-blkmixcfg is six comma-separated 0..31 words"},
    };
    check_refusals("eac3-encode", kRows, "refuse_programme_values");
}

TEST_CASE("every programmeN- metadata field accepts each of its documented spellings",
          "[cli][options][programme2]") {
    static constexpr std::string_view kTokens[] = {
        "programme8=extra.wav", "programme2-layout=51", "programme2-bitrate=96",
        "programme2-heavy", "programme2-mixmeta", "programme2-infomdat",
        "programme2-copyright", "programme2-sourcefscod", "programme2-drc=music-light",
        "programme2-ceiling=-2", "programme2-dialogue=-27", "programme2-dialnorm=auto",
        "programme2-dialnorm=24", "programme2-bsmod=emergency", "programme2-dsurmod=2",
        "programme2-dsurmod=3", "programme2-dsurmod=off", "programme2-cmixlev=-3",
        "programme2-cmixlev=-4.5", "programme2-cmixlev=-6", "programme2-surmixlev=-3",
        "programme2-surmixlev=-6", "programme2-surmixlev=off", "programme2-lfemix=off",
        "programme2-lfemix=7", "programme2-dmixmod=ltrt", "programme2-dmixmod=loro",
        "programme2-dmixmod=none", "programme2-ltrtcmixlev=+3", "programme2-lorocmixlev=0",
        "programme2-ltrtsurmixlev=-1.5", "programme2-lorosurmixlev=off",
        "programme2-dsurexmod=pliiz", "programme2-dheadphonmod=off",
        "programme2-adconvtyp=standard", "programme2-mixlevel=105", "programme2-roomtyp=small",
        "programme2-origbs=on", "programme2-origbs=off", "programme2-pgmscl=mute",
        "programme2-extpgmscl=+12", "programme2-mixdef=none", "programme2-mixdef=premix",
        "programme2-mixdef=reserved", "programme2-mixdef=ext",
        "programme2-premixcmp=dynrng:external:0", "programme2-mixdata=4095",
        "programme2-extmix=1,2,3,4,5,6,7", "programme2-auxmix=off,3",
        "programme2-speechmix=31,31:3,31:7", "programme2-speechmix=4",
        "programme2-paninfo=239:63", "programme2-blkmixcfg=-,-,-,-,-,0",
    };
    check_accepted("eac3-encode", kTokens, "accept_programme");
}

TEST_CASE("AC-3 centre and surround downmix levels reach the bsi a decoder reads back",
          "[cli][options][encode]") {
    const auto dir = scratch_dir();
    const auto wav = write_tone_wav(dir / "mixlev_51.wav", 6);
    const auto out_path = dir / "mixlev_51.ac3";
    const auto log = dir / "mixlev_51.log";

    struct Case {
        std::string_view tokens;
        ac3::meta::CentreMixLevel cmixlev;
        ac3::meta::SurroundMixLevel surmixlev;
    };
    for (const auto& c : {Case{"cmixlev=-4.5 surmixlev=-6", ac3::meta::CentreMixLevel::kMinus4_5dB,
                               ac3::meta::SurroundMixLevel::kMinus6dB},
                          Case{"cmixlev=-6 surmixlev=off", ac3::meta::CentreMixLevel::kMinus6dB,
                               ac3::meta::SurroundMixLevel::kSilent}}) {
        CAPTURE(c.tokens);
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav.string() + "\" \"" + out_path.string() +
                                    "\" 384 51 quiet " + std::string{c.tokens},
                                log);
        INFO(read_log(log));
        REQUIRE(rc == 0);
        const auto frame = first_frame(out_path);
        REQUIRE(frame.cmixlev.has_value());
        REQUIRE(frame.surmixlev.has_value());
        CHECK(*frame.cmixlev == c.cmixlev);
        CHECK(*frame.surmixlev == c.surmixlev);
    }
}

TEST_CASE("AC-3 dsurmod accepts its raw code and its names, reserved code 3 reading as not "
          "indicated",
          "[cli][options][encode]") {
    const auto dir = scratch_dir();
    const auto wav = write_tone_wav(dir / "dsurmod_stereo.wav", 2);
    const auto out_path = dir / "dsurmod_stereo.ac3";
    const auto log = dir / "dsurmod_stereo.log";

    struct Case {
        std::string_view token;
        ac3::meta::SurroundMode mode;
    };
    for (const auto& c : {Case{"dsurmod=on", ac3::meta::SurroundMode::kDolbySurround},
                          Case{"dsurmod=1", ac3::meta::SurroundMode::kNotDolbySurround},
                          Case{"dsurmod=3", ac3::meta::SurroundMode::kNotIndicated}}) {
        CAPTURE(c.token);
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo quiet " + std::string{c.token},
                                log);
        INFO(read_log(log));
        REQUIRE(rc == 0);
        CHECK(first_frame(out_path).info.dsurmod == c.mode);
    }
}

TEST_CASE("AC-3 Annex D downmix preferences reach xbsi1 as asked", "[cli][options][encode]") {
    const auto dir = scratch_dir();
    const auto wav = write_tone_wav(dir / "annexd_51.wav", 6);
    const auto out_path = dir / "annexd_51.ac3";
    const auto log = dir / "annexd_51.log";
    const auto rc = run_cli("encode \"" + wav.string() + "\" \"" + out_path.string() +
                                "\" 384 51 quiet dmixmod=loro ltrtcmixlev=+1.5 lorocmixlev=-4.5 "
                                "ltrtsurmixlev=-1.5 lorosurmixlev=off",
                            log);
    INFO(read_log(log));
    REQUIRE(rc == 0);
    const auto frame = first_frame(out_path);
    // dmixmod= on AC-3 has nowhere to go but Annex D, so it switches the
    // stream to the bsid-6 alternate syntax by itself.
    CHECK(frame.bsid == 6);
    REQUIRE(frame.alternate_bsi.has_value());
    REQUIRE(frame.alternate_bsi->mix.has_value());
    const auto& mix = *frame.alternate_bsi->mix;
    CHECK(mix.dmixmod == ac3::meta::DownmixMode::kLoRo);
    CHECK(mix.ltrtcmixlev == ac3::meta::MixLevel::kPlus1_5dB);
    CHECK(mix.lorocmixlev == ac3::meta::MixLevel::kMinus4_5dB);
    CHECK(mix.ltrtsurmixlev == ac3::meta::MixLevel::kMinus1_5dB);
    CHECK(mix.lorosurmixlev == ac3::meta::MixLevel::kSilent);
}
