#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "mp4/dash.hpp"
#include "mp4/hls.hpp"
#include "mp4/mp4.hpp"

// fragment()'s ISOBMFF output is read back with its own independent box
// walker below, extended from test_mp4.cpp's (moof/traf/tfhd/tfdt/trun added
// for the fragmentation boxes mux() never writes) rather than shared with
// it - the same "an independent reader, not a self-check" reasoning
// test_mp4.cpp's own header comment gives. hls.cpp/dash.cpp's plain-text
// output is asserted against directly, since there is no bitstream
// arithmetic in a manifest string to independently re-derive.

namespace {

using Bytes = std::vector<std::byte>;

[[nodiscard]] std::uint8_t byte_at(std::span<const std::byte> data, std::size_t offset) {
    return std::to_integer<std::uint8_t>(data[offset]);
}

[[nodiscard]] std::uint32_t u32_at(std::span<const std::byte> data, std::size_t offset) {
    return (static_cast<std::uint32_t>(byte_at(data, offset)) << 24) |
           (static_cast<std::uint32_t>(byte_at(data, offset + 1)) << 16) |
           (static_cast<std::uint32_t>(byte_at(data, offset + 2)) << 8) |
           static_cast<std::uint32_t>(byte_at(data, offset + 3));
}

[[nodiscard]] std::uint64_t u64_at(std::span<const std::byte> data, std::size_t offset) {
    return (static_cast<std::uint64_t>(u32_at(data, offset)) << 32) | u32_at(data, offset + 4);
}

[[nodiscard]] std::string fourcc_at(std::span<const std::byte> data, std::size_t offset) {
    std::string s(4, '\0');
    for (std::size_t i = 0; i < 4; ++i) {
        s[i] = static_cast<char>(byte_at(data, offset + i));
    }
    return s;
}

// One ISOBMFF box: size(4) + type(4) + body. `payload`/`length` describe the
// body only (offset past the 8-byte header, size - 8).
struct Element {
    std::string type;
    std::size_t payload = 0;
    std::uint64_t length = 0;
};

// Everything this file's own box walker needs to recurse into - mp4.cpp's
// plain containers, plus moof/traf/mvex, which mux() never writes and
// test_mp4.cpp's own list therefore has no reason to include.
bool is_container(const std::string& type) {
    return type == "moov" || type == "trak" || type == "mdia" || type == "minf" || type == "stbl" ||
           type == "dinf" || type == "mvex" || type == "moof" || type == "traf";
}

void walk(std::span<const std::byte> file, std::size_t pos, std::size_t end,
          std::vector<Element>& out) {
    while (pos < end) {
        REQUIRE(pos + 8 <= end);
        const auto size = u32_at(file, pos);
        const auto type = fourcc_at(file, pos + 4);
        REQUIRE(size >= 8);
        REQUIRE(pos + size <= end);
        out.push_back({type, pos + 8, size - 8});
        if (is_container(type)) {
            walk(file, pos + 8, pos + size, out);
        }
        pos += size;
    }
    REQUIRE(pos == end);
}

std::vector<Element> parse(std::span<const std::byte> file) {
    std::vector<Element> out;
    walk(file, 0, file.size(), out);
    return out;
}

const Element* find(const std::vector<Element>& elements, const std::string& type) {
    for (const auto& e : elements) {
        if (e.type == type) {
            return &e;
        }
    }
    return nullptr;
}

std::size_t count(const std::vector<Element>& elements, const std::string& type) {
    std::size_t n = 0;
    for (const auto& e : elements) {
        if (e.type == type) {
            ++n;
        }
    }
    return n;
}

// ftyp/styp body: major_brand(4), minor_version(4), compatible_brands[](4
// each) - ISO/IEC 14496-12 §4.3/§8.16.2.
struct BrandBox {
    std::string major_brand;
    std::vector<std::string> compatible_brands;
};

BrandBox read_brand_box(std::span<const std::byte> file, const Element& box) {
    BrandBox out;
    out.major_brand = fourcc_at(file, box.payload);
    for (std::size_t off = box.payload + 8; off + 4 <= box.payload + box.length; off += 4) {
        out.compatible_brands.push_back(fourcc_at(file, off));
    }
    return out;
}

bool has_brand(const BrandBox& box, std::string_view brand) {
    return std::find(box.compatible_brands.begin(), box.compatible_brands.end(), brand) !=
           box.compatible_brands.end();
}

// trex body (ISO/IEC 14496-12 §8.8.3): version+flags(4), track_ID(4),
// default_sample_description_index(4), default_sample_duration(4),
// default_sample_size(4), default_sample_flags(4).
struct TrexInfo {
    std::uint32_t track_id = 0;
    std::uint32_t default_sample_duration = 0;
    std::uint32_t default_sample_size = 0;
    std::uint32_t default_sample_flags = 0;
};

TrexInfo read_trex(std::span<const std::byte> file, const Element& trex) {
    // payload+0: version+flags, +4: track_ID, +8: default_sample_description_index,
    // +12: default_sample_duration, +16: default_sample_size, +20: default_sample_flags.
    return {.track_id = u32_at(file, trex.payload + 4),
            .default_sample_duration = u32_at(file, trex.payload + 12),
            .default_sample_size = u32_at(file, trex.payload + 16),
            .default_sample_flags = u32_at(file, trex.payload + 20)};
}

// tfhd body (ISO/IEC 14496-12 §8.8.7): version+flags(4), track_ID(4). No
// optional field is present in what fragment() writes (flags carry only
// default-base-is-moof), so track_ID is always the next 4 bytes.
std::uint32_t read_tfhd_track_id(std::span<const std::byte> file, const Element& tfhd) {
    return u32_at(file, tfhd.payload + 4);
}

// tfhd's own flags live in the FullBox header (bytes 1-3 of the box, after
// the 1-byte version) - ISO/IEC 14496-12 §4.2.
std::uint32_t read_fullbox_flags(std::span<const std::byte> file, const Element& box) {
    return u32_at(file, box.payload) & 0x00FFFFFFU;
}

// tfdt body (ISO/IEC 14496-12 §8.8.12), version 1: version+flags(4),
// baseMediaDecodeTime(8).
std::uint64_t read_tfdt(std::span<const std::byte> file, const Element& tfdt) {
    return u64_at(file, tfdt.payload + 4);
}

// trun body (ISO/IEC 14496-12 §8.8.8), flags data-offset-present |
// sample-size-present only (what fragment() writes): version+flags(4),
// sample_count(4), data_offset(4), [sample_size(4)]*sample_count.
struct TrunInfo {
    std::uint32_t sample_count = 0;
    std::int32_t data_offset = 0;
    std::vector<std::uint32_t> sample_sizes;
};

TrunInfo read_trun(std::span<const std::byte> file, const Element& trun) {
    TrunInfo out;
    out.sample_count = u32_at(file, trun.payload + 4);
    out.data_offset = static_cast<std::int32_t>(u32_at(file, trun.payload + 8));
    out.sample_sizes.resize(out.sample_count);
    for (std::uint32_t i = 0; i < out.sample_count; ++i) {
        out.sample_sizes[i] = u32_at(file, trun.payload + 12 + static_cast<std::size_t>(i) * 4);
    }
    return out;
}

mp4::AudioTrack sample_track(int channels = 6) {
    return mp4::AudioTrack{
        .codec_id = std::string{mp4::kCodecEac3},
        .sample_rate = 48000,
        .channels = channels,
        .samples_per_frame = 1536,
        .codec_config = Bytes{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}}};
}

// A real, multi-frame 5.1 E-AC-3 elementary stream - never silence, never
// frame 0 alone (CONTRIBUTING.md's validation discipline): distinct tones
// per channel so a channel-order or frame-boundary bug cannot hide behind
// identical content. kFrames=10 at kFramesPerFragment=3 spans FOUR
// fragments (3, 3, 3, 1) - enough for a full fragment, a fragment boundary,
// and a shorter-than-nominal final fragment all in the same fixture.
constexpr int kFrames = 10;
constexpr std::uint32_t kFramesPerFragment = 3;

std::vector<Bytes> encode_real_eac3_frames() {
    using ac3::eac3::AccessUnitConfig;
    const AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = ac3::Acmod::k3_2, .lfe = true}};
    ac3::eac3::AccessUnitEncoder encoder{config};

    std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame));
    constexpr std::array<double, 6> kTones{440.0, 660.0, 880.0, 1100.0, 1320.0, 55.0};
    std::vector<Bytes> frames;
    for (int f = 0; f < kFrames; ++f) {
        for (std::size_t ch = 0; ch < 6; ++ch) {
            for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
                const double t = static_cast<double>(f * ac3::kSamplesPerFrame + n) / 48000.0;
                pcm[ch][static_cast<std::size_t>(n)] =
                    static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * kTones[ch] * t));
            }
        }
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        frames.emplace_back(unit->bytes.begin(), unit->bytes.end());
    }
    return frames;
}

struct RealFixture {
    mp4::AudioTrack track;
    std::vector<Bytes> frames;
    mp4::FragmentedOutput fragmented;
};

RealFixture make_real_fixture() {
    const auto frames = encode_real_eac3_frames();
    Bytes stream;
    for (const auto& f : frames) {
        stream.insert(stream.end(), f.begin(), f.end());
    }
    const auto scanned = ac3::io::scan(stream);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->kind == ac3::io::StreamKind::kEac3);

    const mp4::AudioTrack track{
        .codec_id = std::string{mp4::kCodecEac3},
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = ac3::kSamplesPerFrame,
        .codec_config = ac3::io::build_codec_config_box(*scanned),
    };

    const auto result = mp4::fragment(
        track, frames, mp4::FragmentOptions{.frames_per_fragment = kFramesPerFragment});
    REQUIRE(result.has_value());
    return RealFixture{.track = track, .frames = frames, .fragmented = *result};
}

}  // namespace

TEST_CASE("fragment() init segment is well-formed with mvex/trex and an empty sample table",
          "[fmp4]") {
    const auto fixture = make_real_fixture();
    const auto elements = parse(fixture.fragmented.init_segment);

    const auto* ftyp = find(elements, "ftyp");
    REQUIRE(ftyp != nullptr);
    const auto brands = read_brand_box(fixture.fragmented.init_segment, *ftyp);
    CHECK(brands.major_brand == "iso5");
    CHECK(has_brand(brands, "cmfc"));

    REQUIRE(find(elements, "moov") != nullptr);
    REQUIRE(find(elements, "trak") != nullptr);
    REQUIRE(find(elements, "stsd") != nullptr);
    CHECK(count(elements, "trak") == 1);

    // The sample tables exist (ISO/IEC 14496-12 mandates their presence in
    // stbl) but describe ZERO samples - every real sample lives in a moof
    // instead. entry_count sits right after each table's 4-byte
    // version+flags header.
    for (const char* box_cstr : {"stts", "stsc", "stsz", "stco"}) {
        const std::string box{box_cstr};
        const auto* elem = find(elements, box);
        REQUIRE(elem != nullptr);
        const auto entry_count_offset = box == "stsz" ? elem->payload + 8 : elem->payload + 4;
        CHECK(u32_at(fixture.fragmented.init_segment, entry_count_offset) == 0);
    }

    const auto* mvex = find(elements, "mvex");
    REQUIRE(mvex != nullptr);
    const auto* trex = find(elements, "trex");
    REQUIRE(trex != nullptr);
    const auto trex_info = read_trex(fixture.fragmented.init_segment, *trex);
    CHECK(trex_info.track_id == 1);
    CHECK(trex_info.default_sample_duration == ac3::kSamplesPerFrame);
    CHECK(trex_info.default_sample_size == 0);
    // sample_depends_on=2 ("does not depend on others"), every other field
    // (incl. sample_is_non_sync_sample) clear - see fragment.cpp's own
    // comment on build_trex.
    CHECK(trex_info.default_sample_flags == 0x02000000U);
}

TEST_CASE("fragment() groups frames into fragments with correct sequence numbers and durations",
          "[fmp4]") {
    const auto fixture = make_real_fixture();
    const auto& segments = fixture.fragmented.media_segments;

    // 10 frames at 3/fragment: 3, 3, 3, 1.
    REQUIRE(segments.size() == 4);
    const std::array<std::uint32_t, 4> expected_counts{3, 3, 3, 1};
    std::uint64_t expected_base_time = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        CAPTURE(i);
        CHECK(segments[i].sequence_number == i + 1);
        CHECK(segments[i].sample_count == expected_counts[i]);
        CHECK(segments[i].duration_samples ==
              static_cast<std::uint64_t>(expected_counts[i]) * ac3::kSamplesPerFrame);

        const auto elements = parse(segments[i].bytes);
        const auto* styp = find(elements, "styp");
        REQUIRE(styp != nullptr);
        const auto brands = read_brand_box(segments[i].bytes, *styp);
        CHECK(brands.major_brand == "iso5");
        CHECK(has_brand(brands, "cmfc"));

        const auto* mfhd = find(elements, "mfhd");
        REQUIRE(mfhd != nullptr);
        // mfhd body: version+flags(4), sequence_number(4).
        CHECK(u32_at(segments[i].bytes, mfhd->payload + 4) == i + 1);

        const auto* tfhd = find(elements, "tfhd");
        REQUIRE(tfhd != nullptr);
        CHECK(read_tfhd_track_id(segments[i].bytes, *tfhd) == 1);
        CHECK(read_fullbox_flags(segments[i].bytes, *tfhd) == 0x020000U);  // default-base-is-moof

        const auto* tfdt = find(elements, "tfdt");
        REQUIRE(tfdt != nullptr);
        CHECK(read_tfdt(segments[i].bytes, *tfdt) == expected_base_time);
        expected_base_time += segments[i].duration_samples;

        const auto* mdat = find(elements, "mdat");
        REQUIRE(mdat != nullptr);
    }
}

TEST_CASE("fragment() trun sample sizes and mdat bytes reproduce the real encoded frames exactly",
          "[fmp4]") {
    // This is the check that actually exercises default-base-is-moof's
    // arithmetic: trun's data_offset must land EXACTLY on the first sample
    // byte in the mdat that follows moof, for every fragment, or a real
    // player reads garbage.
    const auto fixture = make_real_fixture();
    const auto& segments = fixture.fragmented.media_segments;
    std::size_t frame_cursor = 0;

    for (const auto& segment : segments) {
        const auto elements = parse(segment.bytes);
        const auto* moof = find(elements, "moof");
        const auto* trun = find(elements, "trun");
        const auto* mdat = find(elements, "mdat");
        REQUIRE(moof != nullptr);
        REQUIRE(trun != nullptr);
        REQUIRE(mdat != nullptr);

        const auto trun_info = read_trun(segment.bytes, *trun);
        REQUIRE(trun_info.sample_count == segment.sample_count);

        // moof's box starts at file offset 0 within a media segment ONLY if
        // there were nothing before it - but styp precedes moof here, so
        // "the start of moof" for data_offset purposes is moof's own
        // element position (8 bytes before its payload, for its own
        // size+type header), not byte 0 of the segment.
        const std::size_t moof_start = moof->payload - 8;
        std::size_t cursor = moof_start + static_cast<std::size_t>(trun_info.data_offset);
        REQUIRE(cursor == mdat->payload);  // data_offset lands exactly on the first sample byte

        for (std::uint32_t i = 0; i < trun_info.sample_count; ++i) {
            REQUIRE(frame_cursor < fixture.frames.size());
            const auto& real_frame = fixture.frames[frame_cursor];
            CHECK(trun_info.sample_sizes[i] == real_frame.size());
            REQUIRE(cursor + real_frame.size() <= segment.bytes.size());
            const std::span<const std::byte> at{segment.bytes.data() + cursor, real_frame.size()};
            CHECK(std::equal(at.begin(), at.end(), real_frame.begin(), real_frame.end()));
            cursor += real_frame.size();
            ++frame_cursor;
        }
    }
    CHECK(frame_cursor == fixture.frames.size());
}

TEST_CASE("fragment() rejects what it cannot describe", "[fmp4]") {
    const std::vector<Bytes> one{Bytes(16, std::byte{0})};
    const auto track = sample_track();

    // Explicit empty span: bare {} became ambiguous when the span-of-views
    // fragment overload arrived alongside the owned-list one.
    CHECK(mp4::fragment(track, std::span<const Bytes>{}).error() == mp4::MuxError::kNoFrames);

    auto bad_channels = track;
    bad_channels.channels = 0;
    CHECK(mp4::fragment(bad_channels, one).error() == mp4::MuxError::kInvalidTrack);

    auto bad_codec = track;
    bad_codec.codec_id = "mp4a";
    CHECK(mp4::fragment(bad_codec, one).error() == mp4::MuxError::kInvalidTrack);

    CHECK(mp4::fragment(track, one, mp4::FragmentOptions{.frames_per_fragment = 0}).error() ==
          mp4::MuxError::kInvalidOptions);
}

// --- HLS ---------------------------------------------------------------

TEST_CASE("hls_codec_string is RFC 6381's bare sample-entry fourcc for both codecs", "[hls]") {
    auto track = sample_track();
    track.codec_id = std::string{mp4::kCodecEac3};
    CHECK(mp4::hls_codec_string(track) == "ec-3");
    track.codec_id = std::string{mp4::kCodecAc3};
    CHECK(mp4::hls_codec_string(track) == "ac-3");
}

TEST_CASE("HLS media playlist lists every fragment in order with EXT-X-MAP and correct timing",
          "[hls]") {
    const auto fixture = make_real_fixture();
    const auto playlist = mp4::build_hls_media_playlist(
        fixture.track, fixture.fragmented.media_segments, mp4::HlsOptions{});

    CHECK(playlist.starts_with("#EXTM3U\n"));
    CHECK(playlist.find("#EXT-X-VERSION:7\n") != std::string::npos);
    CHECK(playlist.find("#EXT-X-MAP:URI=\"init.mp4\"\n") != std::string::npos);
    CHECK(playlist.find("#EXT-X-PLAYLIST-TYPE:VOD\n") != std::string::npos);
    CHECK(playlist.find("#EXT-X-ENDLIST\n") != std::string::npos);

    // Every fragment's #EXTINF/URI pair, in order - independently recomputed
    // from the SAME segments the playlist was built from, not copy-pasted
    // from hls.cpp's own formula.
    for (const auto& segment : fixture.fragmented.media_segments) {
        const auto expected_uri = std::format("segment{}.m4s", segment.sequence_number);
        CHECK(playlist.find(expected_uri) != std::string::npos);
    }
    // TARGETDURATION must be an integer >= every #EXTINF value (RFC 8216
    // §4.3.3.1) - the longest fragment here is a full one, 3*1536/48000 s.
    const double max_seconds = 3.0 * static_cast<double>(ac3::kSamplesPerFrame) /
                               static_cast<double>(fixture.track.sample_rate);
    const auto target = static_cast<std::uint64_t>(std::ceil(max_seconds));
    CHECK(playlist.find(std::format("#EXT-X-TARGETDURATION:{}\n", target)) != std::string::npos);
}

TEST_CASE("HLS master playlist signals CODECS and CHANNELS correctly", "[hls]") {
    const auto fixture = make_real_fixture();

    SECTION("plain E-AC-3: CHANNELS defaults to the track's channel count") {
        const auto master = mp4::build_hls_master_playlist(
            fixture.track, fixture.fragmented.media_segments, "audio.m3u8", mp4::HlsOptions{});
        CHECK(master.find("CODECS=\"ec-3\"") != std::string::npos);
        CHECK(master.find(std::format("CHANNELS=\"{}\"", fixture.track.channels)) !=
              std::string::npos);
        // Audio-only content self-references: the URI after EXT-X-STREAM-INF
        // is the same media playlist EXT-X-MEDIA already names (see
        // hls.cpp's own comment) - both must appear, and the line right
        // after EXT-X-STREAM-INF must BE that URI.
        const auto stream_inf_pos = master.find("#EXT-X-STREAM-INF:");
        REQUIRE(stream_inf_pos != std::string::npos);
        const auto next_line = master.find('\n', stream_inf_pos);
        REQUIRE(next_line != std::string::npos);
        const auto uri_end = master.find('\n', next_line + 1);
        CHECK(master.substr(next_line + 1, uri_end - next_line - 1) == "audio.m3u8");
    }

    SECTION("Dolby Atmos: CHANNELS carries the caller-supplied </JOC> form") {
        // mp4:: never reads TS 103 420 object-layer syntax itself (see
        // hls.hpp's own comment) - the caller (which DOES know
        // oba_complexity_index) supplies the exact string.
        const auto master = mp4::build_hls_master_playlist(
            fixture.track, fixture.fragmented.media_segments, "audio.m3u8",
            mp4::HlsOptions{.channels_attribute = "12/JOC"});
        CHECK(master.find("CODECS=\"ec-3\"") != std::string::npos);
        CHECK(master.find("CHANNELS=\"12/JOC\"") != std::string::npos);
    }
}

// --- DASH ----------------------------------------------------------------

TEST_CASE("DASH adaptation set snippet carries the correct codecs, timescale and segment template",
          "[dash]") {
    const auto fixture = make_real_fixture();
    const auto snippet = mp4::build_dash_adaptation_set(
        fixture.track, fixture.fragmented.media_segments, mp4::DashOptions{});

    CHECK(snippet.find("<AdaptationSet") != std::string::npos);
    CHECK(snippet.find("</AdaptationSet>") != std::string::npos);
    CHECK(snippet.find("mimeType=\"audio/mp4\"") != std::string::npos);
    CHECK(snippet.find("codecs=\"ec-3\"") != std::string::npos);
    CHECK(snippet.find(std::format("audioSamplingRate=\"{}\"", fixture.track.sample_rate)) !=
          std::string::npos);
    CHECK(snippet.find(std::format("timescale=\"{}\"", fixture.track.sample_rate)) !=
          std::string::npos);
    CHECK(snippet.find("initialization=\"init.mp4\"") != std::string::npos);
    CHECK(snippet.find("media=\"segment$Number$.m4s\"") != std::string::npos);

    // 10 frames at 3/fragment (the fixture's own kFramesPerFragment): three
    // full 3-frame fragments (4608 samples each) then one short 1-frame
    // fragment (1536) - exactly the run-length pattern a SegmentTimeline
    // should encode as "<S d="4608" r="2"/>" (3 occurrences: r is the
    // repeat count AFTER the first) followed by "<S d="1536"/>" (a single,
    // un-repeated final entry). Independently recomputed from the segments
    // themselves, not copied from dash.cpp's own formula.
    CHECK(snippet.find("<SegmentTimeline>") != std::string::npos);
    CHECK(snippet.find("</SegmentTimeline>") != std::string::npos);
    // The FIRST entry carries @t, the timeline's own start on the track's
    // timeline - 0 here, since these segments are the whole track from its
    // beginning. build_dash_adaptation_set writes it rather than relying on
    // @t's default so that a manifest describing a rolling live WINDOW is
    // correct too (see the rolling-window test further down).
    CHECK(snippet.find("<S t=\"0\" d=\"4608\" r=\"2\"/>") != std::string::npos);
    CHECK(snippet.find("<S d=\"1536\"/>") != std::string::npos);
    // A flat, nominal `duration` attribute is exactly what this test's own
    // sibling regression (see the bug reintroduced further down) is about -
    // it must NOT appear on SegmentTemplate itself once SegmentTimeline is
    // present.
    CHECK(snippet.find("SegmentTemplate timescale=\"48000\" initialization=\"init.mp4\" "
                       "media=\"segment$Number$.m4s\" startNumber=\"1\">") != std::string::npos);

    // Every Representation states its channel configuration. With no Dolby
    // channel map supplied, that is the OTHER scheme DASH-IF IOP Part 8
    // v5.0.0 §5.3.2 allows for E-AC-3 - ISO/IEC 23091-3's CICP
    // ChannelConfiguration, the one TS 103 420 §D.2.3's own example MPD
    // writes. No JOC descriptors: this fixture's stream carries no object
    // layer, and signalling one that is not there would be worse than
    // signalling nothing.
    CHECK(snippet.find("<AudioChannelConfiguration "
                       "schemeIdUri=\"urn:mpeg:mpegB:cicp:ChannelConfiguration\" "
                       "value=\"6\"/>") != std::string::npos);
    CHECK(snippet.find("EC3_ExtensionType") == std::string::npos);
    CHECK(snippet.find("EC3_ExtensionComplexityIndex") == std::string::npos);

    // Balanced enough to be worth writing to a file: every opening tag this
    // snippet introduces has a matching close.
    CHECK(std::ranges::count(snippet, '<') > 0);
    const auto open_representation = snippet.find("<Representation ");
    const auto close_representation = snippet.find("</Representation>");
    REQUIRE(open_representation != std::string::npos);
    REQUIRE(close_representation != std::string::npos);
    CHECK(open_representation < close_representation);
    const auto open_template = snippet.find("<SegmentTemplate ");
    const auto close_template = snippet.find("</SegmentTemplate>");
    REQUIRE(open_template != std::string::npos);
    REQUIRE(close_template != std::string::npos);
    CHECK(open_template < close_template);
    CHECK(close_template < close_representation);
}

TEST_CASE("DASH adaptation set's SegmentTimeline exactly reproduces varied segment durations",
          "[dash]") {
    // A more demanding duration pattern than the shared fixture's
    // "uniform-except-last" one: run-length encoding must not assume only
    // two runs ever occur.
    const mp4::AudioTrack track = sample_track();
    const std::vector<mp4::MediaSegment> segments{
        mp4::MediaSegment{
            .bytes = {}, .sequence_number = 1, .sample_count = 2, .duration_samples = 3072},
        mp4::MediaSegment{
            .bytes = {}, .sequence_number = 2, .sample_count = 2, .duration_samples = 3072},
        mp4::MediaSegment{
            .bytes = {}, .sequence_number = 3, .sample_count = 1, .duration_samples = 1536},
        mp4::MediaSegment{
            .bytes = {}, .sequence_number = 4, .sample_count = 3, .duration_samples = 4608},
        mp4::MediaSegment{
            .bytes = {}, .sequence_number = 5, .sample_count = 3, .duration_samples = 4608},
        mp4::MediaSegment{
            .bytes = {}, .sequence_number = 6, .sample_count = 3, .duration_samples = 4608},
    };
    const auto snippet = mp4::build_dash_adaptation_set(track, segments, mp4::DashOptions{});
    CHECK(snippet.find("<S t=\"0\" d=\"3072\" r=\"1\"/>") != std::string::npos);
    CHECK(snippet.find("<S d=\"1536\"/>") != std::string::npos);
    CHECK(snippet.find("<S d=\"4608\" r=\"2\"/>") != std::string::npos);

    // Independently re-parse every <S> entry out of the snippet (rather than
    // trusting the three find()s above alone) and check the total duration
    // it describes - d*(r+1) summed - against the segments' own total. This
    // is the property a real player actually relies on: get it wrong and
    // playback position/seeking drifts even if individual <S> tags "look"
    // present.
    std::uint64_t reconstructed_total = 0;
    std::size_t pos = 0;
    while ((pos = snippet.find("<S ", pos)) != std::string::npos) {
        // Skips whatever leads the entry (the first one's own @t) and reads
        // the @d/@r pair, so this stays a genuinely independent re-parse
        // rather than a match against one exact spelling.
        const auto d_start = snippet.find(" d=\"", pos) + 4;
        const auto d_end = snippet.find('"', d_start);
        const auto d = std::stoull(snippet.substr(d_start, d_end - d_start));
        std::uint64_t repeat = 0;
        const auto r_pos = snippet.find(" r=\"", d_end);
        const auto tag_end = snippet.find("/>", d_end);
        if (r_pos != std::string::npos && r_pos < tag_end) {
            const auto r_start = r_pos + 4;
            const auto r_end = snippet.find('"', r_start);
            repeat = std::stoull(snippet.substr(r_start, r_end - r_start));
        }
        reconstructed_total += d * (repeat + 1);
        pos = tag_end;
    }
    std::uint64_t expected_total = 0;
    for (const auto& s : segments) {
        expected_total += s.duration_samples;
    }
    CHECK(reconstructed_total == expected_total);
}

TEST_CASE("FragmentWriter's pushed segments are fragment()'s bytes exactly", "[fmp4]") {
    // The writer's whole contract (mp4.hpp): the only state fragment()'s own
    // loop carries across fragments is the running decode time and the
    // sequence number, and both live on the writer instead - so pushing the
    // same frames one at a time reproduces the batch segments byte for byte,
    // styp brands, mfhd, tfdt, trun sizes and mdat payload alike. The shared
    // fixture is deliberately 10 frames at 3 per fragment, so this spans
    // three full fragments and one short trailing one.
    const auto fixture = make_real_fixture();
    auto writer = mp4::FragmentWriter::create(
        fixture.track, mp4::FragmentOptions{.frames_per_fragment = kFramesPerFragment});
    REQUIRE(writer.has_value());

    std::vector<mp4::MediaSegment> streamed;
    for (const auto& frame : fixture.frames) {
        auto closed = writer->push(frame);
        REQUIRE(closed.has_value());
        if (*closed) {
            streamed.push_back(std::move(**closed));
        }
    }
    // A segment comes back on exactly every kFramesPerFragment-th push, so
    // 10 frames at 3 apiece close three fragments before finalize().
    CHECK(streamed.size() == 3);
    auto tail = writer->finalize();
    REQUIRE(tail.has_value());
    REQUIRE(tail->has_value());
    streamed.push_back(std::move(**tail));
    // A second finalize() has nothing left to flush.
    auto again = writer->finalize();
    REQUIRE(again.has_value());
    CHECK_FALSE(again->has_value());

    CHECK(writer->frames_written() == fixture.frames.size());
    const auto& batch = fixture.fragmented.media_segments;
    REQUIRE(streamed.size() == batch.size());
    for (std::size_t i = 0; i < batch.size(); ++i) {
        CHECK(streamed[i].sequence_number == batch[i].sequence_number);
        CHECK(streamed[i].sample_count == batch[i].sample_count);
        CHECK(streamed[i].duration_samples == batch[i].duration_samples);
        CHECK(streamed[i].base_media_decode_time == batch[i].base_media_decode_time);
        REQUIRE(streamed[i].bytes.size() == batch[i].bytes.size());
        CHECK(std::equal(streamed[i].bytes.begin(), streamed[i].bytes.end(),
                         batch[i].bytes.begin(), batch[i].bytes.end()));
    }
    // And the running decode time really is the sum of what came before,
    // recomputed here rather than read back out of the same field.
    std::uint64_t expected_decode_time = 0;
    for (const auto& segment : streamed) {
        CHECK(segment.base_media_decode_time == expected_decode_time);
        expected_decode_time += segment.duration_samples;
    }
}

TEST_CASE("FragmentWriter's init segment is fragment()'s but for the unknown duration", "[fmp4]") {
    // The one deliberate difference between the two initialization segments:
    // a live session cannot know its total duration, so mvhd/tkhd/mdhd carry
    // 0 where the batch form writes the real total. This asserts that by
    // patching the three fields back to the batch total and then requiring
    // FULL byte equality - so anything else that drifted apart (stsd and its
    // dec3 payload, mvex/trex, the empty sample table, the ftyp brands) fails
    // here rather than passing a weaker structural check.
    const auto fixture = make_real_fixture();
    auto writer = mp4::FragmentWriter::create(
        fixture.track, mp4::FragmentOptions{.frames_per_fragment = kFramesPerFragment});
    REQUIRE(writer.has_value());

    Bytes patched = writer->init_segment();
    const auto elements = parse(patched);
    const auto* mvhd = find(elements, "mvhd");
    const auto* tkhd = find(elements, "tkhd");
    const auto* mdhd = find(elements, "mdhd");
    REQUIRE(mvhd != nullptr);
    REQUIRE(tkhd != nullptr);
    REQUIRE(mdhd != nullptr);
    // Payload layouts, ISO/IEC 14496-12 §8.2.2/§8.3.2/§8.4.2 (version 0):
    // mvhd  version+flags(4) creation(4) modification(4) timescale(4) duration
    // tkhd  version+flags(4) creation(4) modification(4) track_ID(4) reserved(4) duration
    // mdhd  version+flags(4) creation(4) modification(4) timescale(4) duration
    const std::array<std::size_t, 3> duration_offsets{mvhd->payload + 16, tkhd->payload + 20,
                                                      mdhd->payload + 16};
    for (const auto offset : duration_offsets) {
        CHECK(u32_at(patched, offset) == 0);
    }

    const auto total_samples =
        static_cast<std::uint32_t>(fixture.frames.size()) * fixture.track.samples_per_frame;
    for (const auto offset : duration_offsets) {
        CHECK(u32_at(fixture.fragmented.init_segment, offset) == total_samples);
        for (std::size_t i = 0; i < 4; ++i) {
            patched[offset + i] =
                std::byte{static_cast<std::uint8_t>(total_samples >> (24 - 8 * i))};
        }
    }
    REQUIRE(patched.size() == fixture.fragmented.init_segment.size());
    CHECK(std::equal(patched.begin(), patched.end(), fixture.fragmented.init_segment.begin(),
                     fixture.fragmented.init_segment.end()));
}

TEST_CASE("FragmentWriter refuses what fragment() refuses", "[fmp4]") {
    const auto track = sample_track();
    CHECK(mp4::FragmentWriter::create({.channels = 0, .codec_config = {std::byte{0}}}).error() ==
          mp4::MuxError::kInvalidTrack);
    CHECK(mp4::FragmentWriter::create(
              mp4::AudioTrack{.codec_id = "mp4a", .codec_config = {std::byte{0}}})
              .error() == mp4::MuxError::kInvalidTrack);
    // No codec_config payload at all: the sample entry would have no
    // dac3/dec3 child to describe the codec with.
    CHECK(mp4::FragmentWriter::create({.sample_rate = 48000, .channels = 2}).error() ==
          mp4::MuxError::kInvalidTrack);
    CHECK(mp4::FragmentWriter::create(track, {.frames_per_fragment = 0}).error() ==
          mp4::MuxError::kInvalidOptions);
    // Unlike fragment(), an empty session is not an error - kNoFrames is a
    // statement about a batch call's arguments, and a live writer that is
    // stopped before its first frame simply has nothing to flush.
    auto writer = mp4::FragmentWriter::create(track);
    REQUIRE(writer.has_value());
    auto tail = writer->finalize();
    REQUIRE(tail.has_value());
    CHECK_FALSE(tail->has_value());
    CHECK(writer->frames_written() == 0);
    CHECK(writer->window().empty());
}

TEST_CASE("FragmentWriter's playlist window rolls, and the live manifests roll with it",
          "[fmp4][hls][dash]") {
    // A rolling live origin: only the last two segments stay listed. The
    // whole point of the window is that a session of any length costs a fixed
    // amount to describe - so the manifests must state where the window now
    // STARTS, not assume it starts at the beginning of the track.
    const auto fixture = make_real_fixture();
    auto writer = mp4::FragmentWriter::create(
        fixture.track, mp4::FragmentOptions{.frames_per_fragment = kFramesPerFragment,
                                            .playlist_window_segments = 2});
    REQUIRE(writer.has_value());
    for (const auto& frame : fixture.frames) {
        REQUIRE(writer->push(frame).has_value());
    }
    REQUIRE(writer->finalize().has_value());

    const auto window = writer->window();
    REQUIRE(window.size() == 2);
    CHECK(window.front().sequence_number == 3);
    CHECK(window.back().sequence_number == 4);
    // The window's own entries still describe the same segments the batch
    // form produced, byte size included.
    const auto& batch = fixture.fragmented.media_segments;
    CHECK(window.front().base_media_decode_time == batch[2].base_media_decode_time);
    CHECK(window.front().byte_size == batch[2].bytes.size());
    CHECK(mp4::segment_info(batch[3]).duration_samples == window.back().duration_samples);

    // RFC 8216 §6.2.2's live Media Playlist: #EXT-X-MEDIA-SEQUENCE is the
    // FIRST listed segment's number (3, not 1), and neither
    // #EXT-X-PLAYLIST-TYPE:VOD nor #EXT-X-ENDLIST appears while the
    // presentation is still growing.
    const auto live =
        mp4::build_hls_media_playlist(fixture.track, window, mp4::HlsOptions{.vod = false});
    CHECK(live.find("#EXT-X-MEDIA-SEQUENCE:3\n") != std::string::npos);
    CHECK(live.find("#EXT-X-ENDLIST") == std::string::npos);
    CHECK(live.find("#EXT-X-PLAYLIST-TYPE") == std::string::npos);
    CHECK(live.find("segment3.m4s") != std::string::npos);
    CHECK(live.find("segment4.m4s") != std::string::npos);
    CHECK(live.find("segment1.m4s") == std::string::npos);

    // The DASH half of the same fact: @startNumber is the window's first
    // segment and the timeline's first <S> states its decode time, so a
    // player joining now lands where the audio actually is rather than at
    // zero.
    const auto snippet = mp4::build_dash_adaptation_set(fixture.track, window);
    CHECK(snippet.find("startNumber=\"3\"") != std::string::npos);
    CHECK(snippet.find(std::format("<S t=\"{}\" d=\"{}\"/>", window.front().base_media_decode_time,
                                   window.front().duration_samples)) != std::string::npos);
}

TEST_CASE("fragment() adds the 'ceao' brand only for an object-audio track", "[fmp4]") {
    // ETSI TS 103 420 §E.5: "The FileTypeBox compatibility brand shall be
    // ceao and should be used to indicate media tracks that conform to this
    // media profile" - DASH-IF IOP Part 8 v5.0.0 §5.3.3 repeats it for DASH.
    // Added to the existing pair, not substituted: §E.2 requires ISO/IEC
    // 23000-19 conformance on top of the profile.
    const auto fixture = make_real_fixture();
    const auto plain = parse(fixture.fragmented.init_segment);
    const auto* plain_ftyp = find(plain, "ftyp");
    REQUIRE(plain_ftyp != nullptr);
    CHECK_FALSE(has_brand(read_brand_box(fixture.fragmented.init_segment, *plain_ftyp), "ceao"));

    const auto object_audio =
        mp4::fragment(fixture.track, fixture.frames,
                      mp4::FragmentOptions{.frames_per_fragment = kFramesPerFragment,
                                           .object_audio_brand = true});
    REQUIRE(object_audio.has_value());
    const auto init = parse(object_audio->init_segment);
    const auto* ftyp = find(init, "ftyp");
    REQUIRE(ftyp != nullptr);
    const auto brands = read_brand_box(object_audio->init_segment, *ftyp);
    CHECK(has_brand(brands, "ceao"));
    CHECK(has_brand(brands, "iso6"));
    CHECK(has_brand(brands, "cmfc"));
    // Every media segment's styp carries it too - a segment served on its own
    // is where a player actually reads the brand from.
    REQUIRE_FALSE(object_audio->media_segments.empty());
    for (const auto& segment : object_audio->media_segments) {
        const auto boxes = parse(segment.bytes);
        const auto* styp = find(boxes, "styp");
        REQUIRE(styp != nullptr);
        CHECK(has_brand(read_brand_box(segment.bytes, *styp), "ceao"));
    }
    // FragmentWriter honours the same option, since it shares the builder.
    auto writer = mp4::FragmentWriter::create(
        fixture.track, mp4::FragmentOptions{.frames_per_fragment = kFramesPerFragment,
                                            .object_audio_brand = true});
    REQUIRE(writer.has_value());
    const auto streamed_init = parse(writer->init_segment());
    const auto* streamed_ftyp = find(streamed_init, "ftyp");
    REQUIRE(streamed_ftyp != nullptr);
    CHECK(has_brand(read_brand_box(writer->init_segment(), *streamed_ftyp), "ceao"));
}

TEST_CASE("DASH signals JOC and the channel configuration the way TS 103 420 D.2 specifies",
          "[dash]") {
    // ETSI TS 103 420 §D.2.2.1: the extension-type descriptor's value "shall
    // be the three character string JOC". §D.2.2.2: the complexity descriptor's
    // value "shall be decimal representation of the eight-bit element
    // complexity_index_type_a in the EC3SpecificBox". Both are named by
    // DASH-IF IOP Part 8 v5.0.0 §5.3.2 as the E-AC-3-with-JOC signalling.
    const auto fixture = make_real_fixture();
    const mp4::DashOptions options{.joc_complexity_index = 12,
                                   .dolby_channel_configuration = "F801"};
    const auto snippet =
        mp4::build_dash_adaptation_set(fixture.track, fixture.fragmented.media_segments, options);
    CHECK(snippet.find("<SupplementalProperty "
                       "schemeIdUri=\"tag:dolby.com,2018:dash:EC3_ExtensionType:2018\" "
                       "value=\"JOC\"/>") != std::string::npos);
    CHECK(snippet.find("<SupplementalProperty "
                       "schemeIdUri=\"tag:dolby.com,2018:dash:EC3_ExtensionComplexityIndex:2018\" "
                       "value=\"12\"/>") != std::string::npos);
    // §5.3.2's other allowed AudioChannelConfiguration scheme, "as defined in
    // TS 102 366 clause I.1.2.1": four hex digits of the 16-bit channel
    // assignment, L/C/R/Ls/Rs/LFE being F801 - the exact value TS 103 420
    // §D.2.3's example MPD carries as its commented alternative.
    CHECK(snippet.find("<AudioChannelConfiguration "
                       "schemeIdUri=\"tag:dolby.com,2014:dash:audio_channel_configuration:2011\" "
                       "value=\"F801\"/>") != std::string::npos);
    CHECK(snippet.find("cicp:ChannelConfiguration") == std::string::npos);

    // ISO/IEC 23009-1's RepresentationBaseType is a SEQUENCE, so both
    // descriptors have to precede the SegmentTemplate, and
    // AudioChannelConfiguration has to precede SupplementalProperty. A
    // manifest that gets this wrong fails schema validation even though every
    // attribute in it is right.
    const auto channels = snippet.find("<AudioChannelConfiguration");
    const auto supplemental = snippet.find("<SupplementalProperty");
    const auto segment_template = snippet.find("<SegmentTemplate");
    REQUIRE(channels != std::string::npos);
    REQUIRE(supplemental != std::string::npos);
    REQUIRE(segment_template != std::string::npos);
    CHECK(channels < supplemental);
    CHECK(supplemental < segment_template);
}

TEST_CASE("build_dash_mpd wraps a Period, static or dynamic", "[dash]") {
    const auto fixture = make_real_fixture();
    const auto& segments = fixture.fragmented.media_segments;
    const auto snippet = mp4::build_dash_adaptation_set(fixture.track, segments);

    // Static: the whole asset exists, so it states its own total - 10 frames
    // of 1536 samples at 48 kHz, recomputed here rather than read back.
    const auto vod = mp4::build_dash_mpd(fixture.track, segments, snippet);
    CHECK(vod.find("type=\"static\"") != std::string::npos);
    CHECK(vod.find(std::format(
              "mediaPresentationDuration=\"PT{:.3f}S\"",
              static_cast<double>(kFrames) * ac3::kSamplesPerFrame / 48000.0)) !=
          std::string::npos);
    CHECK(vod.find("availabilityStartTime") == std::string::npos);
    CHECK(vod.find(snippet) != std::string::npos);
    CHECK(vod.find("</MPD>") != std::string::npos);

    // Dynamic: segments are still appearing, so there is no total to state -
    // stating one would tell a player the stream stops there - and
    // @availabilityStartTime anchors the timeline to wall-clock time. The
    // same attribute set TS 103 420 §D.2.3's own example MPD carries.
    const auto live = mp4::build_dash_mpd(
        fixture.track, segments, snippet,
        mp4::MpdOptions{.is_static = false,
                        .availability_start_time = "2026-08-23T12:30:00Z",
                        .publish_time = "2026-08-23T12:31:00Z",
                        .minimum_update_period_seconds = 1.5,
                        .time_shift_buffer_depth_seconds = 30.0});
    CHECK(live.find("type=\"dynamic\"") != std::string::npos);
    CHECK(live.find("availabilityStartTime=\"2026-08-23T12:30:00Z\"") != std::string::npos);
    CHECK(live.find("publishTime=\"2026-08-23T12:31:00Z\"") != std::string::npos);
    CHECK(live.find("minimumUpdatePeriod=\"PT1.500S\"") != std::string::npos);
    CHECK(live.find("timeShiftBufferDepth=\"PT30.000S\"") != std::string::npos);
    CHECK(live.find("mediaPresentationDuration") == std::string::npos);
    CHECK(live.find("<Period id=\"1\" start=\"PT0S\">") != std::string::npos);

    // publishTime is optional; an empty one omits the attribute rather than
    // writing an empty string a validator would reject.
    const auto no_publish = mp4::build_dash_mpd(
        fixture.track, segments, snippet,
        mp4::MpdOptions{.is_static = false, .availability_start_time = "2026-08-23T12:30:00Z"});
    CHECK(no_publish.find("publishTime") == std::string::npos);
}
