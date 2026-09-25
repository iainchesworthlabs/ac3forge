#pragma once

// Whole ESP-IDF application images, made from nothing for the tests of
// apps/hearth/engine/sink_firmware.hpp: a header, one segment that starts with
// the application description, the checksum byte that ends the last 16-byte
// block, and the SHA-256 the build appends - laid out as ESP-IDF v6.1 lays
// them (esp_app_format.h, esp_image_format.c), so that the file checks walk
// them exactly as they walk a real build's.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/crypto.hpp"

namespace sink_firmware_test {

struct ImageSpec {
    std::uint16_t chip_id = 0x0009;  // ESP32-S3
    std::uint16_t min_rev = 0;
    std::uint16_t max_rev = 99;
    std::uint8_t flash_size = 4;  // 16 MB
    bool hash_appended = true;
    std::string version = "v0.11.0";
    std::string project = "ac3forge_hearth_sink";
    // Stands in for the ELF SHA-256 in the description: any 32 bytes.
    std::uint8_t elf_seed = 1;
    // The segment's length: the 256-byte description and what follows it.
    std::uint32_t segment_bytes = 1024;
};

inline void put16(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint16_t value) {
    bytes[at] = static_cast<std::uint8_t>(value & 0xFF);
    bytes[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

inline void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

inline void put_text(std::vector<std::uint8_t>& bytes, std::size_t at, std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        bytes[at + i] = static_cast<std::uint8_t>(text[i]);
    }
}

// The ELF SHA-256 an image made from `spec` carries, as hex.
inline std::string elf_hex(const ImageSpec& spec) {
    std::string out;
    for (std::size_t i = 0; i < 32; ++i) {
        constexpr std::string_view kHex = "0123456789abcdef";
        const auto byte = static_cast<std::uint8_t>(spec.elf_seed + i);
        out += kHex[static_cast<std::size_t>(byte >> 4)];
        out += kHex[static_cast<std::size_t>(byte & 0x0F)];
    }
    return out;
}

inline std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes) {
    std::array<std::uint8_t, 32> out{};
    if (!ac3::sendspin::crypto::sha256({bytes}, out)) {
        out.fill(0);
    }
    return out;
}

inline std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : bytes) {
        out += kHex[static_cast<std::size_t>(byte >> 4)];
        out += kHex[static_cast<std::size_t>(byte & 0x0F)];
    }
    return out;
}

// Where the checksum byte of an image made from `spec` sits: the last byte
// of the 16-byte block after the segment.
inline std::size_t checksum_at(const ImageSpec& spec) {
    const std::size_t end = 24 + 8 + spec.segment_bytes;
    return ((end + 1 + 15) & ~std::size_t{15}) - 1;
}

inline std::vector<std::uint8_t> make_image(const ImageSpec& spec) {
    std::vector<std::uint8_t> image(24 + 8, 0);
    image[0] = 0xE9;
    image[1] = 1;  // one segment
    image[2] = 2;  // spi_mode: DIO
    image[3] = static_cast<std::uint8_t>((spec.flash_size << 4) | 0x0F);
    put16(image, 12, spec.chip_id);
    image[14] = static_cast<std::uint8_t>(spec.min_rev / 100);
    put16(image, 15, spec.min_rev);
    put16(image, 17, spec.max_rev);
    image[23] = spec.hash_appended ? 1 : 0;
    put32(image, 24, 0x3C000020);  // the segment's load address
    put32(image, 28, spec.segment_bytes);

    std::vector<std::uint8_t> segment(spec.segment_bytes, 0);
    put32(segment, 0, 0xABCD5432);  // esp_app_desc_t's magic word
    put_text(segment, 16, spec.version);
    put_text(segment, 48, spec.project);
    put_text(segment, 112, "v6.1");
    for (std::size_t i = 0; i < 32; ++i) {
        segment[144 + i] = static_cast<std::uint8_t>(spec.elf_seed + i);
    }
    for (std::size_t i = 256; i < segment.size(); ++i) {
        segment[i] = static_cast<std::uint8_t>(i * 7 + spec.elf_seed);
    }
    std::uint8_t checksum = 0xEF;
    for (const std::uint8_t byte : segment) {
        checksum ^= byte;
    }
    image.insert(image.end(), segment.begin(), segment.end());
    image.resize(checksum_at(spec) + 1, 0);
    image.back() = checksum;
    if (spec.hash_appended) {
        const std::array<std::uint8_t, 32> digest = sha256(image);
        image.insert(image.end(), digest.begin(), digest.end());
    }
    return image;
}

// Re-appends the SHA-256 after a test has changed a byte, so that only what
// the test meant to break is broken.
inline void rehash(std::vector<std::uint8_t>& image, const ImageSpec& spec) {
    const std::size_t hashed = checksum_at(spec) + 1;
    const std::array<std::uint8_t, 32> digest = sha256(std::span<const std::uint8_t>(image).first(hashed));
    std::copy(digest.begin(), digest.end(), image.begin() + static_cast<std::ptrdiff_t>(hashed));
}

}  // namespace sink_firmware_test
