#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "ac3/core/tables.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/meta/qc.hpp"
#include "ac3/oba/scene.hpp"

// apps/cli/main.cpp compiles directly into the ac3cli executable, everything
// in an anonymous namespace - there is no library surface parse_options,
// gather_frame or run_atmos_encode's own logic could be linked into this
// binary and called directly. So these are integration tests: they run the
// real, built ac3cli.exe as a subprocess (the same binary a build/verify
// step produces) and inspect what it actually wrote, rather than
// re-implementing its argument parsing against a copy of the source.
//
// AC3CLI_EXE (see tests/CMakeLists.txt) is the absolute path to that binary,
// supplied by CMake via $<TARGET_FILE:ac3cli> - these tests do not run at
// all if AC3FORGE_BUILD_CLI is OFF, the same way the alsa/android platform
// tests above do not run outside their own backend.

namespace fs = std::filesystem;

namespace {

// Scratch space for this file's own tests. AC3FORGE_TEST_SCRATCH_DIR (see
// tests/CMakeLists.txt for why it is a build-tree path and not
// fs::temp_directory_path()) is the whole suite's root; the leaf below is this
// file's own. Duplicated in every test file that needs scratch space rather
// than shared, per this project's per-file test-helper convention - only the
// leaf name differs between the copies.
fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "cli";
    fs::create_directories(dir);
    return dir;
}

// std::system()'s return value is the child's own exit code on Windows
// (cmd.exe /c ... for a plain non-shell-builtin invocation), but on POSIX
// it is the raw wait() status word: WIFEXITED/WEXITSTATUS have to unpack it,
// or exit code 1 arrives here as 256 (1 << 8). Without this, every run_cli*
// helper below would be a Windows-only exit-code check wearing a portable
// face - exactly the trap the comment at this file's frmsiz assertion tests
// used to route around case by case rather than fix once, here.
int child_exit_code(int system_status) {
#ifdef _WIN32
    return system_status;
#else
    if (system_status == -1) {
        return system_status;
    }
    // A signal-terminated child (crash, abort()) has no exit code to report;
    // 128 + signal is the shell convention, and distinguishable from every
    // real ac3cli exit code (0..7 - apps/cli/exit_codes.hpp).
    return WIFEXITED(system_status) ? WEXITSTATUS(system_status)
                                    : 128 + WTERMSIG(system_status);
#endif
}

// Runs `ac3cli <args>`, both streams redirected to `log` so a failing
// assertion can print exactly what the binary said. Returns ac3cli's own
// exit code (apps/cli/exit_codes.hpp), portable across std::system()'s
// platform-specific return-value shape - see child_exit_code above.
int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
#ifdef _WIN32
    // std::system() on Windows hands this to `cmd.exe /c <command>`; since
    // `command` both contains spaces AND starts with its own quoted
    // executable path, the CRT's own argument quoting backslash-escapes
    // those embedded quotes (\") when it wraps `command` for the /c
    // argument - and cmd.exe does not understand \" as an escaped quote, so
    // the escaped command comes out corrupted ("The filename, directory
    // name, or volume label syntax is incorrect", confirmed by reproducing
    // this outside Catch2 too). Wrapping the whole thing in one more pair of
    // quotes first is the standard workaround: cmd.exe's own "strip a
    // matching outer quote pair" rule then removes exactly this pair,
    // handing cmd the original, uncorrupted command line beneath it. POSIX's
    // `sh -c` has no such rule - the same extra pair there would make `sh`
    // read the entire command (redirections included) as one big quoted
    // word, which is exactly the "not found" this guard avoids.
    const std::string wrapped = "\"" + command + "\"";
    return child_exit_code(std::system(wrapped.c_str()));
#else
    return child_exit_code(std::system(command.c_str()));
#endif
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// Runs `ac3cli <args>` with stdin redirected from `in_file` and stdout
// redirected to `out_file`, for exercising the "-" stdin/stdout convention
// (main.cpp's is_stdio_path()) the same way a real shell pipeline would.
// stderr goes to `log`, same diagnostic convention as run_cli above but kept
// separate from stdout, which here is the command's real binary output, not
// somewhere to also stash messages.
int run_cli_stdio(const std::string& args, const fs::path& in_file, const fs::path& out_file,
                  const fs::path& log) {
    const std::string command = "\"" + std::string(AC3CLI_EXE) + "\" " + args + " < \"" +
                                in_file.string() + "\" > \"" + out_file.string() + "\" 2> \"" +
                                log.string() + "\"";
#ifdef _WIN32
    // Same double-quote-wrapping workaround run_cli uses above, and for the
    // same reason - see its comment.
    const std::string wrapped = "\"" + command + "\"";
    return child_exit_code(std::system(wrapped.c_str()));
#else
    return child_exit_code(std::system(command.c_str()));
#endif
}

// Same idea as run_cli_stdio above, but only out_path is "-" - for src=/map=
// commands, which take their input from a real in_path plus src="<file>"
// (there is no second stdin to route a multi-source run's extra files
// through), so only stdout needs redirecting away from the shell's own
// inherited one. stderr still goes to its own `log`, kept separate from
// `out_file` for the same reason run_cli_stdio's does.
int run_cli_stdout(const std::string& args, const fs::path& out_file, const fs::path& log) {
    const std::string command = "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" +
                                out_file.string() + "\" 2> \"" + log.string() + "\"";
#ifdef _WIN32
    // Same double-quote-wrapping workaround run_cli uses above, and for the
    // same reason - see its comment.
    const std::string wrapped = "\"" + command + "\"";
    return child_exit_code(std::system(wrapped.c_str()));
#else
    return child_exit_code(std::system(command.c_str()));
#endif
}

// A short, genuinely non-silent multichannel WAV - per this project's own
// testing convention (see memory: "Codec validation needs real audio"),
// silence gives false passes a real tone does not: a silent leading region
// looks identical to a bug that silenced the whole file, but a tone that
// goes missing does not.
std::vector<std::vector<float>> make_tone_channels(std::size_t channels, std::size_t frames,
                                                    std::uint32_t sample_rate) {
    std::vector<std::vector<float>> out(channels, std::vector<float>(frames));
    for (std::size_t c = 0; c < channels; ++c) {
        const double hz = 220.0 * std::pow(2.0, static_cast<double>(c) * 0.5);
        for (std::size_t n = 0; n < frames; ++n) {
            out[c][n] = static_cast<float>(
                0.5 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) /
                              static_cast<double>(sample_rate)));
        }
    }
    return out;
}

// RMS over [from, from + count) of one channel - used to tell "silence" from
// "the tone is playing" without demanding exact sample equality, which a
// lossy codec never gives.
double rms(const std::vector<float>& channel, std::size_t from, std::size_t count) {
    double sum_sq = 0.0;
    const std::size_t end = std::min(from + count, channel.size());
    for (std::size_t n = from; n < end; ++n) {
        sum_sq += static_cast<double>(channel[n]) * static_cast<double>(channel[n]);
    }
    const std::size_t n = end > from ? end - from : 1;
    return std::sqrt(sum_sq / static_cast<double>(n));
}

// A single mono channel at a chosen amplitude/frequency - unlike
// make_tone_channels above (which ladders frequency across the channels of
// ONE file), the dialnorm=auto tests below need independently-controlled,
// separately-loud tracks: two whole files for src=/map=, or the two channels
// of a hand-built dual-mono WAV, with levels deliberately far enough apart
// that a wrong (blended, swapped, or unrouted) measurement reads as a
// distinctly different number rather than a rounding nuance.
std::vector<float> make_tone(double amp, double hz, std::size_t frames, std::uint32_t sample_rate) {
    std::vector<float> out(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        out[n] = static_cast<float>(
            amp * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) /
                          static_cast<double>(sample_rate)));
    }
    return out;
}

// write_wav_f32 takes a std::span<const std::vector<float>>, which (unlike
// make_tone_channels' already-a-vector result above) a braced-init-list of
// make_tone(...) calls does not implicitly convert to - this takes the list
// as a real std::vector<std::vector<float>> parameter first so callers below
// can still write the channels inline as a brace list.
bool write_wav(const fs::path& path, std::vector<std::vector<float>> channels,
               std::uint32_t sample_rate) {
    return ac3::io::write_wav_f32(path.string(), channels, sample_rate).has_value();
}

// The integer dialnorm/dialnorm2 value the CLI's own "-> dialnorm N" /
// "-> dialnorm2 N" report line prints, straight out of the log text - reads
// the same reported number an operator would, rather than re-deriving one
// from the logged LKFS float and risking a second place rounding could
// disagree. The trailing space in `needle` matters: it is what keeps a
// "dialnorm" search from also matching inside "dialnorm2 30" (the character
// right after "dialnorm" there is '2', not a space).
std::optional<int> reported_value(const std::string& log, std::string_view field) {
    const std::string needle = std::string("-> ") + std::string(field) + " ";
    const auto pos = log.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    return std::stoi(log.substr(pos + needle.size()));
}

// Finds `label` in `log` and parses the (possibly signed, possibly
// fractional) number immediately following it, skipping whitespace in
// between - `ac3cli qc`'s own report lines are "label<spaces>value...", laid
// out with std::format field widths this deliberately does not need to know:
// skipping runs of whitespace instead of a fixed offset means a column-width
// tweak in main.cpp can never silently break these tests the way a
// fixed-offset substring slice would.
std::optional<double> value_after(const std::string& log, std::string_view label) {
    const auto pos = log.find(label);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    std::size_t i = pos + label.size();
    while (i < log.size() && std::isspace(static_cast<unsigned char>(log[i])) != 0) {
        ++i;
    }
    // std::from_chars for floating point does not accept a leading '+' (only
    // '-' is part of its grammar) - qc's own report uses {:+.2f} throughout
    // so every positive number is printed with one, and it has to be
    // skipped here rather than included in the range handed to from_chars
    // below, unlike '-' which from_chars parses directly.
    if (i < log.size() && log[i] == '+') {
        ++i;
    }
    const std::size_t start = i;
    if (i < log.size() && log[i] == '-') {
        ++i;
    }
    while (i < log.size() &&
          (std::isdigit(static_cast<unsigned char>(log[i])) != 0 || log[i] == '.')) {
        ++i;
    }
    if (i == start) {
        return std::nullopt;
    }
    double value = 0.0;
    const auto [ptr, ec] = std::from_chars(log.data() + static_cast<std::ptrdiff_t>(start),
                                           log.data() + static_cast<std::ptrdiff_t>(i), value);
    (void)ptr;
    return ec == std::errc{} ? std::optional<double>(value) : std::nullopt;
}

// The PASS/FAIL of the first "verdict: " line at or after `from` - qc's own
// per-preset overall verdict (as opposed to its two sub-verdicts, loudness
// and true peak, which this deliberately does not match on).
std::optional<bool> gate_verdict_after(const std::string& log, std::size_t from) {
    if (from == std::string::npos) {
        return std::nullopt;
    }
    constexpr std::string_view kNeedle = "verdict: ";
    const auto pos = log.find(kNeedle, from);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    const auto value_pos = pos + kNeedle.size();
    if (log.compare(value_pos, 4, "PASS") == 0) {
        return true;
    }
    if (log.compare(value_pos, 4, "FAIL") == 0) {
        return false;
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("offset= rejects malformed tokens", "[cli][offset]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "offset_parse_in.wav";
    const auto channels = make_tone_channels(2, 4000, 48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    SECTION("missing colon") {
        const auto out_path = dir / "offset_parse_colon.ac3";
        const auto log = dir / "offset_parse_colon.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo offset=1.5",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("offset=") != std::string::npos);
    }

    SECTION("non-numeric source index") {
        const auto out_path = dir / "offset_parse_index.ac3";
        const auto log = dir / "offset_parse_index.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo offset=x:1.0",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("offset=") != std::string::npos);
    }

    SECTION("non-numeric seconds") {
        const auto out_path = dir / "offset_parse_seconds.ac3";
        const auto log = dir / "offset_parse_seconds.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo offset=0:soon",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("offset=") != std::string::npos);
    }

    SECTION("negative seconds") {
        const auto out_path = dir / "offset_parse_negative.ac3";
        const auto log = dir / "offset_parse_negative.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo offset=0:-1",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("offset=") != std::string::npos);
    }

    SECTION("well-formed offset= parses and runs") {
        const auto out_path = dir / "offset_parse_ok.ac3";
        const auto log = dir / "offset_parse_ok.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo offset=0:0.1",
                                log);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
    }
}

TEST_CASE("eac3-encode verify runs the mirror self-check", "[cli][verify]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "verify_in.wav";
    // Six blocks a frame, several frames: the recorded lesson is that frame 0
    // alone gives a false pass, and the tools this exercises (coupling,
    // spectral extension, AHT) say nothing at all on silence.
    const auto channels = make_tone_channels(6, 48000 / 4, 48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    SECTION("a clean encode reports the check and still writes the stream") {
        const auto out_path = dir / "verify_ok.ec3";
        const auto log = dir / "verify_ok.log";
        fs::remove(out_path);
        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 192 all 51 verify",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
        CHECK(text.find("verify: encoder and decoder agree") != std::string::npos);
    }

    SECTION("the same encode without the option says nothing about it") {
        // Named without the word "verify": the paths themselves are echoed
        // into the log, and a filename carrying the word would satisfy the
        // absence check below on its own.
        const auto out_path = dir / "plain_encode.ec3";
        const auto log = dir / "plain_encode.log";
        fs::remove(out_path);
        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 192 all 51",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(text.find("verify") == std::string::npos);
    }

    SECTION("it reaches the tools with no external oracle, and 7.1.4") {
        const auto out_path = dir / "verify_ecpl.ec3";
        const auto log = dir / "verify_ecpl.log";
        fs::remove(out_path);
        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 448 cpl+ecpl+tpn 714 verify",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(text.find("verify: encoder and decoder agree") != std::string::npos);
    }
}

TEST_CASE("fast-mdct is default-on with =off as the negation", "[cli][fast-mdct]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "fastmdct_in.wav";
    const auto channels = make_tone_channels(2, 4000, 48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    SECTION("fast-mdct=off encodes down the direct path") {
        const auto out_path = dir / "fastmdct_off.ac3";
        const auto log = dir / "fastmdct_off.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo fast-mdct=off",
                                log);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
    }

    SECTION("the bare opt-in word from the default-off era still parses") {
        const auto out_path = dir / "fastmdct_bare.ac3";
        const auto log = dir / "fastmdct_bare.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo fast-mdct",
                                log);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
    }

    SECTION("any value other than off is refused, not ignored") {
        const auto out_path = dir / "fastmdct_bad.ac3";
        const auto log = dir / "fastmdct_bad.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo fast-mdct=fast",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("fast-mdct") != std::string::npos);
    }

    SECTION("eac3 spells it tools=nofastmdct, and the old fastmdct token is a no-op") {
        const auto off_path = dir / "fastmdct_eac3_off.ec3";
        const auto log = dir / "fastmdct_eac3.log";
        fs::remove(off_path);
        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    off_path.string() + "\" 192 nofastmdct stereo",
                                log);
        CHECK(rc == 0);
        CHECK(fs::exists(off_path));

        const auto legacy_path = dir / "fastmdct_eac3_legacy.ec3";
        fs::remove(legacy_path);
        const auto rc2 = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                     legacy_path.string() + "\" 192 fastmdct stereo",
                                 log);
        CHECK(rc2 == 0);
        CHECK(fs::exists(legacy_path));
    }

    SECTION("eac3-encode also honors fast-mdct=off directly, not just tools=nofastmdct") {
        const auto out_path = dir / "fastmdct_eac3_direct_off.ec3";
        const auto log = dir / "fastmdct_eac3_direct.log";
        fs::remove(out_path);
        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 192 none stereo fast-mdct=off",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
        // format_tools() only ever emits "nofastmdct" (never a positive "fastmdct"
        // token), so its presence here is proof fast-mdct=off actually reached
        // Tools::fast_mdct rather than merely parsing without effect.
        CHECK(text.find("nofastmdct") != std::string::npos);
    }

    SECTION("dither=off reaches an AC-3 encode, and nodither reaches E-AC-3's tools=") {
        // AC-3 has no tools= string, so dither=off (support.hpp's
        // Options::dither) is its equivalent - the same relationship
        // fast-mdct=off already has to eac3-encode's bare nofastmdct token.
        const auto ac3_path = dir / "nodither.ac3";
        const auto ac3_log = dir / "nodither_ac3.log";
        fs::remove(ac3_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + ac3_path.string() +
                                    "\" 192 stereo dither=off",
                                ac3_log);
        CHECK(rc == 0);
        CHECK(fs::exists(ac3_path));

        const auto eac3_path = dir / "nodither.ec3";
        const auto eac3_log = dir / "nodither_eac3.log";
        fs::remove(eac3_path);
        const auto rc2 = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                     eac3_path.string() + "\" 192 nodither stereo",
                                 eac3_log);
        const auto text = read_log(eac3_log);
        INFO(text);
        CHECK(rc2 == 0);
        CHECK(fs::exists(eac3_path));
        CHECK(text.find("nodither") != std::string::npos);
    }

    SECTION("dither=on is refused, not ignored - only off is a value 'dither' takes") {
        const auto out_path = dir / "dither_bad.ac3";
        const auto log = dir / "dither_bad.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo dither=on",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("dither") != std::string::npos);
    }

    SECTION("eac3-encode-multi (src=/map=) also honors fast-mdct=off directly") {
        const auto out_path = dir / "fastmdct_eac3_multi_off.ec3";
        const auto log = dir / "fastmdct_eac3_multi.log";
        fs::remove(out_path);
        // wav_path is 2-channel; parse_assignment requires a token for every
        // channel of every source, so the reused second copy needs its own
        // (unused) channels explicitly marked none rather than left absent.
        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() +
                                    "\" 192 none stereo src=\"" + wav_path.string() +
                                    "\" map=0.0:L,0.1:R,1.0:none,1.1:none fast-mdct=off",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
        CHECK(text.find("nofastmdct") != std::string::npos);
    }

    SECTION("the [tools] positional still wins when fast-mdct=off is also given") {
        const auto out_path = dir / "fastmdct_eac3_precedence.ec3";
        const auto log = dir / "fastmdct_eac3_precedence.log";
        fs::remove(out_path);
        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 192 fastmdct stereo fast-mdct=off",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
        // meta.fast_mdct=false (fast-mdct=off) seeds p.tools.fast_mdct, but
        // parse_tools() runs after and overwrites the field on a literal
        // fastmdct/nofastmdct token - so the explicit tools token wins.
        CHECK(text.find("nofastmdct") == std::string::npos);
    }

    SECTION("eac3-sine has no [tools] argument, but honors fast-mdct=off directly") {
        const auto out_path = dir / "fastmdct_eac3_sine_off.ec3";
        const auto log = dir / "fastmdct_eac3_sine.log";
        fs::remove(out_path);
        const auto rc = run_cli("eac3-sine \"" + out_path.string() + "\" 1 192 1000 50 stereo "
                                    "fast-mdct=off",
                                log);
        INFO(read_log(log));
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
    }
}

// capture2= is 'live'-only, but its rejection happens in parse_options,
// before Needs::kCapture is even checked (see run_main: parse_options runs
// on the whole trailing-options span before the per-command needs gate) -
// so a malformed token is refused the same way regardless of which command
// carries it, and 'encode' (Needs::kNothing) lets this run without a real
// capture device, the same way the offset= tests above do.
TEST_CASE("the bit stream information tokens reach the wire and round trip",
          "[cli][bsi]") {
    // The library-level round trips live in tests/meta/test_bsi.cpp; what is
    // only reachable here is the GRAMMAR - whether a spelling a person would
    // actually type parses at all. That is a distinct failure: `extpgmscl=+3`
    // parsed everywhere except the command line, because std::from_chars does
    // not accept the leading + a signed decibel figure reads with.
    const auto dir = scratch_dir();
    const auto wav_path = dir / "bsi_in.wav";
    const auto channels = make_tone_channels(6, 3000, 48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    SECTION("AC-3: the informational fields and Annex D come back off the wire") {
        const auto out_path = dir / "bsi_annexd.ac3";
        const auto log = dir / "bsi_annexd.log";
        fs::remove(out_path);
        REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                            "\" 384 51 dmixmod=ltrt ltrtcmixlev=-1.5 lorosurmixlev=off "
                            "dsurexmod=ex adconvtyp=hdcd bsmod=vi mixlevel=105 roomtyp=large "
                            "copyright origbs=off langcod",
                        log) == 0);
        REQUIRE(fs::exists(out_path));

        const auto decode_log = dir / "bsi_annexd_decode.log";
        REQUIRE(run_cli("decode \"" + out_path.string() + "\" \"" +
                            (dir / "bsi_annexd.wav").string() + "\"",
                        decode_log) == 0);
        const auto text = read_log(decode_log);
        INFO(text);
        // dmixmod= alone should have selected bsid 6: on AC-3 there is nowhere
        // else for a preferred downmix to go.
        CHECK(text.find("bsid 6") != std::string::npos);
        CHECK(text.find("visually impaired") != std::string::npos);
        CHECK(text.find("105 dB SPL") != std::string::npos);
        CHECK(text.find("large room") != std::string::npos);
        CHECK(text.find("copyright asserted") != std::string::npos);
        CHECK(text.find("not the original bit stream") != std::string::npos);
        CHECK(text.find("Dolby Surround EX") != std::string::npos);
        CHECK(text.find("A/D converter: HDCD") != std::string::npos);
    }

    SECTION("AC-3: a time code and Annex D are refused together") {
        const auto out_path = dir / "bsi_clash.ac3";
        const auto log = dir / "bsi_clash.log";
        fs::remove(out_path);
        CHECK(run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                          "\" 384 51 annexd timecode=01:02:03",
                      log) != 0);
        CHECK_FALSE(fs::exists(out_path));
    }

    SECTION("AC-3: a time code alone round trips through both halves") {
        const auto out_path = dir / "bsi_timecode.ac3";
        const auto log = dir / "bsi_timecode.log";
        fs::remove(out_path);
        REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                            "\" 256 51 timecode=17:43:46:21.39",
                        log) == 0);
        const auto decode_log = dir / "bsi_timecode_decode.log";
        REQUIRE(run_cli("decode \"" + out_path.string() + "\" \"" +
                            (dir / "bsi_timecode.wav").string() + "\"",
                        decode_log) == 0);
        const auto text = read_log(decode_log);
        INFO(text);
        CHECK(text.find("timecode: 17:43:46:21.39") != std::string::npos);
    }

    SECTION("E-AC-3: the mixmdate depth and infomdat come back off the wire") {
        const auto out_path = dir / "bsi_mixdepth.ec3";
        const auto log = dir / "bsi_mixdepth.log";
        fs::remove(out_path);
        // extpgmscl=+3 is the spelling this section exists for; pgmscl=-6 is
        // the other sign, and mute is the third form.
        REQUIRE(run_cli("eac3-encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                            "\" 448 none 51 mixmeta pgmscl=-6 extpgmscl=+3 mixdef=ext "
                            "premixcmp=compr:local:2 extmix=0,2,0,5,5,off,7 auxmix=1,off "
                            "speechmix=9,3:1,4:5 blkmixcfg=3,-,7,-,-,31 bsmod=commentary "
                            "dsurexmod=pliiz mixlevel=98 roomtyp=small sourcefscod",
                        log) == 0);
        REQUIRE(fs::exists(out_path));

        const auto decode_log = dir / "bsi_mixdepth_decode.log";
        REQUIRE(run_cli("decode \"" + out_path.string() + "\" \"" +
                            (dir / "bsi_mixdepth.wav").string() + "\"",
                        decode_log) == 0);
        const auto text = read_log(decode_log);
        INFO(text);
        CHECK(text.find("commentary") != std::string::npos);
        CHECK(text.find("Pro Logic IIz") != std::string::npos);
        CHECK(text.find("98 dB SPL") != std::string::npos);
        CHECK(text.find("programme scale: -6 dB") != std::string::npos);
        CHECK(text.find("external programme scale: +3 dB") != std::string::npos);
        CHECK(text.find("mixdef 3") != std::string::npos);
        CHECK(text.find("per-block mixing configuration") != std::string::npos);
        CHECK(text.find("twice the coded rate") != std::string::npos);
    }

    SECTION("a value outside its field is refused rather than truncated") {
        const auto out_path = dir / "bsi_bad.ac3";
        const auto log = dir / "bsi_bad.log";
        for (const auto* token : {"mixlevel=120", "bsmod=8", "roomtyp=huge",
                                  "timecode=25:00:00", "pgmscl=+20", "blkmixcfg=1,2,3",
                                  "extmix=0,2,0,5,5,16", "paninfo=240",
                                  "premixcmp=dynrng:external:9"}) {
            fs::remove(out_path);
            INFO(token);
            CHECK(run_cli("eac3-encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                              "\" 448 none 51 " + token,
                          log) != 0);
            CHECK_FALSE(fs::exists(out_path));
        }
    }
}

TEST_CASE("capture2= rejects malformed tokens", "[cli][capture2]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "capture2_parse_in.wav";
    const auto channels = make_tone_channels(2, 4000, 48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    SECTION("non-numeric value") {
        const auto out_path = dir / "capture2_parse_nonnumeric.ac3";
        const auto log = dir / "capture2_parse_nonnumeric.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo capture2=x",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("capture2=") != std::string::npos);
    }

    SECTION("negative value") {
        const auto out_path = dir / "capture2_parse_negative.ac3";
        const auto log = dir / "capture2_parse_negative.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo capture2=-1",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("capture2=") != std::string::npos);
    }

    SECTION("empty value") {
        const auto out_path = dir / "capture2_parse_empty.ac3";
        const auto log = dir / "capture2_parse_empty.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo capture2=",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("capture2=") != std::string::npos);
    }
}

// container= is 'record'/'live'-only, but like capture2= above its rejection
// happens in parse_options, before either command's own logic ever runs - so
// 'encode' (Needs::kNothing) can exercise the parsing without a real capture
// device, the same reasoning capture2='s own test comment gives.
TEST_CASE("container= rejects malformed tokens and accepts the two real ones",
          "[cli][container]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "container_parse_in.wav";
    const auto channels = make_tone_channels(2, 4000, 48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    SECTION("an unrecognised value is refused, not silently ignored") {
        const auto out_path = dir / "container_parse_bad.ac3";
        const auto log = dir / "container_parse_bad.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo container=avi",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK(read_log(log).find("container") != std::string::npos);
    }

    SECTION("container=mkv parses (encode itself never reads it, so the run still succeeds "
           "and writes the plain elementary stream)") {
        const auto out_path = dir / "container_parse_mkv.ac3";
        const auto log = dir / "container_parse_mkv.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo container=mkv",
                                log);
        INFO(read_log(log));
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
    }

    SECTION("container=raw parses, spelling out the default") {
        const auto out_path = dir / "container_parse_raw.ac3";
        const auto log = dir / "container_parse_raw.log";
        fs::remove(out_path);
        const auto rc = run_cli("encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                                    "\" 192 stereo container=raw",
                                log);
        INFO(read_log(log));
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
    }
}

TEST_CASE("offset= applies leading silence and grows the programme", "[cli][offset]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "offset_apply_in.wav";
    constexpr std::size_t kFrames = 4000;
    constexpr std::uint32_t kSampleRate = 48000;
    const auto channels = make_tone_channels(2, kFrames, kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    // Without offset=: the classic single-file path, untouched by this
    // feature - the programme is exactly as long as the source.
    const auto plain_ac3 = dir / "offset_apply_plain.ac3";
    const auto plain_wav = dir / "offset_apply_plain_decoded.wav";
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + plain_ac3.string() +
                        "\" 192 stereo",
                    dir / "offset_apply_plain_encode.log") == 0);
    REQUIRE(run_cli("decode \"" + plain_ac3.string() + "\" \"" + plain_wav.string() + "\"",
                    dir / "offset_apply_plain_decode.log") == 0);
    const auto plain_decoded = ac3::io::read_wav(plain_wav.string());
    REQUIRE(plain_decoded.has_value());

    // With offset=0:0.5: 0.5 s (24000 samples at 48 kHz) of leading silence
    // ahead of the source's own audio.
    constexpr double kOffsetSeconds = 0.5;
    constexpr std::size_t kOffsetSamples = static_cast<std::size_t>(kOffsetSeconds * kSampleRate);
    const auto offset_ac3 = dir / "offset_apply_offset.ac3";
    const auto offset_wav = dir / "offset_apply_offset_decoded.wav";
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + offset_ac3.string() +
                        "\" 192 stereo offset=0:0.5",
                    dir / "offset_apply_offset_encode.log") == 0);
    REQUIRE(run_cli("decode \"" + offset_ac3.string() + "\" \"" + offset_wav.string() + "\"",
                    dir / "offset_apply_offset_decode.log") == 0);
    const auto offset_decoded = ac3::io::read_wav(offset_wav.string());
    REQUIRE(offset_decoded.has_value());

    // The programme is still as long as the longest source once the offset
    // is applied - strictly longer than the same encode without offset=,
    // covering the leading silence PLUS the source's own length, not just
    // the source's own raw length.
    CHECK(offset_decoded->frame_count() > plain_decoded->frame_count());
    CHECK(offset_decoded->frame_count() >= kOffsetSamples + kFrames);

    // Leading kOffsetSamples: silence (a lossy lower bound, not exact zero).
    for (const auto& channel : offset_decoded->channels) {
        CHECK(rms(channel, 0, kOffsetSamples) < 0.02);
    }
    // Just past the offset boundary: the tone is playing again.
    for (const auto& channel : offset_decoded->channels) {
        CHECK(rms(channel, kOffsetSamples + 200, 1000) > 0.1);
    }
}

TEST_CASE("atmos-encode with a keyframes file authors motion", "[cli][atmos-encode]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "atmos_encode_paths_in.wav";
    // Four full 1536-sample frames (128 ms @ 48 kHz) so every frame the
    // per-frame loop visits is a real, unpadded one - six channels, one
    // object per channel, matching atmos-encode's own default addressing
    // (object index == WAV channel index), the same numbering the GUI's
    // exportObjectPaths keys its file by.
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::size_t kFrames = 4 * static_cast<std::size_t>(ac3::kSamplesPerFrame);
    const auto channels = make_tone_channels(6, kFrames, kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    // Object 0 authored to sweep across the room over the clip; every other
    // object index is left unmentioned, so it should keep atmos-encode's own
    // default static placement (see run_atmos_encode's fallback).
    const auto paths_path = dir / "atmos_encode_paths.txt";
    {
        std::ofstream paths{paths_path};
        REQUIRE(paths.is_open());
        paths << "0 0.0  0.9 0.9 0.0  0.6 0.0\n";
        paths << "0 0.09 0.1 0.1 0.0  0.6 0.0\n";
    }

    const auto static_ec3 = dir / "atmos_encode_static.ec3";
    const auto motion_ec3 = dir / "atmos_encode_motion.ec3";

    const auto static_rc =
        run_cli("atmos-encode \"" + wav_path.string() + "\" \"" + static_ec3.string() +
                    "\" 448 6",
                dir / "atmos_encode_static.log");
    const auto motion_rc =
        run_cli("atmos-encode \"" + wav_path.string() + "\" \"" + motion_ec3.string() +
                    "\" 448 6 \"" + paths_path.string() + "\"",
                dir / "atmos_encode_motion.log");

    INFO(read_log(dir / "atmos_encode_static.log"));
    CHECK(static_rc == 0);
    INFO(read_log(dir / "atmos_encode_motion.log"));
    CHECK(motion_rc == 0);
    REQUIRE(fs::exists(static_ec3));
    REQUIRE(fs::exists(motion_ec3));
    CHECK(fs::file_size(static_ec3) > 0);
    CHECK(fs::file_size(motion_ec3) > 0);

    // Authored motion changes the per-frame OAMD/JOC side data the static
    // run reuses unchanged every frame - the two streams must differ.
    std::ifstream static_in{static_ec3, std::ios::binary};
    std::ifstream motion_in{motion_ec3, std::ios::binary};
    const std::vector<char> static_bytes{std::istreambuf_iterator<char>{static_in},
                                         std::istreambuf_iterator<char>{}};
    const std::vector<char> motion_bytes{std::istreambuf_iterator<char>{motion_in},
                                         std::istreambuf_iterator<char>{}};
    // Compared as a bool, not the vectors themselves - CHECK'ing the
    // containers directly asks Catch2 to stringify both (every byte) for a
    // potential diff message, which for multi-KB streams is at best slow and
    // was observed to crash outright.
    const bool differs = static_bytes != motion_bytes;
    CHECK(differs);
}

// atmos-path's file argument used to be the keyframe grammar and only that.
// It now reads an ac3::oba::ObjectScene in JSON as well, told apart by the
// first character - so the two spellings of one scene have to encode to the
// same stream, byte for byte, or "migrate your file" would silently be a
// change to the mix.
TEST_CASE("atmos-path reads a keyframe file and its JSON form identically",
          "[cli][atmos-path][scene]") {
    const auto dir = scratch_dir();
    const auto keyframes_path = dir / "scene_equivalence.txt";
    // Every object the file will have is mentioned in it, so neither run
    // needs atmos-path's own fallback for an index the file skipped - this
    // test is about the two READERS agreeing, not about that policy.
    {
        std::ofstream keyframes{keyframes_path};
        REQUIRE(keyframes.is_open());
        keyframes << "# two objects crossing over\n";
        keyframes << "0 0.0  0.10 0.20  0.00  0.50 0.00\n";
        keyframes << "0 0.25 0.90 0.80  0.40  0.90 0.10   # arrives back right, high\n";
        keyframes << "\n";
        keyframes << "   0 0.40 0.50 0.50 -0.25  0.20 0.00\n";
        keyframes << "1 0.05 0.30 0.70  0.10  0.75 0.05\n";
        keyframes << "1 0.35 0.70 0.30 -0.10  0.25 0.00\n";
    }

    // The same scene through the library, saved as JSON.
    const auto scene_path = dir / "scene_equivalence.json";
    {
        std::ifstream in{keyframes_path, std::ios::binary};
        const std::string text{std::istreambuf_iterator<char>{in},
                               std::istreambuf_iterator<char>{}};
        const auto scene = ac3::oba::scene_from_text(text);
        REQUIRE(scene.has_value());
        REQUIRE(scene->object_count() == 2);
        std::ofstream json{scene_path, std::ios::binary};
        REQUIRE(json.is_open());
        json << ac3::oba::to_json(*scene);
    }

    const auto from_keyframes = dir / "scene_from_keyframes.ec3";
    const auto from_json = dir / "scene_from_json.ec3";
    const auto keyframes_rc =
        run_cli("atmos-path \"" + from_keyframes.string() + "\" \"" + keyframes_path.string() +
                    "\" 1 448 2",
                dir / "scene_from_keyframes.log");
    const auto json_rc = run_cli("atmos-path \"" + from_json.string() + "\" \"" +
                                     scene_path.string() + "\" 1 448 2",
                                 dir / "scene_from_json.log");
    INFO(read_log(dir / "scene_from_keyframes.log"));
    CHECK(keyframes_rc == 0);
    INFO(read_log(dir / "scene_from_json.log"));
    CHECK(json_rc == 0);
    REQUIRE(fs::exists(from_keyframes));
    REQUIRE(fs::exists(from_json));
    REQUIRE(fs::file_size(from_keyframes) > 0);

    std::ifstream a_in{from_keyframes, std::ios::binary};
    std::ifstream b_in{from_json, std::ios::binary};
    const std::vector<char> a{std::istreambuf_iterator<char>{a_in},
                              std::istreambuf_iterator<char>{}};
    const std::vector<char> b{std::istreambuf_iterator<char>{b_in},
                              std::istreambuf_iterator<char>{}};
    // Compared as a bool for the same reason the atmos-encode test above
    // does it: CHECK'ing multi-KB vectors asks Catch2 to stringify both.
    const bool identical = a == b;
    CHECK(identical);
}

TEST_CASE("atmos-path reports a bad scene file without writing one", "[cli][atmos-path][scene]") {
    const auto dir = scratch_dir();

    SECTION("a malformed keyframe line names the file and its line") {
        const auto path = dir / "scene_bad_line.txt";
        {
            std::ofstream out{path};
            out << "# fine\n0 0 0 0 0 1 0\n0 1 2 3\n";
        }
        const auto ec3 = dir / "scene_bad_line.ec3";
        const auto rc = run_cli("atmos-path \"" + ec3.string() + "\" \"" + path.string() + "\" 1",
                                dir / "scene_bad_line.log");
        CHECK(rc != 0);
        const auto log = read_log(dir / "scene_bad_line.log");
        INFO(log);
        CHECK(log.find(":3: expected 'object time_s x y z gain lfe_send'") != std::string::npos);
        CHECK_FALSE(fs::exists(ec3));
    }

    SECTION("a JSON scene this build cannot read says so") {
        const auto path = dir / "scene_bad.json";
        {
            std::ofstream out{path};
            out << R"({"ac3forge_scene": 1, "objects": [{"automation": [{"t": 0, "gian": 1}]}]})";
        }
        const auto ec3 = dir / "scene_bad_json.ec3";
        const auto rc = run_cli("atmos-path \"" + ec3.string() + "\" \"" + path.string() + "\" 1",
                                dir / "scene_bad_json.log");
        CHECK(rc != 0);
        const auto log = read_log(dir / "scene_bad_json.log");
        INFO(log);
        CHECK(log.find("unknown automation member 'gian'") != std::string::npos);
        CHECK_FALSE(fs::exists(ec3));
    }
}

// sign-objects/signing-key= used to be wired into 'atmos' only - 'atmos-path'
// and 'atmos-encode' accepted both flags (parse_options does not know which
// command it is parsing for) and silently ignored them. All three now call
// apply_object_signing, so all three should report a signed frame count.
// decode_signing_key() falls back to raw bytes for anything that is not
// valid base64 (see signing_key.hpp), so any non-empty file is a usable key
// here - the signature's own correctness is test_signing.cpp's concern, not
// this integration test's.
TEST_CASE("sign-objects reaches atmos, atmos-path and atmos-encode alike",
          "[cli][atmos][signing]") {
    const auto dir = scratch_dir();
    const auto key_path = dir / "sign_objects_test.key";
    {
        std::ofstream key{key_path, std::ios::binary};
        REQUIRE(key.is_open());
        key << "not-a-real-key-just-test-material";
    }
    const std::string signing_args =
        " sign-objects signing-key=\"" + key_path.string() + "\"";

    SECTION("atmos") {
        const auto out_path = dir / "sign_objects_atmos.ec3";
        const auto log = dir / "sign_objects_atmos.log";
        const auto rc =
            run_cli("atmos \"" + out_path.string() + "\" 1 448 2 4 objects" + signing_args, log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(text.find("signed") != std::string::npos);
    }

    SECTION("atmos-path") {
        const auto paths_path = dir / "sign_objects_paths.txt";
        {
            std::ofstream paths{paths_path};
            REQUIRE(paths.is_open());
            paths << "0 0.0 0.5 0.5 0.0 0.6 0.0\n";
        }
        const auto out_path = dir / "sign_objects_atmos_path.ec3";
        const auto log = dir / "sign_objects_atmos_path.log";
        const auto rc = run_cli("atmos-path \"" + out_path.string() + "\" \"" +
                                    paths_path.string() + "\" 1 448 1" + signing_args,
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(text.find("signed") != std::string::npos);
    }

    SECTION("atmos-encode") {
        const auto wav_path = dir / "sign_objects_in.wav";
        const auto channels = make_tone_channels(2, 4000, 48000);
        REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());
        const auto out_path = dir / "sign_objects_atmos_encode.ec3";
        const auto log = dir / "sign_objects_atmos_encode.log";
        const auto rc = run_cli("atmos-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 448 2" + signing_args,
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(text.find("signed") != std::string::npos);
    }

    SECTION("atmos-path without sign-objects stays unsigned, same as before") {
        // Deliberately not named "*unsigned*": that spelling contains
        // "signed" as a substring, which the assertion below would then
        // find in the echoed file path rather than in any real message.
        const auto paths_path = dir / "sign_objects_paths_plain.txt";
        {
            std::ofstream paths{paths_path};
            REQUIRE(paths.is_open());
            paths << "0 0.0 0.5 0.5 0.0 0.6 0.0\n";
        }
        const auto out_path = dir / "sign_objects_atmos_path_plain.ec3";
        const auto log = dir / "sign_objects_atmos_path_plain.log";
        const auto rc = run_cli("atmos-path \"" + out_path.string() + "\" \"" +
                                    paths_path.string() + "\" 1 448 1",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
        CHECK(text.find("signed") == std::string::npos);
    }
}

// verify-objects mirrors sign-objects' UX on the read side: decode/monitor
// check each frame's EMDF object signature against signing-key= (same option
// sign-objects already uses) instead of just decoding blind. This is a
// deliberate opt-in design (see docs/concepts/object-signing.md and
// Eac3Decoder's own stance that the protection field is opaque per spec) -
// the last SECTION here is the one that matters most: decoding the very same
// signed stream with no verify-objects token at all must succeed exactly as
// it always has, proving the bypass is real and not just documented.
TEST_CASE("verify-objects checks a decode against the signer's own tag",
          "[cli][atmos][signing][verify]") {
    const auto dir = scratch_dir();
    const auto key_path = dir / "verify_objects_test.key";
    const auto wrong_key_path = dir / "verify_objects_wrong.key";
    {
        std::ofstream key{key_path, std::ios::binary};
        REQUIRE(key.is_open());
        key << "not-a-real-key-just-test-material";
    }
    {
        std::ofstream key{wrong_key_path, std::ios::binary};
        REQUIRE(key.is_open());
        key << "a-completely-different-key-for-mismatch";
    }

    const auto signed_ec3 = dir / "verify_objects_signed.ec3";
    const auto signed_log = dir / "verify_objects_signed.log";
    const auto sign_rc =
        run_cli("atmos \"" + signed_ec3.string() +
                    "\" 1 448 2 4 objects sign-objects signing-key=\"" + key_path.string() + "\"",
                signed_log);
    REQUIRE(sign_rc == 0);
    REQUIRE(fs::exists(signed_ec3));

    SECTION("the same key verifies every frame and the decode succeeds") {
        const auto out_wav = dir / "verify_objects_ok.wav";
        const auto log = dir / "verify_objects_ok.log";
        const auto rc = run_cli("decode \"" + signed_ec3.string() + "\" \"" + out_wav.string() +
                                    "\" verify-objects signing-key=\"" + key_path.string() + "\"",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out_wav));
        CHECK(text.find("valid") != std::string::npos);
        CHECK(text.find("0 mismatched") != std::string::npos);
    }

    SECTION("a wrong key mismatches and the command refuses") {
        const auto out_wav = dir / "verify_objects_wrong_key.wav";
        const auto log = dir / "verify_objects_wrong_key.log";
        const auto rc = run_cli("decode \"" + signed_ec3.string() + "\" \"" + out_wav.string() +
                                    "\" verify-objects signing-key=\"" +
                                    wrong_key_path.string() + "\"",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc != 0);
        CHECK(text.find("mismatch") != std::string::npos);
    }

    SECTION("verify-objects with no key anywhere is a hard error, same shape as sign-objects") {
        const auto out_wav = dir / "verify_objects_no_key.wav";
        const auto log = dir / "verify_objects_no_key.log";
        const auto rc = run_cli(
            "decode \"" + signed_ec3.string() + "\" \"" + out_wav.string() + "\" verify-objects",
            log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc != 0);
        CHECK(text.find("needs a key") != std::string::npos);
    }

    SECTION("decoding the same signed stream WITHOUT verify-objects still succeeds - the "
           "bypass") {
        const auto out_wav = dir / "verify_objects_bypass.wav";
        const auto log = dir / "verify_objects_bypass.log";
        const auto rc =
            run_cli("decode \"" + signed_ec3.string() + "\" \"" + out_wav.string() + "\"", log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out_wav));
    }
}

// keep-partial (item 34): a bare trailing token, same style as heavy/
// mixmeta/sign-objects, that keeps whatever frames a failed encode already
// produced at <name>.partial.<ext> instead of discarding them - see
// write_partial_output in main.cpp. FrameError's own causes (kInvalidBitrate
// and friends - see silent_frame.hpp) are all checked against the fixed
// config (bitrate, tools, channel count, dialnorm...), never per-frame
// audio content, so for a GIVEN invocation either every frame fails
// (nothing to keep - frames stays empty) or none do; there is no reachable
// "some frames succeeded, then a later one failed" case through ac3cli's
// own command line today. These tests cover exactly what IS reachable: the
// token parses and is inert on a run that succeeds, and produces no
// spurious partial file on a run that fails with nothing yet encoded -
// matching EncoderController's own `keep_partial && !frames.empty()` guard
// for the GUI's equivalent preference.
TEST_CASE("keep-partial", "[cli][keep-partial]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "keep_partial_in.wav";
    const auto channels = make_tone_channels(6, 3 * static_cast<std::size_t>(ac3::kSamplesPerFrame),
                                             48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

    SECTION("is inert on a run that succeeds - no spurious partial file") {
        const auto out_path = dir / "keep_partial_ok.ec3";
        const auto partial_path = dir / "keep_partial_ok.partial.ec3";
        const auto log = dir / "keep_partial_ok.log";
        fs::remove(out_path);
        fs::remove(partial_path);

        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 448 cpl 51 keep-partial",
                                log);
        INFO(read_log(log));
        CHECK(rc == 0);
        CHECK(fs::exists(out_path));
        CHECK_FALSE(fs::exists(partial_path));
    }

    // 64 kbps with every Annex E tool on over a 5.1 bed cannot hold the
    // side information at all, let alone any mantissas - refused on the
    // very first frame, deterministically, regardless of the source
    // material (confirmed empirically: this is a config-level ceiling, not
    // a content-dependent one - see this test's own top comment).
    SECTION("a run that fails before any frame succeeds keeps nothing, "
           "keep-partial or not") {
        const auto out_path = dir / "keep_partial_fail.ec3";
        const auto partial_path = dir / "keep_partial_fail.partial.ec3";
        const auto log = dir / "keep_partial_fail.log";
        fs::remove(out_path);
        fs::remove(partial_path);

        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 64 all 51 keep-partial",
                                log);
        INFO(read_log(log));
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        CHECK_FALSE(fs::exists(partial_path));
    }

    SECTION("an unrecognised bare token still fails to parse - keep-partial "
           "is not silently accepting everything") {
        const auto out_path = dir / "keep_partial_typo.ec3";
        const auto log = dir / "keep_partial_typo.log";
        fs::remove(out_path);

        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 448 cpl 51 keep-partiel",
                                log);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
    }
}

// Found by tools/ci/fuzz_eac3_encoder_space.py (roadmap VX1) on its first
// sweep of the Annex E half sample rates.
//
// A nominal Table 5.18 bitrate and an Annex E `fscod2` half rate are each
// legal on their own everywhere else in the CLI, and nothing in its grammar
// marks the pair. But §E2.3.1.3's frmsiz is an 11-bit word count, so a
// syncframe can never exceed ac3::eac3::kMaxFrameWords words: above
// bitrate * kSamplesPerFrame / sample_rate / 16 words the combination is not
// expressible at all. AccessUnitEncoder reports that by building NO
// substreams rather than by failing construction, and run_eac3_encode used to
// meet the resulting channel_count() == 0 with an assert() - a clean (if
// causeless) refusal in a release build, an abort in any build with
// assertions live. Every layout was affected, and every one of the three half
// rates has legal Table 5.18 rates above its ceiling: 320 kbps at 16 kHz,
// 448 at 22.05 kHz, 512 at 24 kHz are the highest that fit.
//
// atmos-encode never took that path and always refused cleanly, which is why
// tools/ci/run_codec_matrix.sh - whose only WAV source is 48 kHz - could not
// have seen this.
TEST_CASE("eac3-encode refuses a rate frmsiz cannot signal at this sample rate",
          "[cli][eac3][frmsiz]") {
    const auto dir = scratch_dir();

    // The exact ceiling per Annex E half rate, and the first legal Table 5.18
    // rate above it. Written out rather than computed so a change to either
    // side of the arithmetic has to be stated here too.
    struct Probe {
        std::uint32_t sample_rate;
        std::uint32_t highest_that_fits;
        std::uint32_t first_that_does_not;
    };
    constexpr std::array<Probe, 3> kProbes{{{16000, 320, 384},
                                            {22050, 448, 512},
                                            {24000, 512, 576}}};

    for (const auto& probe : kProbes) {
        const auto tag = std::to_string(probe.sample_rate);
        INFO("sample rate " << tag);
        const auto wav_path = dir / ("frmsiz_in_" + tag + ".wav");
        const auto channels =
            make_tone_channels(2, 3 * static_cast<std::size_t>(ac3::kSamplesPerFrame),
                               probe.sample_rate);
        REQUIRE(
            ac3::io::write_wav_f32(wav_path.string(), channels, probe.sample_rate).has_value());

        // The highest rate that fits still encodes - so the check above is a
        // ceiling, not a blanket refusal of the half rates.
        {
            const auto out_path = dir / ("frmsiz_ok_" + tag + ".ec3");
            const auto log = dir / ("frmsiz_ok_" + tag + ".log");
            fs::remove(out_path);
            const auto rc =
                run_cli("eac3-encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                            "\" " + std::to_string(probe.highest_that_fits) + " none stereo",
                        log);
            INFO(read_log(log));
            CHECK(rc == 0);
            CHECK(fs::file_size(out_path) > 0);
        }

        // The first rate that does not is refused, not aborted.
        {
            const auto out_path = dir / ("frmsiz_over_" + tag + ".ec3");
            const auto log = dir / ("frmsiz_over_" + tag + ".log");
            fs::remove(out_path);
            const auto rc =
                run_cli("eac3-encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                            "\" " + std::to_string(probe.first_that_does_not) + " none stereo",
                        log);
            const auto text = read_log(log);
            INFO(text);
            CHECK(rc != 0);
            // What actually separates a refusal from the abort this replaced
            // is the MESSAGE, not the exit code: an assertion failure prints
            // its own text and never reaches the diagnosis below. The code
            // itself is deliberately not compared against an exact value -
            // std::system() hands back the child's own code on Windows but a
            // POSIX wait status elsewhere, where exit 1 arrives as 256, so
            // `rc == 1` would be a Windows-only assertion wearing a portable
            // face.
            CHECK(text.find("Assertion") == std::string::npos);
            CHECK(text.find("frmsiz") != std::string::npos);
            CHECK(text.find("words per syncframe") != std::string::npos);
            CHECK_FALSE(fs::exists(out_path));
        }

        // The multi-source entry point builds its own AccessUnitEncoder and
        // had its own copy of the same assert. A `map=` with no `src=` is
        // enough to route through it - run_eac3_encode hands off on either
        // token - and avoids the "more than one source needs map=" refusal a
        // bare second `src=` would meet first.
        {
            const auto out_path = dir / ("frmsiz_multi_" + tag + ".ec3");
            const auto log = dir / ("frmsiz_multi_" + tag + ".log");
            fs::remove(out_path);
            const auto rc =
                run_cli("eac3-encode \"" + wav_path.string() + "\" \"" + out_path.string() +
                            "\" " + std::to_string(probe.first_that_does_not) +
                            " none stereo off map=0.0:L,0.1:R",
                        log);
            const auto text = read_log(log);
            INFO(text);
            CHECK(rc != 0);
            CHECK(text.find("Assertion") == std::string::npos);
            CHECK(text.find("frmsiz") != std::string::npos);
            CHECK_FALSE(fs::exists(out_path));
        }
    }
}

TEST_CASE("bare heavy2 token turns on Ch2 heavy compression on a 1+1 encode",
          "[cli][encode][heavy2]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "heavy2_token_in.wav";
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::size_t kFrames = 4 * static_cast<std::size_t>(ac3::kSamplesPerFrame);
    // Two channels, one two-channel file: layout 1+1's "Ch1, Ch2 in one file"
    // shape (prepare_dual_mono_source), so no in2.wav positional is needed.
    // AC-3, not E-AC-3: run_decode's classic-AC3 path is the one that reports
    // compr2 presence (run_decode_eac3's dual-mono branch prints nothing about
    // metadata at all), so that is the path this test needs to observe through.
    const auto channels = make_tone_channels(2, kFrames, kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    const auto plain_ac3 = dir / "heavy2_token_plain.ac3";
    const auto plain_wav = dir / "heavy2_token_plain_decoded.wav";
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + plain_ac3.string() +
                        "\" 192 1+1",
                    dir / "heavy2_token_plain_encode.log") == 0);
    REQUIRE(run_cli("decode \"" + plain_ac3.string() + "\" \"" + plain_wav.string() + "\"",
                    dir / "heavy2_token_plain_decode.log") == 0);
    const auto plain_log = read_log(dir / "heavy2_token_plain_decode.log");
    INFO(plain_log);
    CHECK(plain_log.find("compr2 present") == std::string::npos);

    // Before the fix, a bare 'heavy2' token was not recognised as an option
    // by run_main's is_option classification (only parse_options itself knew
    // about it), so it fell through to encode's args[] instead - landing on
    // the optional in2.wav positional and making the command fail outright
    // (prepare_dual_mono_source rejects a 2-channel first file once a second
    // input path is given). Succeeding at all is therefore already part of
    // what this proves; 'compr2 present' in the decode is the rest.
    const auto heavy2_ac3 = dir / "heavy2_token_heavy2.ac3";
    const auto heavy2_wav = dir / "heavy2_token_heavy2_decoded.wav";
    const auto encode_rc =
        run_cli("encode \"" + wav_path.string() + "\" \"" + heavy2_ac3.string() +
                    "\" 192 1+1 heavy2",
                dir / "heavy2_token_heavy2_encode.log");
    INFO(read_log(dir / "heavy2_token_heavy2_encode.log"));
    REQUIRE(encode_rc == 0);
    const auto decode_rc = run_cli("decode \"" + heavy2_ac3.string() + "\" \"" +
                                       heavy2_wav.string() + "\"",
                                   dir / "heavy2_token_heavy2_decode.log");
    REQUIRE(decode_rc == 0);
    const auto heavy2_log = read_log(dir / "heavy2_token_heavy2_decode.log");
    INFO(heavy2_log);
    CHECK(heavy2_log.find("compr2 present") != std::string::npos);
}

// The "-" stdin/stdout convention (roadmap item A4): 'ac3cli encode - -'
// reads the WAV from stdin and writes AC-3 to stdout instead of opening
// files by those literal names, and 'decode - -' the same in reverse - see
// is_stdio_path() in main.cpp. This is also the binary-safety proof
// CONTRIBUTING's validation discipline asks for: on Windows, std::cin/
// std::cout default to TEXT mode, which would either corrupt the compressed
// stream (0x0A -> 0x0D 0x0A) or truncate it early (a stray 0x1A read back as
// EOF) the moment either byte value appears - and in four frames of a real,
// non-silent 5.1 tone at 448 kbps, both values appear many times over. So a
// missing or wrong platform/stdio_binary.hpp call shows up here either as a
// decode failure or as a byte mismatch against the file-based reference,
// not as a subtle level difference.
TEST_CASE("encode/decode round trip through '-' matches the file-based one", "[cli][stdio]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::size_t kFrames = 4 * static_cast<std::size_t>(ac3::kSamplesPerFrame);
    const auto channels = make_tone_channels(6, kFrames, kSampleRate);
    const auto wav_path = dir / "stdio_roundtrip_in.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    // Reference: the classic, all-file round trip.
    const auto file_ac3 = dir / "stdio_roundtrip_file.ac3";
    const auto file_wav = dir / "stdio_roundtrip_file_decoded.wav";
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + file_ac3.string() +
                        "\" 448 couple",
                    dir / "stdio_roundtrip_file_encode.log") == 0);
    REQUIRE(run_cli("decode \"" + file_ac3.string() + "\" \"" + file_wav.string() + "\"",
                    dir / "stdio_roundtrip_file_decode.log") == 0);
    const auto file_decoded = ac3::io::read_wav(file_wav.string());
    REQUIRE(file_decoded.has_value());

    // Same round trip, but '-' stands in for both paths at each step: the
    // WAV goes in over stdin and the AC-3 comes out over stdout, then that
    // AC-3 goes back in over stdin and the decoded WAV comes out over
    // stdout.
    const auto stdio_ac3 = dir / "stdio_roundtrip_stdio.ac3";
    const auto stdio_wav = dir / "stdio_roundtrip_stdio_decoded.wav";
    const auto encode_rc = run_cli_stdio("encode - - 448 couple", wav_path, stdio_ac3,
                                         dir / "stdio_roundtrip_encode.log");
    INFO(read_log(dir / "stdio_roundtrip_encode.log"));
    REQUIRE(encode_rc == 0);
    const auto decode_rc =
        run_cli_stdio("decode - -", stdio_ac3, stdio_wav, dir / "stdio_roundtrip_decode.log");
    INFO(read_log(dir / "stdio_roundtrip_decode.log"));
    REQUIRE(decode_rc == 0);

    // The elementary AC-3 stream must be byte-identical either way - "-" is
    // a routing change at the argument-parsing layer, not a different
    // encode path. Compared as a bool first, not the vectors themselves -
    // see the atmos-encode test above for why (Catch2 stringifying a
    // multi-KB mismatch for the diff message is slow and was observed to
    // crash outright).
    std::ifstream file_ac3_in{file_ac3, std::ios::binary};
    std::ifstream stdio_ac3_in{stdio_ac3, std::ios::binary};
    const std::vector<char> file_ac3_bytes{std::istreambuf_iterator<char>{file_ac3_in},
                                           std::istreambuf_iterator<char>{}};
    const std::vector<char> stdio_ac3_bytes{std::istreambuf_iterator<char>{stdio_ac3_in},
                                            std::istreambuf_iterator<char>{}};
    REQUIRE_FALSE(file_ac3_bytes.empty());
    const bool ac3_matches = file_ac3_bytes == stdio_ac3_bytes;
    CHECK(ac3_matches);

    // And the decoded PCM must match too, sample for sample.
    const auto stdio_decoded = ac3::io::read_wav(stdio_wav.string());
    REQUIRE(stdio_decoded.has_value());
    REQUIRE(stdio_decoded->channels.size() == file_decoded->channels.size());
    CHECK(stdio_decoded->sample_rate == file_decoded->sample_rate);
    const bool pcm_matches = stdio_decoded->channels == file_decoded->channels;
    CHECK(pcm_matches);
}

// eac3-encode and atmos-encode share run_encode/run_decode's read_wav_arg/
// write_frames helpers, so "-" reaches them too (see main.cpp's kCommands
// table and this task's own scope note) - covered separately from the
// encode/decode round trip above since each has its own positional argument
// shape (tools/layout for eac3-encode, objects for atmos-encode).
TEST_CASE("eac3-encode and atmos-encode accept '-' for input and output", "[cli][stdio]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::size_t kFrames = 3 * static_cast<std::size_t>(ac3::kSamplesPerFrame);
    const auto channels = make_tone_channels(6, kFrames, kSampleRate);
    const auto wav_path = dir / "stdio_eac3_atmos_in.wav";
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    const auto eac3_out = dir / "stdio_eac3_out.ec3";
    const auto eac3_rc = run_cli_stdio("eac3-encode - - 384 cpl 51", wav_path, eac3_out,
                                       dir / "stdio_eac3.log");
    INFO(read_log(dir / "stdio_eac3.log"));
    CHECK(eac3_rc == 0);
    CHECK(fs::file_size(eac3_out) > 0);

    const auto atmos_out = dir / "stdio_atmos_out.ec3";
    const auto atmos_rc = run_cli_stdio("atmos-encode - - 448 6", wav_path, atmos_out,
                                        dir / "stdio_atmos.log");
    INFO(read_log(dir / "stdio_atmos.log"));
    CHECK(atmos_rc == 0);
    CHECK(fs::file_size(atmos_out) > 0);
}

// A CLI-docs audit ahead of v0.6.0-beta.1 found that "everything but the
// encoded stream goes to stderr, so a '-' pipe is never corrupted" was false
// in two places, neither caught by the round-trip test above because it
// never turns dialnorm=auto or src=/map= on: finish_measurement() (behind
// every dialnorm=auto/dialnorm2=auto path) printed its "measured N LKFS ->
// dialnorm M" line with the no-stream fmt::println overload, which always
// targets stdout regardless of out_path; and run_eac3_encode_multi/
// run_encode_multi's own final summary/routing report used that same
// unconditional stdout instead of threading status_stream(out_path) through
// the way their single-source run_eac3_encode/run_encode siblings already
// did. Both are silent with a real file out_path - the leaked text just
// lands in the terminal beside the file - so this only shows up once
// out_path is "-" and that leaked text starts sharing stdout with the
// encoded stream itself, which is exactly what each SECTION below pipes
// through. The assertion is the same byte-for-byte comparison the round-trip
// test above uses: any leaked line ahead of or after the stream makes the
// piped bytes longer than (and different from) the clean file-based
// reference, not merely "looks corrupted".
TEST_CASE("dialnorm=auto and src=/map= keep '-' output free of interleaved status text",
          "[cli][stdio][dialnorm][src]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 96000;  // 2s, same duration the src=/map= dialnorm
                                            // tests below use - long enough to clear the
                                            // BS.1770 absolute gate reliably.

    SECTION("dialnorm=auto alone: piped eac3-encode matches the file-based one") {
        const auto wav_path = dir / "stdio_dialnorm_in.wav";
        REQUIRE(write_wav(wav_path, {make_tone(0.9, 220.0, kFrames, kRate)}, kRate));

        const auto file_out = dir / "stdio_dialnorm_file.ec3";
        REQUIRE(run_cli("eac3-encode \"" + wav_path.string() + "\" \"" + file_out.string() +
                            "\" 96 none mono dialnorm=auto",
                        dir / "stdio_dialnorm_file.log") == 0);

        const auto piped_out = dir / "stdio_dialnorm_piped.ec3";
        const auto piped_log = dir / "stdio_dialnorm_piped.log";
        const auto rc = run_cli_stdio("eac3-encode - - 96 none mono dialnorm=auto", wav_path,
                                      piped_out, piped_log);
        INFO(read_log(piped_log));
        REQUIRE(rc == 0);

        std::ifstream file_in{file_out, std::ios::binary};
        std::ifstream piped_in{piped_out, std::ios::binary};
        const std::vector<char> file_bytes{std::istreambuf_iterator<char>{file_in},
                                           std::istreambuf_iterator<char>{}};
        const std::vector<char> piped_bytes{std::istreambuf_iterator<char>{piped_in},
                                            std::istreambuf_iterator<char>{}};
        REQUIRE_FALSE(file_bytes.empty());
        const bool matches = file_bytes == piped_bytes;
        CHECK(matches);
    }

    SECTION("src=/map=: piped E-AC-3 multi-source output matches the file-based one") {
        const auto in1 = dir / "stdio_src_eac3_a.wav";
        const auto in2 = dir / "stdio_src_eac3_b.wav";
        REQUIRE(write_wav(in1, {make_tone(0.9, 220.0, kFrames, kRate)}, kRate));
        REQUIRE(write_wav(in2, {make_tone(0.5, 660.0, kFrames, kRate)}, kRate));
        // Same option combination as one command, so the only thing that
        // differs between the file-based reference and the piped run below
        // is out_path itself ("<file>" vs the bare - token).
        const std::string args_tail =
            " 192 none stereo src=\"" + in2.string() + "\" map=0.0:L,1.0:R dialnorm=auto";

        const auto file_out = dir / "stdio_src_eac3_file.ec3";
        REQUIRE(run_cli("eac3-encode \"" + in1.string() + "\" \"" + file_out.string() + "\"" +
                            args_tail,
                        dir / "stdio_src_eac3_file.log") == 0);

        const auto piped_out = dir / "stdio_src_eac3_piped.ec3";
        const auto piped_log = dir / "stdio_src_eac3_piped.log";
        const auto rc = run_cli_stdout("eac3-encode \"" + in1.string() + "\" -" + args_tail,
                                       piped_out, piped_log);
        INFO(read_log(piped_log));
        REQUIRE(rc == 0);

        std::ifstream file_in{file_out, std::ios::binary};
        std::ifstream piped_in{piped_out, std::ios::binary};
        const std::vector<char> file_bytes{std::istreambuf_iterator<char>{file_in},
                                           std::istreambuf_iterator<char>{}};
        const std::vector<char> piped_bytes{std::istreambuf_iterator<char>{piped_in},
                                            std::istreambuf_iterator<char>{}};
        REQUIRE_FALSE(file_bytes.empty());
        const bool matches = file_bytes == piped_bytes;
        CHECK(matches);
    }

    SECTION("src=/map=: piped AC-3 multi-source output matches the file-based one") {
        const auto in1 = dir / "stdio_src_ac3_a.wav";
        const auto in2 = dir / "stdio_src_ac3_b.wav";
        REQUIRE(write_wav(in1, {make_tone(0.9, 220.0, kFrames, kRate)}, kRate));
        REQUIRE(write_wav(in2, {make_tone(0.5, 660.0, kFrames, kRate)}, kRate));
        // run_encode_multi's own copy of the same summary/routing report bug
        // - a separate function from run_eac3_encode_multi (the previous
        // SECTION), so it needs its own proof it was fixed too.
        const std::string args_tail =
            " 192 stereo src=\"" + in2.string() + "\" map=0.0:L,1.0:R dialnorm=auto";

        const auto file_out = dir / "stdio_src_ac3_file.ac3";
        REQUIRE(run_cli("encode \"" + in1.string() + "\" \"" + file_out.string() + "\"" +
                            args_tail,
                        dir / "stdio_src_ac3_file.log") == 0);

        const auto piped_out = dir / "stdio_src_ac3_piped.ac3";
        const auto piped_log = dir / "stdio_src_ac3_piped.log";
        const auto rc = run_cli_stdout("encode \"" + in1.string() + "\" -" + args_tail,
                                       piped_out, piped_log);
        INFO(read_log(piped_log));
        REQUIRE(rc == 0);

        std::ifstream file_in{file_out, std::ios::binary};
        std::ifstream piped_in{piped_out, std::ios::binary};
        const std::vector<char> file_bytes{std::istreambuf_iterator<char>{file_in},
                                           std::istreambuf_iterator<char>{}};
        const std::vector<char> piped_bytes{std::istreambuf_iterator<char>{piped_in},
                                            std::istreambuf_iterator<char>{}};
        REQUIRE_FALSE(file_bytes.empty());
        const bool matches = file_bytes == piped_bytes;
        CHECK(matches);
    }
}

// Roadmap C4: dialnorm=auto/dialnorm2=auto used to be unconditionally
// rejected the moment src=/map= was in play (main.cpp's old "not yet
// supported with src=/map=" error), regardless of whether the routing would
// have made measurement ambiguous. The fix routes/renders the whole
// programme once as a measurement pre-pass - the same BS.1770-4 gated pass
// the single-file path already runs - before the real per-frame encode loop
// renders it again to actually encode it. `loud`/`quiet` are two whole WAV
// files, at clearly different levels, so a bug that measured the wrong
// source (or blended both) reads as a clearly wrong number rather than a
// coincidental match.
TEST_CASE("dialnorm=auto/dialnorm2=auto measure the routed programme with src=/map=",
          "[cli][dialnorm][src]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 96000;  // 2s: several full 400ms BS.1770 gate windows

    const auto loud = dir / "dialnorm_src_loud.wav";
    const auto quiet = dir / "dialnorm_src_quiet.wav";
    REQUIRE(write_wav(loud, {make_tone(0.9, 220.0, kFrames, kRate)}, kRate));
    REQUIRE(write_wav(quiet, {make_tone(0.5, 660.0, kFrames, kRate)}, kRate));

    // Solo measurements: each source alone, through a plain mono target, is
    // the ground truth every assertion below compares against.
    const auto loud_solo_log = dir / "dialnorm_loud_solo.log";
    REQUIRE(run_cli("eac3-encode \"" + loud.string() + "\" \"" +
                        (dir / "loud_solo.ec3").string() + "\" 96 none mono dialnorm=auto",
                    loud_solo_log) == 0);
    const auto loud_solo = reported_value(read_log(loud_solo_log), "dialnorm");
    REQUIRE(loud_solo.has_value());

    const auto quiet_solo_log = dir / "dialnorm_quiet_solo.log";
    REQUIRE(run_cli("eac3-encode \"" + quiet.string() + "\" \"" +
                        (dir / "quiet_solo.ec3").string() + "\" 96 none mono dialnorm=auto",
                    quiet_solo_log) == 0);
    const auto quiet_solo = reported_value(read_log(quiet_solo_log), "dialnorm");
    REQUIRE(quiet_solo.has_value());

    // The two solo levels have to actually differ, or a blending bug and a
    // correct per-source measurement could print the same number by
    // accident and this test would prove nothing.
    REQUIRE(*loud_solo != *quiet_solo);

    SECTION("stereo target: matches an equivalent single-file measurement exactly") {
        // Independent 2-channel equivalent of the src=/map= run below (same
        // two tones, already in coded-channel order) - the strongest
        // cross-check available: a straight L/R map= carries bit-identical
        // audio, so the routed measurement must match this file's own
        // single-file measurement exactly, not merely "some number".
        const auto equiv = dir / "dialnorm_equiv_stereo.wav";
        REQUIRE(write_wav(equiv,
                          {make_tone(0.9, 220.0, kFrames, kRate),
                           make_tone(0.5, 660.0, kFrames, kRate)},
                          kRate));
        const auto equiv_log = dir / "dialnorm_equiv_stereo.log";
        REQUIRE(run_cli("eac3-encode \"" + equiv.string() + "\" \"" +
                            (dir / "equiv_stereo.ec3").string() +
                            "\" 192 none stereo dialnorm=auto",
                        equiv_log) == 0);
        const auto equiv_measured = reported_value(read_log(equiv_log), "dialnorm");
        REQUIRE(equiv_measured.has_value());

        const auto out = dir / "dialnorm_stereo.ec3";
        const auto log = dir / "dialnorm_stereo.log";
        const auto rc = run_cli("eac3-encode \"" + loud.string() + "\" \"" + out.string() +
                                    "\" 192 none stereo src=\"" + quiet.string() +
                                    "\" map=0.0:L,1.0:R dialnorm=auto",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out));
        // Was unconditionally rejected before this fix.
        CHECK(text.find("not yet supported") == std::string::npos);
        const auto measured = reported_value(text, "dialnorm");
        REQUIRE(measured.has_value());
        CHECK(*measured == *equiv_measured);
    }

    SECTION("map= trim is measured on the rendered programme, not the raw source levels") {
        const auto trimmed_out = dir / "dialnorm_multi_trimmed.ec3";
        const auto trimmed_log = dir / "dialnorm_multi_trimmed.log";
        REQUIRE(run_cli("eac3-encode \"" + loud.string() + "\" \"" + trimmed_out.string() +
                            "\" 192 none stereo src=\"" + quiet.string() +
                            "\" map=0.0:L@-12,1.0:R dialnorm=auto",
                        trimmed_log) == 0);
        const auto trimmed = reported_value(read_log(trimmed_log), "dialnorm");
        REQUIRE(trimmed.has_value());

        const auto untrimmed_out = dir / "dialnorm_multi_untrimmed.ec3";
        const auto untrimmed_log = dir / "dialnorm_multi_untrimmed.log";
        REQUIRE(run_cli("eac3-encode \"" + loud.string() + "\" \"" + untrimmed_out.string() +
                            "\" 192 none stereo src=\"" + quiet.string() +
                            "\" map=0.0:L,1.0:R dialnorm=auto",
                        untrimmed_log) == 0);
        const auto untrimmed = reported_value(read_log(untrimmed_log), "dialnorm");
        REQUIRE(untrimmed.has_value());

        // Attenuating the dominant (loud) source by -12 dB before measuring
        // must measurably quieten the programme (a bigger dialnorm number).
        // If this measured the raw, unrouted source files instead of the
        // rendered/trimmed ones, the trim would have no effect at all.
        CHECK(*trimmed > *untrimmed);
    }

    SECTION("1+1 target via src=/map=: each source's channel is measured on its own") {
        const auto out = dir / "dialnorm_dualmono_multi.ec3";
        const auto log = dir / "dialnorm_dualmono_multi.log";
        const auto rc = run_cli("eac3-encode \"" + loud.string() + "\" \"" + out.string() +
                                    "\" 192 none 1+1 src=\"" + quiet.string() +
                                    "\" map=0.0:p1,1.0:p2 dialnorm=auto dialnorm2=auto",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out));
        const auto ch1 = reported_value(text, "dialnorm");
        const auto ch2 = reported_value(text, "dialnorm2");
        REQUIRE(ch1.has_value());
        REQUIRE(ch2.has_value());
        // p1 came from `loud` alone, p2 from `quiet` alone - a correct,
        // per-programme measurement matches each source's own solo number
        // exactly; a blended or swapped measurement would not.
        CHECK(*ch1 == *loud_solo);
        CHECK(*ch2 == *quiet_solo);
    }
}

// Same fix, plain AC-3 (run_encode_multi rather than run_eac3_encode_multi) -
// a separate function in main.cpp with its own copy of the measurement
// pre-pass, so it needs its own proof it was actually fixed too.
TEST_CASE("dialnorm=auto works with src=/map= on the plain AC-3 encode path too",
          "[cli][dialnorm][src]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 96000;
    const auto loud = dir / "dialnorm_ac3_loud.wav";
    const auto quiet = dir / "dialnorm_ac3_quiet.wav";
    REQUIRE(write_wav(loud, {make_tone(0.9, 220.0, kFrames, kRate)}, kRate));
    REQUIRE(write_wav(quiet, {make_tone(0.5, 660.0, kFrames, kRate)}, kRate));

    const auto out = dir / "dialnorm_ac3_multi.ac3";
    const auto log = dir / "dialnorm_ac3_multi.log";
    const auto rc = run_cli("encode \"" + loud.string() + "\" \"" + out.string() +
                                "\" 192 stereo src=\"" + quiet.string() +
                                "\" map=0.0:L,1.0:R dialnorm=auto",
                            log);
    const auto text = read_log(log);
    INFO(text);
    CHECK(rc == 0);
    CHECK(fs::exists(out));
    CHECK(text.find("not yet supported") == std::string::npos);
    CHECK(reported_value(text, "dialnorm").has_value());
}

// route() only ever carries kLocation content into the routing it returns
// (see assignment.hpp's own comment on kObject/kObjectMono/kProgramme* rows
// contributing nothing) - map= itself happily parses a row aimed at obj (the
// destination is legal syntax, see kAssignmentSyntax), but this CLI has no
// object-assembly path of its own to catch what route() drops; that is the
// GUI's (encoder_controller.cpp's encodeObjects). Before this fix, such a
// channel's audio just vanished with no diagnostic at all; now
// routing_for_sources() warns about each row route() cannot carry, naming
// the source/channel and the destination that ate it.
TEST_CASE("map= to an object destination warns instead of silently discarding it",
          "[cli][src][obj]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 4 * static_cast<std::size_t>(ac3::kSamplesPerFrame);

    const auto primary = dir / "obj_warn_primary.wav";
    const auto extra = dir / "obj_warn_extra.wav";
    REQUIRE(write_wav(primary, {make_tone(0.5, 220.0, kFrames, kRate)}, kRate));
    REQUIRE(write_wav(extra, {make_tone(0.5, 660.0, kFrames, kRate)}, kRate));

    SECTION("channel 1.0 mapped to obj: warns, names the row, still encodes") {
        const auto out = dir / "obj_warn.ec3";
        const auto log = dir / "obj_warn.log";
        const auto rc = run_cli("eac3-encode \"" + primary.string() + "\" \"" + out.string() +
                                    "\" 192 none stereo src=\"" + extra.string() +
                                    "\" map=0.0:L,1.0:obj",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out));
        // Named: which row (source 1, channel 0) and what it resolved to.
        CHECK(text.find("1.0") != std::string::npos);
        CHECK(text.find("obj") != std::string::npos);
        CHECK(text.find("warning") != std::string::npos);
    }

    SECTION("a location-only map= stays exactly as quiet as before") {
        const auto out = dir / "obj_warn_quiet.ec3";
        const auto log = dir / "obj_warn_quiet.log";
        const auto rc = run_cli("eac3-encode \"" + primary.string() + "\" \"" + out.string() +
                                    "\" 192 none stereo src=\"" + extra.string() +
                                    "\" map=0.0:L,1.0:R",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(text.find("warning") == std::string::npos);
    }
}

// Roadmap C4's other half: dual mono (1+1) dialnorm=auto looked implemented
// already (measured_dialnorm_channel existed for Ch2), but Ch1's own
// measurement went through measured_dialnorm() with the target's acmod
// (kDualMono) instead - which runs a normal multi-channel BS.1770 pass
// across BOTH wav channels at once, silently reporting the combined loudness
// of Ch1+Ch2 as if they were a coherent stereo pair, rather than Ch1's own
// channel alone (§E1.3: the two programmes are unrelated and share no
// downmix). Ch1 and Ch2 are given comparable levels here specifically so
// that bug - a summed-power measurement roughly 3 dB louder than Ch1 alone -
// would read as a clearly different, clearly wrong dialnorm rather than a
// rounding nuance.
TEST_CASE("dialnorm=auto for 1+1 dual mono measures each programme's own channel, "
          "single-file path",
          "[cli][dialnorm][dual-mono]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 96000;

    const auto ch1_tone = make_tone(0.5, 220.0, kFrames, kRate);
    const auto ch2_tone = make_tone(0.5, 660.0, kFrames, kRate);

    const auto ch1_solo_path = dir / "dm_ch1_solo.wav";
    REQUIRE(write_wav(ch1_solo_path, {ch1_tone}, kRate));
    const auto ch1_log = dir / "dm_ch1_solo.log";
    REQUIRE(run_cli("eac3-encode \"" + ch1_solo_path.string() + "\" \"" +
                        (dir / "dm_ch1_solo.ec3").string() + "\" 96 none mono dialnorm=auto",
                    ch1_log) == 0);
    const auto ch1_solo = reported_value(read_log(ch1_log), "dialnorm");
    REQUIRE(ch1_solo.has_value());

    const auto ch2_solo_path = dir / "dm_ch2_solo.wav";
    REQUIRE(write_wav(ch2_solo_path, {ch2_tone}, kRate));
    const auto ch2_log = dir / "dm_ch2_solo.log";
    REQUIRE(run_cli("eac3-encode \"" + ch2_solo_path.string() + "\" \"" +
                        (dir / "dm_ch2_solo.ec3").string() + "\" 96 none mono dialnorm=auto",
                    ch2_log) == 0);
    const auto ch2_solo = reported_value(read_log(ch2_log), "dialnorm");
    REQUIRE(ch2_solo.has_value());

    const auto dualmono_path = dir / "dm_dualmono.wav";
    REQUIRE(write_wav(dualmono_path, {ch1_tone, ch2_tone}, kRate));

    SECTION("eac3-encode") {
        const auto out = dir / "dm_eac3.ec3";
        const auto log = dir / "dm_eac3.log";
        const auto rc = run_cli("eac3-encode \"" + dualmono_path.string() + "\" \"" +
                                    out.string() +
                                    "\" 192 none 1+1 dialnorm=auto dialnorm2=auto",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out));
        const auto ch1 = reported_value(text, "dialnorm");
        const auto ch2 = reported_value(text, "dialnorm2");
        REQUIRE(ch1.has_value());
        REQUIRE(ch2.has_value());
        CHECK(*ch1 == *ch1_solo);
        CHECK(*ch2 == *ch2_solo);
    }

    SECTION("encode (plain AC-3)") {
        const auto out = dir / "dm_ac3.ac3";
        const auto log = dir / "dm_ac3.log";
        const auto rc = run_cli("encode \"" + dualmono_path.string() + "\" \"" + out.string() +
                                    "\" 192 1+1 dialnorm=auto dialnorm2=auto",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(fs::exists(out));
        const auto ch1 = reported_value(text, "dialnorm");
        const auto ch2 = reported_value(text, "dialnorm2");
        REQUIRE(ch1.has_value());
        REQUIRE(ch2.has_value());
        CHECK(*ch1 == *ch1_solo);
        CHECK(*ch2 == *ch2_solo);
    }
}

// Roadmap C2: `ac3cli qc` - decode a stream, measure it with the real
// BS.1770-4 meter, and compare against the embedded dialnorm/compr and,
// optionally, a named delivery-spec gate. See main.cpp's run_qc/
// report_qc_programme and ac3/meta/qc.hpp for the implementation these tests
// exercise through the real, built binary (this file's own top comment on
// why: main.cpp has no library surface to link against directly).

TEST_CASE("qc preset= rejects an unknown name and accepts every real one", "[cli][qc]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "qc_preset_parse_in.wav";
    const auto channels =
        make_tone_channels(6, 4 * static_cast<std::size_t>(ac3::kSamplesPerFrame), 48000);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());
    const auto ac3_path = dir / "qc_preset_parse.ac3";
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + ac3_path.string() + "\" 192 51",
                    dir / "qc_preset_parse_encode.log") == 0);

    SECTION("an unrecognised value is refused, not silently ignored") {
        const auto log = dir / "qc_preset_bad.log";
        const auto rc = run_cli("qc \"" + ac3_path.string() + "\" preset=bogus", log);
        CHECK(rc != 0);
        CHECK(read_log(log).find("preset") != std::string::npos);
    }

    SECTION("preset=ebu-r128-s2 parses and runs a gate check") {
        const auto log = dir / "qc_preset_ebu.log";
        const auto rc = run_cli("qc \"" + ac3_path.string() + "\" preset=ebu-r128-s2", log);
        const auto text = read_log(log);
        INFO(text);
        (void)rc;  // reflects the gate verdict here, not a parse failure - see below instead
        CHECK(text.find("unknown qc preset") == std::string::npos);
        CHECK(text.find("ebu-r128-s2:") != std::string::npos);
        CHECK(text.find("gates:") != std::string::npos);
    }

    SECTION("preset=atsc-a85 parses and runs a gate check") {
        const auto log = dir / "qc_preset_atsc.log";
        run_cli("qc \"" + ac3_path.string() + "\" preset=atsc-a85", log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("unknown qc preset") == std::string::npos);
        CHECK(text.find("atsc-a85:") != std::string::npos);
    }

    SECTION("preset=netflix parses and runs a gate check") {
        const auto log = dir / "qc_preset_netflix.log";
        run_cli("qc \"" + ac3_path.string() + "\" preset=netflix", log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("unknown qc preset") == std::string::npos);
        CHECK(text.find("netflix:") != std::string::npos);
    }

    SECTION("preset=all checks every preset") {
        const auto log = dir / "qc_preset_all.log";
        run_cli("qc \"" + ac3_path.string() + "\" preset=all", log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("ebu-r128-s2:") != std::string::npos);
        CHECK(text.find("atsc-a85:") != std::string::npos);
        CHECK(text.find("netflix:") != std::string::npos);
    }

    SECTION("no preset at all just measures - no 'gates:' section, exit 0") {
        const auto log = dir / "qc_preset_none.log";
        const auto rc = run_cli("qc \"" + ac3_path.string() + "\"", log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == 0);
        CHECK(text.find("gates:") == std::string::npos);
    }
}

TEST_CASE("qc reports the embedded dialnorm and a sane, self-consistent measured loudness",
          "[cli][qc]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "qc_measure_in.wav";
    constexpr std::uint32_t kSampleRate = 48000;
    // 96000 samples = 2 s: BS.1770's absolute gate needs a full 400 ms
    // block before integrated_lkfs() reports anything at all (LoudnessMeter's
    // own doc comment) - a handful of AC-3 frames is not enough, the same
    // reason the dialnorm=auto tests above all use this same duration.
    constexpr std::size_t kFrames = 96000;
    const auto channels = make_tone_channels(6, kFrames, kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());

    const auto ac3_path = dir / "qc_measure.ac3";
    constexpr int kDialnorm = 17;
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + ac3_path.string() +
                        "\" 256 51 dialnorm=" + std::to_string(kDialnorm),
                    dir / "qc_measure_encode.log") == 0);

    const auto log = dir / "qc_measure.log";
    const auto rc = run_cli("qc \"" + ac3_path.string() + "\"", log);
    const auto text = read_log(log);
    INFO(text);
    // Measure-only (no preset=) never gates - a successful decode always
    // exits 0, whatever the numbers turn out to be.
    CHECK(rc == 0);

    const auto dialnorm = value_after(text, "dialnorm");
    REQUIRE(dialnorm.has_value());
    CHECK(*dialnorm == Catch::Approx(kDialnorm));

    const auto measured = value_after(text, "integrated loudness");
    REQUIRE(measured.has_value());
    // Real, non-silent, non-clipping multichannel content: comfortably
    // inside BS.1770's legal range, nowhere near the -70 LKFS "no
    // meaningful loudness" floor and never above digital 0 dBFS.
    CHECK(*measured > -70.0);
    CHECK(*measured < 0.0);

    const auto claimed = value_after(text, "claimed");
    REQUIRE(claimed.has_value());
    CHECK(*claimed == Catch::Approx(-static_cast<double>(kDialnorm)));

    const auto delta = value_after(text, "delta");
    REQUIRE(delta.has_value());
    // Self-consistency: the printed delta must be exactly measured -
    // claimed - the same arithmetic report_qc_programme performs internally
    // to produce it, and exactly what a sign or operand-order bug there
    // would break.
    CHECK(*delta == Catch::Approx(*measured - *claimed).margin(0.01));
}

// Roadmap C2's own explicit ask: a case where the embedded dialnorm and the
// measured loudness deliberately disagree, so the delta-reporting path is
// genuinely exercised rather than merely the agreement case above. dialnorm=1
// claims the loudest legal dialogue level (-1 LKFS) against a deliberately
// quiet (0.05 amplitude, well under make_tone_channels' own 0.5) real 5.1 mix
// - measured loudness cannot plausibly reach anywhere near -1 LKFS for
// content this quiet, so the mismatch is large and its sign (measured is
// quieter than claimed) is guaranteed by construction, not a coincidence of
// whatever the codec happened to produce.
TEST_CASE("qc's dialnorm-vs-measured delta is genuinely exercised when they deliberately disagree",
          "[cli][qc][dialnorm]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "qc_mismatch_in.wav";
    constexpr std::uint32_t kSampleRate = 48000;
    // 96000 samples = 2 s: BS.1770's absolute gate needs a full 400 ms
    // block before integrated_lkfs() reports anything at all (LoudnessMeter's
    // own doc comment) - a handful of AC-3 frames is not enough, the same
    // reason the dialnorm=auto tests above all use this same duration.
    constexpr std::size_t kFrames = 96000;
    std::vector<std::vector<float>> quiet_channels;
    quiet_channels.reserve(6);
    for (int ch = 0; ch < 6; ++ch) {
        quiet_channels.push_back(
            make_tone(0.05, 200.0 + 137.0 * static_cast<double>(ch), kFrames, kSampleRate));
    }
    REQUIRE(write_wav(wav_path, quiet_channels, kSampleRate));

    const auto ec3_path = dir / "qc_mismatch.ec3";
    constexpr int kWrongDialnorm = 1;  // claims -1 LKFS - as loud as dialnorm can say
    REQUIRE(run_cli("eac3-encode \"" + wav_path.string() + "\" \"" + ec3_path.string() +
                        "\" 192 none 51 dialnorm=" + std::to_string(kWrongDialnorm),
                    dir / "qc_mismatch_encode.log") == 0);

    const auto log = dir / "qc_mismatch.log";
    run_cli("qc \"" + ec3_path.string() + "\" preset=atsc-a85", log);
    const auto text = read_log(log);
    INFO(text);

    const auto dialnorm = value_after(text, "dialnorm");
    REQUIRE(dialnorm.has_value());
    CHECK(*dialnorm == Catch::Approx(kWrongDialnorm));

    const auto measured = value_after(text, "integrated loudness");
    REQUIRE(measured.has_value());

    const auto delta = value_after(text, "delta");
    REQUIRE(delta.has_value());
    // Deliberately, substantially negative - measured is far quieter than
    // the -1 LKFS dialnorm=1 claims, by construction (see this test's own
    // comment above), not a rounding nuance a correct-but-tiny delta could
    // also produce.
    CHECK(*delta < -5.0);

    // The measurement-derived dialnorm this mismatch implies is nowhere near
    // the wrong embedded value - the disagreement is real, not cosmetic.
    const auto implied_pos = text.find("measurement-derived dialnorm would be");
    REQUIRE(implied_pos != std::string::npos);
    const auto implied = value_after(text.substr(implied_pos), "would be");
    REQUIRE(implied.has_value());
    CHECK(*implied != kWrongDialnorm);
    CHECK(*implied > kWrongDialnorm + 10);  // a much quieter (larger) dialnorm code

    // The gate line's own arithmetic is self-consistent too: ATSC A/85's
    // published target/tolerance/ceiling (Sec.6: -24 LKFS +/-2 dB, true peak
    // <= -2 dBTP - see ac3/meta/qc.hpp's own citation) reproduced here as
    // literals to check the CLI's printed numbers against, not derived from
    // them. This does not assume which way the verdict lands (a quiet
    // dialnorm=1 mismatch says nothing about where -22-ish LKFS content
    // happens to sit relative to a -24 LKFS target, and it turns out to
    // land inside tolerance here) - only that the printed verdict is exactly
    // what evaluate_qc_gate's own |delta| <= tolerance / measured <= ceiling
    // rule would produce from the printed numbers.
    constexpr double kAtscTargetLkfs = -24.0;
    constexpr double kAtscToleranceLu = 2.0;
    constexpr double kAtscMaxTruePeakDbtp = -2.0;
    const auto gate_pos = text.find("atsc-a85:");
    REQUIRE(gate_pos != std::string::npos);
    const auto gate_text = text.substr(gate_pos);
    const auto gate_delta = value_after(gate_text, "delta");
    REQUIRE(gate_delta.has_value());
    CHECK(*gate_delta == Catch::Approx(*measured - kAtscTargetLkfs).margin(0.01));
    const auto expect_loudness_pass = std::abs(*gate_delta) <= kAtscToleranceLu;

    const auto peak_section = gate_text.substr(gate_text.find("true peak"));
    const auto gate_peak_measured = value_after(peak_section, "measured");
    REQUIRE(gate_peak_measured.has_value());
    const auto expect_peak_pass = *gate_peak_measured <= kAtscMaxTruePeakDbtp;

    const auto overall = gate_verdict_after(text, gate_pos);
    REQUIRE(overall.has_value());
    CHECK(*overall == (expect_loudness_pass && expect_peak_pass));
}

TEST_CASE(
    "qc preset=all checks every preset, and the exit code matches whether every verdict passed",
    "[cli][qc]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "qc_all_in.wav";
    constexpr std::uint32_t kSampleRate = 48000;
    // 96000 samples = 2 s: BS.1770's absolute gate needs a full 400 ms
    // block before integrated_lkfs() reports anything at all (LoudnessMeter's
    // own doc comment) - a handful of AC-3 frames is not enough, the same
    // reason the dialnorm=auto tests above all use this same duration.
    constexpr std::size_t kFrames = 96000;
    const auto channels = make_tone_channels(6, kFrames, kSampleRate);
    REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, kSampleRate).has_value());
    const auto ec3_path = dir / "qc_all.ec3";
    REQUIRE(run_cli("eac3-encode \"" + wav_path.string() + "\" \"" + ec3_path.string() +
                        "\" 256 cpl 51 dialnorm=auto",
                    dir / "qc_all_encode.log") == 0);

    const auto log = dir / "qc_all.log";
    const auto rc = run_cli("qc \"" + ec3_path.string() + "\" preset=all", log);
    const auto text = read_log(log);
    INFO(text);

    // Every preset in ac3::meta::kQcPresetIds, not a hand-copied subset -
    // preset=all promising "every one" and then silently skipping a row
    // added later is exactly the failure this loop exists to catch.
    std::vector<bool> verdicts;
    for (const auto id : ac3::meta::kQcPresetIds) {
        const auto heading = std::string{ac3::meta::qc_preset_name(id)} + ":";
        INFO("preset " << heading);
        const auto at = text.find(heading);
        REQUIRE(at != std::string::npos);
        const auto verdict = gate_verdict_after(text, at);
        REQUIRE(verdict.has_value());
        verdicts.push_back(*verdict);
        // Each row also names the document edition it was judged against
        // (IO11) - a verdict against an unnamed spec is not auditable.
        const auto source = ac3::meta::qc_preset(id).source;
        CHECK(text.find(std::string{source}) != std::string::npos);
    }
    REQUIRE(verdicts.size() == ac3::meta::kQcPresetIds.size());

    // The exit code (this project's own binary 0/1 convention - see this
    // file's own run_cli comment) must match "every requested gate passed",
    // recomputed from the same three verdicts the log itself printed -
    // whichever way the real BS.1770 numbers actually land, a bug that ORs
    // instead of ANDs the per-preset verdicts (or ignores one preset
    // entirely) shows up here as rc disagreeing with this recomputation.
    const bool expect_success =
        std::all_of(verdicts.begin(), verdicts.end(), [](bool v) { return v; });
    CHECK((rc == 0) == expect_success);
}

// Roadmap IO10: `ac3cli qc layout=rendered`. The bed pass measures only the
// independent substream's Table 5.8 channels, so on a 7.1.4 stream it never
// sees the two dependents' rear and height channels at all; the rendered pass
// measures the assembled program through BS.1770-5 Annex 3's extended
// algorithm, which has a weight for every Table E2.5 position.
TEST_CASE("qc layout=rendered measures a 7.1.4 program's dependents, layout=bed does not",
          "[cli][qc][layout]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "qc_layout_in.wav";
    constexpr std::uint32_t kRate = 48000;
    // 2 s - many BS.1770 400 ms blocks and ~62 access units, well past the
    // "3+ frames of real audio" this project's own validation rule requires
    // before a codec measurement means anything.
    constexpr std::size_t kFrames = 96000;
    // 7.1.4's twelve channels: L C R Ls Rs Lrs Rrs Vhl Vhr Lts Rts LFE. A
    // distinct tone per channel so no two can be confused for one another,
    // and so a permuted layout would not still measure the same total.
    std::vector<std::vector<float>> channels;
    channels.reserve(12);
    for (int ch = 0; ch < 12; ++ch) {
        channels.push_back(
            make_tone(0.2, 180.0 + 91.0 * static_cast<double>(ch), kFrames, kRate));
    }
    REQUIRE(write_wav(wav_path, channels, kRate));

    const auto ec3_path = dir / "qc_layout.ec3";
    REQUIRE(run_cli("eac3-encode \"" + wav_path.string() + "\" \"" + ec3_path.string() +
                        "\" 448 none 714 dialnorm=auto",
                    dir / "qc_layout_encode.log") == 0);

    const auto bed_log = dir / "qc_layout_bed.log";
    REQUIRE(run_cli("qc \"" + ec3_path.string() + "\" layout=bed", bed_log) == 0);
    const auto bed_text = read_log(bed_log);
    INFO("bed:\n" << bed_text);

    const auto rendered_log = dir / "qc_layout_rendered.log";
    REQUIRE(run_cli("qc \"" + ec3_path.string() + "\" layout=rendered", rendered_log) == 0);
    const auto rendered_text = read_log(rendered_log);
    INFO("rendered:\n" << rendered_text);

    // Each pass says which algorithm produced its numbers, and the bed pass
    // says out loud that it left the dependents' channels out - staying
    // quiet about that is what made the old behaviour a trap.
    CHECK(bed_text.find("layout=bed") != std::string::npos);
    CHECK(bed_text.find("BS.1770 Annex 1") != std::string::npos);
    CHECK(bed_text.find("dependent substreams whose channels") != std::string::npos);
    CHECK(rendered_text.find("layout=rendered") != std::string::npos);
    CHECK(rendered_text.find("BS.1770-5 Annex 3") != std::string::npos);
    CHECK(rendered_text.find("dependent substreams whose channels") == std::string::npos);

    // The bed pass reports the Table 5.8 layout it can name (3/2 + LFE, i.e.
    // 5.1 in the spec's own notation); the rendered pass names every Table
    // E2.5 location it actually metered, height channels included.
    CHECK(bed_text.find("3/2 + LFE") != std::string::npos);
    for (const std::string_view location : {"Lrs", "Rrs", "Vhl", "Vhr", "Lts", "Rts"}) {
        INFO("location " << location);
        CHECK(rendered_text.find(location) != std::string::npos);
        CHECK(bed_text.find(location) == std::string::npos);
    }

    // Six more channels of comparable-level tone are being summed, so the
    // rendered measurement has to come out meaningfully louder. This is the
    // substantive check: if layout=rendered were quietly still metering the
    // bed, the two numbers would be identical.
    const auto bed_lkfs = value_after(bed_text, "integrated loudness");
    const auto rendered_lkfs = value_after(rendered_text, "integrated loudness");
    REQUIRE(bed_lkfs.has_value());
    REQUIRE(rendered_lkfs.has_value());
    CHECK(*rendered_lkfs > *bed_lkfs + 1.0);

    // True peak, by contrast, is not channel-weighted and both passes see a
    // full-bandwidth channel at the same level, so it should barely move.
    const auto bed_tp = value_after(bed_text, "true peak");
    const auto rendered_tp = value_after(rendered_text, "true peak");
    REQUIRE(bed_tp.has_value());
    REQUIRE(rendered_tp.has_value());
    CHECK(*rendered_tp == Catch::Approx(*bed_tp).margin(3.0));
}

TEST_CASE("qc layout= rejects anything but bed or rendered", "[cli][qc][layout]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "qc_layout_bad_in.wav";
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 48000;
    REQUIRE(write_wav(wav_path, {make_tone(0.4, 440.0, kFrames, kRate),
                                  make_tone(0.4, 660.0, kFrames, kRate)},
                      kRate));
    const auto ac3_path = dir / "qc_layout_bad.ac3";
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + ac3_path.string() + "\" 192",
                    dir / "qc_layout_bad_encode.log") == 0);

    const auto log = dir / "qc_layout_bad.log";
    const auto rc = run_cli("qc \"" + ac3_path.string() + "\" layout=stereo", log);
    const auto text = read_log(log);
    INFO(text);
    CHECK(rc != 0);
    CHECK(text.find("layout must be bed or rendered") != std::string::npos);
}

// silence/eac3-silence build their frames from build_silent_stereo_frame/
// build_silent_access_unit, both already covered directly (test_frame.cpp,
// encoder/test_eac3.cpp) - what neither of those unit-tests can see is the CLI
// dispatch row itself: whether 'silence <out> [seconds] [bitrate_kbps]' and
// 'eac3-silence <out> [seconds] [bitrate_kbps] [layout]' actually route argv
// to the right parameters. main.cpp's own comment on the command table
// above records six real argv-index bugs found consolidating the old
// if-chain into that table, none of which a unit test on the frame builders
// alone could have caught - only running the built binary can.

TEST_CASE("silence writes the documented default duration/bitrate and decodes to genuine "
         "silence",
         "[cli][silence]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "silence_default.ac3";
    const auto log = dir / "silence_default.log";
    fs::remove(out_path);
    REQUIRE(run_cli("silence \"" + out_path.string() + "\"", log) == 0);

    // Defaults are 5 s at 192 kbps (see main.cpp's kCommands row and
    // run_silence's own u32(2, 5)/u32(3, 192) fallbacks) - pinned here
    // against the same frame_size_bytes() the encoder itself uses, not a
    // transcribed byte count.
    const std::uint64_t expect_count = (5ull * 48000 + 1535) / 1536;
    const auto frame_bytes = ac3::frame_size_bytes(ac3::SampleRate::k48000, 192);
    REQUIRE(frame_bytes.has_value());
    REQUIRE(fs::exists(out_path));
    CHECK(fs::file_size(out_path) == expect_count * *frame_bytes);

    const auto wav_path = dir / "silence_default.wav";
    REQUIRE(run_cli("decode \"" + out_path.string() + "\" \"" + wav_path.string() + "\"",
                    dir / "silence_default_decode.log") == 0);
    const auto decoded = ac3::io::read_wav(wav_path.string());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channels.size() == 2);
    REQUIRE(decoded->frame_count() > 0);
    for (const auto& channel : decoded->channels) {
        CHECK(rms(channel, 0, channel.size()) == 0.0);
    }
}

TEST_CASE("silence threads its seconds and bitrate arguments to the right parameters, "
         "not swapped",
         "[cli][silence]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "silence_explicit.ac3";
    const auto log = dir / "silence_explicit.log";
    fs::remove(out_path);
    // 2 seconds at 384 kbps: distinct enough from the 5 s/192 kbps default
    // that a swapped or misindexed argv read produces a file size neither
    // combination could coincidentally match.
    REQUIRE(run_cli("silence \"" + out_path.string() + "\" 2 384", log) == 0);

    const std::uint64_t expect_count = (2ull * 48000 + 1535) / 1536;
    const auto frame_bytes = ac3::frame_size_bytes(ac3::SampleRate::k48000, 384);
    REQUIRE(frame_bytes.has_value());
    REQUIRE(fs::exists(out_path));
    CHECK(fs::file_size(out_path) == expect_count * *frame_bytes);
}

TEST_CASE("silence rejects an illegal bitrate and leaves no file behind", "[cli][silence]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "silence_bad_bitrate.ac3";
    const auto log = dir / "silence_bad_bitrate.log";
    fs::remove(out_path);
    const auto rc = run_cli("silence \"" + out_path.string() + "\" 1 193", log);
    CHECK(rc != 0);
    CHECK_FALSE(fs::exists(out_path));
    CHECK(read_log(log).find("bitrate") != std::string::npos);
}

// eac3-sine builds an AccessUnitEncoder from a plan the same way eac3-encode
// does (see that command's own "refuses a rate frmsiz cannot signal" test
// above), and had the identical assert() - nchans == tone_hz.size(), rather
// than encode.cpp's channel_count() comparison, but the same root cause: a
// bitrate whose word count does not fit §E2.3.1.3's 11-bit frmsiz leaves
// AccessUnitEncoder with no substreams. eac3-encode's own fix is a
// post-construction check (encode.cpp's eac3_config_accepted()); this command
// asks ac3::plan::validate() BEFORE construction instead, which is also the
// one place precise enough to catch a layout with dependent substreams: each
// dependent gets HALF the plan's bitrate (eac3_config()), so a rate that
// looks framable at face value can still leave a dependent with a frame of no
// words. eac3_config_accepted() checks the plan's own bitrate against the
// whole-frame ceiling, not each substream's actual (possibly halved) share of
// it, so it cannot name frmsiz as the cause there - confirmed empirically
// below: eac3-encode at the same "1 kbit/s over 7.1.4" case this test pins
// falls back to eac3_config_accepted()'s generic "cannot express" message.
TEST_CASE("eac3-sine rejects a rate frmsiz cannot express", "[cli][eac3-sine][frmsiz]") {
    const auto dir = scratch_dir();

    // Shared by every rejection below: a diagnosis naming the field, and no
    // sign of the assertion this used to be.
    const auto check_rejected = [](const std::string& text) {
        CHECK(text.find("error:") != std::string::npos);
        CHECK(text.find("frmsiz") != std::string::npos);
        CHECK(text.find("Assertion") == std::string::npos);
    };

    SECTION("1024 kbit/s at 48 kHz is exactly the ceiling, 1026 is past it") {
        const auto ok_path = dir / "sine_frmsiz_ceiling_ok.ec3";
        const auto ok_log = dir / "sine_frmsiz_ceiling_ok.log";
        fs::remove(ok_path);
        const auto ok_rc = run_cli("eac3-sine \"" + ok_path.string() + "\" 1 1024", ok_log);
        INFO(read_log(ok_log));
        CHECK(ok_rc == 0);
        CHECK(fs::exists(ok_path));

        const auto over_path = dir / "sine_frmsiz_ceiling_over.ec3";
        const auto over_log = dir / "sine_frmsiz_ceiling_over.log";
        fs::remove(over_path);
        const auto over_rc = run_cli("eac3-sine \"" + over_path.string() + "\" 1 1026", over_log);
        const auto text = read_log(over_log);
        INFO(text);
        CHECK(over_rc != 0);
        CHECK_FALSE(fs::exists(over_path));
        check_rejected(text);
    }

    SECTION("0 kbit/s is the same rule from the floor - a frame of no words") {
        const auto out_path = dir / "sine_frmsiz_floor.ec3";
        const auto log = dir / "sine_frmsiz_floor.log";
        fs::remove(out_path);
        const auto rc = run_cli("eac3-sine \"" + out_path.string() + "\" 1 0", log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        check_rejected(text);
    }

    SECTION("an immersive layout's dependents each get half the rate, and that half "
           "has to be framable too") {
        // 1 kbit/s over 7.1.4 halves to 0 kbit/s per dependent - a frame of
        // no words - which plan::validate() refuses by name before the
        // encoder is ever built.
        const auto floor_path = dir / "sine_frmsiz_dependent_floor.ec3";
        const auto floor_log = dir / "sine_frmsiz_dependent_floor.log";
        fs::remove(floor_path);
        const auto floor_rc =
            run_cli("eac3-sine \"" + floor_path.string() + "\" 1 1 440 50 714", floor_log);
        const auto floor_text = read_log(floor_log);
        INFO(floor_text);
        CHECK(floor_rc != 0);
        CHECK_FALSE(fs::exists(floor_path));
        check_rejected(floor_text);

        // 768 kbit/s is comfortably framable at every substream once halved,
        // and actually encodes - the check above is a real ceiling, not a
        // blanket refusal of dependent substreams.
        const auto ok_path = dir / "sine_frmsiz_dependent_ok.ec3";
        const auto ok_log = dir / "sine_frmsiz_dependent_ok.log";
        fs::remove(ok_path);
        const auto ok_rc =
            run_cli("eac3-sine \"" + ok_path.string() + "\" 1 768 440 50 714", ok_log);
        INFO(read_log(ok_log));
        CHECK(ok_rc == 0);
        CHECK(fs::exists(ok_path));
    }

    SECTION("eac3-encode meets the identical dependent-floor case with a less precise "
           "message, since its own check runs after construction against the plan's "
           "whole bitrate rather than each substream's own share of it") {
        const auto wav_path = dir / "sine_frmsiz_encode_compare_in.wav";
        const auto channels =
            make_tone_channels(2, 3 * static_cast<std::size_t>(ac3::kSamplesPerFrame), 48000);
        REQUIRE(ac3::io::write_wav_f32(wav_path.string(), channels, 48000).has_value());

        const auto out_path = dir / "sine_frmsiz_encode_compare.ec3";
        const auto log = dir / "sine_frmsiz_encode_compare.log";
        fs::remove(out_path);
        const auto rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                    out_path.string() + "\" 1 none 714",
                                log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc != 0);
        CHECK_FALSE(fs::exists(out_path));
        // Refused cleanly either way - this is not an assertion regression -
        // but the plan layer's own diagnosis is not among its reasons, since
        // eac3-encode never asks plan::validate() for this: encode.cpp's own
        // post-construction check judges the plan's whole bitrate (1 kbit/s,
        // itself framable) rather than the dependent's halved share of it.
        CHECK(text.find("Assertion") == std::string::npos);
        CHECK(text.find("frmsiz") == std::string::npos);
    }
}

TEST_CASE("eac3-silence threads its layout argument through to the reported label and "
         "substream count, and decodes to genuine silence",
         "[cli][eac3-silence]") {
    const auto dir = scratch_dir();

    const auto stereo_path = dir / "eac3_silence_stereo.ec3";
    const auto stereo_log = dir / "eac3_silence_stereo.log";
    fs::remove(stereo_path);
    REQUIRE(run_cli("eac3-silence \"" + stereo_path.string() + "\"", stereo_log) == 0);
    const auto stereo_text = read_log(stereo_log);
    INFO(stereo_text);
    CHECK(stereo_text.find("2/0 stereo") != std::string::npos);  // the "stereo" default's label
    CHECK(stereo_text.find("1 substreams") != std::string::npos);

    const auto surround_path = dir / "eac3_silence_51.ec3";
    const auto surround_log = dir / "eac3_silence_51.log";
    fs::remove(surround_path);
    REQUIRE(run_cli("eac3-silence \"" + surround_path.string() + "\" 1 192 51", surround_log) ==
           0);
    const auto surround_text = read_log(surround_log);
    INFO(surround_text);
    CHECK(surround_text.find("5.1") != std::string::npos);
    CHECK(surround_text.find("2/0 stereo") == std::string::npos);

    const auto wav_path = dir / "eac3_silence_51.wav";
    REQUIRE(run_cli("decode \"" + surround_path.string() + "\" \"" + wav_path.string() + "\"",
                    dir / "eac3_silence_51_decode.log") == 0);
    const auto decoded = ac3::io::read_wav(wav_path.string());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->frame_count() > 0);
    for (const auto& channel : decoded->channels) {
        CHECK(rms(channel, 0, channel.size()) == 0.0);
    }
}

// decode's own object-layer gap (PRs #168/#169 made Eac3Decoder actually
// populate DecodedAccessUnit::object_metadata/object_audio; nothing on the
// CLI side read either field until now): a plain decode should report the
// object count it found, and objects_dir should export each JOC-
// reconstructed object as its own genuinely non-silent mono WAV - distinct
// from the bed and, since every 'atmos' object gets its own tone, from each
// other too.
TEST_CASE("decode reports the object layer of an Atmos stream and exports it with objects_dir",
          "[cli][decode][atmos]") {
    const auto dir = scratch_dir();
    const auto ec3_path = dir / "decode_atmos_objects.ec3";
    REQUIRE(run_cli("atmos \"" + ec3_path.string() + "\" 1 448 3 4 objects",
                    dir / "decode_atmos_objects_encode.log") == 0);

    // Plain decode, no objects_dir: the summary line is there, no export happens.
    const auto wav_path = dir / "decode_atmos_objects.wav";
    const auto plain_log = dir / "decode_atmos_objects_plain.log";
    REQUIRE(
        run_cli("decode \"" + ec3_path.string() + "\" \"" + wav_path.string() + "\"", plain_log) ==
        0);
    const auto plain_text = read_log(plain_log);
    INFO(plain_text);
    CHECK(plain_text.find("3 dynamic objects") != std::string::npos);
    CHECK(plain_text.find("4 objects") != std::string::npos);
    CHECK(plain_text.find("JOC audio reconstructed") != std::string::npos);
    CHECK(plain_text.find("wrote") == std::string::npos);

    // Same stream, with objects_dir: one object_NN.wav per dynamic object.
    const auto objects_dir = dir / "decode_atmos_objects_out";
    fs::remove_all(objects_dir);
    const auto export_log = dir / "decode_atmos_objects_export.log";
    REQUIRE(run_cli("decode \"" + ec3_path.string() + "\" \"" + wav_path.string() + "\" \"" +
                        objects_dir.string() + "\"",
                    export_log) == 0);
    const auto export_text = read_log(export_log);
    INFO(export_text);
    CHECK(export_text.find("wrote 3 object WAV(s)") != std::string::npos);

    std::vector<std::vector<float>> object_channels;
    for (int i = 0; i < 3; ++i) {
        const auto object_path = objects_dir / ("object_0" + std::to_string(i) + ".wav");
        REQUIRE(fs::exists(object_path));
        const auto decoded = ac3::io::read_wav(object_path.string());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->channels.size() == 1);
        REQUIRE(decoded->frame_count() > 0);
        CHECK(rms(decoded->channels[0], 0, decoded->channels[0].size()) > 0.0);
        object_channels.push_back(decoded->channels[0]);
    }
    // Three distinct orbiting tones in, so not every object should have
    // decoded to the same waveform - compared as a bool for the same reason
    // the atmos-encode motion-vs-static test above does (stringifying a
    // multi-thousand-sample diff on failure is slow and was seen to crash).
    CHECK(object_channels[0] != object_channels[1]);
    CHECK(object_channels[1] != object_channels[2]);
}

TEST_CASE("decode objects_dir warns instead of exporting when there is no object audio to export",
          "[cli][decode][atmos]") {
    const auto dir = scratch_dir();

    SECTION("plain AC-3 has no object layer at all") {
        const auto ac3_path = dir / "decode_objects_plain_ac3.ac3";
        REQUIRE(run_cli("sine \"" + ac3_path.string() + "\" 1 192 1000 50 stereo",
                        dir / "decode_objects_plain_ac3_encode.log") == 0);
        const auto wav_path = dir / "decode_objects_plain_ac3.wav";
        const auto objects_dir = dir / "decode_objects_plain_ac3_out";
        fs::remove_all(objects_dir);
        const auto log = dir / "decode_objects_plain_ac3.log";
        REQUIRE(run_cli("decode \"" + ac3_path.string() + "\" \"" + wav_path.string() + "\" \"" +
                            objects_dir.string() + "\"",
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("no object layer") != std::string::npos);
        CHECK_FALSE(fs::exists(objects_dir));
    }

    SECTION("bed51 mode carries no object container to reconstruct from") {
        const auto ec3_path = dir / "decode_objects_bed51.ec3";
        REQUIRE(run_cli("atmos \"" + ec3_path.string() + "\" 1 448 3 4 bed51",
                        dir / "decode_objects_bed51_encode.log") == 0);
        const auto wav_path = dir / "decode_objects_bed51.wav";
        const auto objects_dir = dir / "decode_objects_bed51_out";
        fs::remove_all(objects_dir);
        const auto log = dir / "decode_objects_bed51.log";
        REQUIRE(run_cli("decode \"" + ec3_path.string() + "\" \"" + wav_path.string() + "\" \"" +
                            objects_dir.string() + "\"",
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("no reconstructed object audio to export") != std::string::npos);
        CHECK_FALSE(fs::exists(objects_dir));
    }
}

// Regression test for a latent bug in run_decode_eac3's Eac3Decoder::flush()
// tail loop: it used to append a flushed substream's channels at pcm[0..N)
// by CODED index, rather than at the Table E2.5 slot decode_access_unit's
// own §E3.8.2 assembly would have used. That is silently correct for a lone
// independent substream (its coded order already matches pcm's established
// index order), but wrong the moment flush() hands back a DEPENDENT on its
// own - its smaller channel set then lands in pcm[0]/pcm[1] (the bed's L/R)
// instead of its own height slots, leaving some pcm[] vectors longer than
// others once interleaved into the WAV.
//
// §3.7 transient pre-noise processing is what can make flush() return a raw
// per-substream result at all (decoder.hpp's own doc comment on
// Eac3Decoder::flush): a substream identity holds its last frame back
// exactly when ITS OWN channels carry a transient on the stream's very last
// frame, with no following frame to release it. tools=tpn turns the tool on
// for every substream uniformly (plan.cpp's apply_tools), but activation is
// decided per substream from its own real audio content - so a bed that ends
// on a sharp transient while its height dependent stays silent desyncs
// exactly like this at end of stream, the same asymmetry
// test_eac3_decoder.cpp's own "decode_access_unit queues a substream..."
// test already proves reliably triggers at the library level.
TEST_CASE(
    "decode assembles a flush()'d transient-pre-noise tail into the correct channel slots, not "
    "coded-order-into-pcm-index",
    "[cli][decode][eac3][transient_prenoise]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "flush_desync_in.wav";

    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrame = static_cast<std::size_t>(ac3::kSamplesPerFrame);
    constexpr std::size_t kSilentFrames = 4;
    // Within the final frame - the same offset the library's own
    // transient_prenoise unit test uses to trigger block switching.
    constexpr std::size_t kOnset = 960;
    constexpr std::size_t kTotalFrames = (kSilentFrames + 1) * kFrame;

    // Bed (L, R): silent until the very last frame, then a loud 1 kHz onset.
    std::vector<float> bed_channel(kTotalFrames, 0.0f);
    for (std::size_t n = kSilentFrames * kFrame + kOnset; n < kTotalFrames; ++n) {
        bed_channel[n] = static_cast<float>(
            0.9 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(n) /
                          static_cast<double>(kRate)));
    }
    // The height dependent (Vhl, Vhr): silent for the whole file, so it never
    // sets transproce itself and keeps releasing every call right up to the
    // stream's last frame - the asymmetry the bug needs. WAV channel order
    // matches Table E2.5/kWavSpeakerOrder for this location set: L, R, then
    // Vhl, Vhr.
    const std::vector<float> silence(kTotalFrames, 0.0f);
    REQUIRE(write_wav(wav_path, {bed_channel, bed_channel, silence, silence}, kRate));

    const auto ec3_path = dir / "flush_desync.ec3";
    const auto encode_log = dir / "flush_desync_encode.log";
    const auto encode_rc = run_cli("eac3-encode \"" + wav_path.string() + "\" \"" +
                                       ec3_path.string() + "\" 192 tpn L,R,Vhl,Vhr",
                                   encode_log);
    INFO(read_log(encode_log));
    REQUIRE(encode_rc == 0);

    const auto wav_out = dir / "flush_desync_out.wav";
    const auto decode_log = dir / "flush_desync_decode.log";
    const auto decode_rc =
        run_cli("decode \"" + ec3_path.string() + "\" \"" + wav_out.string() + "\"", decode_log);
    INFO(read_log(decode_log));
    REQUIRE(decode_rc == 0);

    const auto decoded = ac3::io::read_wav(wav_out.string());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channels.size() == 4);

    // The core symptom this bug produced: some pcm[] vectors longer than
    // others once a flushed substream's channels landed at the wrong index.
    // Every channel must come out exactly as long as every other.
    const auto expected_length = decoded->channels[0].size();
    for (const auto& channel : decoded->channels) {
        CHECK(channel.size() == expected_length);
    }
    // Every real frame must have made it out, including the desynced tail -
    // a fix that merely made the lengths agree by truncating the flush()
    // tail would still pass the check above but fail this one.
    CHECK(expected_length >= kTotalFrames);

    // And the tail must have landed in the RIGHT channels: the bed's onset
    // in L/R (indices 0/1), silence still in the height pair (indices 2/3) -
    // not the reverse, and not smeared across both by a coded-order copy.
    const auto tail_from = kSilentFrames * kFrame + kOnset;
    for (const std::size_t ch : {std::size_t{0}, std::size_t{1}}) {
        CHECK(rms(decoded->channels[ch], tail_from, expected_length - tail_from) > 0.3);
    }
    for (const std::size_t ch : {std::size_t{2}, std::size_t{3}}) {
        CHECK(rms(decoded->channels[ch], tail_from, expected_length - tail_from) < 0.05);
    }
}

TEST_CASE("mode=reference is exactly the two transform off-switches together", "[cli][mode]") {
    const auto dir = scratch_dir();
    const auto log = dir / "mode.log";
    const auto read_bytes = [](const fs::path& p) {
        std::ifstream in{p, std::ios::binary};
        REQUIRE(in.is_open());
        return std::vector<char>{std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{}};
    };
    const auto sine = [&](const fs::path& out, const std::string& tokens) {
        REQUIRE(run_cli("sine \"" + out.string() + "\" 2 192 440 70 stereo " + tokens, log) == 0);
    };
    const auto decode = [&](const fs::path& in, const fs::path& out,
                            const std::string& tokens) {
        REQUIRE(run_cli("decode \"" + in.string() + "\" \"" + out.string() + "\" " + tokens,
                        log) == 0);
    };

    // Encode half: mode=reference must be byte-identical to fast-mdct=off,
    // and the bare default to mode=performance - the mode is an intent-level
    // alias over the two existing switches, never a third behaviour.
    const auto enc_ref = dir / "mode_enc_ref.ac3";
    const auto enc_off = dir / "mode_enc_off.ac3";
    const auto enc_def = dir / "mode_enc_def.ac3";
    const auto enc_perf = dir / "mode_enc_perf.ac3";
    sine(enc_ref, "mode=reference");
    sine(enc_off, "fast-mdct=off");
    sine(enc_def, "");
    sine(enc_perf, "mode=performance");
    CHECK(read_bytes(enc_ref) == read_bytes(enc_off));
    CHECK(read_bytes(enc_def) == read_bytes(enc_perf));

    // Decode half: the same aliasing over fast-imdct, on one fixed stream.
    const auto dec_ref = dir / "mode_dec_ref.wav";
    const auto dec_off = dir / "mode_dec_off.wav";
    const auto dec_def = dir / "mode_dec_def.wav";
    const auto dec_perf = dir / "mode_dec_perf.wav";
    decode(enc_def, dec_ref, "mode=reference");
    decode(enc_def, dec_off, "fast-imdct=off");
    decode(enc_def, dec_def, "");
    decode(enc_def, dec_perf, "mode=performance");
    CHECK(read_bytes(dec_ref) == read_bytes(dec_off));
    CHECK(read_bytes(dec_def) == read_bytes(dec_perf));

    // Order matters and is documented: a later specific switch adjusts one
    // half of an earlier mode.
    const auto enc_mixed = dir / "mode_enc_mixed.ac3";
    sine(enc_mixed, "mode=performance fast-mdct=off");
    CHECK(read_bytes(enc_mixed) == read_bytes(enc_ref));

    // An unknown mode is refused, not ignored.
    CHECK(run_cli("sine \"" + (dir / "mode_bad.ac3").string() + "\" 2 192 440 70 stereo "
                      "mode=fast",
                  log) != 0);
}

// --------------------------------------------------------------------------
// Roadmap IO8: the documented exit-code scheme, per-command help, quiet/
// verbose, and the generated man page and completions.
// --------------------------------------------------------------------------

TEST_CASE("every failure path returns its own documented exit code", "[cli][exit-codes]") {
    const auto dir = scratch_dir();
    const auto log = dir / "exit_codes.log";

    // The numbers here are the contract, not an implementation detail: a
    // script distinguishes a bad command line from a bad file from a failed
    // gate by exactly these. apps/cli/exit_codes.hpp is where they are chosen
    // and docs/cli/metadata-options.md#exit-codes is where they are published;
    // this is what keeps all three agreeing.
    SECTION("0 - success") {
        CHECK(run_cli("silence \"" + (dir / "exit_ok.ac3").string() + "\" 1 192", log) == 0);
    }

    SECTION("1 - usage: too few arguments, an unknown command, an unknown option") {
        CHECK(run_cli("encode", log) == 1);
        CHECK(run_cli("definitely-not-a-command", log) == 1);
        CHECK(run_cli("silence \"" + (dir / "exit_usage.ac3").string() + "\" 1 192 nosuch=1",
                      log) == 1);
    }

    SECTION("1 - usage: a configuration the encoder cannot express") {
        // A layout AC-3 has no coding mode for. Refused before anything is
        // written, and refused as a USAGE error - retrying the same command
        // line cannot help.
        const auto wav_path = dir / "exit_usage_in.wav";
        REQUIRE(write_wav(wav_path, make_tone_channels(2, 4000, 48000), 48000));
        CHECK(run_cli("encode \"" + wav_path.string() + "\" \"" +
                          (dir / "exit_usage2.ac3").string() + "\" 192 714",
                      log) == 1);
    }

    SECTION("2 - input: unreadable, and present-but-not-a-stream") {
        CHECK(run_cli("decode \"" + (dir / "exit_absent.ac3").string() + "\" \"" +
                          (dir / "exit_absent.wav").string() + "\"",
                      log) == 2);
        const auto empty = dir / "exit_empty.ac3";
        { std::ofstream out{empty, std::ios::binary}; }
        CHECK(run_cli("decode \"" + empty.string() + "\" \"" + (dir / "exit_empty.wav").string() +
                          "\"",
                      log) == 2);
    }

    SECTION("3 - output: a destination that cannot be created") {
        // A path whose parent directory does not exist - the one
        // "cannot write" every platform agrees on without needing a
        // permission model.
        const auto wav_path = dir / "exit_out_in.wav";
        REQUIRE(write_wav(wav_path, make_tone_channels(2, 4000, 48000), 48000));
        const auto bad = dir / "no_such_directory" / "out.ac3";
        CHECK(run_cli("encode \"" + wav_path.string() + "\" \"" + bad.string() + "\" 192 stereo",
                      log) == 3);
    }

    SECTION("6 - a QC gate failed, distinct from 2 (qc could not read the file)") {
        // A 440 Hz test tone was never mastered to any delivery target, so
        // preset=all is a genuine FAIL - the whole point of the code being
        // its own rather than folded into "something went wrong".
        const auto stream = dir / "exit_qc.ac3";
        REQUIRE(run_cli("sine \"" + stream.string() + "\" 2 192 440 70 stereo", log) == 0);
        CHECK(run_cli("qc \"" + stream.string() + "\" preset=all", log) == 6);
        CHECK(run_cli("qc \"" + stream.string() + "\"", log) == 0);
        CHECK(run_cli("qc \"" + (dir / "exit_qc_absent.ac3").string() + "\"", log) == 2);
    }
}

TEST_CASE("help prints one command's own row, not the whole manual", "[cli][help]") {
    const auto dir = scratch_dir();
    const auto log = dir / "help.log";

    REQUIRE(run_cli("help", log) == 0);
    const auto full = read_log(log);
    REQUIRE(run_cli("help encode", log) == 0);
    const auto one = read_log(log);

    // The row itself, and the grammars encode actually uses.
    CHECK(one.find("ac3cli encode") != std::string::npos);
    CHECK(one.find("layout:") != std::string::npos);
    CHECK(one.find("metadata options") != std::string::npos);
    // Not the ones it does not: encode has no Annex E tool set and no VBR.
    CHECK(one.find("Annex E coding tools") == std::string::npos);
    CHECK(one.find("vbr (eac3-encode only)") == std::string::npos);
    // And decisively shorter than the whole listing, which is the behaviour
    // change worth asserting: an argument error used to print all of `full`.
    CHECK(one.size() < full.size());

    SECTION("eac3-encode's own help does carry the tool and vbr grammars") {
        REQUIRE(run_cli("help eac3-encode", log) == 0);
        const auto text = read_log(log);
        CHECK(text.find("Annex E coding tools") != std::string::npos);
        CHECK(text.find("vbr (eac3-encode only)") != std::string::npos);
    }

    SECTION("--help and -h are the same thing spelled the other way round") {
        REQUIRE(run_cli("encode --help", log) == 0);
        const auto dashes = read_log(log);
        REQUIRE(run_cli("help encode", log) == 0);
        CHECK(dashes == read_log(log));
        REQUIRE(run_cli("encode -h", log) == 0);
        CHECK(read_log(log) == dashes);
    }

    SECTION("--help wins over an otherwise-unsatisfied argument list") {
        // `ac3cli encode` alone is a usage error; `ac3cli encode --help` is
        // not, which is the whole point of lifting the flag out first.
        CHECK(run_cli("encode --help", log) == 0);
    }

    SECTION("help exit-codes prints the scheme") {
        REQUIRE(run_cli("help exit-codes", log) == 0);
        const auto text = read_log(log);
        CHECK(text.find("QC gate") != std::string::npos);
        CHECK(text.find("unavailable here") != std::string::npos);
    }

    SECTION("an argument error names the command and points at its help") {
        CHECK(run_cli("encode", log) != 0);
        const auto text = read_log(log);
        CHECK(text.find("ac3cli help encode") != std::string::npos);
        // The whole manual is what it must NOT print any more.
        CHECK(text.find("metadata options") == std::string::npos);
        CHECK(text.size() < full.size());
    }

    SECTION("help for something that is not a command is a usage error") {
        CHECK(run_cli("help not-a-command", log) == 1);
    }
}

TEST_CASE("quiet silences status output without touching the payload", "[cli][quiet]") {
    const auto dir = scratch_dir();
    const auto log = dir / "quiet.log";
    const auto out = dir / "quiet.ac3";

    fs::remove(out);
    REQUIRE(run_cli("sine \"" + out.string() + "\" 1 192 440 70 stereo quiet", log) == 0);
    CHECK(read_log(log).empty());
    CHECK(fs::exists(out));
    CHECK(fs::file_size(out) > 0);

    SECTION("without quiet the same run does report itself") {
        REQUIRE(run_cli("sine \"" + (dir / "quiet_off.ac3").string() + "\" 1 192 440 70 stereo",
                        log) == 0);
        CHECK_FALSE(read_log(log).empty());
    }

    SECTION("a reporting command still reports: its answer is its output") {
        REQUIRE(run_cli("levels \"" + out.string() + "\" quiet", log) == 0);
        CHECK(read_log(log).find("per-channel levels") != std::string::npos);
    }

    SECTION("errors are never silenced") {
        CHECK(run_cli("decode \"" + (dir / "quiet_absent.ac3").string() + "\" \"" +
                          (dir / "quiet_absent.wav").string() + "\" quiet",
                      log) == 2);
        CHECK(read_log(log).find("error:") != std::string::npos);
    }
}

TEST_CASE("verbose puts a progress line on stderr, never on stdout", "[cli][verbose]") {
    const auto dir = scratch_dir();
    const auto wav_path = dir / "verbose_in.wav";
    REQUIRE(write_wav(wav_path, make_tone_channels(2, 48000, 48000), 48000));

    const auto out = dir / "verbose_out.ac3";
    const auto log = dir / "verbose.log";
    REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" + out.string() +
                        "\" 192 stereo verbose",
                    log) == 0);
    CHECK(read_log(log).find("encoding") != std::string::npos);

    SECTION("a short run stays quiet about progress unless asked") {
        // One second of audio is 31 frames, far under the threshold at which
        // the line turns itself on - so the default run says nothing about
        // progress at all.
        const auto plain = dir / "verbose_plain.log";
        REQUIRE(run_cli("encode \"" + wav_path.string() + "\" \"" +
                            (dir / "verbose_plain.ac3").string() + "\" 192 stereo",
                        plain) == 0);
        CHECK(read_log(plain).find("encoding ") == std::string::npos);
    }

    SECTION("with a '-' output the progress line stays out of the stream") {
        const auto piped = dir / "verbose_piped.ac3";
        const auto err = dir / "verbose_piped.err";
        REQUIRE(run_cli_stdout("encode \"" + wav_path.string() + "\" - 192 stereo verbose",
                               piped, err) == 0);
        CHECK(fs::file_size(piped) > 0);
        CHECK(read_log(err).find("encoding") != std::string::npos);
        // The payload must still be a decodable AC-3 stream - i.e. nothing
        // human-readable leaked into it.
        const auto back = dir / "verbose_piped.wav";
        CHECK(run_cli("decode \"" + piped.string() + "\" \"" + back.string() + "\" quiet",
                      dir / "verbose_piped_decode.log") == 0);
    }
}

TEST_CASE("man and completions are generated from the command table", "[cli][man]") {
    const auto dir = scratch_dir();
    const auto log = dir / "man.log";

    SECTION("the man page is a section-1 groff page naming every command") {
        REQUIRE(run_cli("man", log) == 0);
        const auto page = read_log(log);
        CHECK(page.find(".TH AC3CLI 1") != std::string::npos);
        CHECK(page.find(".SH COMMANDS") != std::string::npos);
        CHECK(page.find(".SH EXIT STATUS") != std::string::npos);
        // A command from each end of the table, so a truncated render fails.
        CHECK(page.find("ac3cli silence") != std::string::npos);
        CHECK(page.find("ac3cli monitor") != std::string::npos);
        CHECK(page.find("ac3cli completions") != std::string::npos);
    }

    SECTION("each shell gets its own script, and an unknown shell is refused") {
        struct Case {
            const char* shell;
            const char* marker;
        };
        for (const auto& c : {Case{"bash", "complete -F _ac3cli ac3cli"},
                              Case{"zsh", "#compdef ac3cli"},
                              Case{"fish", "complete -c ac3cli"},
                              Case{"powershell", "Register-ArgumentCompleter"}}) {
            INFO(c.shell);
            REQUIRE(run_cli(std::string{"completions "} + c.shell, log) == 0);
            const auto script = read_log(log);
            CHECK(script.find(c.marker) != std::string::npos);
            // Generated from the table, so a command must appear in it.
            CHECK(script.find("eac3-encode") != std::string::npos);
        }
        CHECK(run_cli("completions tcsh", log) == 1);
    }

    SECTION("every bare option token the completions offer is really accepted") {
        // The completion list is a second statement of what parse_options
        // takes (see kOptionTokens' own comment) - this is what keeps it from
        // drifting into offering something the parser refuses. Only the
        // valueless tokens can be checked this cheaply; a key= token's value
        // grammar differs per option.
        REQUIRE(run_cli("completions fish", log) == 0);
        const auto script = read_log(log);
        for (const auto* token : {"couple", "heavy", "heavy2", "mixmeta", "keep-partial",
                                  "sign-objects", "verify-objects", "fast-mdct", "fast-imdct",
                                  "quiet", "verbose"}) {
            INFO(token);
            CHECK(script.find(std::string{"-a '"} + token + "'") != std::string::npos);
            CHECK(run_cli(std::string{"silence \""} + (dir / "man_opt.ac3").string() + "\" 1 192 " +
                              token,
                          dir / "man_opt.log") == 0);
        }
    }
}

// --------------------------------------------------------------------------
// Roadmap IO9: record/live parity with the GUI session. The capture side
// cannot run headlessly - there is no capture endpoint on a CI machine, and
// on a platform with no capture backend at all `record`/`live` are refused
// before their arguments are read - so what is checked here is the option
// surface those commands added, which parse_options settles before dispatch
// and therefore on every platform alike.
// --------------------------------------------------------------------------

TEST_CASE("record/live take options parse or are refused", "[cli][record][live]") {
    const auto dir = scratch_dir();
    const auto log = dir / "take_opts.log";
    const auto probe = dir / "take_opts.ac3";
    // `silence` ignores all of these, which is exactly what makes it a clean
    // probe of the PARSER: a run that reaches the encoder proves the token was
    // accepted, and a refusal proves it was not silently ignored.
    const auto parses = [&](const std::string& token) {
        return run_cli("silence \"" + probe.string() + "\" 1 192 " + token, log);
    };

    SECTION("container= takes all five streamable containers") {
        for (const auto* value : {"raw", "mkv", "matroska", "ts", "mpegts", "spdif", "fmp4", "cmaf"}) {
            INFO(value);
            CHECK(parses(std::string{"container="} + value) == 0);
        }
        CHECK(parses("container=avi") == 1);
        CHECK(read_log(log).find("raw, mkv, ts, spdif or fmp4") != std::string::npos);
        // Plain mp4 cannot be written incrementally (moov/stco need every
        // frame's final offset), so it is refused here rather than silently
        // accepted and then not honoured - unlike fragmented mp4 (fmp4/cmaf
        // above), which has no such constraint.
        CHECK(parses("container=mp4") == 1);
    }

    SECTION("layout=/codec= parse, and a bad codec is refused") {
        CHECK(parses("layout=51") == 0);
        CHECK(parses("layout=L,C,R,LFE,Vhl,Vhr") == 0);
        CHECK(parses("codec=ac3") == 0);
        CHECK(parses("codec=eac3") == 0);
        CHECK(parses("codec=ec3") == 0);
        CHECK(parses("codec=truehd") == 1);
        CHECK(parses("layout=") == 1);
    }

    SECTION("watchdog= takes a non-negative timeout in seconds") {
        CHECK(parses("watchdog=0") == 0);
        CHECK(parses("watchdog=1.5") == 0);
        CHECK(parses("watchdog=-1") == 1);
        CHECK(parses("watchdog=soon") == 1);
    }

    SECTION("objects= is a 1..15 slot budget") {
        CHECK(parses("objects=1") == 0);
        CHECK(parses("objects=15") == 0);
        CHECK(parses("objects=0") == 1);
        CHECK(parses("objects=16") == 1);
    }

    SECTION("downmix= is on or off") {
        CHECK(parses("downmix=on") == 0);
        CHECK(parses("downmix=off") == 0);
        CHECK(parses("downmix=maybe") == 1);
    }
}

TEST_CASE("atmos-encode assembles real objects behind src=/map=",
          "[cli][atmos-encode][map]") {
    const auto dir = scratch_dir();
    constexpr std::uint32_t kRate = 48000;
    constexpr std::size_t kFrames = 24000;  // half a second
    const auto a = dir / "map_obj_a.wav";
    const auto b = dir / "map_obj_b.wav";
    REQUIRE(write_wav(a, make_tone_channels(4, kFrames, kRate), kRate));
    REQUIRE(write_wav(b, make_tone_channels(2, kFrames, kRate), kRate));

    const auto out = dir / "map_obj.ec3";
    const auto log = dir / "map_obj.log";
    // Four objects out of six loaded channels: two on their own, two folded
    // to one mono object, one silenced, plus one from the second source.
    // obj rows come first (in source, then channel order), then the objm
    // group - the order the two front ends have to agree on for a GUI
    // assignment to be reproducible headlessly.
    const auto rc = run_cli(
        "atmos-encode \"" + a.string() + "\" \"" + out.string() + "\" 448 src=\"" + b.string() +
            "\" map=0.0:obj,0.1:obj@-3,0.2-3:objm,1.0:obj,1.1:none",
        log);
    INFO(read_log(log));
    REQUIRE(rc == 0);
    REQUIRE(fs::exists(out));
    CHECK(read_log(log).find("4 objects") != std::string::npos);

    SECTION("the stream really carries them, read back by the decoder") {
        const auto wav_out = dir / "map_obj_back.wav";
        const auto dec_log = dir / "map_obj_decode.log";
        REQUIRE(run_cli("decode \"" + out.string() + "\" \"" + wav_out.string() + "\"",
                        dec_log) == 0);
        // Four dynamic objects plus the bed's LFE.
        CHECK(read_log(dec_log).find("4 dynamic objects") != std::string::npos);
    }

    SECTION("a map= naming no object destination is refused, not silently empty") {
        CHECK(run_cli("atmos-encode \"" + a.string() + "\" \"" + (dir / "map_none.ec3").string() +
                          "\" 448 map=0.0:L,0.1:R,0.2:none,0.3:none",
                      log) == 1);
    }

    SECTION("src= without map= makes every loaded channel an object, in load order") {
        const auto plain = dir / "map_obj_plain.ec3";
        REQUIRE(run_cli("atmos-encode \"" + a.string() + "\" \"" + plain.string() + "\" 448 src=\"" +
                            b.string() + "\"",
                        log) == 0);
        CHECK(read_log(log).find("6 objects") != std::string::npos);
    }

    SECTION("[objects] and map= are alternatives, not a pair") {
        CHECK(run_cli("atmos-encode \"" + a.string() + "\" \"" + (dir / "map_both.ec3").string() +
                          "\" 448 2 src=\"" + b.string() + "\" map=0.0:obj,0.1:none,0.2:none,"
                          "0.3:none,1.0:none,1.1:none",
                      log) == 1);
    }
}

// --- roadmap IO7: the object-layer strip ----------------------------------

TEST_CASE("strip-objects leaves a decodable 5.1 stream with no object metadata",
          "[cli][strip-objects]") {
    const auto dir = scratch_dir();
    const auto log = dir / "strip.log";
    const auto atmos = dir / "strip_atmos.ec3";
    const auto bed = dir / "strip_bed51.ec3";
    const auto atmos_wav = dir / "strip_atmos.wav";
    const auto bed_wav = dir / "strip_bed51.wav";

    REQUIRE(run_cli("atmos \"" + atmos.string() + "\" 1 448 2 4 objects", log) == 0);
    REQUIRE(run_cli("strip-objects \"" + atmos.string() + "\" \"" + bed.string() + "\"", log) == 0);
    const auto report = read_log(log);
    CHECK(report.find("no object metadata remains") != std::string::npos);
    CHECK(fs::file_size(bed) < fs::file_size(atmos));

    // The claim the command makes about the audio, checked through the
    // decoder rather than taken on trust: the bed is bit-identical.
    REQUIRE(run_cli("decode \"" + atmos.string() + "\" \"" + atmos_wav.string() + "\"", log) == 0);
    REQUIRE(run_cli("decode \"" + bed.string() + "\" \"" + bed_wav.string() + "\"", log) == 0);
    const auto read_all_bytes = [](const fs::path& p) {
        std::ifstream in{p, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    };
    CHECK(read_all_bytes(atmos_wav) == read_all_bytes(bed_wav));

    // Nothing left to strip the second time round.
    const auto again = dir / "strip_bed51_again.ec3";
    REQUIRE(run_cli("strip-objects \"" + bed.string() + "\" \"" + again.string() + "\"", log) == 0);
    CHECK(read_all_bytes(again) == read_all_bytes(bed));
}

TEST_CASE("strip-objects refuses an AC-3 stream", "[cli][strip-objects]") {
    const auto dir = scratch_dir();
    const auto log = dir / "strip_ac3.log";
    const auto ac3 = dir / "strip_input.ac3";
    REQUIRE(run_cli("silence \"" + ac3.string() + "\" 1 192", log) == 0);
    CHECK(run_cli("strip-objects \"" + ac3.string() + "\" \"" + (dir / "strip_out.ec3").string() +
                      "\"",
                  log) != 0);
    CHECK(read_log(log).find("E-AC-3") != std::string::npos);
}

// --- roadmap IO6: the MPEG-TS broadcast profiles ---------------------------

TEST_CASE("ts writes the profile it is asked for", "[cli][ts]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ts_profile.log";
    const auto source = dir / "ts_profile.ac3";
    REQUIRE(run_cli("sine \"" + source.string() + "\" 1 448 440 60 51", log) == 0);

    const auto read_all_bytes = [](const fs::path& p) {
        std::ifstream in{p, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    };
    const auto ts_of = [&](std::string_view profile, const fs::path& out) {
        const std::string tail = profile.empty() ? std::string{} : " " + std::string{profile};
        REQUIRE(run_cli("ts \"" + source.string() + "\" \"" + out.string() + "\"" + tail, log) ==
                0);
        return read_all_bytes(out);
    };

    const auto implicit = ts_of("", dir / "ts_implicit.ts");
    const auto dvb = ts_of("dvb", dir / "ts_dvb.ts");
    const auto atsc = ts_of("atsc", dir / "ts_atsc.ts");
    // DVB is the default, so an unqualified invocation is unchanged.
    CHECK(implicit == dvb);
    // ATSC differs only in the PMT, so the files are the same length but not
    // the same bytes - a stream_type and a descriptor apart.
    CHECK(atsc.size() == dvb.size());
    CHECK(atsc != dvb);

    CHECK(run_cli("ts \"" + source.string() + "\" \"" + (dir / "ts_bad.ts").string() + "\" pal",
                  log) != 0);
    CHECK(read_log(log).find("dvb or atsc") != std::string::npos);
}

TEST_CASE("mainid= and asvc= are range-checked", "[cli][ts]") {
    const auto dir = scratch_dir();
    const auto log = dir / "ts_service.log";
    const auto source = dir / "ts_service.ac3";
    const auto out = dir / "ts_service.ts";
    REQUIRE(run_cli("sine \"" + source.string() + "\" 1 192 440 60 stereo", log) == 0);

    CHECK(run_cli("ts \"" + source.string() + "\" \"" + out.string() + "\" atsc mainid=7", log) ==
          0);
    CHECK(run_cli("ts \"" + source.string() + "\" \"" + out.string() + "\" dvb asvc=0xFF", log) ==
          0);
    CHECK(run_cli("ts \"" + source.string() + "\" \"" + out.string() + "\" atsc mainid=8", log) !=
          0);
    CHECK(run_cli("ts \"" + source.string() + "\" \"" + out.string() + "\" dvb asvc=256", log) !=
          0);
    CHECK(run_cli("ts \"" + source.string() + "\" \"" + out.string() + "\" dvb mainid=x", log) !=
          0);
}

TEST_CASE("fmp4 fallback-51 writes the paired rendition into one EXT-X-MEDIA group",
          "[cli][fmp4][strip-objects]") {
    const auto dir = scratch_dir();
    const auto log = dir / "fmp4_fallback.log";
    const auto atmos = dir / "fallback_atmos.ec3";
    const auto out_dir = dir / "fallback_out";
    fs::remove_all(out_dir);
    REQUIRE(run_cli("atmos \"" + atmos.string() + "\" 1 448 2 4 objects", log) == 0);
    REQUIRE(run_cli("fmp4 \"" + atmos.string() + "\" \"" + out_dir.string() + "\" 4 fallback-51",
                    log) == 0);

    CHECK(fs::exists(out_dir / "bed51" / "init.mp4"));
    CHECK(fs::exists(out_dir / "bed51" / "audio.m3u8"));
    std::ifstream master_in{out_dir / "master.m3u8", std::ios::binary};
    const std::string master{std::istreambuf_iterator<char>{master_in},
                             std::istreambuf_iterator<char>{}};
    CHECK(master.find("/JOC\"") != std::string::npos);
    CHECK(master.find("CHANNELS=\"6\"") != std::string::npos);
    CHECK(master.find("URI=\"bed51/audio.m3u8\"") != std::string::npos);

    // A stream with no object layer has no companion to write, and says so
    // rather than writing an empty directory.
    const auto plain = dir / "fallback_plain.ec3";
    const auto plain_dir = dir / "fallback_plain_out";
    fs::remove_all(plain_dir);
    REQUIRE(run_cli("eac3-sine \"" + plain.string() + "\" 1 192 440 50 stereo", log) == 0);
    REQUIRE(run_cli("fmp4 \"" + plain.string() + "\" \"" + plain_dir.string() + "\" 4 fallback-51",
                    log) == 0);
    CHECK(read_log(log).find("carries no object layer") != std::string::npos);
    CHECK_FALSE(fs::exists(plain_dir / "bed51"));
}

// ROADMAP.md's IO2: 'demux' is the inverse of 'mkv', and the pair is only
// worth anything if it is a true inverse - the elementary stream that goes
// into a container has to be the one that comes back out, byte for byte. A
// container reader that dropped a frame, mis-split a block or trimmed a
// trailing byte would still produce something a decoder mostly plays, which
// is exactly why this is checked as bytes and not as audio.
TEST_CASE("demux recovers the exact elementary stream a container wrapped",
          "[cli][demux]") {
    const auto dir = scratch_dir();
    const auto log = dir / "demux.log";

    // read_bytes is a lambda local to another test case above; this one
    // needs the same thing and defines its own rather than reaching into it.
    const auto read_bytes = [](const fs::path& path) {
        std::ifstream in{path, std::ios::binary};
        REQUIRE(in.is_open());
        return std::vector<char>{std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{}};
    };

    // Both containers, from the same elementary stream: whichever one wrapped
    // it, demux has to hand back the identical bytes. `wrap` names the
    // wrapping command, which is also the extension the container gets.
    const auto check_round_trip = [&](const std::string& make, const fs::path& elementary,
                                      const std::string& wrap) {
        const auto container = dir / (elementary.stem().string() + "." + wrap);
        const auto recovered = dir / (elementary.stem().string() + "." + wrap + ".back");
        REQUIRE(run_cli(make, log) == 0);
        REQUIRE(run_cli(wrap + " \"" + elementary.string() + "\" \"" + container.string() + "\"",
                        log) == 0);
        REQUIRE(run_cli("demux \"" + container.string() + "\" \"" + recovered.string() + "\"",
                        log) == 0);
        CHECK(read_bytes(recovered) == read_bytes(elementary));
    };

    SECTION("E-AC-3 through Matroska") {
        const auto es = dir / "demux_eac3.ec3";
        check_round_trip("eac3-sine \"" + es.string() + "\" 2 448 440 60 51", es, "mkv");
    }

    SECTION("AC-3 through Matroska") {
        const auto es = dir / "demux_ac3.ac3";
        check_round_trip("sine \"" + es.string() + "\" 2 192 440 60 stereo", es, "mkv");
    }

    SECTION("E-AC-3 through MP4") {
        const auto es = dir / "demux_eac3_mp4.ec3";
        check_round_trip("eac3-sine \"" + es.string() + "\" 2 448 440 60 51", es, "mp4");
    }

    SECTION("AC-3 through MP4") {
        const auto es = dir / "demux_ac3_mp4.ac3";
        check_round_trip("sine \"" + es.string() + "\" 2 192 440 60 stereo", es, "mp4");
    }

    SECTION("E-AC-3 through MPEG-TS") {
        const auto es = dir / "demux_eac3_ts.ec3";
        check_round_trip("eac3-sine \"" + es.string() + "\" 2 448 440 60 51", es, "ts");
    }

    SECTION("AC-3 through MPEG-TS") {
        const auto es = dir / "demux_ac3_ts.ac3";
        check_round_trip("sine \"" + es.string() + "\" 2 192 440 60 stereo", es, "ts");
    }
}

TEST_CASE("demux refuses what is not a container it reads", "[cli][demux]") {
    const auto dir = scratch_dir();
    const auto log = dir / "demux_bad.log";

    // Same local helper as the round-trip case above.
    const auto read_bytes = [](const fs::path& path) {
        std::ifstream in{path, std::ios::binary};
        REQUIRE(in.is_open());
        return std::vector<char>{std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{}};
    };

    SECTION("a bare elementary stream is not a container") {
        // The single most likely mistake, and the one where naming the file
        // .mkv or .mp4 would have made a name-based guess say yes.
        const auto es = dir / "demux_bare.ac3";
        REQUIRE(run_cli("silence \"" + es.string() + "\" 1 192", log) == 0);
        CHECK(run_cli("demux \"" + es.string() + "\" \"" + (dir / "demux_bare.out").string() +
                          "\"",
                      log) != 0);
    }

    SECTION("a truncated container reports the reader's own error") {
        const auto es = dir / "demux_trunc.ac3";
        const auto mkv = dir / "demux_trunc.mkv";
        const auto cut = dir / "demux_trunc_cut.mkv";
        REQUIRE(run_cli("silence \"" + es.string() + "\" 1 192", log) == 0);
        REQUIRE(run_cli("mkv \"" + es.string() + "\" \"" + mkv.string() + "\"", log) == 0);
        // Keep only the first 20 bytes: past the EBML header id, nowhere
        // near a track, so there is nothing to hand back.
        const auto whole = read_bytes(mkv);
        REQUIRE(whole.size() > 20);
        {
            std::ofstream out{cut, std::ios::binary};
            out.write(reinterpret_cast<const char*>(whole.data()), 20);
        }
        CHECK(run_cli("demux \"" + cut.string() + "\" \"" + (dir / "demux_trunc.out").string() +
                          "\"",
                      log) != 0);
    }

    SECTION("a missing input file") {
        CHECK(run_cli("demux \"" + (dir / "demux_absent.mkv").string() + "\" \"" +
                          (dir / "demux_absent.out").string() + "\"",
                      log) != 0);
    }
}

// Roadmap IO2's remaining half: decode/qc/levels (play/monitor share the same
// read_elementary_stream call and need real audio hardware to exercise, so
// are not re-tested here) all take a container in place of a raw .ac3/.ec3,
// sniffed by content rather than by extension - exactly what demux already
// does, reused via apps/common/container_input.hpp's
// ac3::apps::elementary_stream_from_bytes rather than duplicated a third
// time (support.cpp's own read_elementary_stream, and the GUI's
// qc_controller.cpp/object_decode_controller.cpp).
TEST_CASE("decode/qc/levels accept a container in place of a raw elementary stream",
          "[cli][io2]") {
    const auto dir = scratch_dir();
    const auto log = dir / "io2_widen.log";

    const auto es = dir / "io2_widen.ec3";
    REQUIRE(run_cli("eac3-sine \"" + es.string() + "\" 1 448 440 60 51", log) == 0);
    const auto mkv = dir / "io2_widen.mkv";
    REQUIRE(run_cli("mkv \"" + es.string() + "\" \"" + mkv.string() + "\"", log) == 0);
    const auto mp4 = dir / "io2_widen.mp4";
    REQUIRE(run_cli("mp4 \"" + es.string() + "\" \"" + mp4.string() + "\"", log) == 0);
    const auto ts = dir / "io2_widen.ts";
    REQUIRE(run_cli("ts \"" + es.string() + "\" \"" + ts.string() + "\"", log) == 0);

    SECTION("decode") {
        const auto read_bytes = [](const fs::path& path) {
            std::ifstream in{path, std::ios::binary};
            REQUIRE(in.is_open());
            return std::vector<char>{std::istreambuf_iterator<char>{in},
                                     std::istreambuf_iterator<char>{}};
        };
        const auto from_es = dir / "io2_widen_from_es.wav";
        REQUIRE(run_cli("decode \"" + es.string() + "\" \"" + from_es.string() + "\"", log) == 0);
        const auto expected = read_bytes(from_es);
        for (const auto& container : {mkv, mp4, ts}) {
            CAPTURE(container);
            const auto out = dir / (container.stem().string() + "_decoded.wav");
            CHECK(run_cli("decode \"" + container.string() + "\" \"" + out.string() + "\"", log) ==
                 0);
            CHECK(read_bytes(out) == expected);
        }
    }

    SECTION("qc") {
        for (const auto& container : {mkv, mp4, ts}) {
            CAPTURE(container);
            CHECK(run_cli("qc \"" + container.string() + "\"", log) == 0);
        }
    }

    SECTION("levels") {
        for (const auto& container : {mkv, mp4, ts}) {
            CAPTURE(container);
            CHECK(run_cli("levels \"" + container.string() + "\"", log) == 0);
        }
    }
}

// --- remux (roadmap IO2) -----------------------------------------------------

TEST_CASE("remux converts one container straight to another", "[cli][remux]") {
    const auto dir = scratch_dir();
    const auto log = dir / "remux.log";
    const auto read_bytes = [](const fs::path& path) {
        std::ifstream in{path, std::ios::binary};
        REQUIRE(in.is_open());
        return std::vector<char>{std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{}};
    };

    const auto es = dir / "remux_source.ec3";
    REQUIRE(run_cli("eac3-sine \"" + es.string() + "\" 1 448 440 60 51", log) == 0);
    const auto mkv = dir / "remux_source.mkv";
    REQUIRE(run_cli("mkv \"" + es.string() + "\" \"" + mkv.string() + "\"", log) == 0);

    // Matroska carries no dec3 box at all, so remuxing it to MP4 exercises
    // exactly the claim run_mp4's own comment makes: codec_config is built
    // from the re-scanned bitstream (ac3::io::build_codec_config_box), never
    // from what the source container could or could not declare. If that
    // ever regressed to reading a source-side box instead, this MP4 would
    // have nothing to build one from and mp4::mux would refuse it outright -
    // the round trip through demux below is what proves it wrote a coherent
    // one rather than merely that it wrote *something*.
    SECTION("Matroska to MP4 round-trips through demux") {
        const auto mp4 = dir / "remux_mkv_to_mp4.mp4";
        REQUIRE(run_cli("remux \"" + mkv.string() + "\" \"" + mp4.string() + "\"", log) == 0);
        const auto recovered = dir / "remux_mkv_to_mp4.ec3";
        REQUIRE(run_cli("demux \"" + mp4.string() + "\" \"" + recovered.string() + "\"", log) ==
               0);
        CHECK(read_bytes(recovered) == read_bytes(es));
    }

    SECTION("MP4 to MPEG-TS round-trips through demux") {
        const auto mp4 = dir / "remux_source.mp4";
        REQUIRE(run_cli("mp4 \"" + es.string() + "\" \"" + mp4.string() + "\"", log) == 0);
        const auto ts = dir / "remux_mp4_to_ts.ts";
        REQUIRE(run_cli("remux \"" + mp4.string() + "\" \"" + ts.string() + "\"", log) == 0);
        const auto recovered = dir / "remux_mp4_to_ts.ec3";
        REQUIRE(run_cli("demux \"" + ts.string() + "\" \"" + recovered.string() + "\"", log) == 0);
        CHECK(read_bytes(recovered) == read_bytes(es));
    }

    SECTION("a bare elementary stream remuxes straight to a container too") {
        const auto mp4 = dir / "remux_bare_to_mp4.mp4";
        REQUIRE(run_cli("remux \"" + es.string() + "\" \"" + mp4.string() + "\"", log) == 0);
        const auto recovered = dir / "remux_bare_to_mp4.ec3";
        REQUIRE(run_cli("demux \"" + mp4.string() + "\" \"" + recovered.string() + "\"", log) ==
               0);
        CHECK(read_bytes(recovered) == read_bytes(es));
    }
}

TEST_CASE("remux refuses an output extension it does not write", "[cli][remux]") {
    const auto dir = scratch_dir();
    const auto log = dir / "remux_bad.log";
    const auto es = dir / "remux_bad_source.ec3";
    REQUIRE(run_cli("eac3-sine \"" + es.string() + "\" 1 448 440 60 51", log) == 0);
    const auto mkv = dir / "remux_bad_source.mkv";
    REQUIRE(run_cli("mkv \"" + es.string() + "\" \"" + mkv.string() + "\"", log) == 0);

    const auto rc = run_cli(
        "remux \"" + mkv.string() + "\" \"" + (dir / "remux_bad.xyz").string() + "\"", log);
    CHECK(rc != 0);
    CHECK(read_log(log).find("does not name a container this build writes") != std::string::npos);
}

// --- unspdif (roadmap IO3) ---------------------------------------------------

TEST_CASE("cli: unspdif recovers the exact stream 'spdif' wrapped", "[cli][unspdif]") {
    const auto dir = scratch_dir();
    const auto log = dir / "unspdif.log";
    const auto file_bytes = [](const fs::path& p) {
        std::ifstream in{p, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{}};
    };

    // AC-3 and E-AC-3 both: their burst periods, their Pd units (bits vs
    // bytes) and their carrier rates all differ, so one passing says nothing
    // about the other.
    const auto ac3 = dir / "unspdif_src.ac3";
    REQUIRE(run_cli("sine \"" + ac3.string() + "\" 1 192 1000 50 51", log) == 0);
    const auto ac3_wav = dir / "unspdif_src_ac3.wav";
    REQUIRE(run_cli("spdif \"" + ac3.string() + "\" \"" + ac3_wav.string() + "\"", log) == 0);
    const auto ac3_back = dir / "unspdif_back.ac3";
    REQUIRE(run_cli("unspdif \"" + ac3_wav.string() + "\" \"" + ac3_back.string() + "\"", log) ==
            0);
    CHECK(file_bytes(ac3_back) == file_bytes(ac3));

    const auto ec3 = dir / "unspdif_src.ec3";
    REQUIRE(run_cli("eac3-sine \"" + ec3.string() + "\" 1 192 1000 50 51", log) == 0);
    const auto ec3_wav = dir / "unspdif_src_ec3.wav";
    REQUIRE(run_cli("spdif \"" + ec3.string() + "\" \"" + ec3_wav.string() + "\"", log) == 0);
    const auto ec3_back = dir / "unspdif_back.ec3";
    REQUIRE(run_cli("unspdif \"" + ec3_wav.string() + "\" \"" + ec3_back.string() + "\"", log) ==
            0);
    CHECK(file_bytes(ec3_back) == file_bytes(ec3));

    // What came back is a stream in its own right, not only a byte match:
    // it decodes.
    const auto decoded = dir / "unspdif_back.wav";
    CHECK(run_cli("decode \"" + ec3_back.string() + "\" \"" + decoded.string() + "\"", log) == 0);
}

TEST_CASE("cli: unspdif reads a bare carrier as well as a WAV", "[cli][unspdif]") {
    const auto dir = scratch_dir();
    const auto log = dir / "unspdif_raw.log";
    const auto ac3 = dir / "unspdif_raw_src.ac3";
    REQUIRE(run_cli("sine \"" + ac3.string() + "\" 1 192 1000 50 stereo", log) == 0);
    const auto wav = dir / "unspdif_raw.wav";
    REQUIRE(run_cli("spdif \"" + ac3.string() + "\" \"" + wav.string() + "\"", log) == 0);

    // The same carrier with its RIFF header cut off - what a capture tool
    // that dumps raw device bytes leaves behind. kWavHeaderBytes is 44 for
    // every WAV this project writes (see write_wav_pcm16_raw).
    std::ifstream in{wav, std::ios::binary};
    REQUIRE(in.is_open());
    const std::vector<char> whole{std::istreambuf_iterator<char>{in},
                                  std::istreambuf_iterator<char>{}};
    REQUIRE(whole.size() > 44);
    const auto raw = dir / "unspdif_raw.carrier";
    {
        std::ofstream out{raw, std::ios::binary};
        out.write(whole.data() + 44, static_cast<std::streamsize>(whole.size() - 44));
    }
    const auto back = dir / "unspdif_raw_back.ac3";
    REQUIRE(run_cli("unspdif \"" + raw.string() + "\" \"" + back.string() + "\"", log) == 0);

    std::ifstream a{ac3, std::ios::binary};
    std::ifstream b{back, std::ios::binary};
    const std::vector<char> expected{std::istreambuf_iterator<char>{a},
                                     std::istreambuf_iterator<char>{}};
    const std::vector<char> got{std::istreambuf_iterator<char>{b},
                                std::istreambuf_iterator<char>{}};
    CHECK(got == expected);
}

TEST_CASE("cli: unspdif refuses ordinary PCM and leaves no output behind", "[cli][unspdif]") {
    const auto dir = scratch_dir();
    const auto log = dir / "unspdif_pcm.log";
    const auto ac3 = dir / "unspdif_pcm_src.ac3";
    REQUIRE(run_cli("sine \"" + ac3.string() + "\" 1 192 1000 50 stereo", log) == 0);
    const auto pcm = dir / "unspdif_pcm.wav";
    REQUIRE(run_cli("decode \"" + ac3.string() + "\" \"" + pcm.string() + "\"", log) == 0);

    const auto out = dir / "unspdif_pcm_out.ac3";
    fs::remove(out);
    CHECK(run_cli("unspdif \"" + pcm.string() + "\" \"" + out.string() + "\"", log) != 0);
    CHECK(read_log(log).find("no AC-3 or E-AC-3 bursts") != std::string::npos);
    // A failed run leaves no half-written stream to be mistaken for output.
    CHECK_FALSE(fs::exists(out));

    // And a file that is not there at all is a clean error, not a crash.
    CHECK(run_cli("unspdif \"" + (dir / "definitely_absent.wav").string() + "\" \"" +
                      out.string() + "\"",
                  log) != 0);
}

TEST_CASE("cli: unspdif writes a clean stream to stdout, status text to stderr", "[cli][unspdif]") {
    // The convention encode/decode already follow (status_stream): with "-"
    // as the output, the human-readable report must not land in the middle of
    // the binary a pipeline is reading. Worth its own test because getting it
    // wrong is invisible until something downstream chokes - the report is
    // valid-looking text prepended to a valid stream.
    const auto dir = scratch_dir();
    const auto log = dir / "unspdif_stdout.log";
    const auto ac3 = dir / "unspdif_stdout_src.ac3";
    REQUIRE(run_cli("sine \"" + ac3.string() + "\" 1 192 1000 50 stereo", log) == 0);
    const auto wav = dir / "unspdif_stdout.wav";
    REQUIRE(run_cli("spdif \"" + ac3.string() + "\" \"" + wav.string() + "\"", log) == 0);

    const auto piped = dir / "unspdif_stdout.ac3";
    REQUIRE(run_cli_stdout("unspdif \"" + wav.string() + "\" -", piped, log) == 0);

    std::ifstream a{ac3, std::ios::binary};
    std::ifstream b{piped, std::ios::binary};
    const std::vector<char> expected{std::istreambuf_iterator<char>{a},
                                     std::istreambuf_iterator<char>{}};
    const std::vector<char> got{std::istreambuf_iterator<char>{b},
                                std::istreambuf_iterator<char>{}};
    CHECK(got == expected);
    // And the report did go somewhere - to stderr, not nowhere.
    CHECK(read_log(log).find("unwrapped") != std::string::npos);
}

TEST_CASE("cli: unspdif reads the carrier from stdin", "[cli][unspdif]") {
    // The natural shape of this on a machine with a real S/PDIF input is a
    // capture tool piped straight in, so "-" has to work on the input side
    // too - and it takes a different code path from the file one, which
    // seeks and walks the RIFF chunk list. Here the WAV header is simply
    // scanned past, which is only safe because a header cannot contain a
    // preamble with a syncframe behind it.
    const auto dir = scratch_dir();
    const auto log = dir / "unspdif_stdin.log";
    const auto ec3 = dir / "unspdif_stdin_src.ec3";
    REQUIRE(run_cli("eac3-sine \"" + ec3.string() + "\" 1 192 1000 50 stereo", log) == 0);
    const auto wav = dir / "unspdif_stdin.wav";
    REQUIRE(run_cli("spdif \"" + ec3.string() + "\" \"" + wav.string() + "\"", log) == 0);

    // Both ends piped at once, the way a shell would use it.
    const auto piped = dir / "unspdif_stdin.ec3";
    REQUIRE(run_cli_stdio("unspdif - -", wav, piped, log) == 0);

    std::ifstream a{ec3, std::ios::binary};
    std::ifstream b{piped, std::ios::binary};
    const std::vector<char> expected{std::istreambuf_iterator<char>{a},
                                     std::istreambuf_iterator<char>{}};
    const std::vector<char> got{std::istreambuf_iterator<char>{b},
                                std::istreambuf_iterator<char>{}};
    CHECK(got == expected);
}
