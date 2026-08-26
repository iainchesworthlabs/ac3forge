#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <vector>

#include "ac4/ac4.hpp"

namespace {

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

// Real Dolby Encoding Engine 6.5.4 output (tools/generators/gen_ac4_baseline.py),
// not a stream this project's own tooling produced - see docs/verification.md's
// AC-4 section and CONTRIBUTING.md's Oracles list, #3.
std::filesystem::path fixture_path() {
    return AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR "/ac4-stereo-64/dee.ac4";
}

}  // namespace

TEST_CASE("scan walks every sync frame of a real DEE AC-4 stream with CRCs intact", "[ac4]") {
    const auto data = read_file(fixture_path());
    const auto result = ac4::scan(data);

    CHECK_FALSE(result.stopped_at.has_value());
    REQUIRE(result.frames.size() == 73);
    for (const auto& frame : result.frames) {
        CAPTURE(frame.offset);
        CHECK(frame.sync_word == 0xAC41);
        REQUIRE(frame.crc_ok.has_value());
        CHECK(*frame.crc_ok);
    }
    // Annex G.3.2: frame_size is the trailing raw_ac4_frame()'s own byte
    // count, so consecutive frames' offsets have to be contiguous with no
    // gap or overlap.
    for (std::size_t i = 1; i < result.frames.size(); ++i) {
        const auto& prev = result.frames[i - 1];
        const std::size_t prev_total =
            prev.raw_ac4_frame.size() + 4 /* sync+frame_size */ + 2 /* crc */;
        CHECK(result.frames[i].offset == prev.offset + prev_total);
    }
}

TEST_CASE("parse_raw_frame reads a real stereo DEE frame's TOC and presentation", "[ac4]") {
    const auto data = read_file(fixture_path());
    const auto scanned = ac4::scan(data);
    REQUIRE(scanned.frames.size() == 73);

    // Frame 0. Every field below is cross-checked against MediaInfo's own
    // (dlb_ac4lib-based) reading of this exact fixture - see
    // docs/verification.md.
    const auto result = ac4::parse_raw_frame(scanned.frames[0].raw_ac4_frame);
    REQUIRE(result.has_value());
    const auto& toc = result->toc;

    CHECK(toc.bitstream_version == 2);
    CHECK(toc.sample_rate_hz == 48000);
    CHECK(toc.frame_rate_index == 13);  // Table 83's "(23,44)" row, 2048 samples/frame -
                                        // matches MediaInfo's "23.438 FPS (2048 SPF)"
                                        // for source material with no embedded frame rate.
    CHECK(toc.n_presentations == 1);
    CHECK(toc.payload_base == 1);

    REQUIRE(toc.presentations_v1.size() == 1);
    CHECK(toc.presentations_v1[0].group_refs == std::vector<int>{0});

    REQUIRE(toc.substream_groups.size() == 1);
    const auto& group = toc.substream_groups[0];
    CHECK(group.b_substreams_present);
    REQUIRE(group.substreams.size() == 1);
    CHECK(group.substreams[0].channel_mode_name == "Stereo");
    REQUIRE(group.substreams[0].ch_mode.has_value());
    CHECK(*group.substreams[0].ch_mode == 1);
    REQUIRE(group.substreams[0].substream_index.has_value());
    CHECK(*group.substreams[0].substream_index == 1);

    CHECK(toc.n_substreams == 3);
    REQUIRE(result->substreams.size() == 3);
    // Table 15/50: which substream_index_table() row is channel audio is
    // decided by ac4_substream_info_chan()'s own substream_index (1 here),
    // not by table position - rows 0 and 2 are ac4_presentation_substream()
    // and emdf_payloads_substream(), different shapes this parser reports
    // by byte range only.
    CHECK_FALSE(result->substreams[0].is_channel_audio);
    CHECK(result->substreams[1].is_channel_audio);
    REQUIRE(result->substreams[1].audio_size.has_value());
    CHECK(*result->substreams[1].audio_size == 396);
    CHECK(result->substreams[1].size == 402);
    CHECK_FALSE(result->substreams[2].is_channel_audio);

    // §4.3.3.12.4 Pseudocode 1: every substream's byte span has to land
    // fully inside the frame that declared it.
    std::size_t end = 0;
    for (const auto& sub : result->substreams) {
        CHECK(sub.offset + sub.size <= scanned.frames[0].raw_ac4_frame.size());
        end = std::max(end, sub.offset + sub.size);
    }
    CHECK(end <= scanned.frames[0].raw_ac4_frame.size());
}

TEST_CASE("parse_raw_frame agrees with itself across every frame of a real stream", "[ac4]") {
    // Not a per-field ground-truth check (that's the frame-0 test above) -
    // this proves the parser stays synchronised for 73 consecutive frames
    // of real, varying-size VBR content rather than only the one frame
    // that was used to debug it.
    const auto data = read_file(fixture_path());
    const auto scanned = ac4::scan(data);
    REQUIRE(scanned.frames.size() == 73);

    for (const auto& frame : scanned.frames) {
        CAPTURE(frame.offset);
        const auto result = ac4::parse_raw_frame(frame.raw_ac4_frame);
        REQUIRE(result.has_value());
        CHECK(result->toc.bitstream_version == 2);
        CHECK(result->toc.n_presentations == 1);
        REQUIRE(result->toc.substream_groups.size() == 1);
        REQUIRE(result->toc.substream_groups[0].substreams.size() == 1);
        CHECK(result->toc.substream_groups[0].substreams[0].channel_mode_name == "Stereo");
        std::size_t total = 0;
        for (const auto& sub : result->substreams) {
            total += sub.size;
        }
        CHECK(total <= frame.raw_ac4_frame.size());
    }
}

TEST_CASE("parse_raw_frame rejects a frame truncated inside the TOC", "[ac4]") {
    const auto data = read_file(fixture_path());
    const auto scanned = ac4::scan(data);
    REQUIRE(!scanned.frames.empty());
    const auto& raw = scanned.frames[0].raw_ac4_frame;

    for (const std::size_t cut : {std::size_t{0}, std::size_t{1}, std::size_t{5}, raw.size() / 2}) {
        CAPTURE(cut);
        const auto result = ac4::parse_raw_frame(raw.subspan(0, cut));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac4::Error::kTruncated);
    }
}

TEST_CASE("scan reports kLostSync at the offset of a corrupted sync word", "[ac4]") {
    auto data = read_file(fixture_path());
    const auto first = ac4::scan(data);
    REQUIRE(first.frames.size() > 1);
    const std::size_t second_frame_offset = first.frames[1].offset;

    data[second_frame_offset] = std::byte{0x00};  // was the high byte of 0xAC41
    const auto result = ac4::scan(data);
    REQUIRE(result.stopped_at.has_value());
    CHECK(*result.stopped_at == ac4::Error::kLostSync);
    CHECK(result.stopped_at_offset == second_frame_offset);
    // Everything before the corruption still parsed.
    CHECK(result.frames.size() == 1);
}

TEST_CASE("parse_raw_frame refuses a substream group flipped to object-coded", "[ac4]") {
    // A single deliberate bit flip on real, valid data, at the exact
    // position ac4_substream_group_info()'s b_channel_coded (§6.2.1.6)
    // occupies in this fixture's frame 0 (bit 99 of raw_ac4_frame, traced
    // and cross-checked against tools/references/ac4_parse.py while this
    // parser was being written) - proves the ObjectCodedGroup refusal path
    // is reachable and clean, without hand-encoding a whole synthetic
    // A-JOC/OAMD bitstream this parser does not otherwise need to build.
    auto data = read_file(fixture_path());
    const auto scanned = ac4::scan(data);
    REQUIRE(!scanned.frames.empty());

    std::vector<std::byte> raw(scanned.frames[0].raw_ac4_frame.begin(),
                               scanned.frames[0].raw_ac4_frame.end());
    constexpr std::size_t kChannelCodedBit = 99;
    const std::size_t byte_index = kChannelCodedBit / 8;
    const auto mask = static_cast<std::byte>(0x80u >> (kChannelCodedBit % 8));
    REQUIRE((raw[byte_index] & mask) != std::byte{0});  // was 1 (channel-coded)
    raw[byte_index] &= static_cast<std::byte>(~std::to_integer<unsigned>(mask) & 0xFF);

    const auto result = ac4::parse_raw_frame(raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac4::Error::kObjectCodedGroup);
}

TEST_CASE("parse_raw_frame refuses bitstream_version above 2", "[ac4]") {
    // §6.3.2.1.1: only bitstream_version 0-2 are decodable. The first byte's
    // top two bits are bitstream_version's raw 2-bit field; 0b11 (3) plus a
    // variable_bits(2) extension of 0 leaves it at 3, deliberately not the
    // 3 + 16*n a longer extension would produce - the smallest value that
    // exercises the refusal.
    const std::vector<std::byte> raw = {std::byte{0xC0}, std::byte{0x00}, std::byte{0x00},
                                        std::byte{0x00}};
    const auto result = ac4::parse_raw_frame(raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac4::Error::kUnsupportedBitstreamVersion);
}

TEST_CASE("describe returns a distinct, non-empty string for every Error", "[ac4]") {
    for (const auto error :
         {ac4::Error::kTruncated, ac4::Error::kLostSync, ac4::Error::kUnsupportedBitstreamVersion,
          ac4::Error::kObjectCodedGroup}) {
        CAPTURE(static_cast<int>(error));
        CHECK_FALSE(ac4::describe(error).empty());
    }
}
