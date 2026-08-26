#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/wav.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
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
// shares; the leaf name below is this file's own.
fs::path scratch_dir() {
    auto dir = fs::path{AC3FORGE_TEST_SCRATCH_DIR} / "recording_sink";
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
