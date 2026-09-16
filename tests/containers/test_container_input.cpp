#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ac3/encoder/silent_frame.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "container_input.hpp"
#include "matroska/matroska.hpp"
#include "mp4/mp4.hpp"
#include "mp4/reader.hpp"
#include "mpegts/mpegts.hpp"

namespace {

using Bytes = std::vector<std::byte>;

// A 48 kHz track as the MP4 reader reports one, with `edits`.
mp4::ReadTrack track_with(std::vector<mp4::EditListEntry> edits, std::uint32_t timescale = 48000,
                          std::uint32_t movie_timescale = 48000) {
    mp4::ReadTrack track;
    track.sample_rate = 48000;
    track.timescale = timescale;
    track.movie_timescale = movie_timescale;
    track.edits = std::move(edits);
    return track;
}

}  // namespace

// Roadmap IO2's own regression (tools/ci/fuzz_encoder_space.py's
// REGRESSION_SEEDS, seed 3600083275727211684): AC-3 at 48 kbit/s and 48 kHz
// codes exactly 192-byte frames - one of MPEG-TS's own three packet grid
// strides - so a stream whose frames repeat a byte at that same offset every
// frame (a low-entropy signal encodes near-identically frame to frame) looks
// exactly like a packet grid to a check that only counts recurrence. This
// builds that collision directly rather than relying on a fuzz seed landing
// on it: a real, syntactically-valid AC-3 header (so sniff_container's own
// elementary-stream check has something genuine to find) padded with 0x47 at
// a fixed 192-byte stride for several "frames" in a row.
TEST_CASE("sniff_container does not mistake a repetitive elementary stream for MPEG-TS",
          "[containers][io2]") {
    // The smallest legal AC-3 frame at 48 kHz (Table 5.18's lowest rung,
    // 32 kbit/s) is 64 words = 128 bytes - ac3::io::read_frame_header only
    // ever reads the syncinfo/bsi header (well under 128 bytes for a plain
    // 2/0 layout), so everything past it is free to overwrite.
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = 32});
    REQUIRE(frame.has_value());
    REQUIRE(ac3::io::read_frame_header(*frame).has_value());

    constexpr std::size_t kStride = 192;    // one of the three grid strides
    constexpr int kRepeats = 8;             // past kTsSyncRuns's own 5
    Bytes stream(kStride * kRepeats, std::byte{0x00});
    // The real header goes at the very start, untouched - what makes this a
    // genuine elementary stream and not just an arbitrary byte pattern. The
    // 0x47 recurrence starts one stride later, at i=1, so it never
    // overwrites the sync word this test depends on: the grid still needs
    // only 5 consecutive hits, and i=1..7 already gives it 7.
    std::ranges::copy(*frame, stream.begin());
    for (int i = 1; i < kRepeats; ++i) {
        stream[static_cast<std::size_t>(i) * kStride] = std::byte{0x47};
    }

    CHECK(ac3::apps::sniff_container(stream) == ac3::apps::ContainerKind::kUnknown);
}

TEST_CASE("sniff_container still finds a real MPEG-TS packet grid", "[containers][io2]") {
    // The negative case beside the one above: a plain 0x47 recurrence with
    // no valid frame header anywhere in it (the read_frame_header check
    // fails immediately on all-zero bytes) is still read as a transport
    // stream - the fix narrows the false positive, it does not disable the
    // grid check.
    constexpr std::size_t kStride = 188;
    constexpr int kRepeats = 6;
    Bytes stream(kStride * kRepeats, std::byte{0x00});
    for (int i = 0; i < kRepeats; ++i) {
        stream[static_cast<std::size_t>(i) * kStride] = std::byte{0x47};
    }

    CHECK(ac3::apps::sniff_container(stream) == ac3::apps::ContainerKind::kMpegTs);
}

TEST_CASE("elementary_stream_from_bytes leaves a bare elementary stream untouched",
          "[containers][io2]") {
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());

    const auto result = ac3::apps::elementary_stream_from_bytes(*frame);
    CHECK(result.error.empty());
    CHECK(result.bytes == *frame);
    CHECK(result.trim.start == 0);
    CHECK_FALSE(result.trim.length.has_value());
    CHECK(result.trim_note.empty());
    CHECK(result.container.kind == ac3::apps::ContainerKind::kUnknown);
    CHECK(result.container.codec_id.empty());
    CHECK(result.container.samples == 0);
    CHECK_FALSE(result.container.codec_box.has_value());
}

TEST_CASE("elementary_stream_from_bytes reports what each container says about its track",
          "[containers][io2]") {
    using ac3::apps::ContainerKind;
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());
    Bytes stream;
    for (int i = 0; i < 4; ++i) {
        stream.insert(stream.end(), frame->begin(), frame->end());
    }
    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    const std::span<const std::span<const std::byte>> units(scanned->access_units);

    SECTION("MP4, with its codec configuration box") {
        mp4::AudioTrack track;
        track.codec_id = std::string{mp4::kCodecAc3};
        track.sample_rate = 48000;
        track.channels = 2;
        track.codec_config = ac3::io::build_codec_config_box(*scanned);
        const auto file = mp4::mux(track, units);
        REQUIRE(file.has_value());
        const auto result = ac3::apps::elementary_stream_from_bytes(*file);
        REQUIRE(result.error.empty());
        const auto& facts = result.container;
        CHECK(facts.kind == ContainerKind::kMp4);
        CHECK(ac3::apps::container_token(facts.kind) == "mp4");
        CHECK(facts.codec_id == "ac-3");
        CHECK(facts.track == 1);
        CHECK(facts.language == "und");
        CHECK(facts.samples == 4);
        CHECK(facts.sample_rate == 48000);
        CHECK(facts.channels == 2);
        CHECK(facts.timescale == 48000);
        CHECK(facts.edits == 0);
        REQUIRE(facts.codec_box.has_value());
        CHECK(facts.codec_box->type == "dac3");
        CHECK(facts.codec_box->fscod == 0);
        CHECK(facts.codec_box->bsid == 8);
        CHECK(facts.codec_box->acmod == 2);
        CHECK_FALSE(facts.codec_box->lfeon);
        CHECK(facts.codec_box->bit_rate_code == 10);  // Table 5.18: 192 kbit/s
        CHECK(facts.codec_box->independent_substreams == 0);
        CHECK(facts.codec_box->bytes > 0);
    }
    SECTION("Matroska") {
        matroska::AudioTrack track;
        track.codec_id = std::string{matroska::kCodecAc3};
        track.sample_rate = 48000;
        track.channels = 2;
        const auto file = matroska::mux(track, units);
        REQUIRE(file.has_value());
        const auto result = ac3::apps::elementary_stream_from_bytes(*file);
        REQUIRE(result.error.empty());
        const auto& facts = result.container;
        CHECK(facts.kind == ContainerKind::kMatroska);
        CHECK(ac3::apps::container_token(facts.kind) == "matroska");
        CHECK(facts.codec_id == "A_AC3");
        CHECK(facts.track == 1);
        CHECK(facts.samples == 4);
        CHECK(facts.channels == 2);
        CHECK_FALSE(facts.codec_box.has_value());
    }
    SECTION("MPEG-TS") {
        mpegts::AudioTrack track;
        track.codec = mpegts::AudioCodec::kAc3;
        mpegts::MuxOptions options;
        options.profile = mpegts::BroadcastProfile::kAtsc;
        options.program_number = 3;
        options.pmt_pid = 0x0200;
        options.audio_pid = 0x0210;
        const auto file = mpegts::mux(track, units, options);
        REQUIRE(file.has_value());
        const auto result = ac3::apps::elementary_stream_from_bytes(*file);
        REQUIRE(result.error.empty());
        const auto& facts = result.container;
        CHECK(facts.kind == ContainerKind::kMpegTs);
        CHECK(ac3::apps::container_token(facts.kind) == "mpegts");
        CHECK(facts.codec_id.empty());
        CHECK(facts.track == 0x0210);
        CHECK(facts.language.empty());
        CHECK(facts.samples > 0);
        CHECK(facts.program_number == 3);
        CHECK(facts.pmt_pid == 0x0200);
        CHECK(facts.stream_type == 0x81);
        CHECK(facts.signalling == "atsc_stream_type");
        CHECK(facts.packet_size == 188);
    }
    CHECK(ac3::apps::container_token(ContainerKind::kUnknown).empty());
}

// An MP4 edit list, read as the part of the stream a player should play.

TEST_CASE("trim_from_edit_list reads the edit list an audio encoder writes",
          "[containers][edit-list]") {
    using ac3::apps::trim_from_edit_list;
    std::string note = "left over from before";

    SECTION("no edit list, or only empty edits: all of the track") {
        auto trim = trim_from_edit_list(track_with({}), note);
        CHECK(trim.start == 0);
        CHECK_FALSE(trim.length.has_value());
        CHECK(note.empty());

        trim = trim_from_edit_list(
            track_with({{.segment_duration = 480, .media_time = -1}}), note);
        CHECK(trim.start == 0);
        CHECK_FALSE(trim.length.has_value());
        CHECK(note.empty());
    }

    SECTION("one edit, counted in samples") {
        const auto trim = trim_from_edit_list(
            track_with({{.segment_duration = 44800, .media_time = 256}}), note);
        CHECK(trim.start == 256);
        REQUIRE(trim.length.has_value());
        CHECK(*trim.length == 44800);
        CHECK(note.empty());
    }

    SECTION("an empty edit ahead of it is a delay, not audio, and changes nothing") {
        const auto trim = trim_from_edit_list(
            track_with({{.segment_duration = 1000, .media_time = -1},
                        {.segment_duration = 44800, .media_time = 1024}}),
            note);
        CHECK(trim.start == 1024);
        REQUIRE(trim.length.has_value());
        CHECK(*trim.length == 44800);
    }

    SECTION("a duration in a 1000 Hz movie timescale, to the nearest sample") {
        const auto at_48k = trim_from_edit_list(
            track_with({{.segment_duration = 1234, .media_time = 256}}, 48000, 1000), note);
        REQUIRE(at_48k.length.has_value());
        CHECK(*at_48k.length == 1234 * 48);

        auto track = track_with({{.segment_duration = 7, .media_time = 0}}, 44100, 1000);
        track.sample_rate = 44100;
        const auto rounded_up = trim_from_edit_list(track, note);  // 308.7 samples
        REQUIRE(rounded_up.length.has_value());
        CHECK(*rounded_up.length == 309);
        track.edits[0].segment_duration = 3;
        const auto rounded_down = trim_from_edit_list(track, note);  // 132.3 samples
        REQUIRE(rounded_down.length.has_value());
        CHECK(*rounded_down.length == 132);
    }

    SECTION("a media timescale other than the rate") {
        const auto doubled = trim_from_edit_list(
            track_with({{.segment_duration = 48000, .media_time = 512}}, 96000), note);
        CHECK(doubled.start == 256);
        const auto ninety = trim_from_edit_list(
            track_with({{.segment_duration = 48000, .media_time = 45000}}, 90000), note);
        CHECK(ninety.start == 24000);
    }

    SECTION("a zero duration, or no movie timescale to count one in, runs to the end") {
        const auto zero = trim_from_edit_list(
            track_with({{.segment_duration = 0, .media_time = 256}}), note);
        CHECK(zero.start == 256);
        CHECK_FALSE(zero.length.has_value());

        const auto no_movie = trim_from_edit_list(
            track_with({{.segment_duration = 44800, .media_time = 256}}, 48000, 0), note);
        CHECK(no_movie.start == 256);
        CHECK_FALSE(no_movie.length.has_value());
    }

    SECTION("any other shape is not applied, and says why") {
        const auto two = trim_from_edit_list(
            track_with({{.segment_duration = 1000, .media_time = 0},
                        {.segment_duration = 1000, .media_time = 5000}}),
            note);
        CHECK(two.start == 0);
        CHECK_FALSE(two.length.has_value());
        CHECK(note.find("2 edits with audio") != std::string::npos);

        const auto fast = trim_from_edit_list(
            track_with({{.segment_duration = 1000, .media_time = 0, .media_rate = 0x00020000}}),
            note);
        CHECK(fast.start == 0);
        CHECK(note.find("another speed") != std::string::npos);

        auto no_timescale = track_with({{.segment_duration = 1000, .media_time = 256}}, 0);
        const auto unread = trim_from_edit_list(no_timescale, note);
        CHECK(unread.start == 0);
        CHECK(note.find("no timescale") != std::string::npos);
    }

    SECTION("a value no real file holds saturates rather than wrapping") {
        const auto huge = trim_from_edit_list(
            track_with({{.segment_duration = 1,
                         .media_time = std::numeric_limits<std::int64_t>::max()}},
                       1),
            note);
        CHECK(huge.start == std::numeric_limits<std::uint64_t>::max());
    }
}

TEST_CASE("elementary_stream_from_bytes reports an MP4's edit list as a trim",
          "[containers][edit-list]") {
    const auto frame = ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
    REQUIRE(frame.has_value());
    Bytes stream;
    for (int i = 0; i < 3; ++i) {
        stream.insert(stream.end(), frame->begin(), frame->end());
    }
    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->access_units.size() == 3);

    mp4::AudioTrack track;
    track.codec_id = std::string{mp4::kCodecAc3};
    track.sample_rate = 48000;
    track.channels = 2;
    track.codec_config = ac3::io::build_codec_config_box(*scanned);
    const std::span<const std::span<const std::byte>> units(scanned->access_units);

    mp4::MuxOptions edited;
    edited.edit = mp4::MuxOptions::Edit{.start_samples = 256, .duration_samples = (3 * 1536) - 356};
    const auto with_edit = mp4::mux(track, units, edited);
    REQUIRE(with_edit.has_value());
    const auto trimmed = ac3::apps::elementary_stream_from_bytes(*with_edit);
    CHECK(trimmed.error.empty());
    CHECK(trimmed.bytes == stream);
    CHECK(trimmed.trim.start == 256);
    REQUIRE(trimmed.trim.length.has_value());
    CHECK(*trimmed.trim.length == (3 * 1536) - 356);
    CHECK(trimmed.trim_note.empty());

    const auto without_edit = mp4::mux(track, units);
    REQUIRE(without_edit.has_value());
    const auto whole = ac3::apps::elementary_stream_from_bytes(*without_edit);
    CHECK(whole.bytes == stream);
    CHECK(whole.trim.start == 0);
    CHECK_FALSE(whole.trim.length.has_value());
}
