// The checks an update over the network makes before it writes anything,
// tested on the host from synthetic bytes - see ac3forge/firmware_image.hpp's
// own header comment for why this can be.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac3forge/firmware_image.hpp"

using ac3forge::BoardFacts;
using ac3forge::ContentDigest;
using ac3forge::host_is_the_board;
using ac3forge::ImageHead;
using ac3forge::kImageHeadBytes;
using ac3forge::parse_content_digest;
using ac3forge::parse_image_head;
using ac3forge::refuse_image;

namespace {

constexpr std::uint16_t kEsp32s3 = 0x0009;
constexpr std::uint16_t kEsp32c6 = 0x000D;
constexpr std::uint16_t kEsp32p4 = 0x0012;
constexpr std::uint8_t k4MB = 2;
constexpr std::uint8_t k16MB = 4;

struct HeadSpec {
    std::uint16_t chip_id = kEsp32s3;
    std::uint16_t min_rev = 0;
    std::uint16_t max_rev = 99;
    std::uint8_t flash_size = k16MB;
    bool hash_appended = true;
    std::uint8_t segments = 5;
    std::string version = "v0.10.0-beta.1-1858-gb49a966c";
    std::string project = "ac3forge_hearth_sink";
    std::string idf = "v6.1";
};

void put16(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint16_t value) {
    bytes[at] = static_cast<std::uint8_t>(value & 0xFF);
    bytes[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put_text(std::vector<std::uint8_t>& bytes, std::size_t at, std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        bytes[at + i] = static_cast<std::uint8_t>(text[i]);
    }
}

// The first 288 bytes of an application image, laid out as ESP-IDF v6.1's
// esp_image_header_t, esp_image_segment_header_t and esp_app_desc_t lay them.
std::vector<std::uint8_t> head_bytes(const HeadSpec& spec) {
    std::vector<std::uint8_t> bytes(kImageHeadBytes, 0);
    bytes[0] = 0xE9;
    bytes[1] = spec.segments;
    bytes[2] = 2;                                                           // spi_mode: DIO
    bytes[3] = static_cast<std::uint8_t>((spec.flash_size << 4) | 0x0F);    // speed in the low nibble
    put16(bytes, 12, spec.chip_id);
    bytes[14] = static_cast<std::uint8_t>(spec.min_rev / 100);
    put16(bytes, 15, spec.min_rev);
    put16(bytes, 17, spec.max_rev);
    bytes[23] = spec.hash_appended ? 1 : 0;
    // esp_app_desc_t at 32: the magic word, then the texts.
    bytes[32] = 0x32;
    bytes[33] = 0x54;
    bytes[34] = 0xCD;
    bytes[35] = 0xAB;
    put_text(bytes, 48, spec.version);
    put_text(bytes, 80, spec.project);
    put_text(bytes, 144, spec.idf);
    for (std::size_t i = 0; i < 32; ++i) {
        bytes[176 + i] = static_cast<std::uint8_t>(i + 1);
    }
    return bytes;
}

BoardFacts s3_board() {
    BoardFacts board;
    board.chip_id = kEsp32s3;
    board.revision_full = 2;
    board.flash_size = k16MB;
    board.project = "ac3forge_hearth_sink";
    return board;
}

ImageHead parsed(const HeadSpec& spec) {
    const auto result = parse_image_head(head_bytes(spec));
    REQUIRE(result.head.has_value());
    return *result.head;
}

}  // namespace

TEST_CASE("an application image's head is read field by field", "[io][firmware_image]") {
    HeadSpec spec;
    spec.chip_id = kEsp32p4;
    spec.min_rev = 100;
    spec.max_rev = 199;
    const auto result = parse_image_head(head_bytes(spec));
    REQUIRE(result.head.has_value());
    CHECK(result.why.empty());
    const ImageHead& head = *result.head;
    CHECK(head.chip_id == kEsp32p4);
    CHECK(head.min_rev_full == 100);
    CHECK(head.max_rev_full == 199);
    CHECK(head.flash_size == k16MB);
    CHECK(head.hash_appended);
    CHECK(head.version == "v0.10.0-beta.1-1858-gb49a966c");
    CHECK(head.project == "ac3forge_hearth_sink");
    CHECK(head.idf_version == "v6.1");
    CHECK(head.elf_sha256[0] == 1);
    CHECK(head.elf_sha256[31] == 32);
}

TEST_CASE("a text field that fills its whole array is read to its end", "[io][firmware_image]") {
    HeadSpec spec;
    spec.version = std::string(32, 'v');  // no terminator left in the field
    CHECK(parsed(spec).version == std::string(32, 'v'));
}

TEST_CASE("bytes that are not an application image's head are refused, saying why",
          "[io][firmware_image]") {
    SECTION("too few bytes") {
        const auto bytes = head_bytes({});
        const auto result = parse_image_head(std::span(bytes).first(kImageHeadBytes - 1));
        CHECK_FALSE(result.head.has_value());
        CHECK(result.why.find("ended before") != std::string::npos);
    }
    SECTION("the wrong first byte, which a merged factory image or an ELF has") {
        auto bytes = head_bytes({});
        bytes[0] = 0x7F;  // an ELF's
        const auto result = parse_image_head(bytes);
        CHECK_FALSE(result.head.has_value());
        CHECK(result.why.find("not an ESP-IDF application image") != std::string::npos);
        CHECK(result.why.find("ac3forge_hearth_sink.bin") != std::string::npos);
    }
    SECTION("a segment count no application has") {
        HeadSpec spec;
        spec.segments = 0;
        CHECK_FALSE(parse_image_head(head_bytes(spec)).head.has_value());
        spec.segments = 17;
        const auto result = parse_image_head(head_bytes(spec));
        CHECK_FALSE(result.head.has_value());
        CHECK(result.why.find("17 segments") != std::string::npos);
    }
    SECTION("no application description where ESP-IDF puts one") {
        auto bytes = head_bytes({});
        bytes[35] = 0x00;
        const auto result = parse_image_head(bytes);
        CHECK_FALSE(result.head.has_value());
        CHECK(result.why.find("no application description") != std::string::npos);
    }
}

TEST_CASE("an image for this board is not refused", "[io][firmware_image]") {
    CHECK_FALSE(refuse_image(parsed({}), s3_board()).has_value());
}

TEST_CASE("an image for another chip is refused, naming both chips", "[io][firmware_image]") {
    HeadSpec spec;
    spec.chip_id = kEsp32c6;
    const auto why = refuse_image(parsed(spec), s3_board());
    REQUIRE(why.has_value());
    CHECK(*why == "this image is for an ESP32-C6, and this board is an ESP32-S3");
}

TEST_CASE("chip revisions outside the image's range are refused both ways", "[io][firmware_image]") {
    // The P4 on the desk is v1.3: a default build needs v3.1, and a build from
    // sdkconfig.p4 accepts v1.0 to v1.99.
    BoardFacts p4 = s3_board();
    p4.chip_id = kEsp32p4;
    p4.revision_full = 103;

    HeadSpec for_v3;
    for_v3.chip_id = kEsp32p4;
    for_v3.min_rev = 301;
    for_v3.max_rev = 399;
    const auto too_old = refuse_image(parsed(for_v3), p4);
    REQUIRE(too_old.has_value());
    CHECK(*too_old == "this image needs chip revision v3.1 or newer, and this chip is v1.3");

    HeadSpec for_v1;
    for_v1.chip_id = kEsp32p4;
    for_v1.min_rev = 100;
    for_v1.max_rev = 199;
    CHECK_FALSE(refuse_image(parsed(for_v1), p4).has_value());

    BoardFacts p4_v3 = p4;
    p4_v3.revision_full = 301;
    const auto too_new = refuse_image(parsed(for_v1), p4_v3);
    REQUIRE(too_new.has_value());
    CHECK(*too_new == "this image runs on chip revisions up to v1.99, and this chip is v3.1");

    // ESP-IDF's own exception, a chip whose eFuse says to skip the maximum.
    p4_v3.ignore_max_revision = true;
    CHECK_FALSE(refuse_image(parsed(for_v1), p4_v3).has_value());
}

TEST_CASE("a maximum revision the build did not set is not a limit", "[io][firmware_image]") {
    BoardFacts board = s3_board();
    board.revision_full = 350;
    for (const std::uint16_t unset : {std::uint16_t{0}, std::uint16_t{65535}}) {
        HeadSpec spec;
        spec.max_rev = unset;
        CHECK_FALSE(refuse_image(parsed(spec), board).has_value());
    }
}

TEST_CASE("another project, another flash size and a missing SHA-256 are each refused",
          "[io][firmware_image]") {
    HeadSpec other_project;
    other_project.project = "i2s_player";
    CHECK(refuse_image(parsed(other_project), s3_board()) ==
          std::optional<std::string>("this image is i2s_player, not ac3forge_hearth_sink"));

    HeadSpec small_flash;
    small_flash.flash_size = k4MB;
    CHECK(refuse_image(parsed(small_flash), s3_board()) ==
          std::optional<std::string>(
              "this image was built for 4 MB of flash, and this board's is set for 16 MB of flash"));

    HeadSpec no_hash;
    no_hash.hash_appended = false;
    const auto why = refuse_image(parsed(no_hash), s3_board());
    REQUIRE(why.has_value());
    CHECK(why->find("no SHA-256") != std::string::npos);
}

TEST_CASE("a refusal names the chip before anything else about the image", "[io][firmware_image]") {
    HeadSpec wrong_everything;
    wrong_everything.chip_id = kEsp32c6;
    wrong_everything.project = "i2s_player";
    wrong_everything.flash_size = k4MB;
    const auto why = refuse_image(parsed(wrong_everything), s3_board());
    REQUIRE(why.has_value());
    CHECK(why->find("ESP32-C6") != std::string::npos);
}

namespace {

void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

// A slot of erased flash holding a whole image: the header, segments with
// data of the given lengths, zeros up to the checksum byte that ends a 16-byte
// block, and 32 bytes standing for the SHA-256 the build appends.
std::vector<std::uint8_t> slot_holding(std::initializer_list<std::uint32_t> lengths, std::size_t slot_bytes,
                                       bool hash_appended = true) {
    std::vector<std::uint8_t> slot(slot_bytes, 0xFF);
    std::fill_n(slot.begin(), ac3forge::kImageHeaderBytes, std::uint8_t{0});
    slot[0] = 0xE9;
    slot[1] = static_cast<std::uint8_t>(lengths.size());
    slot[23] = hash_appended ? 1 : 0;
    std::size_t at = ac3forge::kImageHeaderBytes;
    for (const std::uint32_t length : lengths) {
        put32(slot, at, 0x3C000020);
        put32(slot, at + 4, length);
        at += ac3forge::kSegmentHeaderBytes;
        std::fill_n(slot.begin() + static_cast<std::ptrdiff_t>(at), length, std::uint8_t{0x5A});
        at += length;
    }
    while ((at + 1) % 16 != 0) {
        slot[at++] = 0;
    }
    slot[at++] = 0xEF;  // stands for the checksum
    if (hash_appended) {
        std::fill_n(slot.begin() + static_cast<std::ptrdiff_t>(at), 32, std::uint8_t{0xAB});
    }
    return slot;
}

// Reads the slot as esp_partition_read would, failing past its end.
auto reader(const std::vector<std::uint8_t>& slot) {
    return [&slot](std::size_t offset, std::span<std::uint8_t> out) {
        if (offset > slot.size() || out.size() > slot.size() - offset) {
            return false;
        }
        std::copy_n(slot.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    };
}

}  // namespace

TEST_CASE("an image in a slot is walked to where its own SHA-256 lies", "[io][firmware_image]") {
    SECTION("segments that end inside a 16-byte block") {
        // 24 + (8 + 4) + (8 + 20) + (8 + 36) = 108; the checksum byte ends
        // the block at 112.
        const auto slot = slot_holding({4, 20, 36}, 4096);
        const auto walked = ac3forge::walk_image(reader(slot), slot.size());
        REQUIRE(walked.extent.has_value());
        CHECK(walked.why.empty());
        CHECK(walked.extent->hashed_bytes == 112);
        CHECK(walked.extent->hash_appended);
        CHECK(slot[112] == 0xAB);
    }
    SECTION("segments that end on a block's boundary take a whole block more") {
        // 24 + (8 + 36) + (8 + 36) = 112: fifteen zeros and the checksum.
        const auto slot = slot_holding({36, 36}, 4096);
        const auto walked = ac3forge::walk_image(reader(slot), slot.size());
        REQUIRE(walked.extent.has_value());
        CHECK(walked.extent->hashed_bytes == 128);
    }
    SECTION("an image without an appended SHA-256 says so") {
        const auto slot = slot_holding({4}, 4096, false);
        const auto walked = ac3forge::walk_image(reader(slot), slot.size());
        REQUIRE(walked.extent.has_value());
        CHECK(walked.extent->hashed_bytes == 48);
        CHECK_FALSE(walked.extent->hash_appended);
    }
}

TEST_CASE("a slot with no image, or one that runs past its end, is not walked", "[io][firmware_image]") {
    const auto why = [](const std::vector<std::uint8_t>& slot, std::size_t limit) {
        const auto walked = ac3forge::walk_image(reader(slot), limit);
        CHECK_FALSE(walked.extent.has_value());
        return walked.why;
    };
    SECTION("erased flash") {
        CHECK(why(std::vector<std::uint8_t>(4096, 0xFF), 4096) == "it holds no application image");
    }
    SECTION("no segments, or more than ESP-IDF allows") {
        auto slot = slot_holding({4}, 4096);
        slot[1] = 0;
        CHECK(why(slot, slot.size()) == "its image's header names 0 segments");
        slot[1] = 17;
        CHECK(why(slot, slot.size()) == "its image's header names 17 segments");
    }
    SECTION("a segment longer than the slot") {
        auto slot = slot_holding({4, 20}, 4096);
        put32(slot, 24 + 8 + 4 + 4, 0xFFFFFFF0);  // the second segment's length
        CHECK(why(slot, slot.size()) == "its image runs past the end of the slot");
    }
    SECTION("an appended SHA-256 past the slot's end") {
        // hashed_bytes is 112, so the SHA-256 would end at 144.
        const auto slot = slot_holding({4, 20, 36}, 4096);
        CHECK(why(slot, 143) == "its image runs past the end of the slot");
        CHECK(ac3forge::walk_image(reader(slot), 144).extent.has_value());
    }
    SECTION("a slot that cannot be read") {
        const std::vector<std::uint8_t> nothing;
        CHECK(why(nothing, 4096) == "its first bytes could not be read");
    }
}

TEST_CASE("Content-Digest's sha-256 member is read, and its absence is not an error",
          "[io][firmware_image]") {
    // sha-256 of the empty string, base64.
    const std::array<std::uint8_t, 32> empty_sha = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
    const auto digest = parse_content_digest("sha-256=:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=:");
    REQUIRE(digest.kind == ContentDigest::Kind::kSha256);
    CHECK(digest.sha256 == empty_sha);

    // Among other members, in any order, with the spaces a dictionary allows.
    const auto among = parse_content_digest(
        "sha-512=:z4PhNX7vuL3xVChQ1m2AB9Yg5AULVxXcg/SpIdNs6c5H0NE8XYXysP+DGNKHfuwvY7kxvUdBeoGlODJ6+SfaPg==:, "
        "sha-256=:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=:");
    REQUIRE(among.kind == ContentDigest::Kind::kSha256);
    CHECK(among.sha256 == empty_sha);

    CHECK(parse_content_digest("").kind == ContentDigest::Kind::kAbsent);
    CHECK(parse_content_digest("sha-512=:AAAA:").kind == ContentDigest::Kind::kAbsent);
}

TEST_CASE("a sha-256 member that is not 32 bytes of base64 is malformed", "[io][firmware_image]") {
    CHECK(parse_content_digest("sha-256=47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=").kind ==
          ContentDigest::Kind::kMalformed);  // no colons
    CHECK(parse_content_digest("sha-256=:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hS:").kind ==
          ContentDigest::Kind::kMalformed);  // 27 bytes
    CHECK(parse_content_digest("sha-256=:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU*:").kind ==
          ContentDigest::Kind::kMalformed);  // not base64
    CHECK(parse_content_digest("sha-256=:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSu=FU:").kind ==
          ContentDigest::Kind::kMalformed);  // padding in the middle
}

TEST_CASE("the Host check passes the board's own names and addresses only", "[io][firmware_image]") {
    const std::vector<std::string> names = {"hearth-eb2c64"};
    CHECK(host_is_the_board("192.168.1.99", names));
    CHECK(host_is_the_board("192.168.1.99:80", names));
    CHECK(host_is_the_board("127.0.0.1:8080", names));  // QEMU's port forward
    CHECK(host_is_the_board("[fe80::1]", names));
    CHECK(host_is_the_board("[fe80::1]:80", names));
    CHECK(host_is_the_board("hearth-eb2c64", names));
    CHECK(host_is_the_board("hearth-eb2c64.local", names));
    CHECK(host_is_the_board("Hearth-EB2C64.Local.", names));
    CHECK(host_is_the_board("hearth-eb2c64.local:80", names));
    CHECK(host_is_the_board("", names));  // no browser sends a request without one

    // A page that points its own name at the board sends that name.
    CHECK_FALSE(host_is_the_board("attacker.example", names));
    CHECK_FALSE(host_is_the_board("hearth-eb2c64.attacker.example", names));
    CHECK_FALSE(host_is_the_board("hearth-eb2c64.lan", names));  // a router's name: not proposed
    CHECK_FALSE(host_is_the_board("192.168.1.256", names));
    CHECK_FALSE(host_is_the_board("192.168.1", names));
    CHECK_FALSE(host_is_the_board("[]", names));
    CHECK_FALSE(host_is_the_board("other-board.local", names));
}
