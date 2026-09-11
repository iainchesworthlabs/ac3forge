#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/emdf/emdf.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/oba/oamd.hpp"

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
// `redirects` follows the arguments on the command line.
int run_cli_redirected(const std::string& args, const std::string& redirects) {
    const std::string command = "\"" + std::string(AC3CLI_EXE) + "\" " + args + redirects;
#ifdef _WIN32
    const std::string wrapped = "\"" + command + "\"";
    return std::system(wrapped.c_str());
#else
    return std::system(command.c_str());
#endif
}

int run_cli(const std::string& args, const fs::path& log) {
    return run_cli_redirected(args, " > \"" + log.string() + "\" 2>&1");
}

// stdout and stderr in separate files. quiet's contract is about which of the
// two a line reaches - nothing on stdout, errors still on stderr - and the one
// log run_cli merges them into cannot show that.
int run_cli_split(const std::string& args, const fs::path& out, const fs::path& err) {
    return run_cli_redirected(args, " > \"" + out.string() + "\" 2> \"" + err.string() + "\"");
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// Half a second of silent 5.1 E-AC-3 whose every frame carries `program` as an
// OAMD payload (TS 103 420 §5.5) in an EMDF container, with no JOC payload
// beside it: a decoder reads the object layer and reconstructs no object
// audio. This is for the program shapes this project's own encoder never
// writes - AtmosEncoder's programs are always dynamic objects plus the bed's
// LFE.
void write_oamd_stream(const fs::path& path, const ac3::oba::Program& program,
                       std::span<const ac3::oba::DynamicObject> objects) {
    const auto payload = ac3::oba::build_payload(program, objects);
    const std::vector<ac3::emdf::Payload> payloads = {
        {.id = ac3::emdf::kPayloadIdOamd, .bytes = payload}};
    const auto container = ac3::emdf::build_container(payloads);

    ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    const std::vector<float> silence(static_cast<std::size_t>(encoder.samples_per_frame()), 0.0F);
    const std::vector<std::span<const float>> channels(
        static_cast<std::size_t>(encoder.channel_count()), silence);
    std::ofstream out{path, std::ios::binary};
    REQUIRE(out.is_open());
    for (int frame_index = 0; frame_index < 16; ++frame_index) {
        const auto frame = encoder.encode_frame(channels, container);
        REQUIRE(frame.has_value());
        out.write(reinterpret_cast<const char*>(frame->data()),
                  static_cast<std::streamsize>(frame->size()));
    }
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

TEST_CASE("monitor describes a stream's object layer the way decode does",
          "[cli][audio-io][atmos][concurrency]") {
    // monitor printed its own copy of decode's object-count line, and the copy
    // kept only the form this project's own streams need - "N dynamic objects
    // + the bed's LFE = M objects" - whatever the program was. decode names a
    // bed program's channels instead, and counts the LFE only when there is
    // one, so monitor misdescribed both programs below. Each stream is built
    // here because AtmosEncoder writes neither, and each is silent, like the
    // cases below, because on a machine with speakers monitor plays it.
    //
    // decode's report is checked on every machine. monitor prints its own only
    // once a render endpoint opens; without one, the check is that it spoke.
    const auto dir = scratch_dir();
    const auto check_both = [&dir](const std::string& name, const ac3::oba::Program& program,
                                   std::span<const ac3::oba::DynamicObject> objects,
                                   const std::string& line) {
        const auto stream = dir / (name + ".ec3");
        write_oamd_stream(stream, program, objects);

        const auto decode_log = dir / (name + "_decode.log");
        REQUIRE(run_cli("decode \"" + stream.string() + "\" \"" +
                            (dir / (name + ".wav")).string() + "\"",
                        decode_log) == 0);
        const auto decoded = read_log(decode_log);
        INFO("decode:\n" + decoded);
        CHECK(decoded.find(line) != std::string::npos);

        const auto monitor_log = dir / (name + "_monitor.log");
        const auto rc = run_cli("monitor \"" + stream.string() + "\"", monitor_log);
        const auto monitored = read_log(monitor_log);
        INFO("monitor:\n" + monitored);
        check_spoke_either_way(rc, monitored);
        if (rc == 0) {
            CHECK(monitored.find(line) != std::string::npos);
        }
    };
    const std::array<ac3::oba::DynamicObject, 2> objects{{
        {.position = {.x = 0.25, .y = 0.5, .z = 0.0}, .gain_db = 0.0},
        {.position = {.x = 0.75, .y = 0.5, .z = 0.0}, .gain_db = 0.0},
    }};

    SECTION("a bed program, as channel-based immersive content is, names its bed") {
        constexpr auto k514 = static_cast<std::uint16_t>(
            ac3::oba::bed::k51 | ac3::oba::bed::kTflTfr | ac3::oba::bed::kTblTbr);
        check_both("monitor_bed_program",
                   {.dynamic_only = false, .bed = k514, .dynamic_objects = 2}, objects,
                   "  bed [L R C LFE Ls Rs Tfl Tfr Tbl Tbr] + 2 dynamic objects = 12 objects, "
                   "OAMD present (JOC audio not reconstructed)");
    }
    SECTION("a dynamic-object-only program with no LFE object does not claim one") {
        check_both("monitor_no_lfe", {.dynamic_only = true, .lfe = false, .dynamic_objects = 2},
                   objects,
                   "  2 dynamic objects = 2 objects, OAMD present (JOC audio not reconstructed)");
    }
}

TEST_CASE("monitor prints nothing on stdout under quiet, whichever way it goes",
          "[cli][audio-io][quiet][concurrency]") {
    // Three of monitor's status lines went to stdout through plain
    // fmt::println, so quiet did not silence them: the verify-objects
    // summary, printed before any device is touched; the §7.8 fold note,
    // printed when the endpoint has fewer channels than the programme; and
    // the object-count line, printed once the sink has started. A signed
    // object stream reaches the first on any build with a monitor backend,
    // and the object-count line wherever a render endpoint opens - the fold
    // note too when that endpoint is narrower than the 5.1 bed. Silent, like
    // the case below: on a machine with speakers this plays out loud.
    const auto dir = scratch_dir();
    const auto key = dir / "monitor_quiet.key";
    {
        std::ofstream out{key, std::ios::binary};
        REQUIRE(out.is_open());
        out << "not-a-real-key-just-test-material";
    }
    const auto bed = dir / "monitor_quiet_bed.ac3";
    const auto pcm = dir / "monitor_quiet.wav";
    const auto stream = dir / "monitor_quiet.ec3";
    const auto setup = dir / "monitor_quiet_setup.log";
    REQUIRE(run_cli("silence \"" + bed.string() + "\" 1", setup) == 0);
    REQUIRE(run_cli("decode \"" + bed.string() + "\" \"" + pcm.string() + "\"", setup) == 0);
    REQUIRE(run_cli("atmos-encode \"" + pcm.string() + "\" \"" + stream.string() +
                        "\" 448 sign-objects signing-key=\"" + key.string() + "\"",
                    setup) == 0);

    const std::string monitor = "monitor \"" + stream.string() +
                                "\" verify-objects signing-key=\"" + key.string() + "\"";
    const auto out = dir / "monitor_quiet.out";
    const auto err = dir / "monitor_quiet.err";

    // Without quiet first, to show the lines are there to silence. A build
    // with no monitor backend refuses the command before it reads the stream.
    const auto loud_rc = run_cli_split(monitor, out, err);
    const auto loud = read_log(out);
    const auto loud_err = read_log(err);
    INFO("without quiet, stdout:\n" + loud + "\nstderr:\n" + loud_err);
    if (loud_err.find("is unavailable on this platform") == std::string::npos) {
        CHECK(loud.find("object signature") != std::string::npos);
    }
    if (loud_rc == 0) {
        CHECK(loud.find("OAMD present") != std::string::npos);
    }

    const auto rc = run_cli_split(monitor + " quiet", out, err);
    const auto quiet_err = read_log(err);
    INFO("with quiet, stderr:\n" + quiet_err);
    CHECK(read_log(out).empty());
    if (rc != 0) {
        CHECK(quiet_err.find("error") != std::string::npos);
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
