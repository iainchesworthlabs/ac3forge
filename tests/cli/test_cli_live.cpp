#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// The device-facing half of ac3cli: devices/outputs/record/live/monitor.
//
// These commands are the only place in this repository where the audio
// backend, the lock-free SPSC ring, the silence watchdog and the clock-drift
// servo are driven together by real code rather than by a unit test - and
// they were also the only part of apps/cli that no test touched at all
// (commands/audio_io.cpp and commands/live_audio.cpp both measured 0.0% line
// coverage when tools/checks/coverage_report.sh was first pointed at apps/,
// roadmap VX15).
//
// Every case here is written to hold on a machine with a working capture or
// render endpoint AND on a headless CI container with neither, because that
// is the only assertion worth making about a device path in CI: whichever
// way the enumeration goes, the command must terminate, say which way it
// went, and never fail silently. That is a weaker claim than "recording
// works" and deliberately so - it is the claim that can actually be checked
// without hardware, and it is exactly the claim the stdout/stderr-leak and
// argv-mangling bugs in this CLI's history broke.
//
// [concurrency] on every case: this file and tests/audio/ are what the
// ThreadSanitizer leg runs (roadmap VX16, `ctest -L concurrency` - see
// CMakePresets.json's test-linux-llvm-tsan preset). A race between the
// capture callback thread and the encoder thread is invisible to the
// ASan+UBSan leg, and these are the paths that start those threads.

namespace fs = std::filesystem;

namespace {

// See tests/cli/test_cli.cpp's own scratch_dir for the reasoning this copy
// shares; the leaf name below is this file's own.
fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "cli_live";
    fs::create_directories(dir);
    return dir;
}

// Same subprocess shape, and the same Windows cmd.exe quoting workaround, as
// tests/cli/test_cli.cpp's own run_cli - see that file for why the extra
// outer quote pair is needed there and must not be used on POSIX.
int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(AC3CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
#ifdef _WIN32
    const std::string wrapped = "\"" + command + "\"";
    return std::system(wrapped.c_str());
#else
    return std::system(command.c_str());
#endif
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// A device command has two legitimate outcomes and no third one: it did the
// work, or it said why it could not. What must never happen is a zero exit
// with nothing written, or a non-zero exit with nothing written - both of
// which look identical to a caller and are how a silently broken device path
// survives CI.
void check_spoke_either_way(int rc, const std::string& output) {
    CHECK_FALSE(output.empty());
    if (rc != 0) {
        CHECK(output.find("error") != std::string::npos);
    }
    // Under the ThreadSanitizer leg this binary is instrumented too, and a
    // race inside ac3cli would end the SUBPROCESS - which every check above
    // would otherwise read as an ordinary "no device here" refusal. TSan's
    // report never says "error", so the check above already fails on one by
    // accident; this says so on purpose, and names the reason in the output.
    CHECK(output.find("ThreadSanitizer") == std::string::npos);
    CHECK(output.find("AddressSanitizer") == std::string::npos);
    CHECK(output.find("runtime error:") == std::string::npos);
}

}  // namespace

TEST_CASE("devices enumerates or explains itself, and never does neither",
          "[cli][audio-io][concurrency]") {
    const auto log = scratch_dir() / "devices.log";
    const auto rc = run_cli("devices", log);
    const auto out = read_log(log);
    check_spoke_either_way(rc, out);
    // Whichever branch ran, it named what it was talking about: either the
    // "no active capture endpoints found" line, the table's own header, or
    // the platform's reason for having no capture capability at all.
    CHECK((out.find("capture") != std::string::npos || out.find("idx") != std::string::npos ||
           out.find("unavailable") != std::string::npos));
}

TEST_CASE("outputs enumerates or explains itself, and points at the spdif substitute",
          "[cli][audio-io][concurrency]") {
    const auto log = scratch_dir() / "outputs.log";
    const auto rc = run_cli("outputs", log);
    const auto out = read_log(log);
    check_spoke_either_way(rc, out);
    // main.cpp's Needs::kPassthrough branch is the one refusal in this CLI
    // that offers a portable alternative rather than just saying no; when it
    // is the branch that ran, the offer has to actually be there.
    if (out.find("is unavailable on this platform") != std::string::npos) {
        CHECK(out.find("ac3cli spdif") != std::string::npos);
    }
}

TEST_CASE("record either captures a real endpoint or refuses by name",
          "[cli][audio-io][concurrency]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "record.ac3";
    const auto log = dir / "record.log";
    fs::remove(out_path);

    // One second at the default bitrate: long enough to start the capture
    // thread, the ring and the watchdog on a machine that has an endpoint,
    // short enough not to matter to the suite's runtime on one that does not.
    const auto rc = run_cli("record \"" + out_path.string() + "\" 1", log);
    const auto out = read_log(log);
    check_spoke_either_way(rc, out);

    // The invariant that holds on both machines: a refusal leaves nothing
    // behind. A half-written .ac3 from a command that reported failure is
    // exactly the state 'keep-partial' exists to make explicit elsewhere.
    if (rc != 0) {
        CHECK_FALSE(fs::exists(out_path));
    } else {
        CHECK(fs::exists(out_path));
        CHECK(fs::file_size(out_path) > 0);
    }
}

TEST_CASE("live either runs a capture-to-monitor session or refuses by name",
          "[cli][audio-io][concurrency]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "live.ac3";
    const auto log = dir / "live.log";
    fs::remove(out_path);
    // <out> <capture_device> [seconds] - capture device 0 and a one-second
    // session, the shortest run main.cpp's argument table accepts.
    const auto rc = run_cli("live \"" + out_path.string() + "\" 0 1", log);
    check_spoke_either_way(rc, read_log(log));
    if (rc != 0) {
        CHECK_FALSE(fs::exists(out_path));
    }
}

TEST_CASE("a malformed positions= token is refused, by name whenever a device let it get there",
          "[cli][audio-io][concurrency]") {
    // A bad positions= token is checked well after the capture_device index
    // is (run_live's own order: device range checks, then mode/positions
    // validation, then capture.start()), so on a headless CI container with
    // no capture endpoint at all, device index 0 is out of range FIRST and
    // that is the refusal actually reported - still a refusal, just not
    // about positions=. The unconditional claim is "refused, nothing
    // written"; the positions=-specific claim only holds once a device
    // error is ruled out.
    const auto out_path = scratch_dir() / "positions_malformed.ac3";
    const auto log = scratch_dir() / "positions_malformed.log";
    fs::remove(out_path);
    const auto rc = run_cli(
        "live \"" + out_path.string() + "\" 0 1 192 -2 -2 atmos positions=not-a-real-token", log);
    const auto out = read_log(log);
    REQUIRE(rc != 0);
    CHECK_FALSE(fs::exists(out_path));
    if (out.find("capture device index") == std::string::npos) {
        CHECK(out.find("positions=") != std::string::npos);
        CHECK(out.find("scheme") != std::string::npos);
    }
}

TEST_CASE("positions= is refused outright with mode=channels",
          "[cli][audio-io][concurrency]") {
    // Unlike the malformed-token case above, this is a pure input-shape
    // conflict - live_audio.cpp checks it before any device enumeration, so
    // the refusal is unconditional once run_live() actually runs. On a build
    // with no capture backend compiled in at all (CI's no-alsa/posix leg),
    // main.cpp refuses the whole 'live' command one level higher, before
    // run_live() is ever reached, with its own "unavailable on this
    // platform" message - still a refusal, just not this one.
    const auto out_path = scratch_dir() / "positions_channels.ac3";
    const auto log = scratch_dir() / "positions_channels.log";
    fs::remove(out_path);
    const auto rc = run_cli(
        "live \"" + out_path.string() + "\" 0 1 192 -2 -2 channels positions=osc:9000", log);
    const auto out = read_log(log);
    REQUIRE(rc != 0);
    CHECK_FALSE(fs::exists(out_path));
    if (out.find("is unavailable on this platform") == std::string::npos) {
        CHECK(out.find("positions=") != std::string::npos);
    }
}

TEST_CASE("live mode=atmos positions=osc either runs a live-driven session or refuses by name",
          "[cli][audio-io][concurrency]") {
    const auto dir = scratch_dir();
    const auto out_path = dir / "live_positions.ec3";
    const auto log = dir / "live_positions.log";
    fs::remove(out_path);
    // port 0 asks the OS for an ephemeral port - this can never collide with
    // anything else running on the machine, the same reason
    // tests/audio/test_live_positions.cpp binds the same way.
    const auto rc =
        run_cli("live \"" + out_path.string() + "\" 0 1 192 -2 -2 atmos positions=osc:local:0",
                log);
    const auto out = read_log(log);
    check_spoke_either_way(rc, out);
    if (rc == 0) {
        // A real session ran: the listener status line is unconditional
        // whenever positions= is honoured, whether or not anything was
        // actually sent to it.
        CHECK(out.find("positions: OSC on") != std::string::npos);
        CHECK(fs::exists(out_path));
    } else {
        CHECK_FALSE(fs::exists(out_path));
    }
}

TEST_CASE("monitor either plays a stream or refuses by name", "[cli][audio-io][concurrency]") {
    const auto dir = scratch_dir();
    const auto stream = dir / "monitor_in.ac3";
    const auto log = dir / "monitor.log";

    // A real, decodable stream, so a machine that DOES have a render endpoint
    // exercises the decode-and-play path rather than bailing on a bad input.
    REQUIRE(run_cli("silence \"" + stream.string() + "\" 1", dir / "monitor_silence.log") == 0);
    REQUIRE(fs::exists(stream));

    const auto rc = run_cli("monitor \"" + stream.string() + "\"", log);
    check_spoke_either_way(rc, read_log(log));
}
