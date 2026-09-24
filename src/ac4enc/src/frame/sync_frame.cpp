#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ac4enc/encoder.hpp"

// Part 1 Annex G.3 and G.4 (Part 2 Annex G refers to it): the sync word, the
// frame_size, the raw frame and, after sync word 0xAC41, a CRC over frame_size
// and the raw frame.

namespace ac4 {
namespace {

// G.4.2: generator x^16 + x^15 + x^2 + 1, register starting at 0, input bits
// MSB first, no reflection and no final XOR.
[[nodiscard]] std::uint16_t crc16(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0;
    for (const std::byte b : bytes) {
        for (int bit = 7; bit >= 0; --bit) {
            const auto in = (std::to_integer<unsigned>(b) >> static_cast<unsigned>(bit)) & 1U;
            const auto top = (crc >> 15U) & 1U;
            crc = (crc << 1U) & 0xFFFFU;
            if ((top ^ in) != 0) {
                crc ^= 0x8005U;
            }
        }
    }
    return static_cast<std::uint16_t>(crc);
}

void put(std::vector<std::byte>& out, std::uint32_t value, int bytes) {
    for (int b = bytes - 1; b >= 0; --b) {
        out.push_back(static_cast<std::byte>((value >> (8U * static_cast<unsigned>(b))) & 0xFFU));
    }
}

}  // namespace

std::vector<std::byte> sync_frame(std::span<const std::byte> raw_ac4_frame, bool crc) {
    std::vector<std::byte> out;
    out.reserve(raw_ac4_frame.size() + 9);
    put(out, crc ? 0xAC41U : 0xAC40U, 2);
    const std::size_t protected_start = out.size();
    // G.3.2: frame_size escapes to 24 bits at 0xFFFF.
    if (raw_ac4_frame.size() < 0xFFFF) {
        put(out, static_cast<std::uint32_t>(raw_ac4_frame.size()), 2);
    } else {
        put(out, 0xFFFFU, 2);
        put(out, static_cast<std::uint32_t>(raw_ac4_frame.size()), 3);
    }
    out.insert(out.end(), raw_ac4_frame.begin(), raw_ac4_frame.end());
    if (crc) {
        put(out, crc16(std::span<const std::byte>(out).subspan(protected_start)), 2);
    }
    return out;
}

}  // namespace ac4
