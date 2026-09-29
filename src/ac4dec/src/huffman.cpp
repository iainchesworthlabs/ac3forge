#include "huffman.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ac4::detail {

int huff_decode(BitReader& reader, const Codebook& codebook, std::string_view element) {
    const std::size_t start = reader.position();
    // The shortcut settles a codeword of kHuffFastBits or fewer at one lookup. Bits
    // past the end of the substream read as zeros there, so a codeword found that
    // runs past it is not one; the search below then finds none either, since a
    // shorter codeword that fitted would be a prefix of it.
    if (!codebook.fast.empty()) {
        const std::uint16_t entry = codebook.fast[reader.peek_raw(kHuffFastBits)];
        const int length = entry & 15;
        if (length != 0 && static_cast<std::size_t>(length) <= reader.remaining_bits()) {
            const auto index = static_cast<std::uint16_t>(entry >> 4U);
            reader.consume(length);
            reader.emit(start, length, index, element);
            return index;
        }
    }
    const int max_bits = std::min<int>(codebook.max_bits, static_cast<int>(reader.remaining_bits()));
    const std::uint32_t window = reader.peek_raw(max_bits);
    for (int length = 1; length <= max_bits; ++length) {
        const std::uint16_t first = codebook.length_start[static_cast<std::size_t>(length)];
        const std::uint16_t last = codebook.length_start[static_cast<std::size_t>(length) + 1];
        if (first == last) {
            continue;
        }
        const std::uint32_t code = window >> static_cast<unsigned>(max_bits - length);
        const auto begin = codebook.sorted.begin() + first;
        const auto end = codebook.sorted.begin() + last;
        const auto found = std::lower_bound(
            begin, end, code, [](const HuffEntry& entry, std::uint32_t value) { return entry.code < value; });
        if (found != end && found->code == code) {
            reader.consume(length);
            reader.emit(start, length, found->index, element);
            return found->index;
        }
    }
    return -1;
}

std::expected<int, SyntaxError> huff_codeword(BitReader& reader, const Codebook& codebook,
                                              std::string_view element,
                                              const CodewordReasons& reasons) {
    const int index = huff_decode(reader, codebook, element);
    if (index >= 0) {
        return index;
    }
    if (reader.remaining_bits() < static_cast<std::size_t>(codebook.max_bits)) {
        return fail(DecodeError::kTruncated, reasons.truncated);
    }
    return fail(DecodeError::kInvalidStream, reasons.invalid);
}

}  // namespace ac4::detail
