#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "matroska/matroska.hpp"
#include "matroska/reader.hpp"

// The reader's tests come in two halves, for two different questions.
//
// Round-trip (against matroska::mux/Writer) answers "does the reader
// understand what this project writes". That is the cheap half, and on its
// own it proves very little: a reader and a writer that share a
// misunderstanding round-trip perfectly.
//
// So the rest build Matroska by hand, byte by byte, out of shapes the
// writer never emits - all three lacing forms, BlockGroup-wrapped Blocks,
// several tracks, unknown-size clusters, and a pile of malformed layouts.
// Those are what a file from a disc rip or another muxer actually looks
// like, and they are the half that can fail.

namespace {

using Bytes = std::vector<std::byte>;

// --- a hand EBML writer, independent of src/matroska/src -------------------
// Deliberately NOT matroska.cpp's put_* helpers: a test that builds its
// input with the same code the reader was written against tests the pair
// against itself. These are transcribed from the EBML element layout.

void put_u8(Bytes& out, std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }

void put_id(Bytes& out, std::uint32_t id) {
    const int width = id <= 0xFF ? 1 : id <= 0xFFFF ? 2 : id <= 0xFF'FFFF ? 3 : 4;
    for (int i = width - 1; i >= 0; --i) {
        put_u8(out, static_cast<std::uint8_t>(id >> (8 * i)));
    }
}

// A size vint at an explicit width, so a test can pin the narrow forms the
// writer never chooses.
void put_vint_width(Bytes& out, std::uint64_t value, int width) {
    const auto marker = static_cast<std::uint64_t>(1) << (7 * width);
    const std::uint64_t encoded = value | marker;
    for (int i = width - 1; i >= 0; --i) {
        put_u8(out, static_cast<std::uint8_t>(encoded >> (8 * i)));
    }
}

void put_vint(Bytes& out, std::uint64_t value) {
    int width = 1;
    while (width < 8 && value >= (1ULL << (7 * width)) - 1) {
        ++width;
    }
    put_vint_width(out, value, width);
}

void put_bytes(Bytes& out, std::span<const std::byte> in) {
    out.insert(out.end(), in.begin(), in.end());
}

void put_element(Bytes& out, std::uint32_t id, std::span<const std::byte> body) {
    put_id(out, id);
    put_vint(out, body.size());
    put_bytes(out, body);
}

void put_uint_element(Bytes& out, std::uint32_t id, std::uint64_t value) {
    int width = 1;
    while (width < 8 && (value >> (8 * width)) != 0) {
        ++width;
    }
    Bytes body;
    for (int i = width - 1; i >= 0; --i) {
        put_u8(body, static_cast<std::uint8_t>(value >> (8 * i)));
    }
    put_element(out, id, body);
}

void put_f64_element(Bytes& out, std::uint32_t id, double value) {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    Bytes body;
    for (int i = 7; i >= 0; --i) {
        put_u8(body, static_cast<std::uint8_t>(bits >> (8 * i)));
    }
    put_element(out, id, body);
}

void put_f32_element(Bytes& out, std::uint32_t id, float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    Bytes body;
    for (int i = 3; i >= 0; --i) {
        put_u8(body, static_cast<std::uint8_t>(bits >> (8 * i)));
    }
    put_element(out, id, body);
}

void put_string_element(Bytes& out, std::uint32_t id, std::string_view value) {
    Bytes body;
    for (const char c : value) {
        put_u8(body, static_cast<std::uint8_t>(c));
    }
    put_element(out, id, body);
}

constexpr std::uint32_t kEbmlHeader = 0x1A45DFA3;
constexpr std::uint32_t kDocType = 0x4282;
constexpr std::uint32_t kSegment = 0x18538067;
constexpr std::uint32_t kTracks = 0x1654AE6B;
constexpr std::uint32_t kTrackEntry = 0xAE;
constexpr std::uint32_t kTrackNumber = 0xD7;
constexpr std::uint32_t kTrackType = 0x83;
constexpr std::uint32_t kCodecId = 0x86;
constexpr std::uint32_t kLanguage = 0x22B59C;
constexpr std::uint32_t kAudio = 0xE1;
constexpr std::uint32_t kSamplingFrequency = 0xB5;
constexpr std::uint32_t kChannels = 0x9F;
constexpr std::uint32_t kCluster = 0x1F43B675;
constexpr std::uint32_t kClusterTimestamp = 0xE7;
constexpr std::uint32_t kSimpleBlock = 0xA3;
constexpr std::uint32_t kBlockGroup = 0xA0;
constexpr std::uint32_t kBlock = 0xA1;
constexpr std::uint32_t kCues = 0x1C53BB6B;
constexpr std::uint32_t kAttachments = 0x1941A469;

Bytes ebml_header(std::string_view doc_type = "matroska") {
    Bytes body;
    put_string_element(body, kDocType, doc_type);
    Bytes out;
    put_element(out, kEbmlHeader, body);
    return out;
}

struct TrackSpec {
    std::uint64_t number = 1;
    std::uint64_t type = 2;  // audio
    std::string codec_id{"A_EAC3"};
    double sample_rate = 48000.0;
    std::uint64_t channels = 6;
    std::string language{"eng"};
    bool float32_rate = false;
};

Bytes track_entry(const TrackSpec& spec) {
    Bytes audio;
    if (spec.float32_rate) {
        put_f32_element(audio, kSamplingFrequency, static_cast<float>(spec.sample_rate));
    } else {
        put_f64_element(audio, kSamplingFrequency, spec.sample_rate);
    }
    put_uint_element(audio, kChannels, spec.channels);

    Bytes entry;
    put_uint_element(entry, kTrackNumber, spec.number);
    put_uint_element(entry, kTrackType, spec.type);
    put_string_element(entry, kCodecId, spec.codec_id);
    put_string_element(entry, kLanguage, spec.language);
    put_element(entry, kAudio, audio);

    Bytes out;
    put_element(out, kTrackEntry, entry);
    return out;
}

// A SimpleBlock/Block payload: track vint, int16 relative timestamp, flags,
// then whatever the caller laced together.
Bytes block_payload(std::uint64_t track, std::int16_t relative, std::uint8_t flags,
                    std::span<const std::byte> laced) {
    Bytes out;
    put_vint(out, track);
    put_u8(out, static_cast<std::uint8_t>(static_cast<std::uint16_t>(relative) >> 8));
    put_u8(out, static_cast<std::uint8_t>(static_cast<std::uint16_t>(relative) & 0xFF));
    put_u8(out, flags);
    put_bytes(out, laced);
    return out;
}

Bytes frame_of(std::size_t size, std::uint8_t fill) {
    return Bytes(size, static_cast<std::byte>(fill));
}

std::vector<std::span<const std::byte>> views_of(const std::vector<Bytes>& frames) {
    return {frames.begin(), frames.end()};
}

// Collects a demux/Reader result into owned bytes so a comparison survives
// the buffer going away.
std::vector<Bytes> owned(std::span<const std::span<const std::byte>> frames) {
    std::vector<Bytes> out;
    out.reserve(frames.size());
    for (const auto& f : frames) {
        out.emplace_back(f.begin(), f.end());
    }
    return out;
}

// Drives matroska::Reader over `file` in fixed-size chunks, returning the
// frames it produced. Chunk sizes that do not divide any element cleanly are
// the point: an element must survive being split across pushes.
std::vector<Bytes> read_in_chunks(std::span<const std::byte> file, std::size_t chunk,
                                  const matroska::ReadOptions& options = {}) {
    matroska::Reader reader{options};
    std::vector<Bytes> got;
    const auto sink = [&got](std::span<const std::byte> frame) {
        got.emplace_back(frame.begin(), frame.end());
    };
    for (std::size_t offset = 0; offset < file.size(); offset += chunk) {
        const auto take = std::min(chunk, file.size() - offset);
        REQUIRE(reader.push(file.subspan(offset, take), sink).has_value());
    }
    REQUIRE(reader.finish().has_value());
    return got;
}

}  // namespace

TEST_CASE("Matroska round-trips mux()'s frames back byte-for-byte", "[matroska][reader]") {
    const std::vector<Bytes> frames{frame_of(700, 0x11), frame_of(512, 0x22),
                                    frame_of(1024, 0x33), frame_of(64, 0x44)};
    const matroska::AudioTrack track{.codec_id = std::string{matroska::kCodecEac3},
                                     .sample_rate = 48000,
                                     .channels = 6,
                                     .samples_per_frame = 1536,
                                     .language = "eng"};
    const auto file = matroska::mux(track, views_of(frames));
    REQUIRE(file.has_value());

    const auto out = matroska::demux(*file);
    REQUIRE(out.has_value());
    CHECK(out->track.codec_id == "A_EAC3");
    CHECK(out->track.sample_rate == 48000);
    CHECK(out->track.channels == 6);
    CHECK(out->track.language == "eng");
    CHECK(out->track.track_number == 1);
    CHECK(owned(out->frames) == frames);
}

TEST_CASE("Matroska reads back a Writer's unknown-size Segment", "[matroska][reader][writer]") {
    // The streaming writer leaves Segment open-ended (EBML's reserved
    // all-ones size) and omits Duration - the one shape mux() never
    // produces, and the one a live recording always has.
    const std::vector<Bytes> frames{frame_of(300, 0xA0), frame_of(300, 0xA1),
                                    frame_of(300, 0xA2), frame_of(300, 0xA3),
                                    frame_of(300, 0xA4)};
    auto writer = matroska::Writer::create(
        matroska::AudioTrack{.codec_id = std::string{matroska::kCodecAc3},
                             .sample_rate = 44100,
                             .channels = 2,
                             .samples_per_frame = 1536,
                             .language = "und"},
        matroska::MuxOptions{.cluster_ms = 100, .writing_app = "ac3forge"});
    REQUIRE(writer.has_value());

    Bytes file = writer->header();
    for (const auto& frame : frames) {
        auto closed = writer->push(frame);
        REQUIRE(closed.has_value());
        file.insert(file.end(), closed->begin(), closed->end());
    }
    const auto tail = writer->finalize();
    file.insert(file.end(), tail.begin(), tail.end());

    const auto out = matroska::demux(file);
    REQUIRE(out.has_value());
    CHECK(out->track.codec_id == "A_AC3");
    CHECK(out->track.sample_rate == 44100);
    CHECK(out->track.channels == 2);
    CHECK(owned(out->frames) == frames);
}

TEST_CASE("Matroska Reader over arbitrary chunk boundaries matches demux()",
          "[matroska][reader]") {
    const std::vector<Bytes> frames{frame_of(700, 0x11), frame_of(3, 0x22), frame_of(1500, 0x33),
                                    frame_of(64, 0x44), frame_of(900, 0x55)};
    const auto file = matroska::mux(
        matroska::AudioTrack{.codec_id = std::string{matroska::kCodecEac3},
                             .sample_rate = 48000,
                             .channels = 6,
                             .samples_per_frame = 1536,
                             .language = "und"},
        views_of(frames), matroska::MuxOptions{.cluster_ms = 50, .writing_app = "ac3forge"});
    REQUIRE(file.has_value());

    // 1 byte at a time splits every id, every size vint and every frame;
    // the primes split them in different places again.
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{3}, std::size_t{7},
                                    std::size_t{64}, std::size_t{997}, file->size()}) {
        INFO("chunk size " << chunk);
        CHECK(read_in_chunks(*file, chunk) == frames);
    }
}

TEST_CASE("Matroska Reader reports the track and frame count it read", "[matroska][reader]") {
    const std::vector<Bytes> frames{frame_of(100, 1), frame_of(100, 2), frame_of(100, 3)};
    const auto file =
        matroska::mux(matroska::AudioTrack{.codec_id = std::string{matroska::kCodecEac3},
                                           .sample_rate = 48000,
                                           .channels = 2,
                                           .samples_per_frame = 1536,
                                           .language = "und"},
                      views_of(frames));
    REQUIRE(file.has_value());

    matroska::Reader reader{};
    CHECK_FALSE(reader.track_found());
    const auto sink = [](std::span<const std::byte>) {};
    REQUIRE(reader.push(*file, sink).has_value());
    REQUIRE(reader.finish().has_value());
    CHECK(reader.track_found());
    CHECK(reader.track().codec_id == "A_EAC3");
    CHECK(reader.frames_read() == frames.size());
}

TEST_CASE("Matroska reads every lacing form", "[matroska][reader][lacing]") {
    // matroska::mux never laces. Every one of these blocks is hand-built,
    // because a file from another muxer is where lacing actually comes from.
    const Bytes a = frame_of(5, 0xA1);
    const Bytes b = frame_of(9, 0xB2);
    const Bytes c = frame_of(4, 0xC3);

    Bytes clusters;

    SECTION("Xiph lacing") {
        Bytes laced;
        put_u8(laced, 2);  // frame count - 1
        put_u8(laced, 5);  // size of a
        put_u8(laced, 9);  // size of b; c takes the remainder
        put_bytes(laced, a);
        put_bytes(laced, b);
        put_bytes(laced, c);

        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x02, laced));
        put_element(clusters, kCluster, cluster);
    }

    SECTION("Xiph lacing with a size past 255") {
        const Bytes big = frame_of(300, 0xD4);
        Bytes laced;
        put_u8(laced, 1);    // two frames
        put_u8(laced, 255);  // 300 = 255 + 45, the 0xFF-run encoding
        put_u8(laced, 45);
        put_bytes(laced, big);
        put_bytes(laced, c);

        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x02, laced));
        put_element(clusters, kCluster, cluster);

        Bytes file = ebml_header();
        Bytes segment;
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{}));
        put_element(segment, kTracks, tracks);
        put_bytes(segment, clusters);
        put_element(file, kSegment, segment);

        const auto out = matroska::demux(file);
        REQUIRE(out.has_value());
        REQUIRE(owned(out->frames) == std::vector<Bytes>{big, c});
        return;
    }

    SECTION("EBML lacing") {
        Bytes laced;
        put_u8(laced, 2);   // three frames
        put_vint(laced, 5); // size of a
        // Signed delta to b's size: 9 - 5 = +4, biased by 2^6 - 1 at width 1.
        put_vint_width(laced, static_cast<std::uint64_t>(4 + 63), 1);
        put_bytes(laced, a);
        put_bytes(laced, b);
        put_bytes(laced, c);

        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x06, laced));
        put_element(clusters, kCluster, cluster);
    }

    SECTION("fixed-size lacing") {
        Bytes laced;
        put_u8(laced, 2);  // three frames, all 5 bytes
        put_bytes(laced, a);
        put_bytes(laced, frame_of(5, 0xB2));
        put_bytes(laced, frame_of(5, 0xC3));

        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x04, laced));
        put_element(clusters, kCluster, cluster);

        Bytes file = ebml_header();
        Bytes segment;
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{}));
        put_element(segment, kTracks, tracks);
        put_bytes(segment, clusters);
        put_element(file, kSegment, segment);

        const auto out = matroska::demux(file);
        REQUIRE(out.has_value());
        REQUIRE(owned(out->frames) ==
                std::vector<Bytes>{a, frame_of(5, 0xB2), frame_of(5, 0xC3)});
        return;
    }

    Bytes file = ebml_header();
    Bytes segment;
    Bytes tracks;
    put_bytes(tracks, track_entry(TrackSpec{}));
    put_element(segment, kTracks, tracks);
    put_bytes(segment, clusters);
    put_element(file, kSegment, segment);

    const auto out = matroska::demux(file);
    REQUIRE(out.has_value());
    CHECK(owned(out->frames) == std::vector<Bytes>{a, b, c});
}

TEST_CASE("Matroska reads a BlockGroup-wrapped Block", "[matroska][reader]") {
    // The writer only emits SimpleBlock; a muxer that wants to carry a
    // BlockDuration has to use the BlockGroup form instead, and plenty do.
    const Bytes frame = frame_of(256, 0x5A);

    Bytes group;
    put_element(group, kBlock, block_payload(1, 0, 0x00, frame));
    put_uint_element(group, 0x9B, 32);  // BlockDuration, an element to skip past

    Bytes cluster;
    put_uint_element(cluster, kClusterTimestamp, 0);
    put_element(cluster, kBlockGroup, group);

    Bytes file = ebml_header();
    Bytes segment;
    Bytes tracks;
    put_bytes(tracks, track_entry(TrackSpec{}));
    put_element(segment, kTracks, tracks);
    put_element(segment, kCluster, cluster);
    put_element(file, kSegment, segment);

    const auto out = matroska::demux(file);
    REQUIRE(out.has_value());
    REQUIRE(out->frames.size() == 1);
    CHECK(owned(out->frames)[0] == frame);
}

TEST_CASE("Matroska track selection picks the audio AC-3 track", "[matroska][reader]") {
    // Track 1 is video, track 2 is a subtitle codec on an audio track type,
    // track 3 is the E-AC-3 one. Only the last is auto-selectable, and its
    // blocks are the only ones that come out.
    Bytes tracks;
    put_bytes(tracks, track_entry(TrackSpec{.number = 1, .type = 1, .codec_id = "V_MPEG4/ISO/AVC"}));
    put_bytes(tracks, track_entry(TrackSpec{.number = 2, .type = 2, .codec_id = "A_AAC"}));
    put_bytes(tracks,
              track_entry(TrackSpec{.number = 3, .type = 2, .codec_id = "A_EAC3", .channels = 8}));

    const Bytes wanted = frame_of(200, 0x33);
    Bytes cluster;
    put_uint_element(cluster, kClusterTimestamp, 0);
    put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80, frame_of(50, 0x11)));
    put_element(cluster, kSimpleBlock, block_payload(2, 0, 0x80, frame_of(50, 0x22)));
    put_element(cluster, kSimpleBlock, block_payload(3, 0, 0x80, wanted));

    Bytes file = ebml_header();
    Bytes segment;
    put_element(segment, kTracks, tracks);
    put_element(segment, kCluster, cluster);
    put_element(file, kSegment, segment);

    SECTION("auto-selection takes the E-AC-3 track and nothing else") {
        const auto out = matroska::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->track.track_number == 3);
        CHECK(out->track.channels == 8);
        REQUIRE(out->frames.size() == 1);
        CHECK(owned(out->frames)[0] == wanted);
    }

    SECTION("an explicit track number overrides the codec filter") {
        const auto out = matroska::demux(file, matroska::ReadOptions{.track_number = 2});
        REQUIRE(out.has_value());
        CHECK(out->track.codec_id == "A_AAC");
        REQUIRE(out->frames.size() == 1);
        CHECK(owned(out->frames)[0] == frame_of(50, 0x22));
    }

    SECTION("a track number nothing matches is kNoAudioTrack") {
        const auto out = matroska::demux(file, matroska::ReadOptions{.track_number = 99});
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kNoAudioTrack);
    }
}

TEST_CASE("Matroska reads a 32-bit SamplingFrequency and a defaulted track", "[matroska][reader]") {
    SECTION("a float32 sample rate") {
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{.sample_rate = 32000.0, .float32_rate = true}));
        Bytes file = ebml_header();
        Bytes segment;
        put_element(segment, kTracks, tracks);
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80, frame_of(8, 0x01)));
        put_element(segment, kCluster, cluster);
        put_element(file, kSegment, segment);

        const auto out = matroska::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->track.sample_rate == 32000);
    }

    SECTION("an Audio element omitted entirely keeps Matroska's own defaults") {
        Bytes entry;
        put_uint_element(entry, kTrackNumber, 1);
        put_uint_element(entry, kTrackType, 2);
        put_string_element(entry, kCodecId, "A_AC3");
        Bytes tracks;
        put_element(tracks, kTrackEntry, entry);

        Bytes file = ebml_header();
        Bytes segment;
        put_element(segment, kTracks, tracks);
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80, frame_of(8, 0x01)));
        put_element(segment, kCluster, cluster);
        put_element(file, kSegment, segment);

        const auto out = matroska::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->track.sample_rate == 8000);
        CHECK(out->track.channels == 1);
        CHECK(out->track.language == "und");
    }
}

TEST_CASE("Matroska skips elements it has no use for", "[matroska][reader]") {
    // A Cues index and an Attachment sit between the tracks and the audio in
    // any real file. Neither is ever buffered - the reader counts past them.
    Bytes file = ebml_header();
    Bytes segment;
    Bytes tracks;
    put_bytes(tracks, track_entry(TrackSpec{}));
    put_element(segment, kTracks, tracks);
    put_element(segment, kAttachments, frame_of(4096, 0xEE));
    put_element(segment, kCues, frame_of(2048, 0xCC));

    const Bytes frame = frame_of(128, 0x77);
    Bytes cluster;
    put_uint_element(cluster, kClusterTimestamp, 0);
    put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80, frame));
    put_element(segment, kCluster, cluster);
    put_element(file, kSegment, segment);

    const auto out = matroska::demux(file);
    REQUIRE(out.has_value());
    REQUIRE(out->frames.size() == 1);
    CHECK(owned(out->frames)[0] == frame);

    // And the same across chunk boundaries, where a skip spans pushes.
    CHECK(read_in_chunks(file, 100) == std::vector<Bytes>{frame});
}

TEST_CASE("Matroska reads an unknown-size Cluster ended by its sibling", "[matroska][reader]") {
    // A live muxer may leave the Cluster open too, not just the Segment.
    // Nothing declares where it ends; the next Segment-level id does.
    const Bytes first = frame_of(64, 0x01);
    const Bytes second = frame_of(64, 0x02);

    Bytes file = ebml_header();
    Bytes segment;
    Bytes tracks;
    put_bytes(tracks, track_entry(TrackSpec{}));
    put_element(segment, kTracks, tracks);

    put_id(segment, kCluster);
    put_vint_width(segment, (std::uint64_t{1} << 56) - 1, 8);  // unknown size
    put_uint_element(segment, kClusterTimestamp, 0);
    put_element(segment, kSimpleBlock, block_payload(1, 0, 0x80, first));

    put_id(segment, kCluster);
    put_vint_width(segment, (std::uint64_t{1} << 56) - 1, 8);
    put_uint_element(segment, kClusterTimestamp, 1000);
    put_element(segment, kSimpleBlock, block_payload(1, 0, 0x80, second));

    put_element(file, kSegment, segment);

    const auto out = matroska::demux(file);
    REQUIRE(out.has_value());
    CHECK(owned(out->frames) == std::vector<Bytes>{first, second});
    CHECK(read_in_chunks(file, 5) == std::vector<Bytes>{first, second});
}

TEST_CASE("Matroska truncated mid-cluster keeps the frames before the cut", "[matroska][reader]") {
    // How a live recording actually ends: the process stopped. Everything
    // written before the cut is real audio and is returned; only a cut
    // before the track was ever described has nothing to give back.
    const std::vector<Bytes> frames{frame_of(400, 0x11), frame_of(400, 0x22),
                                    frame_of(400, 0x33)};
    const auto complete =
        matroska::mux(matroska::AudioTrack{.codec_id = std::string{matroska::kCodecEac3},
                                           .sample_rate = 48000,
                                           .channels = 2,
                                           .samples_per_frame = 1536,
                                           .language = "und"},
                      views_of(frames), matroska::MuxOptions{.cluster_ms = 30});
    REQUIRE(complete.has_value());

    SECTION("cut after some frames") {
        const Bytes cut{complete->begin(), complete->end() - 300};
        const auto out = matroska::demux(cut);
        REQUIRE(out.has_value());
        CHECK(out->frames.size() < frames.size());
        for (std::size_t i = 0; i < out->frames.size(); ++i) {
            CHECK(owned(out->frames)[i] == frames[i]);
        }
    }

    SECTION("cut before the track is described") {
        const Bytes cut{complete->begin(), complete->begin() + 30};
        const auto out = matroska::demux(cut);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kTruncated);
    }
}

TEST_CASE("Matroska refuses what is not Matroska", "[matroska][reader]") {
    SECTION("empty input") {
        const auto out = matroska::demux({});
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kNotMatroska);
    }

    SECTION("an MP4 file's ftyp") {
        const Bytes ftyp{std::byte{0}, std::byte{0},   std::byte{0},   std::byte{0x18},
                         std::byte{'f'}, std::byte{'t'}, std::byte{'y'}, std::byte{'p'}};
        const auto out = matroska::demux(ftyp);
        REQUIRE_FALSE(out.has_value());
        // A leading 0x00 has no EBML id marker at all.
        CHECK(out.error() == matroska::DemuxError::kMalformed);
    }

    SECTION("a valid element that is not the EBML header") {
        Bytes file;
        put_element(file, kSegment, {});
        const auto out = matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kNotMatroska);
    }

    SECTION("an EBML header but no tracks") {
        const auto file = ebml_header();
        const auto out = matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kTruncated);
    }

    SECTION("tracks that hold nothing selectable") {
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{.type = 1, .codec_id = "V_VP9"}));
        Bytes file = ebml_header();
        Bytes segment;
        put_element(segment, kTracks, tracks);
        put_element(file, kSegment, segment);
        const auto out = matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kNoAudioTrack);
    }
}

TEST_CASE("Matroska rejects malformed and hostile layouts", "[matroska][reader]") {
    const auto with_cluster = [](const Bytes& cluster_body) {
        Bytes file = ebml_header();
        Bytes segment;
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{}));
        put_element(segment, kTracks, tracks);
        put_element(segment, kCluster, cluster_body);
        put_element(file, kSegment, segment);
        return file;
    };

    SECTION("a Xiph lace whose declared sizes overrun the block") {
        Bytes laced;
        put_u8(laced, 1);    // two frames
        put_u8(laced, 200);  // but only 10 bytes follow
        put_bytes(laced, frame_of(10, 0xFF));
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x02, laced));
        const auto out = matroska::demux(with_cluster(cluster));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kMalformed);
    }

    SECTION("a fixed-size lace that does not divide evenly") {
        Bytes laced;
        put_u8(laced, 2);  // three frames out of 10 bytes
        put_bytes(laced, frame_of(10, 0xFF));
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x04, laced));
        const auto out = matroska::demux(with_cluster(cluster));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kMalformed);
    }

    SECTION("an EBML lace whose delta drives a size negative") {
        Bytes laced;
        put_u8(laced, 2);    // three frames
        put_vint(laced, 4);  // first is 4 bytes
        put_vint_width(laced, 0, 1);  // delta of -63, taking the next size below zero
        put_bytes(laced, frame_of(20, 0xFF));
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, block_payload(1, 0, 0x80 | 0x06, laced));
        const auto out = matroska::demux(with_cluster(cluster));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kMalformed);
    }

    SECTION("a block too short to hold its own header") {
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        put_element(cluster, kSimpleBlock, frame_of(2, 0x81));
        const auto out = matroska::demux(with_cluster(cluster));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kMalformed);
    }

    SECTION("a wanted leaf claiming more bytes than the limit allows") {
        Bytes file = ebml_header();
        Bytes segment;
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{}));
        put_element(segment, kTracks, tracks);
        Bytes cluster;
        put_uint_element(cluster, kClusterTimestamp, 0);
        // A SimpleBlock header declaring 4 GiB, with nothing behind it.
        put_id(cluster, kSimpleBlock);
        put_vint(cluster, 4ULL << 30);
        put_element(segment, kCluster, cluster);
        put_element(file, kSegment, segment);
        const auto out = matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kLimitExceeded);
    }

    SECTION("nesting past max_depth") {
        Bytes file = ebml_header();
        // BlockGroup is a descended master, so a stack of them is the
        // cheapest way for an input to ask for unbounded depth.
        Bytes inner;
        for (int i = 0; i < 40; ++i) {
            Bytes wrapped;
            put_element(wrapped, kBlockGroup, inner);
            inner = wrapped;
        }
        Bytes segment;
        Bytes tracks;
        put_bytes(tracks, track_entry(TrackSpec{}));
        put_element(segment, kTracks, tracks);
        put_element(segment, kCluster, inner);
        put_element(file, kSegment, segment);
        const auto out = matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kLimitExceeded);
    }

    SECTION("an unknown size on an element that may not have one") {
        Bytes file = ebml_header();
        Bytes segment;
        put_id(segment, kTracks);
        put_vint_width(segment, (std::uint64_t{1} << 56) - 1, 8);
        put_element(file, kSegment, segment);
        const auto out = matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kMalformed);
    }

    SECTION("a Channels value no layout could mean") {
        Bytes audio;
        put_uint_element(audio, kChannels, 100000);
        Bytes entry;
        put_uint_element(entry, kTrackNumber, 1);
        put_uint_element(entry, kTrackType, 2);
        put_string_element(entry, kCodecId, "A_EAC3");
        put_element(entry, kAudio, audio);
        Bytes tracks;
        put_element(tracks, kTrackEntry, entry);
        Bytes file = ebml_header();
        Bytes segment;
        put_element(segment, kTracks, tracks);
        put_element(file, kSegment, segment);
        const auto out = matroska::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == matroska::DemuxError::kMalformed);
    }
}

TEST_CASE("Matroska describe() names every demux error", "[matroska][reader]") {
    for (const auto error :
         {matroska::DemuxError::kNotMatroska, matroska::DemuxError::kTruncated,
          matroska::DemuxError::kMalformed, matroska::DemuxError::kNoAudioTrack,
          matroska::DemuxError::kLimitExceeded}) {
        CHECK_FALSE(matroska::describe(error).empty());
        CHECK(matroska::describe(error) != "unknown error");
    }
}
