#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// What an update over the network checks before it writes anything
// (planning/esp32-ota.md, "An update, on the board"): that the bytes arriving
// are the head of an ESP-IDF application image, that the image is for this
// chip, this chip revision, this project and this flash size; what the
// request's Content-Digest says the whole file's SHA-256 is; and that the
// request names this board in its Host header. Also how much of an image
// already in a slot its own SHA-256 covers, so that each slot can be checked
// by reading it.
//
// Free of ESP-IDF, as hardware_info.hpp and sink_plan.hpp beside it are, and
// for the same reason: the layouts are copied here from ESP-IDF v6.1's own
// headers (esp_app_format.h's esp_image_header_t and
// esp_image_segment_header_t, esp_app_desc.h's esp_app_desc_t) so that every
// rule can be exercised on a laptop from synthetic bytes -
// tests/io/test_firmware_image.cpp - and firmware.cpp, which is ESP-IDF-only,
// fills BoardFacts from the chip and the running image and calls these.
//
// ESP-IDF checks the chip and its revision again when the whole image is in
// flash (esp_ota_end, through esp_image_verify and
// bootloader_common_check_chip_validity). These checks come first so that a
// wrong image is refused before anything is erased, with a reply that says
// which check it failed rather than a bare "image invalid".

namespace ac3forge {

// esp_image_header_t, 24 bytes; esp_image_segment_header_t, 8; and
// esp_app_desc_t, 256, which the build places at the start of the image's
// first segment. esp_ota_get_partition_description reads it from the same
// offset.
inline constexpr std::size_t kImageHeaderBytes = 24;
inline constexpr std::size_t kSegmentHeaderBytes = 8;
inline constexpr std::size_t kAppDescBytes = 256;
inline constexpr std::size_t kImageHeadBytes = kImageHeaderBytes + kSegmentHeaderBytes + kAppDescBytes;

inline constexpr std::uint8_t kImageMagic = 0xE9;           // ESP_IMAGE_HEADER_MAGIC
inline constexpr std::uint32_t kAppDescMagic = 0xABCD5432;  // ESP_APP_DESC_MAGIC_WORD
inline constexpr std::uint8_t kMaxSegments = 16;            // ESP_IMAGE_MAX_SEGMENTS

// What the head of an image says about it.
struct ImageHead {
    std::uint16_t chip_id = 0;       // esp_chip_id_t
    std::uint16_t min_rev_full = 0;  // major * 100 + minor
    std::uint16_t max_rev_full = 0;
    std::uint8_t flash_size = 0;     // esp_image_flash_size_t: 2 is 4 MB, 4 is 16 MB
    bool hash_appended = false;      // the build appended the image's SHA-256
    std::string version;
    std::string project;
    std::string idf_version;
    std::array<std::uint8_t, 32> elf_sha256{};
};

// The head, or why the bytes are not one. `why` is a reply's text.
struct ParsedHead {
    std::optional<ImageHead> head;
    std::string why;
};

// What the running board is, for the image to be held against.
struct BoardFacts {
    std::uint16_t chip_id = 0;
    std::uint16_t revision_full = 0;
    std::uint8_t flash_size = 0;
    std::string project;
    // ESP-IDF skips the maximum revision check on a chip whose eFuse says to
    // (efuse_hal_get_disable_wafer_version_major()); so does refuse_image.
    bool ignore_max_revision = false;
};

namespace detail {

[[nodiscard]] inline std::uint16_t u16_at(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}

[[nodiscard]] inline std::uint32_t u32_at(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16) | (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

// A fixed-size text field, up to its terminator: the build fills them and a
// field that exactly fills its array has no terminator to stop at.
[[nodiscard]] inline std::string text_at(std::span<const std::uint8_t> bytes, std::size_t at, std::size_t size) {
    const auto field = bytes.subspan(at, size);
    const auto end = std::find(field.begin(), field.end(), std::uint8_t{0});
    return {field.begin(), end};
}

// A chip ID as the chip's name, for a reply someone reads.
[[nodiscard]] inline std::string chip_name(std::uint16_t chip_id) {
    switch (chip_id) {
        case 0x0000:
            return "ESP32";
        case 0x0002:
            return "ESP32-S2";
        case 0x0005:
            return "ESP32-C3";
        case 0x0009:
            return "ESP32-S3";
        case 0x000C:
            return "ESP32-C2";
        case 0x000D:
            return "ESP32-C6";
        case 0x0010:
            return "ESP32-H2";
        case 0x0012:
            return "ESP32-P4";
        case 0x0014:
            return "ESP32-C61";
        case 0x0017:
            return "ESP32-C5";
        case 0x0019:
            return "ESP32-H21";
        case 0x001C:
            return "ESP32-H4";
        case 0x0020:
            return "ESP32-S31";
        default:
            return "chip ID " + std::to_string(chip_id);
    }
}

[[nodiscard]] inline std::string revision_text(std::uint16_t full) {
    return "v" + std::to_string(full / 100) + "." + std::to_string(full % 100);
}

// esp_image_flash_size_t as the size it names.
[[nodiscard]] inline std::string flash_size_text(std::uint8_t code) {
    if (code > 7) {
        return "an unknown flash size";
    }
    return std::to_string(1U << code) + " MB of flash";
}

// ESP-IDF's IS_FIELD_SET (bootloader_common_loader.c): 0 and 65535 both mean
// the build set no maximum.
[[nodiscard]] inline bool revision_field_set(std::uint16_t full) { return full != 0 && full != 65535; }

}  // namespace detail

// The head of an application image from its first kImageHeadBytes bytes.
[[nodiscard]] inline ParsedHead parse_image_head(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kImageHeadBytes) {
        return {std::nullopt, "the upload ended before the image's header did"};
    }
    if (bytes[0] != kImageMagic) {
        return {std::nullopt,
                "this is not an ESP-IDF application image (its first byte is not 0xE9); send "
                "ac3forge_hearth_sink.bin, not the merged factory image or the ELF"};
    }
    const std::uint8_t segments = bytes[1];
    if (segments == 0 || segments > kMaxSegments) {
        return {std::nullopt, "this image's header names " + std::to_string(segments) +
                                  " segments, which no ESP-IDF application has"};
    }
    constexpr std::size_t desc = kImageHeaderBytes + kSegmentHeaderBytes;
    if (detail::u32_at(bytes, desc) != kAppDescMagic) {
        return {std::nullopt,
                "this image has no application description where ESP-IDF puts one; it is not an "
                "application built by ESP-IDF"};
    }
    ImageHead head;
    head.flash_size = static_cast<std::uint8_t>(bytes[3] >> 4);
    head.chip_id = detail::u16_at(bytes, 12);
    head.min_rev_full = detail::u16_at(bytes, 15);
    head.max_rev_full = detail::u16_at(bytes, 17);
    head.hash_appended = bytes[23] == 1;
    head.version = detail::text_at(bytes, desc + 16, 32);
    head.project = detail::text_at(bytes, desc + 48, 32);
    head.idf_version = detail::text_at(bytes, desc + 112, 32);
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(desc + 144), head.elf_sha256.size(),
                head.elf_sha256.begin());
    return {head, {}};
}

// How much of an image in a slot its own SHA-256 covers, and whether one is
// appended: what the bootloader checks at every boot, found by reading
// (esp_image_format.c's process_segments, process_checksum and
// process_appended_hash_and_sig). The hash covers the header, each segment
// and the checksum byte that ends the image's last 16-byte block; the 32
// bytes after that are the hash.
struct ImageExtent {
    std::size_t hashed_bytes = 0;
    bool hash_appended = false;
};

// The extent, or why the slot holds no image that could be checked. `why` is
// a console line's text.
struct WalkedImage {
    std::optional<ImageExtent> extent;
    std::string why;
};

// Walks the image in a slot of `limit` bytes. `read(offset, out)` fills `out`
// from the slot and returns false when it cannot.
template <typename Read>
[[nodiscard]] WalkedImage walk_image(Read&& read, std::size_t limit) {
    std::array<std::uint8_t, kImageHeaderBytes> header{};
    if (limit < kImageHeaderBytes || !read(std::size_t{0}, std::span<std::uint8_t>(header))) {
        return {std::nullopt, "its first bytes could not be read"};
    }
    if (header[0] != kImageMagic) {
        return {std::nullopt, "it holds no application image"};
    }
    const std::uint8_t segments = header[1];
    if (segments == 0 || segments > kMaxSegments) {
        return {std::nullopt, "its image's header names " + std::to_string(segments) + " segments"};
    }
    std::size_t at = kImageHeaderBytes;
    for (std::uint8_t i = 0; i < segments; ++i) {
        std::array<std::uint8_t, kSegmentHeaderBytes> segment{};
        if (limit - at < kSegmentHeaderBytes || !read(at, std::span<std::uint8_t>(segment))) {
            return {std::nullopt, "its image runs past the end of the slot"};
        }
        at += kSegmentHeaderBytes;
        const std::uint32_t length = detail::u32_at(segment, 4);
        if (limit - at < length) {
            return {std::nullopt, "its image runs past the end of the slot"};
        }
        at += length;
    }
    const std::size_t hashed = (at + 1 + 15) & ~std::size_t{15};
    const bool appended = header[23] == 1;
    if (hashed > limit || (appended && limit - hashed < 32)) {
        return {std::nullopt, "its image runs past the end of the slot"};
    }
    return {ImageExtent{hashed, appended}, {}};
}

// Why this board must not take `image`, as a reply's text, or nothing when it
// may. The order is the order a person would want to hear it in: the chip
// first, then what else about the image does not fit.
[[nodiscard]] inline std::optional<std::string> refuse_image(const ImageHead& image, const BoardFacts& board) {
    if (image.chip_id != board.chip_id) {
        return "this image is for an " + detail::chip_name(image.chip_id) + ", and this board is an " +
               detail::chip_name(board.chip_id);
    }
    if (board.revision_full < image.min_rev_full) {
        return "this image needs chip revision " + detail::revision_text(image.min_rev_full) +
               " or newer, and this chip is " + detail::revision_text(board.revision_full);
    }
    if (detail::revision_field_set(image.max_rev_full) && !board.ignore_max_revision &&
        board.revision_full > image.max_rev_full) {
        return "this image runs on chip revisions up to " + detail::revision_text(image.max_rev_full) +
               ", and this chip is " + detail::revision_text(board.revision_full);
    }
    if (image.project != board.project) {
        return "this image is " + (image.project.empty() ? std::string{"an unnamed project"} : image.project) +
               ", not " + board.project;
    }
    if (image.flash_size != board.flash_size) {
        return "this image was built for " + detail::flash_size_text(image.flash_size) +
               ", and this board's is set for " + detail::flash_size_text(board.flash_size);
    }
    if (!image.hash_appended) {
        return "this image carries no SHA-256 of itself, so damage to it could not be found";
    }
    return std::nullopt;
}

// What a request's Content-Digest header says (RFC 9530): the sha-256 member,
// `sha-256=:<base64>:`. Absent, when the header or the member is not there - a
// `curl -T` upload sends none - and malformed when the member is there and is
// not 32 bytes of base64.
struct ContentDigest {
    enum class Kind : std::uint8_t { kAbsent, kSha256, kMalformed };
    Kind kind = Kind::kAbsent;
    std::array<std::uint8_t, 32> sha256{};
};

namespace detail {

[[nodiscard]] inline int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

// Standard base64 with its padding, into exactly `out.size()` bytes; false for
// anything else.
[[nodiscard]] inline bool base64_decode_exact(std::string_view text, std::span<std::uint8_t> out) {
    if (text.size() % 4 != 0) {
        return false;
    }
    std::size_t written = 0;
    for (std::size_t i = 0; i < text.size(); i += 4) {
        std::array<int, 4> v{};
        int padding = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=') {
                // Padding only in a final group's last two places.
                if (i + 4 != text.size() || k < 2) {
                    return false;
                }
                ++padding;
                v[k] = 0;
                continue;
            }
            if (padding > 0) {
                return false;
            }
            v[k] = base64_value(c);
            if (v[k] < 0) {
                return false;
            }
        }
        const std::uint32_t triple = (static_cast<std::uint32_t>(v[0]) << 18) |
                                     (static_cast<std::uint32_t>(v[1]) << 12) |
                                     (static_cast<std::uint32_t>(v[2]) << 6) | static_cast<std::uint32_t>(v[3]);
        const std::array<std::uint8_t, 3> bytes{static_cast<std::uint8_t>(triple >> 16),
                                                static_cast<std::uint8_t>(triple >> 8),
                                                static_cast<std::uint8_t>(triple)};
        for (int k = 0; k < 3 - padding; ++k) {
            if (written == out.size()) {
                return false;
            }
            out[written++] = bytes[static_cast<std::size_t>(k)];
        }
    }
    return written == out.size();
}

[[nodiscard]] inline std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace detail

[[nodiscard]] inline ContentDigest parse_content_digest(std::string_view header) {
    ContentDigest digest;
    while (!header.empty()) {
        const std::size_t comma = header.find(',');
        const std::string_view member = detail::trim(header.substr(0, comma));
        header = comma == std::string_view::npos ? std::string_view{} : header.substr(comma + 1);
        const std::size_t equals = member.find('=');
        if (equals == std::string_view::npos || detail::trim(member.substr(0, equals)) != "sha-256") {
            continue;  // another algorithm, which this board does not compute
        }
        const std::string_view value = detail::trim(member.substr(equals + 1));
        if (value.size() < 2 || value.front() != ':' || value.back() != ':' ||
            !detail::base64_decode_exact(value.substr(1, value.size() - 2), digest.sha256)) {
            digest.kind = ContentDigest::Kind::kMalformed;
            return digest;
        }
        digest.kind = ContentDigest::Kind::kSha256;
        return digest;
    }
    return digest;
}

namespace detail {

[[nodiscard]] inline std::string lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

[[nodiscard]] inline bool is_ipv4(std::string_view text) {
    int parts = 0;
    while (true) {
        const std::size_t dot = text.find('.');
        const std::string_view part = text.substr(0, dot);
        if (part.empty() || part.size() > 3) {
            return false;
        }
        int value = 0;
        for (const char c : part) {
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10 + (c - '0');
        }
        if (value > 255) {
            return false;
        }
        ++parts;
        if (dot == std::string_view::npos) {
            break;
        }
        text.remove_prefix(dot + 1);
    }
    return parts == 4;
}

}  // namespace detail

// Whether a request's Host header names this board: an IPv4 address, an IPv6
// address in brackets, or one of `names` bare or ending ".local", each with or
// without a port and a trailing dot (planning/esp32-ota.md, "Routes"). A page
// that points its own host name at the board's address sends that name here,
// which is what this refuses. An empty header passes: no browser sends one.
[[nodiscard]] inline bool host_is_the_board(std::string_view host, std::span<const std::string> names) {
    host = detail::trim(host);
    if (host.empty()) {
        return true;
    }
    if (host.front() == '[') {
        const std::size_t close = host.find(']');
        return close != std::string_view::npos && close > 1 &&
               (close + 1 == host.size() || host[close + 1] == ':');
    }
    const std::size_t colon = host.find(':');
    std::string name = detail::lower(host.substr(0, colon));
    if (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    if (detail::is_ipv4(name)) {
        return true;
    }
    for (const std::string& board : names) {
        const std::string own = detail::lower(board);
        if (!own.empty() && (name == own || name == own + ".local")) {
            return true;
        }
    }
    return false;
}

}  // namespace ac3forge
