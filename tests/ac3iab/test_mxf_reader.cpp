#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "ac3iab/ac3iab.hpp"
#include "ac3iab/mxf.hpp"

// ac3iab::parse_mxf_iab (mxf.hpp) - roadmap item IM1 phase 2. These tests build MXF-level KLV
// fixtures byte-by-byte, independently of src/ac3iab/src/mxf_reader.cpp's own implementation - the
// same "independent fixture" convention test_ac3iab.cpp already establishes for its own IAB
// bitstream fixtures. Key byte values are transcribed directly from SMPTE ST 377-1:2019 Table 4/6
// (Partition Pack Key), ST 379-1:2009 Table 2 (Essence Element Key) and ST 2067-201:2021 Table 4.2
// (the registered IAB Essence Element Key UL), with citations at each fixture helper - see
// mxf_reader.cpp's own header comment for the full design.

namespace {

void put_u8(std::vector<std::byte>& out, std::uint8_t v) {
    out.push_back(static_cast<std::byte>(v));
}

void put_bytes(std::vector<std::byte>& out, const std::vector<std::byte>& more) {
    out.insert(out.end(), more.begin(), more.end());
}

// SMPTE ST 336:2017 §5.3: short-form BER length (value < 128) or one-byte-count long form
// (128 <= value <= 255) - both used across these fixtures to exercise both encodings.
std::vector<std::byte> ber_length(std::size_t value) {
    std::vector<std::byte> out;
    if (value < 0x80) {
        put_u8(out, static_cast<std::uint8_t>(value));
    } else {
        put_u8(out, 0x81);  // long form, 1 following length byte
        put_u8(out, static_cast<std::uint8_t>(value));
    }
    return out;
}

std::vector<std::byte> klv(const std::array<std::uint8_t, 16>& key,
                           const std::vector<std::byte>& value) {
    std::vector<std::byte> out;
    for (auto b : key) {
        put_u8(out, b);
    }
    put_bytes(out, ber_length(value.size()));
    put_bytes(out, value);
    return out;
}

// ST 377-1 Table 4 (bytes 1-13, byte 8 "vvh" taken as 0x01 here - a real, permitted value, not the
// only one this reader accepts, see mxf_reader.cpp's own wildcard-byte-8 comment) + Table 6 (byte
// 14 = 0x02 "MXF Header Partition", byte 15 = 0x04 "Closed and Complete").
constexpr std::array<std::uint8_t, 16> kHeaderPartitionKey = {
    0x06, 0x0E, 0x2B, 0x34, 0x02, 0x05, 0x01, 0x01, 0x0D, 0x01, 0x02, 0x01, 0x01, 0x02, 0x04, 0x00};

// ST 377-1 §6.3.3.
constexpr std::array<std::uint8_t, 16> kFillItemKey = {
    0x06, 0x0E, 0x2B, 0x34, 0x01, 0x01, 0x01, 0x02, 0x03, 0x01, 0x02, 0x10, 0x01, 0x00, 0x00, 0x00};

// ST 2067-201 Table 4.2: urn:smpte:ul:060E2B34.01020101.0D010301.16cc0Dnn (byte 8 and byte 16 both
// concrete here - 0x01 is a real, valid value for each, matching this project's synthetic fixture
// convention elsewhere of using the spec's own worked values where one is given).
constexpr std::array<std::uint8_t, 16> kIabEssenceKey = {
    0x06, 0x0E, 0x2B, 0x34, 0x01, 0x02, 0x01, 0x01, 0x0D, 0x01, 0x03, 0x01, 0x16, 0xCC, 0x0D, 0x01};

// A Key sharing the IAB essence key's Item-Type byte (0x16, "GC Sound" - ST 379-1 Table 2) but a
// different Essence Element Type (byte 15), the way a real file's BWF/AES3/Dolby E sound essence
// would - confirms byte 13 alone is not treated as a match (see mxf_reader.cpp's own comment on
// why that byte cannot identify IAB by itself).
constexpr std::array<std::uint8_t, 16> kOtherSoundEssenceKey = {
    0x06, 0x0E, 0x2B, 0x34, 0x01, 0x02, 0x01, 0x01, 0x0D, 0x01, 0x03, 0x01, 0x16, 0x01, 0x01, 0x01};

// SMPTE ST 2098-2:2022 §7 Table 2 / §8: the smallest legal elementary IABitstream - one Preamble
// (PreambleTag 0x01, PreambleLength 0) followed by one IAFrame segment (IAFrameTag 0x02) wrapping
// one IAElement(IA_FRAME) (ElementID Plex(8) = 0x08, ElementSize Plex(8) = 4) whose 4-byte payload
// is the smallest legal IaFrame (§10.2): Version = 1, SampleRate code = 0 (48 kHz), BitDepth code
// = 0 (16-bit), FrameRate code = 0x3 (48 fps - any non-Reserved code works), MaxRendered Plex(8) =
// 0, already byte-aligned, SubElementCount Plex(8) = 0 (no Beds/Objects/essence - this fixture only
// needs to prove the MXF unwrap step reaches parse_iabitstream() correctly, not exercise its own
// element parsing, which test_ac3iab.cpp already covers thoroughly).
std::vector<std::byte> minimal_elementary_iabitstream() {
    std::vector<std::byte> out;
    put_u8(out, 0x01);  // PreambleTag
    put_u8(out, 0x00);  // PreambleLength byte 0 (BE32 = 0)
    put_u8(out, 0x00);
    put_u8(out, 0x00);
    put_u8(out, 0x00);
    put_u8(out, 0x02);  // IAFrameTag
    put_u8(out, 0x00);  // IAFrameLength BE32 = 6 (ElementID+ElementSize+4-byte payload)
    put_u8(out, 0x00);
    put_u8(out, 0x00);
    put_u8(out, 0x06);
    put_u8(out, 0x08);  // ElementID Plex(8) = 0x08 (IA_FRAME, §10.1.1 Table 14)
    put_u8(out, 0x04);  // ElementSize Plex(8) = 4
    put_u8(out, 0x01);  // Version = 1
    put_u8(out, 0x03);  // SampleRate=0, BitDepth=0, FrameRate=0x3 packed into one byte
    put_u8(out, 0x00);  // MaxRendered Plex(8) = 0
    put_u8(out, 0x00);  // SubElementCount Plex(8) = 0
    return out;
}

std::expected<std::vector<ac3iab::IABitstreamFrame>, ac3iab::IabError> parse(
    const std::vector<std::byte>& bytes) {
    std::string view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::istringstream in(std::move(view), std::ios::binary);
    return ac3iab::parse_mxf_iab(in);
}

}  // namespace

TEST_CASE("parse_mxf_iab extracts a clip-wrapped IABitstream past a Partition Pack and Fill Item",
          "[ac3iab][mxf]") {
    std::vector<std::byte> file;
    put_bytes(file, klv(kHeaderPartitionKey, {}));
    put_bytes(file, klv(kFillItemKey, {std::byte{0x00}, std::byte{0x00}}));
    put_bytes(file, klv(kIabEssenceKey, minimal_elementary_iabitstream()));

    auto frames = parse(file);
    REQUIRE(frames.has_value());
    REQUIRE(frames->size() == 1);
    CHECK((*frames)[0].frame.version == 1);
    CHECK((*frames)[0].frame.sample_rate == 48000);
    CHECK((*frames)[0].frame.bit_depth == 16);
    CHECK((*frames)[0].frame.beds.empty());
    CHECK((*frames)[0].frame.objects.empty());
}

TEST_CASE("parse_mxf_iab matches the essence element key with a long-form BER length",
          "[ac3iab][mxf]") {
    // Padding the Value with arbitrary bytes would desync parse_iabitstream()'s own framing, so
    // instead this concatenates whole, self-framing copies of the minimal elementary IABitstream -
    // legal per §7 Table 2's own repeating while(true) structure, the same way several real frames
    // follow one another in a real file - until the Value is provably >= 128 bytes (each copy is
    // 16 bytes; 8 copies = 128), forcing ber_length() into its long form.
    std::vector<std::byte> payload;
    while (payload.size() < 0x80) {
        put_bytes(payload, minimal_elementary_iabitstream());
    }
    REQUIRE(payload.size() >= 0x80);

    std::vector<std::byte> file;
    put_bytes(file, klv(kHeaderPartitionKey, {}));
    put_bytes(file, klv(kIabEssenceKey, payload));

    auto frames = parse(file);
    REQUIRE(frames.has_value());
    CHECK(frames->size() == payload.size() / 16);
}

TEST_CASE("parse_mxf_iab refuses a file with no matching essence element key", "[ac3iab][mxf]") {
    std::vector<std::byte> file;
    put_bytes(file, klv(kHeaderPartitionKey, {}));
    put_bytes(file, klv(kOtherSoundEssenceKey, minimal_elementary_iabitstream()));

    auto frames = parse(file);
    REQUIRE_FALSE(frames.has_value());
    CHECK(frames.error() == ac3iab::IabError::kMxfNoIabEssence);
    CHECK_FALSE(ac3iab::describe(frames.error()).empty());
    CHECK(ac3iab::describe(frames.error()) != "unknown ac3iab error");
}

TEST_CASE("parse_mxf_iab refuses an empty file", "[ac3iab][mxf]") {
    auto frames = parse({});
    REQUIRE_FALSE(frames.has_value());
    CHECK(frames.error() == ac3iab::IabError::kMxfNoIabEssence);
}

TEST_CASE("parse_mxf_iab reports a truncated Key", "[ac3iab][mxf]") {
    std::vector<std::byte> file;
    put_u8(file, 0x06);
    put_u8(file, 0x0E);  // only 2 of 16 Key bytes present

    auto frames = parse(file);
    REQUIRE_FALSE(frames.has_value());
    CHECK(frames.error() == ac3iab::IabError::kTruncated);
}

TEST_CASE("parse_mxf_iab reports a truncated Value", "[ac3iab][mxf]") {
    auto full = klv(kIabEssenceKey, minimal_elementary_iabitstream());
    // Length still claims the full Value; fewer bytes actually follow.
    full.resize(full.size() - 4);

    auto frames = parse(full);
    REQUIRE_FALSE(frames.has_value());
    CHECK(frames.error() == ac3iab::IabError::kTruncated);
}

TEST_CASE("parse_mxf_iab rejects the reserved indefinite-length BER token", "[ac3iab][mxf]") {
    std::vector<std::byte> file;
    for (auto b : kIabEssenceKey) {
        put_u8(file, b);
    }
    put_u8(file, 0x80);  // ST 336 Annex I: 0x80 (indefinite length) is forbidden in a KLV Length

    auto frames = parse(file);
    REQUIRE_FALSE(frames.has_value());
    CHECK(frames.error() == ac3iab::IabError::kMxfBadKlv);
    CHECK(ac3iab::describe(frames.error()) != "unknown ac3iab error");
}
