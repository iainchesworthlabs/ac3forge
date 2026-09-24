#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"

// The synthetic generators (apps/cli/commands/synth.cpp: silence, sine,
// orbit, eac3-silence, eac3-sine) and the two object generators that sit
// beside them in commands/atmos.cpp (atmos, atmos-path), at the level a user
// meets them: the real binary as a subprocess, its exit code, what it says,
// and the file it leaves - or, on every refusal here, does not leave. These
// commands stream their output as they encode with keep-partial hard-off
// (their output is regenerable), so "no file after a failure" is part of
// each command's contract rather than an accident of when the error struck.
//
// Every run is one second long: 32 AC-3 frames at 48 kHz, which is short
// enough to cost next to nothing and long enough to count.
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
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("cli_synth_" + scratch_pid_suffix());
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

// An output path whose parent directory does not exist, so opening it for
// writing fails the same way on every platform.
fs::path unwritable(const fs::path& dir, std::string_view name) {
    return dir / "no_such_directory" / name;
}

// A command that must stop with `exit_code`, say `message`, and leave no
// file at `out_path`.
void check_refused(const std::string& args, const fs::path& out_path, const fs::path& log,
                   int exit_code, std::string_view message) {
    fs::remove(out_path);
    const auto rc = run_cli(args, log);
    const auto text = read_log(log);
    INFO(text);
    CHECK(rc == exit_code);
    CHECK(text.find(message) != std::string::npos);
    CHECK_FALSE(fs::exists(out_path));
}

}  // namespace

TEST_CASE("orbit writes a 5.1 AC-3 stream of the asked length and reports the orbit period",
          "[cli][synth]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "orbit.ac3";
    const auto log = dir / "orbit.log";
    fs::remove(out_path);
    const auto rc = run_cli("orbit " + quoted(out_path) + " 1 192 4", log);
    const auto text = read_log(log);
    INFO(text);
    REQUIRE(rc == 0);
    CHECK(text.find("wrote 32 5.1 frames: 440 Hz tone orbiting every 4 s -> " +
                    out_path.string()) != std::string::npos);
    // The channel summary is what shows no speaker was left out of the orbit.
    CHECK(text.find("per-channel levels (3/2 + LFE):") != std::string::npos);

    // 192 kbit/s at 48 kHz is 768 bytes per 1536-sample frame (Table 5.18).
    const auto bytes = read_bytes(out_path);
    CHECK(bytes.size() == 32u * 768u);
    const auto frames = ac3::split_frames(bytes);
    REQUIRE(frames.has_value());
    CHECK(frames->size() == 32u);
    ac3::FrameDecoder decoder;
    const auto first = decoder.decode_frame(frames->front());
    REQUIRE(first.has_value());
    CHECK(first->acmod == ac3::Acmod::k3_2);
    CHECK(first->lfe);
    CHECK(first->bitrate_kbps == 192u);
}

TEST_CASE("orbit refuses an illegal bit rate and an unwritable output, leaving no file",
          "[cli][synth]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "orbit_refused.ac3";
    const auto log = dir / "orbit_refused.log";
    check_refused("orbit " + quoted(out_path) + " 1 191", out_path, log, 1,
                  "error: bitrate must be a legal AC-3 rate");
    const auto nowhere = unwritable(dir, "orbit.ac3");
    check_refused("orbit " + quoted(nowhere) + " 1", nowhere, log, 3,
                  "error: cannot open " + nowhere.string() + " for writing");
}

TEST_CASE("eac3-silence writes one repeated access unit per frame of the asked duration",
          "[cli][synth]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "eac3_silence.ec3";
    const auto log = dir / "eac3_silence.log";
    fs::remove(out_path);
    const auto rc = run_cli("eac3-silence " + quoted(out_path) + " 1 192 51", log);
    const auto text = read_log(log);
    INFO(text);
    REQUIRE(rc == 0);
    CHECK(text.find("wrote 32 silent E-AC-3 5.1 access units (1 substreams, 768 bytes each, "
                    "bsid 16) to " +
                    out_path.string()) != std::string::npos);
    const auto bytes = read_bytes(out_path);
    CHECK(bytes.size() == 32u * 768u);
    const auto frames = ac3::split_frames(bytes);
    REQUIRE(frames.has_value());
    CHECK(frames->size() == 32u);
    // Every unit is the same unit - that is what "repeated" means here.
    for (const auto& frame : *frames) {
        CHECK(std::equal(frame.begin(), frame.end(), frames->front().begin(),
                         frames->front().end()));
    }
}

TEST_CASE("eac3-silence refuses an unknown layout, an unframable rate and an unwritable output",
          "[cli][synth]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "eac3_silence_refused.ec3";
    const auto log = dir / "eac3_silence_refused.log";
    check_refused("eac3-silence " + quoted(out_path) + " 1 192 bogus", out_path, log, 1,
                  "error: unknown layout 'bogus' (mono | stereo | 1+1 | 51 | 71 | 512 | 514 | "
                  "714)");
    check_refused("eac3-silence " + quoted(out_path) + " 1 7 51", out_path, log, 1,
                  "error: invalid E-AC-3 configuration");
    const auto nowhere = unwritable(dir, "silence.ec3");
    check_refused("eac3-silence " + quoted(nowhere) + " 1", nowhere, log, 3,
                  "error: cannot open " + nowhere.string() + " for writing");
}

TEST_CASE("silence, sine and eac3-sine refuse an unwritable output with the output exit code",
          "[cli][synth]") {
    const auto dir = scratch_dir();
    const auto log = dir / "synth_unwritable.log";
    for (const std::string_view command : {"silence", "sine", "eac3-sine"}) {
        CAPTURE(command);
        const auto nowhere = unwritable(dir, std::string{command} + ".bin");
        check_refused(std::string{command} + " " + quoted(nowhere) + " 1", nowhere, log, 3,
                      "error: cannot open " + nowhere.string() + " for writing");
    }
}

TEST_CASE("silence, sine and eac3-sine refuse a rate they cannot frame, leaving no file",
          "[cli][synth]") {
    const auto dir = scratch_dir();
    const auto log = dir / "synth_rate.log";
    const auto ac3_out = dir / "synth_rate.ac3";
    const auto ec3_out = dir / "synth_rate.ec3";
    check_refused("silence " + quoted(ac3_out) + " 1 191", ac3_out, log, 1,
                  "error: bitrate must be one of the 19 legal AC-3 rates");
    check_refused("sine " + quoted(ac3_out) + " 1 191", ac3_out, log, 1,
                  "error: bitrate must be a legal AC-3 rate");
    check_refused("eac3-sine " + quoted(ec3_out) + " 1 7", ec3_out, log, 1,
                  "error: invalid E-AC-3 configuration");
    // 7.1 is a real layout, just not one AC-3 can carry: that is a
    // different refusal from a name nobody knows.
    check_refused("sine " + quoted(ac3_out) + " 1 192 1000 50 71", ac3_out, log, 1,
                  "error: AC-3 cannot carry 7.1 - that channel selection needs dependent "
                  "substreams, which only E-AC-3 has");
    check_refused("sine " + quoted(ac3_out) + " 1 192 1000 50 bogus", ac3_out, log, 1,
                  "error: unknown layout 'bogus' (mono | stereo | 1+1 | 51)");
}

TEST_CASE("sine's c layout suffix turns coupling on in every block", "[cli][synth]") {
    const auto dir = scratch_dir();
    const auto log = dir / "sine_couple.log";
    const auto probe_log = dir / "sine_couple_probe.log";

    const auto coupled = dir / "sine_coupled.ac3";
    REQUIRE(run_cli("sine " + quoted(coupled) + " 1 384 1000 50 51c", log) == 0);
    REQUIRE(run_cli("probe " + quoted(coupled), probe_log) == 0);
    const auto coupled_text = read_log(probe_log);
    INFO(coupled_text);
    CHECK(coupled_text.find("coupling      192 of 192 block(s)") != std::string::npos);

    // Without the suffix the same layout codes every channel on its own.
    const auto plain = dir / "sine_plain.ac3";
    REQUIRE(run_cli("sine " + quoted(plain) + " 1 384 1000 50 51", log) == 0);
    REQUIRE(run_cli("probe " + quoted(plain), probe_log) == 0);
    const auto plain_text = read_log(probe_log);
    INFO(plain_text);
    CHECK(plain_text.find("coupling ") == std::string::npos);
}

// The bare 'couple' token is the other spelling metadata-options.md gives
// for sine's coupling. It used to be read as though the layout carried the
// 'c' suffix, so the layout lost its last character instead: "51 couple"
// was refused as "unknown layout '5'", and the default as 'stere'.
TEST_CASE("sine's bare couple token turns coupling on for a named and the default layout",
          "[cli][synth]") {
    const auto dir = scratch_dir();
    const auto log = dir / "sine_couple_token.log";
    const auto probe_log = dir / "sine_couple_token_probe.log";
    struct Case {
        std::string_view name;
        std::string_view args;
    };
    for (const auto& c : {Case{"sine_couple_51.ac3", " 1 384 1000 50 51 couple"},
                          Case{"sine_couple_default.ac3", " 1 384 1000 50 couple"}}) {
        CAPTURE(c.args);
        const auto coupled = dir / c.name;
        REQUIRE(run_cli("sine " + quoted(coupled) + std::string{c.args}, log) == 0);
        REQUIRE(run_cli("probe " + quoted(coupled), probe_log) == 0);
        const auto text = read_log(probe_log);
        INFO(text);
        CHECK(text.find("coupling      192 of 192 block(s)") != std::string::npos);
    }
}

// Every generator counts whole seconds, and the [seconds] argument used to
// go through the same silent-fallback parse as the other numbers, so "0.5"
// quietly became the 5 s default. A token that is present but not a whole
// number is now refused by name.
TEST_CASE("the generators refuse a seconds argument that is not a whole number",
          "[cli][synth]") {
    const auto dir = scratch_dir();
    const auto log = dir / "synth_seconds.log";
    const auto out = dir / "synth_seconds.bin";
    for (const std::string_view command :
         {"silence", "sine", "orbit", "atmos", "eac3-silence", "eac3-sine"}) {
        CAPTURE(command);
        for (const std::string_view seconds : {"0.5", "5s", "-1"}) {
            CAPTURE(seconds);
            check_refused(std::string{command} + " " + quoted(out) + " " + std::string{seconds},
                          out, log, 1,
                          "error: seconds must be a whole number (got '" + std::string{seconds} +
                              "')");
        }
    }
    // atmos-path takes its seconds one argument later, after the scene file.
    check_refused("atmos-path " + quoted(out) + " scene.txt 0.5", out, log, 1,
                  "error: seconds must be a whole number (got '0.5')");
}

TEST_CASE("quiet silences a generator's report without changing what it writes",
          "[cli][synth]") {
    const auto dir = scratch_dir();
    const auto loud = dir / "sine_loud.ac3";
    const auto hushed = dir / "sine_quiet.ac3";
    const auto log = dir / "sine_quiet.log";
    REQUIRE(run_cli("sine " + quoted(loud) + " 1 192", log) == 0);
    CHECK(read_log(log).find("wrote 32 2/0 stereo frames (192 kbps)") != std::string::npos);
    REQUIRE(run_cli("sine " + quoted(hushed) + " 1 192 quiet", log) == 0);
    CHECK(read_log(log).empty());
    CHECK(read_bytes(loud) == read_bytes(hushed));
}

TEST_CASE("atmos refuses an object count, mode or rate it cannot honour", "[cli][synth][atmos]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "atmos_refused.ec3";
    const auto log = dir / "atmos_refused.log";
    check_refused("atmos " + quoted(out_path) + " 1 448 16", out_path, log, 1,
                  "error: 1 to 15 objects (the bed's LFE is the 16th, and TS 103 420 "
                  "\xC2\xA7" "8.3.2.2 caps the total at 16)");
    check_refused("atmos " + quoted(out_path) + " 1 448 2 4 sideways", out_path, log, 1,
                  "error: mode is 'objects' (default) or 'bed51'");
    // The frame's metadata and mantissas share one budget, so fifteen
    // objects' OAMD/JOC side data cannot fit a 64 kbit/s frame.
    check_refused("atmos " + quoted(out_path) + " 1 64 15", out_path, log, 1,
                  "error: cannot encode 15 objects at 64 kbps \xE2\x80\x94 the metadata and "
                  "the mantissas share one frame, so try a higher bit rate");
    const auto nowhere = unwritable(dir, "atmos.ec3");
    check_refused("atmos " + quoted(nowhere) + " 1 448 2", nowhere, log, 3,
                  "error: cannot open " + nowhere.string() + " for writing");
}

TEST_CASE("atmos sign-objects with an unreadable key discards the whole stream",
          "[cli][synth][atmos]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "atmos_badkey.ec3";
    const auto log = dir / "atmos_badkey.log";
    const auto key = dir / "no_such_key.pem";
    fs::remove(key);
    // Signing rewrites every frame after the encode, so the frames are held
    // back rather than streamed - and a key that cannot be read leaves
    // nothing on disk at all, not an unsigned stream.
    check_refused("atmos " + quoted(out_path) + " 1 448 2 4 objects sign-objects signing-key=" +
                      quoted(key),
                  out_path, log, 5, "error: cannot read signing key file '" + key.string() + "'");
}

TEST_CASE("atmos-path refuses a scene it cannot read or that names too many objects",
          "[cli][synth][atmos]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "atmos_path_refused.ec3";
    const auto log = dir / "atmos_path_refused.log";
    const auto write_scene = [&dir](std::string_view name, std::string_view body) {
        const auto path = dir / name;
        std::ofstream{path, std::ios::binary} << body;
        return path;
    };
    const auto two = write_scene("scene_two.txt", "0 0 0.1 0.2 0 1 0\n1 0 0.9 0.2 0 1 0\n");
    const auto sparse = write_scene("scene_sparse.txt", "0 0 0.1 0.2 0 1 0\n5 0 0.9 0.2 0 1 0\n");
    const auto garbage = write_scene("scene_garbage.txt", "garbage line\n");
    const auto duplicate =
        write_scene("scene_duplicate.txt", "0 0 0.1 0.2 0 1 0\n0 0 0.3 0.2 0 1 0\n");
    const auto missing = dir / "scene_missing.txt";
    fs::remove(missing);

    check_refused("atmos-path " + quoted(out_path) + " " + quoted(missing) + " 1 448", out_path,
                  log, 2, "error: cannot open " + missing.string());
    check_refused("atmos-path " + quoted(out_path) + " " + quoted(garbage) + " 1 448", out_path,
                  log, 2,
                  "error: " + garbage.string() + ":1: expected 'object time_s x y z gain lfe_send'");
    // Two keyframes at one instant for one object parse line by line but
    // cannot make a scene - reported against the file, without a line.
    check_refused("atmos-path " + quoted(out_path) + " " + quoted(duplicate) + " 1 448", out_path,
                  log, 2,
                  "error: " + duplicate.string() + ": object 0 has two automation points at t=0");
    check_refused("atmos-path " + quoted(out_path) + " " + quoted(two) + " 1 448 16", out_path,
                  log, 1, "error: 1 to 15 objects");
    check_refused("atmos-path " + quoted(out_path) + " " + quoted(sparse) + " 1 448 2", out_path,
                  log, 1,
                  "error: " + sparse.string() +
                      " has keyframes up to object index 5, more than the 2 objects requested");
    check_refused("atmos-path " + quoted(out_path) + " " + quoted(two) + " 1 64 2", out_path, log,
                  1, "error: cannot encode 2 objects at 64 kbps");
    const auto nowhere = unwritable(dir, "atmos_path.ec3");
    check_refused("atmos-path " + quoted(nowhere) + " " + quoted(two) + " 1 448", nowhere, log, 3,
                  "error: cannot open " + nowhere.string() + " for writing");
}

TEST_CASE("atmos-path pads a scene out to the asked object count", "[cli][synth][atmos]") {
    const auto dir = scratch_dir();
    const auto scene = dir / "scene_pad.txt";
    std::ofstream{scene, std::ios::binary} << "0 0 0.1 0.2 0 1 0\n1 0 0.9 0.2 0 1 0\n";
    const auto out_path = dir / "atmos_path_pad.ec3";
    const auto log = dir / "atmos_path_pad.log";
    fs::remove(out_path);
    // Object 2 is not in the file: it keeps atmos-path's own default
    // placement rather than being refused or silenced.
    const auto rc = run_cli("atmos-path " + quoted(out_path) + " " + quoted(scene) + " 1 448 3",
                            log);
    const auto text = read_log(log);
    INFO(text);
    REQUIRE(rc == 0);
    CHECK(text.find("wrote 32 E-AC-3 access units to " + out_path.string() + " (3 objects from " +
                    scene.string() + ")") != std::string::npos);

    const auto probe_log = dir / "atmos_path_pad_probe.log";
    REQUIRE(run_cli("probe " + quoted(out_path) + " json=1", probe_log) == 0);
    const auto probe = read_log(probe_log);
    INFO(probe);
    // Three dynamic objects plus the bed's LFE.
    CHECK(probe.find("\"dynamic\": 3") != std::string::npos);
    CHECK(probe.find("\"total\": 4") != std::string::npos);
}
