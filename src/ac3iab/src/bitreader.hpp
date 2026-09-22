#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "ac3iab/ac3iab.hpp"

// A minimal MSB-first bit reader scoped to one element's payload span, matching §5.1's bit-
// order rule ("bitstream data fields... shall be encoded... most-significant-bit-first...
// the first bit of the bitstream shall correspond to the most significant bit of the first
// byte"). Position 0 here always means "the start of the current element's payload" (right
// after its own ElementID/ElementSize header).
//
// That scoping is what makes AlignBits ("extra bits to get to byte alignment relative to the
// start of the element") trivial to implement: ElementSize (§10.1.2) is defined in whole
// bytes, and the per-element syntax tables in §9 place an explicit AlignBits step at each
// point a run of non-byte-multiple fields (1-bit flags, 2-bit prefixes, 16-bit positions, a
// per-channel or per-sub-block loop, ...) needs to be padded back to a byte boundary before
// the next byte-oriented field - align_to_byte() below is called at exactly those points in
// iab_reader.cpp, the same "read the loop, then align" shape
// DTSProAudio/iab-validator's own reference deserializers use (confirmed directly against
// that MIT-licensed implementation as an external oracle, not transcribed from it - see
// iab.cpp's own header comment). Because every element ends on one of those explicit
// boundaries, a complete IAElement (header + payload) always consumes a whole number of
// bytes, so sibling elements inside a SubElementCount loop come out byte-aligned to each
// other without this reader ever needing to track a position relative to anything but its
// own current element.
namespace ac3iab::detail {

class BitReader {
public:
    explicit BitReader(std::span<const std::byte> data) : data_(data) {}

    // count must be <= 64. Every field this format defines fits that bound except UserID
    // (128 bits, §10.10.1), which read_bytes() below reads directly as raw bytes instead.
    [[nodiscard]] std::expected<std::uint64_t, IabError> read_bits(unsigned count);

    // §5.2 Plex(n): reads `initial_width` bits; if that value is all-ones (the escape code for
    // that width), doubles the width and reads again, repeating until a non-escape value is
    // read. Fails with kBadEscape if the width would need to grow past 32 bits - this format's
    // own symbol ceiling (0xFFFFFFFE, §5.2) guarantees a compliant stream never escapes a
    // 32-bit field.
    [[nodiscard]] std::expected<std::uint64_t, IabError> read_plex(unsigned initial_width);

    // Aligns to the next byte boundary (a no-op if already aligned), then returns exactly
    // `count` bytes. Every byte-oriented field in this format (UserID, NUL-terminated text,
    // PCMData, the opaque AudioDataDLC payload) is placed at a point the syntax tables already
    // guarantee is byte-aligned (see this class's own header comment) - the align_to_byte()
    // call here makes that precondition explicit rather than assumed.
    [[nodiscard]] std::expected<std::span<const std::byte>, IabError> read_bytes(std::size_t count);

    void align_to_byte() { bit_pos_ = ((bit_pos_ + 7) / 8) * 8; }

    [[nodiscard]] std::size_t bits_remaining() const { return data_.size() * 8 - bit_pos_; }
    [[nodiscard]] std::size_t bytes_consumed() const { return (bit_pos_ + 7) / 8; }

private:
    std::span<const std::byte> data_;
    std::size_t bit_pos_ = 0;
};

}  // namespace ac3iab::detail
