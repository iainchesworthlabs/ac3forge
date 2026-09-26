#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "audio/alsa_null_device.hpp"

// The device-facing ac3cli commands against software ALSA devices
// (audio/alsa_null_device.hpp), run as the real binary with its own
// ALSA_CONFIG_PATH. Where cli/test_cli_live.cpp asserts only what holds with
// or without a device - "it ran, or it said why not" - every case here has a
// device, so it asserts that the command DID the work: record writes a
// decodable take of the length asked for, live runs its legs and exits
// cleanly, monitor and identify play to the end, and an output that fails
// mid-run is named as gone rather than hanging or passing silently.
//
// Each case writes its own configuration, choosing what `default` is for
// capture and for playback: plain null (silence in, anything out), a `file`
// replay of an IEC 61937 carrier (a bitstreaming source), a one-channel
// capture, or a playback device whose writes fail for good.
// No card is involved, so the render-endpoint list stays empty and 'play',
// which needs one, is not reachable here (see cli/test_cli_live.cpp for its
// refusal).
//
// ALSA configurations only (tests/CMakeLists.txt), and [concurrency] on every
// case for the same reason cli/test_cli_live.cpp gives: these start the
// capture and render threads.

namespace fs = std::filesystem;

namespace {

namespace alsa_null = ac3test::alsa_null;

constexpr int kExitInput = 2;
constexpr int kExitOutput = 3;
constexpr int kExitRuntime = 5;

fs::path scratch_dir() {
    return alsa_null::scratch_dir("cli_live_alsa");
}

// What `default` is: `capture` and `playback` are each a PCM name in quotes
// or an inline PCM definition.
fs::path write_default_config(const fs::path& path, std::string_view capture,
                              std::string_view playback) {
    std::string body{alsa_null::kNullDevices};
    body += "pcm.mono { type multi slaves.a { pcm \"null\" channels 1 } "
            "bindings.0 { slave a channel 0 } }\n";
    body += alsa_null::kFailingPlayback;
    body += "pcm.!default { type asym capture.pcm " + std::string{capture} +
            " playback.pcm " + std::string{playback} + " }\n";
    return alsa_null::write_config(path, body);
}

fs::path null_config() {
    static const fs::path path =
        write_default_config(scratch_dir() / "null.conf", "\"null\"", "\"null\"");
    return path;
}

// std::system's status as the child's exit code.
int exit_code(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int run_cli(const fs::path& config, const std::string& args, const fs::path& log) {
    const std::string command = alsa_null::env_prefix(config) + "\"" + std::string(AC3CLI_EXE) +
                                "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return exit_code(std::system(command.c_str()));
}

// run_cli under coreutils' `timeout`, for a command whose regression is a
// hang: the child is killed after `seconds` and the case sees exit code 124
// rather than waiting with it for ever.
constexpr int kTimedOut = 124;

int run_cli_bounded(const fs::path& config, const std::string& args, const fs::path& log,
                    int seconds) {
    const std::string command = alsa_null::env_prefix(config) + "timeout " +
                                std::to_string(seconds) + " \"" + std::string(AC3CLI_EXE) +
                                "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return exit_code(std::system(command.c_str()));
}

std::string read_text(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.is_open());
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        bytes[i] = static_cast<std::byte>(text[i]);
    }
    return bytes;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

// The sanitizer legs instrument ac3cli too; a report there would otherwise
// read as an ordinary failure line.
void check_clean(const std::string& output) {
    CHECK_FALSE(contains(output, "ThreadSanitizer"));
    CHECK_FALSE(contains(output, "AddressSanitizer"));
    CHECK_FALSE(contains(output, "runtime error:"));
}

// Splits an AC-3 file into its syncframes and decodes every one, returning
// how many there were; every frame must decode to `channels` channels.
std::size_t decoded_ac3_frames(const fs::path& path, std::size_t channels) {
    const auto bytes = read_bytes(path);
    const auto frames = ac3::split_frames(bytes);
    REQUIRE(frames.has_value());
    ac3::FrameDecoder decoder;
    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_frame(frame);
        REQUIRE(decoded.has_value());
        CHECK(decoded->channels.size() == channels);
    }
    return frames->size();
}

std::size_t eac3_access_units(const fs::path& path) {
    const auto bytes = read_bytes(path);
    const auto units = ac3::split_access_units(bytes);
    REQUIRE(units.has_value());
    return units->size();
}

// `count` distinct stereo AC-3 frames: a tone whose pitch moves frame by
// frame, so no two frames are alike and an out-of-order or repeated frame
// would show.
std::vector<std::vector<std::byte>> tone_frames(int count) {
    ac3::FrameEncoder encoder{{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0}};
    std::vector<std::vector<std::byte>> frames;
    std::vector<float> left(ac3::kSamplesPerFrame);
    std::vector<float> right(ac3::kSamplesPerFrame);
    for (int f = 0; f < count; ++f) {
        const double hz = 220.0 + 20.0 * f;
        for (int i = 0; i < ac3::kSamplesPerFrame; ++i) {
            const double t = static_cast<double>(f * ac3::kSamplesPerFrame + i) / 48000.0;
            left[static_cast<std::size_t>(i)] =
                static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * hz * t));
            right[static_cast<std::size_t>(i)] = -left[static_cast<std::size_t>(i)];
        }
        const std::vector<std::span<const float>> channels{left, right};
        auto frame = encoder.encode_frame(channels);
        REQUIRE(frame.has_value());
        frames.push_back(std::move(*frame));
    }
    return frames;
}

// An IEC 61937 pause burst (data type 3) the size of an AC-3 one: a burst
// the recorder must step over rather than record.
std::vector<std::byte> pause_burst() {
    std::vector<std::byte> burst(ac3::iec61937::kBurstBytes, std::byte{0});
    // Pa, Pb, Pc (data type 3), Pd (payload length in bits), little-endian.
    const std::uint16_t header[4] = {0xF872, 0x4E1F, 0x0003, 0x0020};
    for (std::size_t w = 0; w < 4; ++w) {
        burst[2 * w] = static_cast<std::byte>(header[w] & 0xFF);
        burst[2 * w + 1] = static_cast<std::byte>(header[w] >> 8);
    }
    return burst;
}

// The IEC 61937 carrier of `frames`, as a float capture would deliver it:
// each 16-bit word of each burst divided by 32768, interleaved stereo. With
// `pause_after`, a pause burst follows that many frames.
void write_float_carrier(const fs::path& path, const std::vector<std::vector<std::byte>>& frames,
                         std::optional<std::size_t> pause_after = std::nullopt) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    REQUIRE(out.is_open());
    const auto emit = [&out](std::span<const std::byte> burst) {
        for (std::size_t i = 0; i + 1 < burst.size(); i += 2) {
            const auto word = static_cast<std::int16_t>(
                std::to_integer<std::uint16_t>(burst[i]) |
                (std::to_integer<std::uint16_t>(burst[i + 1]) << 8));
            const float sample = static_cast<float>(word) / 32768.0F;
            out.write(reinterpret_cast<const char*>(&sample), sizeof(sample));
        }
    };
    for (std::size_t f = 0; f < frames.size(); ++f) {
        const auto burst = ac3::iec61937::wrap_frame(frames[f]);
        REQUIRE(burst.has_value());
        emit(*burst);
        if (pause_after && *pause_after == f + 1) {
            emit(pause_burst());
        }
    }
    REQUIRE(out.good());
}

// A loopback UDP port nothing holds right now: bound to port 0, read back and
// released. positions=osc wants a real port (0 is refused), and a fixed one
// could collide with a parallel ctest run.
int free_udp_port() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    ::close(fd);
    return ntohs(address.sin_port);
}

// A configuration whose default capture replays `carrier`.
fs::path carrier_config(const fs::path& path, const fs::path& carrier) {
    const std::string capture = "{ type file slave.pcm \"null\" file \"/dev/null\" infile \"" +
                                carrier.string() + "\" format raw }";
    return write_default_config(path, capture, "\"null\"");
}

}  // namespace

// ---------------------------------------------------------------------------
// devices / record
// ---------------------------------------------------------------------------

TEST_CASE("devices lists the configured default capture endpoint with its rate and width",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto log = scratch_dir() / "devices.log";
    REQUIRE(run_cli(null_config(), "devices", log) == 0);
    const auto out = read_text(log);
    CHECK(contains(out, "idx"));
    CHECK(contains(out, "input"));
    CHECK(contains(out, "48000"));
    CHECK(contains(out, "[default]"));
}

TEST_CASE("record captures the default endpoint into a decodable AC-3 take of the asked length",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto take = dir / "record_default.ac3";
    const auto log = dir / "record_default.log";
    REQUIRE(run_cli(null_config(), "record \"" + take.string() + "\" 1", log) == 0);
    const auto out = read_text(log);
    check_clean(out);
    CHECK(contains(out, "48000 Hz, 2 ch"));
    // One second at 48 kHz is 31.25 frames, rounded up to whole frames.
    CHECK(decoded_ac3_frames(take, 2) == 32);
    CHECK(contains(out, "wrote 32 frames"));
}

TEST_CASE("record writes a 5.1 E-AC-3 take into Matroska when asked for one",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto take = dir / "record_51.mkv";
    const auto log = dir / "record_51.log";
    REQUIRE(run_cli(null_config(),
                    "record \"" + take.string() + "\" 1 384 0 layout=51 codec=eac3 container=mkv",
                    log) == 0);
    check_clean(read_text(log));
    const auto bytes = read_bytes(take);
    REQUIRE(bytes.size() > 4);
    CHECK(bytes[0] == std::byte{0x1A});  // EBML magic
    CHECK(bytes[1] == std::byte{0x45});

    const auto demuxed = dir / "record_51.ec3";
    REQUIRE(run_cli(null_config(), "demux \"" + take.string() + "\" \"" + demuxed.string() + "\"",
                    dir / "record_51_demux.log") == 0);
    CHECK(eac3_access_units(demuxed) == 32);
    const auto probe_log = dir / "record_51_probe.log";
    REQUIRE(run_cli(null_config(), "probe \"" + demuxed.string() + "\"", probe_log) == 0);
    const auto probe = read_text(probe_log);
    CHECK(contains(probe, "E-AC-3"));
    CHECK(contains(probe, "3/2 + LFE"));
}

TEST_CASE("record streams a take into every other container it offers",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    SECTION("MPEG-TS") {
        const auto take = dir / "record.ts";
        REQUIRE(run_cli(null_config(), "record \"" + take.string() + "\" 1 192 0 container=ts",
                        dir / "record_ts.log") == 0);
        const auto bytes = read_bytes(take);
        REQUIRE(bytes.size() >= 188);
        CHECK(bytes.size() % 188 == 0);
        CHECK(bytes[0] == std::byte{0x47});
    }
    SECTION("the IEC 61937 WAV carrier") {
        const auto take = dir / "record_spdif.wav";
        REQUIRE(run_cli(null_config(),
                        "record \"" + take.string() + "\" 1 192 0 container=spdif",
                        dir / "record_spdif.log") == 0);
        const auto recovered = dir / "record_spdif.ac3";
        REQUIRE(run_cli(null_config(),
                        "unspdif \"" + take.string() + "\" \"" + recovered.string() + "\"",
                        dir / "record_unspdif.log") == 0);
        CHECK(decoded_ac3_frames(recovered, 2) == 32);
    }
    SECTION("a fragmented-MP4 folder") {
        const auto folder = dir / "record_fmp4";
        fs::remove_all(folder);
        REQUIRE(run_cli(null_config(),
                        "record \"" + folder.string() + "\" 1 192 0 container=fmp4",
                        dir / "record_fmp4.log") == 0);
        CHECK(fs::is_directory(folder));
        CHECK(fs::exists(folder / "audio.m3u8"));
        CHECK(fs::exists(folder / "manifest.mpd"));
    }
}

TEST_CASE("record places a one-channel capture onto a stereo take",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto config = write_default_config(dir / "mono_capture.conf", "\"mono\"", "\"null\"");
    const auto take = dir / "record_mono.ac3";
    const auto log = dir / "record_mono.log";
    REQUIRE(run_cli(config, "record \"" + take.string() + "\" 1", log) == 0);
    CHECK(contains(read_text(log), "48000 Hz, 1 ch"));
    CHECK(decoded_ac3_frames(take, 2) == 32);
}

TEST_CASE("record keeps a bitstreaming capture's elementary stream, byte for byte",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto frames = tone_frames(40);
    const auto carrier = dir / "carrier_ac3.f32";
    write_float_carrier(carrier, frames);
    const auto config = carrier_config(dir / "carrier.conf", carrier);

    SECTION("into a bare elementary stream") {
        const auto take = dir / "record_bitstream.ac3";
        const auto log = dir / "record_bitstream.log";
        REQUIRE(run_cli(config, "record \"" + take.string() + "\" 1", log) == 0);
        const auto out = read_text(log);
        check_clean(out);
        CHECK(contains(out, "bitstreaming Dolby Digital (data type 0x01)"));
        CHECK(contains(out, "no re-encode happened"));

        // What the source sent, from its first burst: a whole second's worth
        // of the 40 frames, in order and unaltered.
        const auto recorded = read_bytes(take);
        const auto split = ac3::split_frames(recorded);
        REQUIRE(split.has_value());
        REQUIRE(split->size() >= 30);
        REQUIRE(split->size() <= frames.size());
        for (std::size_t i = 0; i < split->size(); ++i) {
            const auto& expected = frames[i];
            CHECK(std::vector<std::byte>((*split)[i].begin(), (*split)[i].end()) == expected);
        }
    }
    SECTION("a container asked for is declined, by name, for the bare stream") {
        const auto take = dir / "record_bitstream_mkv.ac3";
        const auto log = dir / "record_bitstream_mkv.log";
        REQUIRE(run_cli(config, "record \"" + take.string() + "\" 1 192 0 container=mkv", log) ==
                0);
        const auto out = read_text(log);
        CHECK(contains(out, "container=mkv does not apply to a passthrough capture"));
        const auto recorded = read_bytes(take);
        REQUIRE(recorded.size() >= 2);
        CHECK(recorded[0] == std::byte{0x0B});  // an AC-3 syncword, not EBML
        CHECK(recorded[1] == std::byte{0x77});
    }
}

TEST_CASE("record declines every container for a bitstreaming capture, and steps over other bursts",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto frames = tone_frames(40);
    SECTION("ts, spdif and fmp4 are each named and declined") {
        const auto carrier = dir / "carrier_containers.f32";
        write_float_carrier(carrier, frames);
        const auto config = carrier_config(dir / "carrier_containers.conf", carrier);
        for (const std::string container : {"ts", "spdif", "fmp4"}) {
            CAPTURE(container);
            const auto take = dir / ("record_bitstream_" + container + ".ac3");
            const auto log = dir / ("record_bitstream_" + container + ".log");
            REQUIRE(run_cli(config,
                            "record \"" + take.string() + "\" 1 192 0 container=" + container,
                            log) == 0);
            CHECK(contains(read_text(log),
                           "container=" + container + " does not apply to a passthrough capture"));
            const auto recorded = read_bytes(take);
            const auto split = ac3::split_frames(recorded);
            REQUIRE(split.has_value());
            CHECK(split->size() >= 30);
        }
    }
    SECTION("a pause burst mid-carrier is counted and left out of the take") {
        const auto carrier = dir / "carrier_pause.f32";
        write_float_carrier(carrier, frames, /*pause_after=*/12);
        const auto config = carrier_config(dir / "carrier_pause.conf", carrier);
        const auto take = dir / "record_bitstream_pause.ac3";
        const auto log = dir / "record_bitstream_pause.log";
        REQUIRE(run_cli(config, "record \"" + take.string() + "\" 1", log) == 0);
        CHECK(contains(read_text(log), "1 burst(s) of another data type skipped"));
        const auto recorded = read_bytes(take);
        const auto split = ac3::split_frames(recorded);
        REQUIRE(split.has_value());
        REQUIRE(split->size() >= 30);
        for (std::size_t i = 0; i < split->size(); ++i) {
            CHECK(std::vector<std::byte>((*split)[i].begin(), (*split)[i].end()) == frames[i]);
        }
    }
}

TEST_CASE("record refuses an out-of-range device and an unwritable take by name",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    {
        const auto log = dir / "record_range.log";
        CHECK(run_cli(null_config(), "record \"" + (dir / "never.ac3").string() + "\" 1 192 7",
                      log) == 1);
        CHECK(contains(read_text(log), "device index 7 out of range"));
    }
    {
        // Opening the take is deferred until the bitstream check has decided,
        // so this is refused mid-capture, not before it.
        const auto log = dir / "record_unwritable.log";
        const auto take = dir / "no" / "such" / "dir" / "take.ac3";
        CHECK(run_cli(null_config(), "record \"" + take.string() + "\" 1", log) == kExitOutput);
        CHECK(contains(read_text(log), "error:"));
        CHECK_FALSE(fs::exists(take));
    }
}

// ---------------------------------------------------------------------------
// live
// ---------------------------------------------------------------------------

TEST_CASE("live encodes the capture while monitoring it, and names the passthrough it lacks",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto take = dir / "live_stereo.ac3";
    const auto log = dir / "live_stereo.log";
    REQUIRE(run_cli(null_config(), "live \"" + take.string() + "\" 0 1 192 -1 -1", log) == 0);
    const auto out = read_text(log);
    check_clean(out);
    CHECK(contains(out, "monitoring on \"default endpoint\""));
    // No card, so no digital output for the passthrough leg to take: said,
    // and the take goes on without it.
    CHECK(contains(out, "warning: passthrough unavailable"));
    CHECK(contains(out, "wrote 32 AC-3 frames"));
    CHECK(decoded_ac3_frames(take, 2) == 32);
}

TEST_CASE("live monitors a 5.1 E-AC-3 channel session through the E-AC-3 decoder",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto take = dir / "live_51.ec3";
    const auto log = dir / "live_51.log";
    REQUIRE(run_cli(null_config(),
                    "live \"" + take.string() + "\" 0 1 384 -1 -2 channels layout=51 codec=eac3",
                    log) == 0);
    const auto out = read_text(log);
    check_clean(out);
    CHECK(contains(out, "monitoring on"));
    CHECK(contains(out, "E-AC-3 access units"));
    CHECK(eac3_access_units(take) == 32);
}

TEST_CASE("live mode=atmos pans the capture as objects and monitors the bed",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto take = dir / "live_atmos.ec3";
    const auto log = dir / "live_atmos.log";
    REQUIRE(run_cli(null_config(),
                    "live \"" + take.string() + "\" 0 1 384 -1 -2 atmos objects=3", log) == 0);
    const auto out = read_text(log);
    check_clean(out);
    CHECK(contains(out, "3 object slots, 2 bound to captured channels, 1 carried silent"));
    CHECK(eac3_access_units(take) == 32);
}

TEST_CASE("live mode=atmos positions=osc runs with its OSC listener bound",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto take = dir / "live_osc.ec3";
    const auto log = dir / "live_osc.log";
    REQUIRE(run_cli(null_config(),
                    "live \"" + take.string() + "\" 0 1 384 -2 -2 atmos positions=osc:local:" +
                        std::to_string(free_udp_port()),
                    log) == 0);
    const auto out = read_text(log);
    check_clean(out);
    CHECK(contains(out, "positions: OSC on 127.0.0.1:"));
    CHECK(contains(out, "positions: 0 datagrams"));
    CHECK(eac3_access_units(take) == 32);
}

TEST_CASE("live resamples a second capture device into lockstep with the first",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto take = dir / "live_capture2.ec3";
    const auto log = dir / "live_capture2.log";
    REQUIRE(run_cli(null_config(),
                    "live \"" + take.string() + "\" 0 1 384 -2 -2 atmos capture2=0", log) == 0);
    const auto out = read_text(log);
    check_clean(out);
    CHECK(contains(out, "capture2: "));
    CHECK(contains(out, "nominal ratio 1.000000"));
    CHECK(contains(out, "capture2 drift:"));
    // Two stereo devices, one object per captured channel.
    CHECK(contains(out, "4 object slots, 4 bound"));
}

TEST_CASE("live refuses map= in a channel session, after the capture has opened",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto log = dir / "live_map.log";
    CHECK(run_cli(null_config(),
                  "live \"" + (dir / "live_map.ac3").string() + "\" 0 1 192 -2 -2 channels map=0.0",
                  log) == 1);
    CHECK(contains(read_text(log), "map= binds capture channels to OBJECT slots"));
}

TEST_CASE("live stops rather than encoding a bitstreaming capture as audio",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto carrier = dir / "live_carrier.f32";
    write_float_carrier(carrier, tone_frames(40));
    const auto config = carrier_config(dir / "live_carrier.conf", carrier);
    const auto log = dir / "live_carrier.log";
    CHECK(run_cli(config, "live \"" + (dir / "live_carrier.ac3").string() + "\" 0 1", log) ==
          kExitInput);
    const auto out = read_text(log);
    check_clean(out);
    CHECK(contains(out, "is bitstreaming Dolby Digital over IEC 61937"));
    CHECK(contains(out, "'ac3cli record <out.ec3> <seconds> 0 0'"));
}

TEST_CASE("live carries on when its monitor output goes away, and says when it went",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto config = write_default_config(dir / "live_full.conf", "\"null\"", "\"full\"");
    const auto take = dir / "live_gone.ac3";
    const auto log = dir / "live_gone.log";
    CHECK(run_cli(config, "live \"" + take.string() + "\" 0 1 192 -1 -2", log) == kExitRuntime);
    const auto out = read_text(log);
    check_clean(out);
    CHECK(contains(out, "monitoring stopped, the take carries on"));
    CHECK(contains(out, "error: monitor output \"default endpoint\" went away"));
    // The take itself is whole.
    CHECK(decoded_ac3_frames(take, 2) == 32);
}

TEST_CASE("live mode=atmos binds objects from map= and holds objects= as the budget",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto take = dir / "live_map.ec3";
    SECTION("map= binds the named channels, objects= adds unbound slots") {
        const auto log = dir / "live_map_ok.log";
        REQUIRE(run_cli(null_config(),
                        "live \"" + take.string() +
                            "\" 0 1 384 -2 -2 atmos map=0.0:obj,0.1:obj objects=3",
                        log) == 0);
        CHECK(contains(read_text(log), "3 object slots, 2 bound to captured channels"));
    }
    SECTION("a budget smaller than map= needs is refused, not truncated") {
        const auto log = dir / "live_map_budget.log";
        CHECK(run_cli(null_config(),
                      "live \"" + take.string() + "\" 0 1 384 -2 -2 atmos map=0.0:obj,0.1:obj objects=1",
                      log) == 1);
        CHECK(contains(read_text(log), "map= assigns 2 objects but objects=1 allows 1"));
    }
    SECTION("a map= with no object destination is refused") {
        const auto log = dir / "live_map_none.log";
        CHECK(run_cli(null_config(),
                      "live \"" + take.string() + "\" 0 1 384 -2 -2 atmos map=0.0:none,0.1:none", log) == 1);
        CHECK(contains(read_text(log), "map= names no obj/objm destination"));
    }
    SECTION("a map= that does not parse is refused") {
        const auto log = dir / "live_map_bad.log";
        CHECK(run_cli(null_config(),
                      "live \"" + take.string() + "\" 0 1 384 -2 -2 atmos map=9.9:obj", log) == 1);
        CHECK(contains(read_text(log), "bad map= spec"));
    }
}

TEST_CASE("live refuses what a session cannot be, once the capture device is known",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto take = (dir / "live_refused.ec3").string();
    {
        const auto log = dir / "live_atmos_layout.log";
        CHECK(run_cli(null_config(), "live \"" + take + "\" 0 1 384 -2 -2 atmos layout=51", log) ==
              1);
        CHECK(contains(read_text(log), "layout=/codec= describe a channel session"));
    }
    {
        const auto log = dir / "live_capture2_range.log";
        CHECK(run_cli(null_config(), "live \"" + take + "\" 0 1 384 -2 -2 atmos capture2=5", log) ==
              1);
        CHECK(contains(read_text(log), "capture2 device index 5 out of range"));
    }
    {
        const auto log = dir / "live_capture_range.log";
        CHECK(run_cli(null_config(), "live \"" + take + "\" 3 1", log) == 1);
        CHECK(contains(read_text(log), "capture device index 3 out of range"));
    }
    {
        // An OSC port something else already holds: refused outright rather
        // than silently falling back to the orbit.
        const int held = ::socket(AF_INET, SOCK_DGRAM, 0);
        REQUIRE(held >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(held, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        socklen_t length = sizeof(address);
        REQUIRE(::getsockname(held, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        const auto port = std::to_string(ntohs(address.sin_port));
        const auto log = dir / "live_osc_busy.log";
        const int rc = run_cli(null_config(),
                               "live \"" + take + "\" 0 1 384 -2 -2 atmos positions=osc:local:" + port,
                               log);
        ::close(held);
        CHECK(rc == 4);
        CHECK(contains(read_text(log), "error: positions=osc:"));
    }
}

TEST_CASE("live stops, and keeps what it has, when the encoder cannot express the session",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto take = dir / "live_unencodable.ec3";
    const auto log = dir / "live_unencodable.log";
    // Fifteen objects do not fit in 64 kbps: the first frame fails, so the
    // take is empty, and that is what the command ends on.
    CHECK(run_cli(null_config(), "live \"" + take.string() + "\" 0 1 64 -2 -2 atmos objects=15",
                  log) == kExitOutput);
    const auto out = read_text(log);
    CHECK(contains(out, "error: cannot encode 15 objects at 64 kbps"));
    CHECK(contains(out, "Nothing was encoded."));
    CHECK_FALSE(fs::exists(take));
}

// ---------------------------------------------------------------------------
// monitor / identify
// ---------------------------------------------------------------------------

TEST_CASE("monitor decodes and plays a stream to the end on the default output",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    SECTION("stereo AC-3") {
        const auto stream = dir / "monitor_stereo.ac3";
        REQUIRE(run_cli(null_config(), "silence \"" + stream.string() + "\" 1",
                        dir / "monitor_stereo_make.log") == 0);
        const auto log = dir / "monitor_stereo.log";
        REQUIRE(run_cli(null_config(), "monitor \"" + stream.string() + "\"", log) == 0);
        const auto out = read_text(log);
        check_clean(out);
        CHECK(contains(out, "(2 channels, 48000 Hz) on \"default endpoint\""));
        CHECK(contains(out, "played 32 frames"));
    }
    SECTION("5.1 E-AC-3, as access units") {
        const auto stream = dir / "monitor_51.ec3";
        REQUIRE(run_cli(null_config(), "eac3-silence \"" + stream.string() + "\" 1 384 51",
                        dir / "monitor_51_make.log") == 0);
        const auto log = dir / "monitor_51.log";
        REQUIRE(run_cli(null_config(), "monitor \"" + stream.string() + "\"", log) == 0);
        const auto out = read_text(log);
        check_clean(out);
        CHECK(contains(out, "(6 channels, 48000 Hz)"));
        CHECK(contains(out, "played 32 access units"));
    }
    SECTION("5.1 E-AC-3 folded to stereo when asked") {
        const auto stream = dir / "monitor_51_fold.ec3";
        REQUIRE(run_cli(null_config(), "eac3-silence \"" + stream.string() + "\" 1 384 51",
                        dir / "monitor_51_fold_make.log") == 0);
        const auto log = dir / "monitor_51_eac3_fold.log";
        REQUIRE(run_cli(null_config(), "monitor \"" + stream.string() + "\" -1 downmix=loro",
                        log) == 0);
        const auto out = read_text(log);
        CHECK(contains(out, "(2 channels, 48000 Hz)"));
        CHECK(contains(out, "played 32 access units"));
    }
    SECTION("dual-mono E-AC-3, in coded order") {
        const auto stream = dir / "monitor_dual.ec3";
        REQUIRE(run_cli(null_config(), "eac3-sine \"" + stream.string() + "\" 1 192 440 20 1+1",
                        dir / "monitor_dual_make.log") == 0);
        const auto log = dir / "monitor_dual.log";
        REQUIRE(run_cli(null_config(), "monitor \"" + stream.string() + "\"", log) == 0);
        const auto out = read_text(log);
        CHECK(contains(out, "(2 channels, 48000 Hz)"));
        CHECK(contains(out, "played 32 access units"));
    }
    SECTION("5.1 AC-3 folded to stereo when asked") {
        const auto stream = dir / "monitor_51.ac3";
        REQUIRE(run_cli(null_config(), "sine \"" + stream.string() + "\" 1 448 440 20 51",
                        dir / "monitor_51_ac3_make.log") == 0);
        const auto log = dir / "monitor_51_fold.log";
        REQUIRE(run_cli(null_config(), "monitor \"" + stream.string() + "\" -1 downmix=loro",
                        log) == 0);
        const auto out = read_text(log);
        CHECK(contains(out, "(2 channels, 48000 Hz)"));
        CHECK(contains(out, "played 32 frames"));
    }
}

TEST_CASE("monitor plays to the end on an output that under-runs every buffer",
          "[cli][audio-io][alsa-null][concurrency]") {
    // `mono` (multi over null, one channel) drops into XRUN each time its
    // start threshold starts it, so a write in every five meets -EPIPE. The
    // period that write carried used to be dropped uncounted, frames_rendered
    // never caught frames_submitted, and monitor's final drain waited for
    // ever; bounded here, so that regression fails instead of hanging.
    const auto dir = scratch_dir();
    const auto config = write_default_config(dir / "monitor_xrun.conf", "\"null\"", "\"mono\"");
    const auto stream = dir / "monitor_xrun.ac3";
    REQUIRE(run_cli(null_config(), "sine \"" + stream.string() + "\" 1 96 440 50 mono",
                    dir / "monitor_xrun_make.log") == 0);
    const auto log = dir / "monitor_xrun.log";
    const int code = run_cli_bounded(config, "monitor \"" + stream.string() + "\"", log, 60);
    CHECK(code != kTimedOut);
    CHECK(code == 0);
    const auto out = read_text(log);
    check_clean(out);
    CHECK(contains(out, "(1 channels, 48000 Hz)"));
    CHECK(contains(out, "played 32 frames"));
}

TEST_CASE("monitor names an output that goes away mid-stream instead of waiting on it",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto config = write_default_config(dir / "monitor_full.conf", "\"null\"", "\"full\"");
    SECTION("AC-3") {
        const auto stream = dir / "monitor_gone.ac3";
        REQUIRE(run_cli(null_config(), "silence \"" + stream.string() + "\" 2",
                        dir / "monitor_gone_make.log") == 0);
        const auto log = dir / "monitor_gone.log";
        CHECK(run_cli(config, "monitor \"" + stream.string() + "\"", log) == kExitRuntime);
        const auto out = read_text(log);
        check_clean(out);
        CHECK(contains(out, "error: \"default endpoint\" went away"));
    }
    SECTION("E-AC-3") {
        const auto stream = dir / "monitor_gone.ec3";
        REQUIRE(run_cli(null_config(), "eac3-silence \"" + stream.string() + "\" 2 384 51",
                        dir / "monitor_gone_ec3_make.log") == 0);
        const auto log = dir / "monitor_gone_ec3.log";
        CHECK(run_cli(config, "monitor \"" + stream.string() + "\"", log) == kExitRuntime);
        CHECK(contains(read_text(log), "error: \"default endpoint\" went away"));
    }
}

TEST_CASE("identify walks every speaker of the layout on the default output",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    SECTION("the output's own (assumed stereo) layout") {
        const auto log = dir / "identify_default.log";
        REQUIRE(run_cli(null_config(), "identify -1 - 1", log) == 0);
        const auto out = read_text(log);
        check_clean(out);
        CHECK(contains(out, "(the backend does not say; assumed)"));
        CHECK(contains(out, "slot  0 L    -> output 0"));
        CHECK(contains(out, "slot  1 R    -> output 1"));
    }
    SECTION("an explicit 5.1 layout, a patch that swaps the first two slots and a quieter tone") {
        const auto log = dir / "identify_51.log";
        REQUIRE(run_cli(null_config(), "identify -1 5.1 1 1,0,2,3,4,5 -30", log) == 0);
        const auto out = read_text(log);
        check_clean(out);
        CHECK(contains(out, "patch:    1,0,2,3,4,5"));
        CHECK(contains(out, "slot  0 L    -> output 1"));
        CHECK(contains(out, "slot  1 C    -> output 0"));
        CHECK(contains(out, "LFE"));
    }
    SECTION("a slot left unpatched is skipped, and said to be") {
        const auto log = dir / "identify_skip.log";
        REQUIRE(run_cli(null_config(), "identify -1 - 1 0,-", log) == 0);
        CHECK(contains(read_text(log), "not patched - skipped"));
    }
}

TEST_CASE("identify refuses a patch or a level it cannot use, once the output is open",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    {
        const auto log = dir / "identify_bad_patch.log";
        CHECK(run_cli(null_config(), "identify -1 - 1 0,1,2,3", log) == kExitInput);
        CHECK(contains(read_text(log), "is not a patch for 2 outputs"));
    }
    {
        const auto log = dir / "identify_bad_level.log";
        CHECK(run_cli(null_config(), "identify -1 - 1 - 20", log) == kExitInput);
        CHECK(contains(read_text(log), "dB is outside"));
    }
}

TEST_CASE("identify stops at the slot where its output went away",
          "[cli][audio-io][alsa-null][concurrency]") {
    const auto dir = scratch_dir();
    const auto config = write_default_config(dir / "identify_full.conf", "\"null\"", "\"full\"");
    const auto log = dir / "identify_gone.log";
    CHECK(run_cli(config, "identify -1 - 1", log) == kExitRuntime);
    const auto out = read_text(log);
    check_clean(out);
    CHECK(contains(out, "went away"));
    CHECK(contains(out, "stopped at slot 0"));
}

// ---------------------------------------------------------------------------
// AC-4 (planning/ac4.md, phase I1): record and live take codec=ac4, and
// monitor decodes AC-4. A take's frames are checked by decoding them back,
// through each container they were written into.
// ---------------------------------------------------------------------------

namespace {

// Decodes an AC-4 stream, or a container holding one, and returns decode's
// status line.
std::string decoded_ac4(const fs::path& path) {
    const auto wav = fs::path{path}.replace_extension(".decoded.wav");
    const auto log = fs::path{path}.replace_extension(".decode.log");
    REQUIRE(run_cli(null_config(), "decode \"" + path.string() + "\" \"" + wav.string() + "\"",
                    log) == 0);
    const auto out = read_text(log);
    CHECK(contains(out, "AC-4 frames"));
    return out;
}

}  // namespace

TEST_CASE("record encodes an AC-4 take with codec=ac4 into each container it offers",
          "[cli][audio-io][alsa-null][concurrency][ac4]") {
    const auto dir = scratch_dir();
    SECTION("raw sync frames") {
        const auto take = dir / "record.ac4";
        const auto log = dir / "record_ac4.log";
        REQUIRE(run_cli(null_config(), "record \"" + take.string() + "\" 1 64 0 codec=ac4", log) ==
                0);
        const auto out = read_text(log);
        check_clean(out);
        CHECK(contains(out, "AC-4 frames (64 kbps, 2/0 stereo)"));
        decoded_ac4(take);
    }
    SECTION("MPEG-TS, demuxed back") {
        const auto take = dir / "record_ac4.ts";
        REQUIRE(run_cli(null_config(),
                        "record \"" + take.string() + "\" 1 64 0 codec=ac4 container=ts",
                        dir / "record_ac4_ts.log") == 0);
        const auto demuxed = dir / "record_ac4_ts.ac4";
        REQUIRE(run_cli(null_config(),
                        "demux \"" + take.string() + "\" \"" + demuxed.string() + "\"",
                        dir / "record_ac4_demux.log") == 0);
        decoded_ac4(demuxed);
    }
    SECTION("IEC 61937-14 bursts, unwrapped back") {
        const auto take = dir / "record_ac4_spdif.wav";
        REQUIRE(run_cli(null_config(),
                        "record \"" + take.string() + "\" 1 64 0 codec=ac4 container=spdif",
                        dir / "record_ac4_spdif.log") == 0);
        const auto recovered = dir / "record_ac4_spdif.ac4";
        REQUIRE(run_cli(null_config(),
                        "unspdif \"" + take.string() + "\" \"" + recovered.string() + "\"",
                        dir / "record_ac4_unspdif.log") == 0);
        decoded_ac4(recovered);
    }
    SECTION("a CMAF folder of fragments") {
        const auto folder = dir / "record_ac4_fmp4";
        fs::remove_all(folder);
        REQUIRE(run_cli(null_config(),
                        "record \"" + folder.string() + "\" 1 64 0 codec=ac4 container=fmp4",
                        dir / "record_ac4_fmp4.log") == 0);
        const auto init = read_text(folder / "init.mp4");
        CHECK(contains(init, "ca4m"));
        CHECK(contains(init, "dac4"));
        CHECK(fs::exists(folder / "segment1.m4s"));
        CHECK(contains(read_text(folder / "master.m3u8"), "CODECS=\"ac-4."));
    }
    SECTION("Matroska is refused, for want of a codec ID") {
        const auto take = dir / "record_ac4.mkv";
        const auto log = dir / "record_ac4_mkv.log";
        CHECK(run_cli(null_config(),
                      "record \"" + take.string() + "\" 1 64 0 codec=ac4 container=mkv",
                      log) == kExitOutput);
        CHECK(contains(read_text(log), "Matroska registers no codec ID for AC-4"));
    }
    SECTION("a layout AC-4's encoder does not take is refused") {
        const auto log = dir / "record_ac4_71.log";
        CHECK(run_cli(null_config(),
                      "record \"" + (dir / "record_ac4_71.ac4").string() +
                          "\" 1 256 0 codec=ac4 layout=71",
                      log) == 1);
        CHECK(contains(read_text(log), "the AC-4 encoder takes mono, stereo, 5.0 and 5.1"));
    }
}

TEST_CASE("live encodes an AC-4 session and monitors it through the AC-4 decoder",
          "[cli][audio-io][alsa-null][concurrency][ac4]") {
    const auto dir = scratch_dir();
    const auto take = dir / "live.ac4";
    const auto log = dir / "live_ac4.log";
    REQUIRE(run_cli(null_config(), "live \"" + take.string() + "\" 0 1 64 -1 -2 codec=ac4", log) ==
            0);
    const auto out = read_text(log);
    check_clean(out);
    CHECK(contains(out, "monitoring on \"default endpoint\""));
    CHECK(contains(out, "AC-4 frames (64 kbps, 2/0 stereo)"));
    decoded_ac4(take);
}

TEST_CASE("monitor decodes and plays an AC-4 stream to the end on the default output",
          "[cli][audio-io][alsa-null][concurrency][ac4]") {
    const auto dir = scratch_dir();
    const auto stream = dir / "monitor.ac4";
    REQUIRE(run_cli(null_config(), "record \"" + stream.string() + "\" 1 64 0 codec=ac4",
                    dir / "monitor_ac4_make.log") == 0);
    const auto log = dir / "monitor_ac4.log";
    REQUIRE(run_cli(null_config(), "monitor \"" + stream.string() + "\"", log) == 0);
    const auto out = read_text(log);
    check_clean(out);
    CHECK(contains(out, "(AC-4, presentation 0, 2 channels, 48000 Hz)"));
    CHECK(contains(out, "played "));
}
