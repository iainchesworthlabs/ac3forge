#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/crc16.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"

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

void write_bytes(const fs::path& path, const std::vector<std::byte>& data) {
    std::ofstream out{path, std::ios::binary};
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(data.data()),
             static_cast<std::streamsize>(data.size()));
}

// Overwrite `count` bits at `offset` and restore the syncframe's trailing
// crc2, so a patched frame is still a legal, CRC-clean syncframe and the
// decoder's own semantic checks (not a CRC failure) are what reject it.
// Copied from tests/decoder/test_eac3_decoder.cpp's own helper of the same
// name - see that file's "the E-AC-3 decoder rejects malformed spectral
// extension streams" test, which this file's own "monitor reports a decode
// failure" test below reuses field-for-field.
void patch_bits(std::vector<std::byte>& frame, std::size_t offset, int count,
                std::uint32_t value) {
    for (int i = 0; i < count; ++i) {
        const std::size_t bit = offset + static_cast<std::size_t>(i);
        const auto mask = static_cast<std::uint8_t>(0x80U >> (bit & 7U));
        const auto set = (value >> (count - 1 - i)) & 1U;
        auto& target = frame[bit >> 3];
        target = set != 0 ? (target | std::byte{mask}) : (target & static_cast<std::byte>(~mask));
    }
    const auto bytes = frame.size();
    const std::uint16_t crc2 = ac3::crc16(std::span<const std::byte>{frame}.subspan(2, bytes - 4));
    frame[bytes - 2] = static_cast<std::byte>(crc2 >> 8);
    frame[bytes - 1] = static_cast<std::byte>(crc2 & 0xFF);
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

// 'play' (apps/cli/commands/audio_io.cpp's run_play) had no test in this
// suite at all until this one - unlike devices/outputs/record/live/monitor
// above, added when this file was, roadmap VX15 never reached it.

TEST_CASE("play either streams to a device or refuses by name", "[cli][audio-io][concurrency]") {
    const auto dir = scratch_dir();
    const auto stream = dir / "play_in.ac3";
    REQUIRE(run_cli("silence \"" + stream.string() + "\" 1", dir / "play_silence.log") == 0);
    REQUIRE(fs::exists(stream));

    const auto log = dir / "play.log";
    const auto rc = run_cli("play \"" + stream.string() + "\"", log);
    check_spoke_either_way(rc, read_log(log));
}

TEST_CASE("play refuses a stream too short to hold a syncframe, before any device is touched",
          "[cli][audio-io]") {
    // 3 bytes: not empty (read_elementary_stream's own "nothing at all"
    // refusal is a different, already-covered branch), but short of the 6
    // stream_bsid() needs. run_play reads this off the file before
    // enumerating or opening anything, so this holds identically on a
    // machine with real render hardware and on one with none at all.
    //
    // run_spatial has the identical check (live_audio.cpp's own line, one
    // read_all()/apply_object_verification() call ahead of it) but it is not
    // exercised here: main.cpp's Needs::kSpatial gate refuses the whole
    // 'spatial' command before run_spatial() is ever called on any build
    // without a real spatial backend - "this build has no spatial backend:
    // ISpatialAudioObjectRenderStream is a Windows-only API" (confirmed
    // against this exact build). Every line inside run_spatial() is
    // therefore unreachable through the CLI on the Linux/ALSA build this
    // suite runs on, and on any other non-Windows build - not merely
    // untested here, but dead from this entry point on every platform this
    // repository's CI actually runs a coverage job on. There is no
    // Windows coverage leg to reach it from either.
    const auto path = scratch_dir() / "too_short.ac3";
    write_bytes(path, {std::byte{0x0B}, std::byte{0x77}, std::byte{0x00}});

    const auto log = scratch_dir() / "play_too_short.log";
    const auto rc = run_cli("play \"" + path.string() + "\"", log);
    const auto out = read_log(log);
    INFO(out);
    CHECK(rc != 0);
    CHECK(out.find("too short to hold a syncframe") != std::string::npos);
}

TEST_CASE("play refuses a stream that claims E-AC-3/AC-3 but does not split into valid units",
          "[cli][audio-io]") {
    // run_play reads bsid straight off byte 5 to decide which of
    // split_access_units/split_frames to call, then reports whichever of
    // them fails - both checks run well before device enumeration, so
    // neither depends on what render hardware the machine running this test
    // has. A bad sync word (bytes 0-1) is enough to fail either split call
    // regardless of the rest of the header, which is why the two vectors
    // below only need to differ in the one byte (5) that decides bsid.
    SECTION("bsid > 8 (E-AC-3): split_access_units finds no valid access unit") {
        const auto path = scratch_dir() / "play_bad_eac3.ec3";
        write_bytes(path, {std::byte{0x0B}, std::byte{0x77}, std::byte{0x00}, std::byte{0x00},
                           std::byte{0x00}, std::byte{0x50}});
        const auto log = scratch_dir() / "play_bad_eac3.log";
        const auto rc = run_cli("play \"" + path.string() + "\"", log);
        const auto out = read_log(log);
        INFO(out);
        CHECK(rc != 0);
        CHECK(out.find("is not a valid E-AC-3 stream") != std::string::npos);
    }

    SECTION("bsid <= 8 (AC-3): split_frames finds no valid frame") {
        const auto path = scratch_dir() / "play_bad_ac3.ac3";
        // byte 4's top two bits (fscod) are 0b11, A/52's own reserved value -
        // syncframe_bytes() refuses it outright rather than looking up a
        // frame size.
        write_bytes(path, {std::byte{0x0B}, std::byte{0x77}, std::byte{0x00}, std::byte{0x00},
                           std::byte{0xFF}, std::byte{0x08}});
        const auto log = scratch_dir() / "play_bad_ac3.log";
        const auto rc = run_cli("play \"" + path.string() + "\"", log);
        const auto out = read_log(log);
        INFO(out);
        CHECK(rc != 0);
        CHECK(out.find("is not a valid AC-3 stream") != std::string::npos);
    }
}

TEST_CASE("monitor reports a decode failure by name, distinct from a device refusal",
          "[cli][audio-io]") {
    // A semantically invalid but framing-correct, CRC-correct E-AC-3 access
    // unit - spxbegf placed past spxendf, collapsing the spectral extension
    // region to nothing (see ac3::describe(DecodeError::kInvalidStream)) -
    // the exact vector tests/decoder/test_eac3_decoder.cpp's "the E-AC-3
    // decoder rejects malformed spectral extension streams" test already
    // validates bit-for-bit at the library level, reused here through the
    // CLI. run_monitor decodes its first access unit before ever calling
    // MonitorSink::start() (that only happens once a decode actually
    // succeeds), so unlike every other 'monitor' case in this file, this one
    // never depends on what render hardware is present.
    ac3::eac3::AccessUnitEncoder encoder{{.independent = {.bitrate_kbps = 448,
                                                          .acmod = ac3::Acmod::k3_2,
                                                          .lfe = true,
                                                          .spx = true,
                                                          .spx_atten = false}}};
    REQUIRE(encoder.channel_count() == 6);
    std::vector<std::vector<float>> pcm(
        6, std::vector<float>(static_cast<std::size_t>(ac3::kSamplesPerFrame)));
    const double tones[6] = {1000.0, 800.0, 1200.0, 600.0, 1400.0, 60.0};
    for (std::size_t ch = 0; ch < pcm.size(); ++ch) {
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                0.3 * std::sin(2.0 * std::numbers::pi * tones[ch] * static_cast<double>(i) /
                              48000.0));
        }
    }
    const std::vector<std::span<const float>> views{pcm[0], pcm[1], pcm[2], pcm[3], pcm[4], pcm[5]};
    const auto unit = encoder.encode_access_unit(views);
    REQUIRE(unit.has_value());
    auto broken = unit->bytes;
    // Bit offsets straight from test_eac3_decoder.cpp's own comment: bsi (54
    // bits) + audfrm (85 bits, spx on / attenuation off / nothing else
    // coupled) + block 0's dithflag(5)/dynrnge(1) prefix (6 bits) puts
    // spxinu at bit 145, followed by chinspx[0..4] (5), spxstrtf (2),
    // spxbegf (3), spxendf (3).
    constexpr std::size_t kSpxinuBit = 145;
    constexpr std::size_t kSpxbegfBit = kSpxinuBit + 1 + 5 + 2;
    constexpr std::size_t kSpxendfBit = kSpxbegfBit + 3;
    patch_bits(broken, kSpxbegfBit, 3, 7);  // begin_subbnd = 11
    patch_bits(broken, kSpxendfBit, 3, 0);  // end_subbnd = 5

    const auto path = scratch_dir() / "monitor_decode_fail.ec3";
    write_bytes(path, broken);
    const auto log = scratch_dir() / "monitor_decode_fail.log";
    const auto rc = run_cli("monitor \"" + path.string() + "\"", log);
    const auto out = read_log(log);
    INFO(out);
    CHECK(rc != 0);
    CHECK(out.find("error: decode failed:") != std::string::npos);
}
