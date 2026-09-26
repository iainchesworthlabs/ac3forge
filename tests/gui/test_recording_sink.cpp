#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "platform/process.hpp"

#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "ac4/ac4.hpp"
#include "ac4enc/encoder.hpp"
#include "matroska/matroska.hpp"
#include "mp4/mp4.hpp"
#include "mpegts/mpegts.hpp"
#include "recording_sink.hpp"

// RecordingSink exists so a GUI take's encoded frames leave for disk as they
// are produced instead of accumulating until Stop. These tests hold it to
// the standard the streamed CLI paths were held to: for every container the
// sink streams, the file it leaves behind must be what the corresponding
// one-shot writer would have produced for the same frames - byte for byte
// where the format permits (elementary, MPEG-TS via mpegts::Writer's own
// mux-equality contract, the IEC 61937 WAV carrier), and equal to the
// incremental writer's own composed output for Matroska (whose streamed form
// differs from mux() by design - the unknown-size Segment).
//
// The fragmented-MP4 cases below exercise Fmp4FolderWriter, which the sink
// only delegates to: EncoderController's own live session writes its folder
// through the same class, so what is asserted here holds for both of the
// GUI's write-as-you-go paths.

namespace fs = std::filesystem;

namespace {

// See tests/cli/test_cli.cpp's own scratch_dir for the reasoning this copy
// shares, including the PID fold; the leaf name below is this file's own.
std::string scratch_pid_suffix() { return ac3::test::platform::process_id(); }

fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / ("recording_sink_" + scratch_pid_suffix());
    fs::create_directories(dir);
    return dir;
}

std::vector<std::byte> read_file_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.is_open());
    in.seekg(0, std::ios::end);
    std::vector<std::byte> bytes(static_cast<std::size_t>(in.tellg()));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(in.good());
    return bytes;
}

// Real frames, not synthetic bytes: the IEC 61937 path parses each frame's
// own header (sync word, fscod), so only genuine bitstream will do - and
// what is genuine enough for the strictest container serves the rest too -
// the fragmented-MP4 path re-scans a whole access unit for its dec3 box.
std::string read_file_text(const fs::path& path) {
    const auto bytes = read_file_bytes(path);
    std::string out(bytes.size(), '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[i] = static_cast<char>(bytes[i]);
    }
    return out;
}

std::vector<std::vector<std::byte>> silent_ac3_frames(std::size_t count) {
    std::vector<std::vector<std::byte>> frames;
    for (std::size_t i = 0; i < count; ++i) {
        auto frame = ac3::build_silent_stereo_frame({});
        REQUIRE(frame.has_value());
        frames.push_back(std::move(*frame));
    }
    return frames;
}

std::vector<std::vector<std::byte>> silent_eac3_units(std::size_t count) {
    const auto unit = ac3::eac3::build_silent_access_unit(
        {.independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}});
    REQUIRE(unit.has_value());
    return {count, unit->bytes};
}

std::vector<std::span<const std::byte>> as_views(
    const std::vector<std::vector<std::byte>>& frames) {
    return {frames.begin(), frames.end()};
}

std::vector<std::byte> pushed_through(RecordingSink::Container container, bool eac3,
                                      const fs::path& path,
                                      const std::vector<std::vector<std::byte>>& frames) {
    RecordingSink sink;
    REQUIRE(sink.open(path.string(),
                      {.container = container, .eac3 = eac3, .sample_rate = 48000, .channels = 2})
                .empty());
    for (const auto& frame : frames) {
        REQUIRE(sink.push(frame).empty());
    }
    REQUIRE(sink.close().empty());
    CHECK(sink.frames() == frames.size());
    return read_file_bytes(path);
}

}  // namespace

TEST_CASE("RecordingSink's elementary stream is the frames, concatenated", "[gui]") {
    const auto frames = silent_ac3_frames(5);
    const auto file = pushed_through(RecordingSink::Container::kElementary, false,
                                     scratch_dir() / "take.ac3", frames);
    std::vector<std::byte> expected;
    for (const auto& frame : frames) {
        expected.insert(expected.end(), frame.begin(), frame.end());
    }
    CHECK(file == expected);
}

TEST_CASE("RecordingSink's MPEG-TS take is byte-identical to mpegts::mux", "[gui]") {
    const auto frames = silent_ac3_frames(7);
    const auto file = pushed_through(RecordingSink::Container::kMpegts, false,
                                     scratch_dir() / "take.ts", frames);
    const auto one_shot = mpegts::mux(
        mpegts::AudioTrack{.codec = mpegts::AudioCodec::kAc3,
                           .sample_rate = 48000,
                           .channels = 2,
                           .samples_per_frame = ac3::kSamplesPerFrame},
        as_views(frames));
    REQUIRE(one_shot.has_value());
    CHECK(file == *one_shot);
}

TEST_CASE("RecordingSink's IEC 61937 take is byte-identical to the one-shot carrier",
          "[gui]") {
    const bool eac3 = GENERATE(false, true);
    const auto frames = eac3 ? silent_eac3_units(6) : silent_ac3_frames(6);
    const auto file =
        pushed_through(RecordingSink::Container::kSpdif, eac3,
                       scratch_dir() / (eac3 ? "take_eac3.wav" : "take_ac3.wav"), frames);

    const auto payload = ac3::iec61937::wrap_stream(as_views(frames), eac3);
    REQUIRE(payload.has_value());
    const auto one_shot = scratch_dir() / "carrier_one_shot.wav";
    REQUIRE(ac3::io::write_wav_pcm16_raw(one_shot.string(), *payload,
                                         eac3 ? 48000U * 4 : 48000U, 2)
                .has_value());
    CHECK(file == read_file_bytes(one_shot));
}

TEST_CASE("RecordingSink's Matroska take matches matroska::Writer's own composition",
          "[gui]") {
    const auto frames = silent_ac3_frames(40);
    const auto file = pushed_through(RecordingSink::Container::kMatroska, false,
                                     scratch_dir() / "take.mkv", frames);

    auto writer = matroska::Writer::create(
        matroska::AudioTrack{.codec_id = std::string{matroska::kCodecAc3},
                             .sample_rate = 48000,
                             .channels = 2,
                             .samples_per_frame = ac3::kSamplesPerFrame});
    REQUIRE(writer.has_value());
    std::vector<std::byte> expected = writer->header();
    for (const auto& frame : frames) {
        const auto closed = writer->push(frame);
        REQUIRE(closed.has_value());
        expected.insert(expected.end(), closed->begin(), closed->end());
    }
    const auto tail = writer->finalize();
    expected.insert(expected.end(), tail.begin(), tail.end());
    CHECK(file == expected);
}

TEST_CASE("RecordingSink's fragmented-MP4 take matches mp4::fragment's own segments", "[gui]") {
    // The one container here that writes a FOLDER. Its media segments are
    // mp4::fragment()'s byte for byte (mp4::FragmentWriter's own contract), so
    // this checks each written segment*.m4s against the batch form over the
    // same frames, and that the manifests the session leaves behind are the
    // closed, VOD-shaped pair rather than the live ones it wrote while
    // running. 100 frames at the default 48 per fragment is three segments -
    // two full and a short one - so a fragment boundary and a partial flush
    // are both in the take.
    const auto frames = silent_ac3_frames(100);
    const auto dir = scratch_dir() / "take_fmp4";
    fs::remove_all(dir);
    RecordingSink sink;
    REQUIRE(sink.open(dir.string(), {.container = RecordingSink::Container::kFmp4,
                                     .eac3 = false,
                                     .sample_rate = 48000,
                                     .channels = 2})
                .empty());
    for (const auto& frame : frames) {
        REQUIRE(sink.push(frame).empty());
    }
    REQUIRE(sink.close().empty());
    CHECK(sink.frames() == frames.size());

    // The batch form, built the way the sink builds its own track - from a
    // scan of the stream, since the dac3 payload is bitstream syntax.
    std::vector<std::byte> stream;
    for (const auto& frame : frames) {
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    const mp4::AudioTrack track{.codec_id = std::string{mp4::kCodecAc3},
                                .sample_rate = 48000,
                                .channels = scanned->channels,
                                .samples_per_frame = ac3::kSamplesPerFrame,
                                .codec_config = ac3::io::build_codec_config_box(*scanned)};
    const auto batch = mp4::fragment(track, as_views(frames));
    REQUIRE(batch.has_value());
    REQUIRE(batch->media_segments.size() == 3);

    CHECK(fs::exists(dir / "init.mp4"));
    for (const auto& segment : batch->media_segments) {
        const auto path = dir / fmt::format("segment{}.m4s", segment.sequence_number);
        REQUIRE(fs::exists(path));
        CHECK(read_file_bytes(path) == segment.bytes);
    }
    CHECK_FALSE(fs::exists(dir / "segment4.m4s"));

    const auto playlist = read_file_text(dir / "audio.m3u8");
    CHECK(playlist.find("#EXT-X-MAP:URI=\"init.mp4\"") != std::string::npos);
    CHECK(playlist.find("#EXT-X-ENDLIST") != std::string::npos);
    CHECK(playlist.find("segment3.m4s") != std::string::npos);
    CHECK(read_file_text(dir / "master.m3u8").find("CODECS=\"ac-3\"") != std::string::npos);
    const auto mpd = read_file_text(dir / "manifest.mpd");
    CHECK(mpd.find("type=\"static\"") != std::string::npos);
    CHECK(mpd.find("</MPD>") != std::string::npos);
}

TEST_CASE("RecordingSink's fragmented-MP4 folder is live-shaped mid-take", "[gui]") {
    // While the session is still running the manifests must be the LIVE ones:
    // no #EXT-X-ENDLIST for a playlist that will grow again, and a dynamic
    // MPD anchored to wall-clock time rather than one claiming a total
    // duration the take has not reached. Checked between pushes, since that
    // is the only moment the distinction exists.
    const auto frames = silent_ac3_frames(60);
    const auto dir = scratch_dir() / "take_fmp4_live";
    fs::remove_all(dir);
    RecordingSink sink;
    REQUIRE(sink.open(dir.string(), {.container = RecordingSink::Container::kFmp4,
                                     .eac3 = false,
                                     .sample_rate = 48000,
                                     .channels = 2})
                .empty());
    for (const auto& frame : frames) {
        REQUIRE(sink.push(frame).empty());
    }
    // 60 frames at 48 per fragment: one segment has closed, 12 frames are
    // still pending - exactly the mid-take state.
    REQUIRE(fs::exists(dir / "segment1.m4s"));
    CHECK_FALSE(fs::exists(dir / "segment2.m4s"));
    const auto live_playlist = read_file_text(dir / "audio.m3u8");
    CHECK(live_playlist.find("#EXT-X-MEDIA-SEQUENCE:1") != std::string::npos);
    CHECK(live_playlist.find("#EXT-X-ENDLIST") == std::string::npos);
    CHECK(live_playlist.find("#EXT-X-PLAYLIST-TYPE") == std::string::npos);
    const auto live_mpd = read_file_text(dir / "manifest.mpd");
    CHECK(live_mpd.find("type=\"dynamic\"") != std::string::npos);
    CHECK(live_mpd.find("availabilityStartTime=\"") != std::string::npos);
    CHECK(live_mpd.find("mediaPresentationDuration") == std::string::npos);

    REQUIRE(sink.close().empty());
    // The trailing partial fragment is flushed and both manifests close.
    CHECK(fs::exists(dir / "segment2.m4s"));
    CHECK(read_file_text(dir / "audio.m3u8").find("#EXT-X-ENDLIST") != std::string::npos);
    CHECK(read_file_text(dir / "manifest.mpd").find("type=\"static\"") != std::string::npos);
}

TEST_CASE("RecordingSink with zero frames removes the file and says so", "[gui]") {
    const auto path = scratch_dir() / "empty_take.ac3";
    RecordingSink sink;
    REQUIRE(sink.open(path.string(), {.container = RecordingSink::Container::kElementary,
                                      .eac3 = false,
                                      .sample_rate = 48000,
                                      .channels = 2})
                .empty());
    CHECK(sink.close() == "Nothing was encoded.");
    CHECK_FALSE(fs::exists(path));
}

TEST_CASE("RecordingSink with zero frames leaves a file that was already there in place",
          "[gui]") {
    // An empty take removes only what its own open() created. A path that
    // already held the user's file keeps it: open() truncates it, as a take
    // to an existing path always does, but "nothing was encoded" must not
    // delete it. (Before the fix this was removed outright - and, pointed at
    // a device node by a process running as root, so was the node.)
    const auto path = scratch_dir() / "empty_take_existing.ac3";
    { std::ofstream{path, std::ios::binary} << "the user's own bytes"; }
    for (const auto container : {RecordingSink::Container::kElementary,
                                 RecordingSink::Container::kMatroska,
                                 RecordingSink::Container::kMpegts,
                                 RecordingSink::Container::kSpdif}) {
        CAPTURE(static_cast<int>(container));
        RecordingSink sink;
        REQUIRE(sink.open(path.string(),
                          {.container = container, .eac3 = false, .sample_rate = 48000,
                           .channels = 2})
                    .empty());
        CHECK(sink.close() == "Nothing was encoded.");
        CHECK(fs::is_regular_file(path));
    }
    fs::remove(path);
}

TEST_CASE("RecordingSink with zero frames through a symlink removes neither the link nor its target",
          "[gui]") {
    const auto dir = scratch_dir();
    const auto target = dir / "empty_take_target.ac3";
    const auto link = dir / "empty_take_link.ac3";
    const RecordingSink::Config config{.container = RecordingSink::Container::kElementary,
                                       .eac3 = false,
                                       .sample_rate = 48000,
                                       .channels = 2};
    std::error_code ec;
    fs::remove(link, ec);
    fs::remove(target, ec);
    SECTION("a link to an existing file keeps both") {
        { std::ofstream{target, std::ios::binary} << "x"; }
        fs::create_symlink(target, link, ec);
        if (ec) {
            SKIP("cannot create a symlink here");
        }
        RecordingSink sink;
        REQUIRE(sink.open(link.string(), config).empty());
        CHECK(sink.close() == "Nothing was encoded.");
        CHECK(fs::is_symlink(link));
        CHECK(fs::is_regular_file(target));
    }
    SECTION("a dangling link keeps the link, and the file opening it made") {
        // The link was the user's, so it stays - and so does the file open()
        // created through it: the sink removes only a plain file it created
        // at the path itself, and never deletes anything through a link.
        fs::create_symlink(target, link, ec);
        if (ec) {
            SKIP("cannot create a symlink here");
        }
        RecordingSink sink;
        REQUIRE(sink.open(link.string(), config).empty());
        CHECK(sink.close() == "Nothing was encoded.");
        CHECK(fs::is_symlink(link));
        CHECK(fs::exists(target));
    }
    fs::remove(link, ec);
    fs::remove(target, ec);
}

TEST_CASE("RecordingSink reports an uncreatable destination at open, not at stop", "[gui]") {
    RecordingSink sink;
    const auto problem =
        sink.open((scratch_dir() / "no" / "such" / "dir" / "take.ac3").string(),
                  {.container = RecordingSink::Container::kElementary,
                   .eac3 = false,
                   .sample_rate = 48000,
                   .channels = 2});
    CHECK(problem == "Could not open the output file for writing.");
}

TEST_CASE("RecordingSink's IEC 61937 take patches its header as it goes and stays one-shot equal",
          "[gui]") {
    // 40 frames: past the 32nd, where the carrier's header is patched mid-take.
    const auto frames = silent_ac3_frames(40);
    const auto path = scratch_dir() / "spdif_long.wav";
    const auto streamed =
        pushed_through(RecordingSink::Container::kSpdif, /*eac3=*/false, path, frames);
    std::vector<std::byte> payload;
    for (const auto& frame : frames) {
        const auto burst = ac3::iec61937::wrap_frame(frame);
        REQUIRE(burst.has_value());
        payload.insert(payload.end(), burst->begin(), burst->end());
    }
    // 44-byte header, then the bursts exactly.
    REQUIRE(streamed.size() == 44 + payload.size());
    CHECK(std::vector<std::byte>(streamed.begin() + 44, streamed.end()) == payload);
}

TEST_CASE("RecordingSink reports an uncreatable IEC 61937 carrier at open", "[gui]") {
    RecordingSink sink;
    const auto problem = sink.open((scratch_dir() / "no" / "such" / "dir" / "take.wav").string(),
                                   {.container = RecordingSink::Container::kSpdif,
                                    .eac3 = true,
                                    .sample_rate = 48000,
                                    .channels = 6});
    CHECK(problem == "Could not open the output file for writing.");
}

TEST_CASE("RecordingSink refuses a track no container can describe, at open", "[gui]") {
    for (const auto container :
         {RecordingSink::Container::kMatroska, RecordingSink::Container::kMpegts}) {
        RecordingSink sink;
        const auto problem =
            sink.open((scratch_dir() / "no_channels.bin").string(),
                      {.container = container, .eac3 = false, .sample_rate = 48000, .channels = 0});
        CHECK_FALSE(problem.empty());
        // Nothing was opened, so there is nothing to close or report.
        CHECK(sink.close().empty());
    }
}

TEST_CASE("RecordingSink that was never opened closes quietly", "[gui]") {
    RecordingSink sink;
    CHECK(sink.close().empty());
    CHECK(sink.frames() == 0);
}

TEST_CASE("RecordingSink reports bytes it cannot wrap into IEC 61937 bursts", "[gui]") {
    const std::vector<std::byte> not_a_frame(512, std::byte{0x5A});
    for (const bool eac3 : {false, true}) {
        RecordingSink sink;
        const auto path = scratch_dir() / (eac3 ? "garbage_eac3.wav" : "garbage_ac3.wav");
        REQUIRE(sink.open(path.string(), {.container = RecordingSink::Container::kSpdif,
                                          .eac3 = eac3,
                                          .sample_rate = 48000,
                                          .channels = 2})
                    .empty());
        CHECK(sink.push(not_a_frame) == "Could not wrap the stream into IEC 61937 bursts.");
        CHECK(sink.frames() == 0);
        CHECK(sink.close() == "Nothing was encoded.");
        CHECK_FALSE(fs::exists(path));
    }
}

TEST_CASE("RecordingSink reports an access unit too large for one MPEG-TS PES packet", "[gui]") {
    RecordingSink sink;
    REQUIRE(sink.open((scratch_dir() / "huge.ts").string(),
                      {.container = RecordingSink::Container::kMpegts,
                       .eac3 = true,
                       .sample_rate = 48000,
                       .channels = 2})
                .empty());
    const std::vector<std::byte> huge(70'000, std::byte{0});
    const auto problem = sink.push(huge);
    CHECK_FALSE(problem.empty());
    CHECK(sink.frames() == 0);
}

TEST_CASE("RecordingSink's fragmented-MP4 take reports a bad folder, a bad frame and an empty take",
          "[gui]") {
    {
        // A folder cannot be made under a regular file.
        const auto blocker = scratch_dir() / "fmp4_blocker";
        { std::ofstream{blocker} << "x"; }
        RecordingSink sink;
        const auto problem = sink.open((blocker / "take").string(),
                                       {.container = RecordingSink::Container::kFmp4,
                                        .eac3 = true,
                                        .sample_rate = 48000,
                                        .channels = 6});
        CHECK_FALSE(problem.empty());
    }
    {
        const auto folder = scratch_dir() / "fmp4_bad_frame";
        fs::remove_all(folder);
        RecordingSink sink;
        REQUIRE(sink.open(folder.string(), {.container = RecordingSink::Container::kFmp4,
                                            .eac3 = true,
                                            .sample_rate = 48000,
                                            .channels = 6})
                    .empty());
        const std::vector<std::byte> not_a_unit(256, std::byte{0x5A});
        CHECK_FALSE(sink.push(not_a_unit).empty());
    }
    {
        // Nothing encoded: the folder open() made is removed again.
        const auto folder = scratch_dir() / "fmp4_empty";
        fs::remove_all(folder);
        RecordingSink sink;
        REQUIRE(sink.open(folder.string(), {.container = RecordingSink::Container::kFmp4,
                                            .eac3 = true,
                                            .sample_rate = 48000,
                                            .channels = 6})
                    .empty());
        CHECK(fs::is_directory(folder));
        CHECK(sink.close() == "Nothing was encoded.");
        CHECK_FALSE(fs::exists(folder));
    }
    {
        // ...but a folder the user already had, even an empty one, is theirs
        // and stays.
        const auto folder = scratch_dir() / "fmp4_empty_existing";
        fs::remove_all(folder);
        fs::create_directories(folder);
        RecordingSink sink;
        REQUIRE(sink.open(folder.string(), {.container = RecordingSink::Container::kFmp4,
                                            .eac3 = true,
                                            .sample_rate = 48000,
                                            .channels = 6})
                    .empty());
        CHECK(sink.close() == "Nothing was encoded.");
        CHECK(fs::is_directory(folder));
        fs::remove_all(folder);
    }
}

// Portable as written: on a system without a /dev/full character device the
// helper below finds none and both cases SKIP, so no preprocessor branch is
// needed to keep them off other platforms.
// /dev/full: every write the kernel sees fails with ENOSPC, the disk-full case
// a long take can actually meet. The streams are buffered, so the failure
// surfaces a few frames in - which is exactly how it surfaces on a real disk.
//
// Reached through a symlink of this test's own, never by its real name, so
// that a regression of close()'s "only remove what open() created" rule costs
// this test its link rather than the machine its device node: a take that
// fails before its first frame is an empty take, and an empty take's close()
// used to remove whatever was at the path.
namespace {

std::optional<fs::path> full_device_link() {
    std::error_code ec;
    if (!fs::is_character_file("/dev/full", ec)) {
        return std::nullopt;
    }
    const auto link = scratch_dir() / "dev_full";
    fs::remove(link, ec);
    fs::create_symlink("/dev/full", link, ec);
    if (ec) {
        return std::nullopt;
    }
    return link;
}

}  // namespace

TEST_CASE("RecordingSink names a full disk in each container's own words", "[gui]") {
    const auto full = full_device_link();
    if (!full) {
        SKIP("no /dev/full character device here");
    }
    struct Case {
        RecordingSink::Container container;
        bool eac3;
        const char* message;
    };
    for (const auto& c : {Case{RecordingSink::Container::kElementary, false,
                               "Writing the stream failed."},
                          Case{RecordingSink::Container::kMatroska, false,
                               "Writing the Matroska file failed."},
                          Case{RecordingSink::Container::kMpegts, false,
                               "Writing the MPEG-TS file failed."},
                          Case{RecordingSink::Container::kSpdif, false,
                               "Writing the WAV carrier failed."},
                          Case{RecordingSink::Container::kSpdif, true,
                               "Writing the WAV carrier failed."}}) {
        CAPTURE(static_cast<int>(c.container), c.eac3);
        // The link predates every take, so no empty take removed it.
        REQUIRE(fs::is_symlink(*full));
        RecordingSink sink;
        REQUIRE(sink.open(full->string(), {.container = c.container,
                                           .eac3 = c.eac3,
                                           .sample_rate = 48000,
                                           .channels = c.eac3 ? 6 : 2})
                    .empty());
        const auto frames = c.eac3 ? silent_eac3_units(1) : silent_ac3_frames(1);
        std::string problem;
        for (int i = 0; i < 2000 && problem.empty(); ++i) {
            problem = sink.push(frames.front());
        }
        CHECK(problem == c.message);
        // Closing a take whose disk filled up never reports a clean finish:
        // the write failure again, or - where the very first frame was the
        // one that failed - an empty take. The carrier's close() is the
        // exception, with nothing buffered left to fail.
        const bool empty_take = sink.frames() == 0;
        const auto closed = sink.close();
        if (empty_take) {
            CHECK(closed == "Nothing was encoded.");
        } else if (c.container != RecordingSink::Container::kSpdif) {
            CHECK(closed == c.message);
        }
    }
    CHECK(fs::is_character_file("/dev/full"));
}

TEST_CASE("RecordingSink reports a take that only fails as it is closed", "[gui]") {
    const auto full = full_device_link();
    if (!full) {
        SKIP("no /dev/full character device here");
    }
    // One small frame fits in the stream's buffer, so the failure arrives with
    // the flush at close().
    RecordingSink sink;
    REQUIRE(sink.open(full->string(), {.container = RecordingSink::Container::kElementary,
                                       .eac3 = false,
                                       .sample_rate = 48000,
                                       .channels = 2})
                .empty());
    const auto small = ac3::build_silent_stereo_frame({.bitrate_kbps = 32});
    REQUIRE(small.has_value());
    REQUIRE(small->size() < 512);
    REQUIRE(sink.push(*small).empty());
    CHECK(sink.close() == "Writing the stream failed.");
}

// ---------------------------------------------------------------------------
// AC-4 takes (planning/ac4.md, phase I1): record and live codec=ac4 describe
// the stream to the sink themselves (take_sink_config), and push each frame
// with its I-frame flag. What the sink writes is held here to the one-shot
// writers over the same frames, as the AC-3 and E-AC-3 cases above are.
// ---------------------------------------------------------------------------

namespace {

// An AC-4 take: the encoder's frames as sync frames with the CRC, the raw
// frame an MP4 sample holds of each, each frame's I-frame flag, and the table
// of contents that describes the stream. Three seconds of stereo with an
// I-frame every fifth frame, so a fragment of mp4's default 48 frames has to
// wait for the next I-frame to close.
struct Ac4Frames {
    std::vector<std::vector<std::byte>> sync;
    std::vector<std::vector<std::byte>> raw;
    std::vector<bool> iframes;
    ac4::Toc toc;
};

const Ac4Frames& ac4_frames() {
    static const Ac4Frames take = [] {
        ac4::EncoderConfig config;
        config.channels = 2;
        config.bitrate_kbps = 64;
        config.iframe_interval = 5;
        auto encoder = ac4::Encoder::create(config);
        REQUIRE(encoder.has_value());
        std::vector<float> left(3 * 48000);
        std::vector<float> right(left.size());
        for (std::size_t n = 0; n < left.size(); ++n) {
            const double t = static_cast<double>(n) / 48000.0;
            left[n] = static_cast<float>(0.2 * std::sin(2.0 * std::numbers::pi * 440.0 * t));
            right[n] = static_cast<float>(0.2 * std::sin(2.0 * std::numbers::pi * 660.0 * t));
        }
        const std::vector<std::span<const float>> views{left, right};
        auto frames = encoder->encode(views);
        REQUIRE(frames.has_value());
        auto rest = encoder->flush();
        REQUIRE(rest.has_value());
        frames->insert(frames->end(), rest->begin(), rest->end());
        Ac4Frames out;
        out.toc = encoder->toc();
        for (const ac4::EncodedFrame& frame : *frames) {
            out.raw.push_back(frame.raw_ac4_frame);
            out.sync.push_back(ac4::sync_frame(frame.raw_ac4_frame, true));
            out.iframes.push_back(frame.iframe);
        }
        return out;
    }();
    return take;
}

// What ac3cli's take_sink_config says of the stream, built the same way.
RecordingSink::Ac4Carriage carriage_of(const Ac4Frames& take) {
    RecordingSink::Ac4Carriage carriage;
    carriage.samples_per_frame = 2048;
    std::size_t largest = 0;
    for (const auto& frame : take.sync) {
        largest = std::max(largest, frame.size());
    }
    const auto type = ac3::iec61937::ac4_burst_type_for(largest, 1, 13);
    REQUIRE(type.has_value());
    const auto link = ac3::iec61937::ac4_burst_timing(*type, 1, 13);
    REQUIRE(link.has_value());
    const bool hbr16 = *type == ac3::iec61937::BurstDataType::kAc4Hbr16;
    carriage.burst_type = *type;
    carriage.carrier_rate_hz = hbr16 ? link->link_rate_hz / 4 : link->link_rate_hz;
    carriage.carrier_channels = hbr16 ? 8 : 2;
    const auto timing = ac4::media_timing(take.toc);
    REQUIRE(timing.has_value());
    carriage.fmp4.audio = mp4::AudioTrack{.codec_id = std::string{mp4::kCodecAc4},
                                          .sample_rate = 48000,
                                          .channels = 2,
                                          .samples_per_frame = timing->sample_delta,
                                          .codec_config = ac4::build_dac4(take.toc),
                                          .rfc6381 = ac4::rfc6381_codec_string(take.toc),
                                          .timescale = timing->timescale};
    carriage.fmp4.brands = {"ca4m", "ca4s"};
    return carriage;
}

RecordingSink::Config ac4_config(RecordingSink::Container container) {
    RecordingSink::Config config{
        .container = container, .eac3 = false, .sample_rate = 48000, .channels = 2};
    config.ac4 = carriage_of(ac4_frames());
    return config;
}

// Pushes the take through a sink for `container`, each frame with its flag.
void push_ac4(RecordingSink::Container container, const fs::path& path) {
    const Ac4Frames& take = ac4_frames();
    RecordingSink sink;
    REQUIRE(sink.open(path.string(), ac4_config(container)).empty());
    for (std::size_t i = 0; i < take.sync.size(); ++i) {
        REQUIRE(sink.push(take.sync[i], take.iframes[i]).empty());
    }
    REQUIRE(sink.close().empty());
    CHECK(sink.frames() == take.sync.size());
}

}  // namespace

TEST_CASE("RecordingSink writes an AC-4 take raw and as MPEG-TS as the one-shot writers do",
          "[gui][ac4]") {
    const Ac4Frames& take = ac4_frames();
    const auto raw = scratch_dir() / "take.ac4";
    push_ac4(RecordingSink::Container::kElementary, raw);
    std::vector<std::byte> expected;
    for (const auto& frame : take.sync) {
        expected.insert(expected.end(), frame.begin(), frame.end());
    }
    CHECK(read_file_bytes(raw) == expected);

    const auto ts = scratch_dir() / "take_ac4.ts";
    push_ac4(RecordingSink::Container::kMpegts, ts);
    const auto one_shot = mpegts::mux(mpegts::AudioTrack{.codec = mpegts::AudioCodec::kAc4,
                                                         .sample_rate = 48000,
                                                         .channels = 2,
                                                         .samples_per_frame = 2048},
                                      as_views(take.sync));
    REQUIRE(one_shot.has_value());
    CHECK(read_file_bytes(ts) == *one_shot);
}

TEST_CASE("RecordingSink packs an AC-4 take in IEC 61937-14 bursts as the packer does",
          "[gui][ac4]") {
    const Ac4Frames& take = ac4_frames();
    const auto file = scratch_dir() / "take_ac4_spdif.wav";
    push_ac4(RecordingSink::Container::kSpdif, file);
    const auto carriage = carriage_of(take);
    ac3::iec61937::Ac4BurstPacker packer{carriage.burst_type};
    std::vector<std::byte> payload;
    for (const auto& frame : take.sync) {
        const auto burst = packer.push(frame);
        REQUIRE(burst.has_value());
        payload.insert(payload.end(), burst->begin(), burst->end());
    }
    const auto one_shot = scratch_dir() / "take_ac4_spdif_one_shot.wav";
    REQUIRE(ac3::io::write_wav_pcm16_raw(one_shot.string(), payload, carriage.carrier_rate_hz,
                                         carriage.carrier_channels)
                .has_value());
    CHECK(read_file_bytes(file) == read_file_bytes(one_shot));
}

TEST_CASE("RecordingSink fragments an AC-4 take at its I-frames as mp4::fragment does",
          "[gui][ac4]") {
    const Ac4Frames& take = ac4_frames();
    const auto dir = scratch_dir() / "take_ac4_fmp4";
    fs::remove_all(dir);
    push_ac4(RecordingSink::Container::kFmp4, dir);

    // The batch form over the raw frames, with the same flags and brands.
    const auto carriage = carriage_of(take);
    const auto batch = mp4::fragment(
        carriage.fmp4.audio, as_views(take.raw),
        mp4::FragmentOptions{.sync_samples = take.iframes, .brands = carriage.fmp4.brands});
    REQUIRE(batch.has_value());
    // The first fragment waits past 48 frames for the I-frame at frame 50.
    REQUIRE(batch->media_segments.size() >= 2);
    CHECK(batch->media_segments.front().sample_count == 50);
    // The init segment's ftyp and sample entry are the batch form's; its
    // durations are not, since a take writes it before it knows its length.
    const auto init = read_file_bytes(dir / "init.mp4");
    REQUIRE(init.size() >= 32);
    CHECK(std::equal(init.begin(), init.begin() + 32, batch->init_segment.begin()));
    CHECK(read_file_text(dir / "init.mp4").find("dac4") != std::string::npos);
    for (const auto& segment : batch->media_segments) {
        const auto path = dir / fmt::format("segment{}.m4s", segment.sequence_number);
        REQUIRE(fs::exists(path));
        CHECK(read_file_bytes(path) == segment.bytes);
    }
    CHECK(read_file_text(dir / "master.m3u8").find("CODECS=\"ac-4.") != std::string::npos);
    CHECK(read_file_text(dir / "manifest.mpd").find("type=\"static\"") != std::string::npos);
}

TEST_CASE("RecordingSink refuses AC-4 in Matroska and a frame that is not a sync frame",
          "[gui][ac4]") {
    {
        RecordingSink sink;
        const auto problem = sink.open((scratch_dir() / "take_ac4.mkv").string(),
                                       ac4_config(RecordingSink::Container::kMatroska));
        CHECK(problem.find("Matroska registers no codec ID for AC-4") != std::string::npos);
    }
    {
        const auto folder = scratch_dir() / "take_ac4_bad_frame";
        fs::remove_all(folder);
        RecordingSink sink;
        REQUIRE(sink.open(folder.string(), ac4_config(RecordingSink::Container::kFmp4)).empty());
        const std::vector<std::byte> not_a_frame(3, std::byte{0xAC});
        CHECK(sink.push(not_a_frame) == "An AC-4 frame was not a whole sync frame.");
        CHECK(sink.close() == "Nothing was encoded.");
    }
    {
        // No burst type carries the frames: the caller says so with no link.
        auto config = ac4_config(RecordingSink::Container::kSpdif);
        config.ac4->carrier_rate_hz = 0;
        RecordingSink sink;
        CHECK(sink.open((scratch_dir() / "take_ac4_no_burst.wav").string(), config)
                  .find("No IEC 61937-14 burst type") != std::string::npos);
    }
}
