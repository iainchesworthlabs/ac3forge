#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"

#include "iclforge/ac3/encoder/assignment.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/objects/scene.hpp"
#include "iclforge/ac4/ac4.hpp"
#include "ac4_encode_core.hpp"
#include "ac4_objects_core.hpp"
#include "iclforge/ac4dec/decoder.hpp"

// forge atmos-encode with codec=ac4 (planning/ac4.md, I5b): the source's channels
// as AC-4 objects, run against the real binary and held to the steps the page takes
// (apps/common/ac4_objects_core.hpp, ac4_encode_core.hpp): the file the command
// writes is the file those steps write for the same sources, assignment and scene.
// The Qt Quick suite (apps/gui/tests/qml/tst_e2e_ac4_objects.qml) runs the line the
// page echoes through this command and compares the bytes; this file holds the
// command's half, and what it refuses.
//
// run_cli and the helpers below are trimmed copies of test_cli.cpp's own,
// duplicated per this project's per-file test-helper convention.

namespace fs = std::filesystem;

using iclforge::apps::Ac4ObjectSlot;
using iclforge::ac3::plan::Assignment;
using iclforge::ac3::plan::Destination;
using iclforge::ac3::plan::DestinationKind;
using iclforge::ac3::plan::SourceShape;
using Location = iclforge::ac3::eac3::chanmap::Location;

namespace {

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_atmos_encode_ac4_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::string quoted(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

std::vector<std::byte> file_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.is_open());
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(raw.size());
    std::ranges::transform(raw, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

std::vector<std::byte> joined(const iclforge::apps::Ac4Packaged& packaged) {
    std::vector<std::byte> out;
    for (const auto& chunk : packaged.chunks) {
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    return out;
}

constexpr std::uint32_t kRate = 48000;

// `channels` channels of `frames` samples, one tone each at its own frequency.
fs::path tone_wav(const fs::path& path, std::size_t channels, std::size_t frames = 3 * 2048) {
    std::vector<std::vector<float>> data(channels, std::vector<float>(frames));
    for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t n = 0; n < frames; ++n) {
            data[c][n] = static_cast<float>(
                0.2 * std::sin(2.0 * std::numbers::pi * (301.0 + 197.0 * static_cast<double>(c)) *
                               static_cast<double>(n) / kRate));
        }
    }
    REQUIRE(iclforge::ac3::io::write_wav_f32(path.string(), data, kRate).has_value());
    return path;
}

iclforge::ac3::io::WavData read(const fs::path& path) {
    auto wav = iclforge::ac3::io::read_wav(path.string());
    REQUIRE(wav.has_value());
    return std::move(*wav);
}

Destination obj(double trim_db = 0.0) {
    return {.kind = DestinationKind::kObject, .trim_db = trim_db};
}
Destination objm() {
    return {.kind = DestinationKind::kObjectMono};
}
Destination at(Location location) {
    return {.kind = DestinationKind::kLocation, .location = location};
}

// An authored scene: `count` objects, each held for the first 40 ms at one place and moving
// to another by 90 ms, at unity.
iclforge::oba::ObjectScene moving_scene(std::size_t count) {
    std::vector<iclforge::oba::SceneObject> objects;
    for (std::size_t i = 0; i < count; ++i) {
        const double x = 0.15 + 0.7 * static_cast<double>(i) / static_cast<double>(count);
        iclforge::oba::SceneObject o;
        o.name = "object " + std::to_string(i);
        o.automation = {
            {.time_s = 0.0, .position = {.x = x, .y = 0.2, .z = 0.0}, .gain = 0.5},
            {.time_s = 0.09, .position = {.x = 1.0 - x, .y = 0.8, .z = 0.5}, .gain = 0.5}};
        objects.push_back(std::move(o));
    }
    auto scene = iclforge::oba::ObjectScene::create(std::move(objects));
    REQUIRE(scene.has_value());
    return std::move(*scene);
}

fs::path write_scene(const fs::path& path, const iclforge::oba::ObjectScene& scene) {
    std::ofstream{path, std::ios::binary} << iclforge::oba::to_json(scene);
    return path;
}

// The bytes the shared steps write for one WAV file's channels as objects, given the scene.
std::vector<std::byte> shared_bytes(const std::vector<const iclforge::ac3::io::WavData*>& sources,
                                    const std::vector<std::size_t>& offsets,
                                    const Assignment& assignment,
                                    const iclforge::oba::ObjectScene& scene, int kbps,
                                    iclforge::ac4::ObjectCoding coding, bool mp4, bool crc) {
    std::vector<SourceShape> shapes;
    std::vector<iclforge::apps::Ac4SourceView> views;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        shapes.push_back({.channels = sources[i]->channels.size(), .label = "s"});
        views.push_back({.channels = sources[i]->channels, .offset_samples = offsets[i]});
    }
    const auto slots = iclforge::apps::ac4_object_slots(assignment, shapes);
    const auto flat = iclforge::apps::ac4_flat_planes(views);
    iclforge::apps::Ac4ObjectsParams params;
    params.sample_rate_hz = kRate;
    params.bitrate_kbps = kbps;
    params.coding = coding;
    const auto encoded = iclforge::apps::encode_ac4_scene(params, slots, flat, scene);
    REQUIRE(encoded.has_value());
    const auto packaged = iclforge::apps::package_ac4(encoded->frames, encoded->toc, mp4, crc);
    REQUIRE(packaged.has_value());
    return joined(*packaged);
}

Assignment every_channel_an_object(std::size_t channels) {
    Assignment assignment;
    for (std::size_t c = 0; c < channels; ++c) {
        assignment.set(0, c, obj());
    }
    return assignment;
}

// The objects a stream's first decoded frame carries.
std::size_t decoded_objects(const std::vector<std::byte>& stream) {
    const iclforge::ac4::ScanResult scan = iclforge::ac4::scan(stream);
    REQUIRE_FALSE(scan.frames.empty());
    iclforge::ac4::Decoder decoder;
    std::size_t most = 0;
    for (const iclforge::ac4::SyncFrame& frame : scan.frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        REQUIRE(decoded.has_value());
        if (decoded->has_value()) {
            most = std::max(most, (**decoded).objects.size());
        }
    }
    return most;
}

struct Refusal {
    std::string args;
    int exit_code;
    std::string message;
};

}  // namespace

TEST_CASE("atmos-encode codec=ac4 writes a raw stream of the shared steps' bytes, A-JOC and direct",
          "[cli][atmos][ac4]") {
    const auto dir = scratch_dir();
    const auto in = tone_wav(dir / "raw_in.wav", 3);
    const auto scene = moving_scene(3);
    const auto paths = write_scene(dir / "raw_paths.json", scene);
    const auto wav = read(in);

    for (const bool direct : {false, true}) {
        CAPTURE(direct);
        const auto out = dir / (direct ? "raw_direct.ac4" : "raw_ajoc.ac4");
        const auto log = dir / "raw.log";
        REQUIRE(run_cli("atmos-encode " + quoted(in) + " " + quoted(out) + " 256 3 " +
                            quoted(paths) + " codec=ac4" + (direct ? " coding=direct" : ""),
                        log) == 0);
        const auto text = read_log(log);
        INFO(text);
        CHECK(text.find("encoded ") != std::string::npos);
        CHECK(text.find(direct ? "3 objects (3 dynamic), direct-coded" : "A-JOC") !=
              std::string::npos);
        CHECK(text.find("the decoder's output lags the input by 4385 samples") !=
              std::string::npos);

        const auto want = shared_bytes(
            {&wav}, {0}, every_channel_an_object(3), scene, 256,
            direct ? iclforge::ac4::ObjectCoding::kDirect : iclforge::ac4::ObjectCoding::kAjoc,
            false, true);
        const auto got = file_bytes(out);
        REQUIRE_FALSE(got.empty());
        CHECK(got == want);
        // The sync word 0xAC41 carries the CRC, and the stream decodes to the three objects.
        CHECK(got[0] == std::byte{0xAC});
        CHECK(got[1] == std::byte{0x41});
        CHECK(decoded_objects(got) == 3);
    }
}

TEST_CASE(
    "atmos-encode codec=ac4 crc=off drops the sync frames' CRC and names an MP4 file by its suffix",
    "[cli][atmos][ac4]") {
    const auto dir = scratch_dir();
    const auto in = tone_wav(dir / "mp4_in.wav", 2);
    const auto scene = moving_scene(2);
    const auto paths = write_scene(dir / "mp4_paths.json", scene);
    const auto wav = read(in);
    const auto log = dir / "mp4.log";

    const auto no_crc = dir / "no_crc.ac4";
    REQUIRE(run_cli("atmos-encode " + quoted(in) + " " + quoted(no_crc) + " 192 2 " +
                        quoted(paths) + " codec=ac4 crc=off",
                    log) == 0);
    CHECK(file_bytes(no_crc) == shared_bytes({&wav}, {0}, every_channel_an_object(2), scene, 192,
                                             iclforge::ac4::ObjectCoding::kAjoc, false, false));

    const auto mp4 = dir / "objects.mp4";
    REQUIRE(run_cli("atmos-encode " + quoted(in) + " " + quoted(mp4) + " 192 2 " + quoted(paths) +
                        " codec=ac4",
                    log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("MP4, codecs ac-4.") != std::string::npos);
    const auto got = file_bytes(mp4);
    CHECK(got == shared_bytes({&wav}, {0}, every_channel_an_object(2), scene, 192,
                              iclforge::ac4::ObjectCoding::kAjoc, true, true));
    // 'ftyp' at byte 4.
    REQUIRE(got.size() > 8);
    CHECK(std::string(reinterpret_cast<const char*>(got.data()) + 4, 4) == "ftyp");
}

TEST_CASE("atmos-encode codec=ac4 takes src=, map= and offset= the way the page's assignment does",
          "[cli][atmos][ac4]") {
    const auto dir = scratch_dir();
    // Source 0 is three channels, source 1 two, and source 1 starts 20 ms in.
    const auto a = tone_wav(dir / "map_a.wav", 3);
    const auto b = tone_wav(dir / "map_b.wav", 2, 2 * 2048);
    const auto wav_a = read(a);
    const auto wav_b = read(b);
    // 0.1 and 0.2 fold to one object and 0.0 goes to an object of its own; 1.0 is held at the
    // left speaker, 1.1 is the LFE.
    Assignment assignment;
    assignment.set(0, 0, obj(-3.0));
    assignment.set(0, 1, objm());
    assignment.set(0, 2, objm());
    assignment.set(1, 0, at(Location::kLeft));
    assignment.set(1, 1, at(Location::kLfe));
    const auto scene = moving_scene(2);  // the two dynamic objects, in map= order
    const auto paths = write_scene(dir / "map_paths.json", scene);
    const auto out = dir / "map_out.ac4";
    const auto log = dir / "map.log";
    REQUIRE(run_cli("atmos-encode " + quoted(a) + " " + quoted(out) + " 320 0 " + quoted(paths) +
                        " src=" + quoted(b) +
                        " map=0.0:obj@-3,0.1-2:objm,1.0:L,1.1:LFE offset=1:0.02 codec=ac4",
                    log) == 0);
    const auto text = read_log(log);
    INFO(text);
    const std::size_t offset = static_cast<std::size_t>(std::llround(0.02 * kRate));
    const auto want = shared_bytes({&wav_a, &wav_b}, {0, offset}, assignment, scene, 320,
                                   iclforge::ac4::ObjectCoding::kAjoc, false, true);
    const auto got = file_bytes(out);
    CHECK(got == want);
    // Two dynamic objects, one held at the speaker, and the LFE.
    CHECK(decoded_objects(got) == 4);
    CHECK(text.find("4 objects (2 dynamic)") != std::string::npos);
}

TEST_CASE("atmos-encode codec=ac4 without a scene file places each channel where E-AC-3's does",
          "[cli][atmos][ac4]") {
    const auto dir = scratch_dir();
    const auto in = tone_wav(dir / "default_in.wav", 6);
    const auto out = dir / "default_out.ac4";
    const auto log = dir / "default.log";
    // Six channels are a 5.1 file: L C R Ls Rs from the layout's own azimuths, the LFE
    // channel fanned like the rest of atmos-encode does it. The CLI takes them, one
    // object each, and the stream decodes to six.
    REQUIRE(run_cli("atmos-encode " + quoted(in) + " " + quoted(out) + " 384 codec=ac4", log) == 0);
    INFO(read_log(log));
    CHECK(decoded_objects(file_bytes(out)) == 6);
    // objects=2 takes the first two channels alone.
    const auto two = dir / "default_two.ac4";
    REQUIRE(run_cli("atmos-encode " + quoted(in) + " " + quoted(two) + " 256 2 codec=ac4", log) ==
            0);
    CHECK(decoded_objects(file_bytes(two)) == 2);
}

TEST_CASE("atmos-encode codec=ac4 refuses what an object stream cannot carry",
          "[cli][atmos][ac4]") {
    const auto dir = scratch_dir();
    const auto stereo = tone_wav(dir / "refuse_stereo.wav", 2);
    const auto wide = tone_wav(dir / "refuse_65.wav", 65, 2048);
    const auto out = dir / "refused.ac4";
    const auto log = dir / "refused.log";
    const auto run = [&](const fs::path& in, std::string_view rest) {
        return "atmos-encode " + quoted(in) + " " + quoted(out) + " 384 " + std::string{rest};
    };
    for (const Refusal& row : std::vector<Refusal>{
             {run(stereo, "codec=ac4 sign-objects"), 1,
              "sign-objects signs E-AC-3's EMDF object container"},
             {run(stereo, "codec=ac4 dialnorm=auto"), 1, "dialnorm=auto measures a bed's loudness"},
             {run(wide, "codec=ac4"), 1, "65 objects: an AC-4 object stream holds 64 at most"},
             {run(stereo,
                  "0 codec=ac4 src=" + quoted(stereo) + " map=0.0:none,0.1:none,1.0:none,1.1:none"),
              1, "an AC-4 object stream needs at least one object that is not the LFE"},
             {run(stereo,
                  "2 codec=ac4 src=" + quoted(stereo) + " map=0.0:obj,0.1:obj,1.0:none,1.1:none"),
              1,
              "[objects] counts the source channels to turn into objects, which map= states "
              "instead"},
             {run(stereo,
                  "0 codec=ac4 src=" + quoted(stereo) + " map=0.0:obj,0.1:LFE,1.0:LFE,1.1:none"),
              1, "2 channels are assigned to an LFE: an AC-4 object stream has one LFE object"},
             {run(stereo, "codec=ac4 coding=surround"), 1, "coding must be ajoc or direct"},
             {run(stereo, "codec=ac4 crc=maybe"), 1, "crc is on"},
             // coding= and crc= are AC-4 objects' options: without codec=ac4 they are refused,
             // not ignored.
             {run(stereo, "coding=direct"), 1, "coding= and crc= are the options of AC-4 objects"},
             {run(stereo, "crc=off"), 1, "coding= and crc= are the options of AC-4 objects"}}) {
        CAPTURE(row.args);
        fs::remove(out);
        const auto rc = run_cli(row.args, log);
        const auto text = read_log(log);
        INFO(text);
        CHECK(rc == row.exit_code);
        CHECK(text.find(row.message) != std::string::npos);
        CHECK_FALSE(fs::exists(out));
    }
    // crc= names a raw stream's CRC, which an MP4 sample lacks.
    const auto mp4 = dir / "refused.mp4";
    REQUIRE(run_cli("atmos-encode " + quoted(stereo) + " " + quoted(mp4) + " 256 codec=ac4 crc=off",
                    log) == 1);
    CHECK(read_log(log).find("an MP4 sample is the raw frame alone") != std::string::npos);
    CHECK_FALSE(fs::exists(mp4));
}

TEST_CASE("atmos-encode without codec=ac4 is still E-AC-3", "[cli][atmos][ac4]") {
    const auto dir = scratch_dir();
    const auto in = tone_wav(dir / "eac3_in.wav", 2);
    const auto out = dir / "eac3_out.ec3";
    const auto log = dir / "eac3.log";
    REQUIRE(run_cli("atmos-encode " + quoted(in) + " " + quoted(out) + " 448", log) == 0);
    const auto text = read_log(log);
    INFO(text);
    CHECK(text.find("E-AC-3 access units") != std::string::npos);
    // The E-AC-3 stream's sync word, not AC-4's.
    const auto got = file_bytes(out);
    REQUIRE(got.size() > 2);
    CHECK(got[0] == std::byte{0x0B});
    CHECK(got[1] == std::byte{0x77});
    // codec=eac3 says the same.
    const auto named = dir / "eac3_named.ec3";
    REQUIRE(run_cli("atmos-encode " + quoted(in) + " " + quoted(named) + " 448 codec=eac3", log) ==
            0);
    CHECK(file_bytes(named) == got);
}
