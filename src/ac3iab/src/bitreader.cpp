#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "bitreader.hpp"

namespace ac3iab::detail {

std::expected<std::uint64_t, IabError> BitReader::read_bits(unsigned count) {
    if (count > 64 || count > bits_remaining()) {
        return std::unexpected(IabError::kTruncated);
    }

    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i) {
        const std::size_t byte_index = bit_pos_ / 8;
        const unsigned bit_index = 7 - static_cast<unsigned>(bit_pos_ % 8);  // MSB-first within each byte, §5.1
        const auto byte = std::to_integer<unsigned>(data_[byte_index]);
        value = (value << 1) | ((byte >> bit_index) & 0x1u);
        ++bit_pos_;
    }
    return value;
}

std::expected<std::uint64_t, IabError> BitReader::read_plex(unsigned initial_width) {
    unsigned width = initial_width;
    while (true) {
        auto value = read_bits(width);
        if (!value) {
            return value;
        }
        const std::uint64_t escape = (std::uint64_t{1} << width) - 1;
        if (*value != escape) {
            return value;
        }
        // §5.2: symbols to be Plex-encoded are guaranteed <= 0xFFFFFFFE, so a compliant
        // stream never needs to escape past a 32-bit field - width reaching 32 here and still
        // reading all-ones means either a corrupt stream or the reserved 0xFFFFFFFF value.
        if (width >= 32) {
            return std::unexpected(IabError::kBadEscape);
        }
        width *= 2;
    }
}

std::expected<std::span<const std::byte>, IabError> BitReader::read_bytes(std::size_t count) {
    align_to_byte();
    if (count > bits_remaining() / 8) {
        return std::unexpected(IabError::kTruncated);
    }
    const std::size_t start = bit_pos_ / 8;
    bit_pos_ += count * 8;
    return data_.subspan(start, count);
}

}  // namespace ac3iab::detail
