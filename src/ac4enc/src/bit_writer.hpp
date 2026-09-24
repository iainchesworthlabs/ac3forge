#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ac4/syntax.hpp"
#include "huffman_codebook.hpp"

// Writes one substream's (or the table of contents') bits, MSB first, and
// records each syntax element it writes as an ac4::SyntaxRecord, in the shape
// the decoder's reader records what it reads (ac4/syntax.hpp): offsets from
// the start of this writer's bits, one record per element, none for
// byte_align or fill_bits.

namespace ac4::detail {

class BitWriter {
   public:
    // `substream` is the index the records carry; the table of contents'
    // writer takes a null sink, since no trace records the table of contents.
    explicit BitWriter(int substream = 0, SyntaxSink sink = {}) noexcept;

    // A writer that keeps its records instead of sending them, for bits whose
    // place in the substream is not known yet; append() sends them on.
    [[nodiscard]] static BitWriter buffered();

    // A fixed-width field. `bits` is 0 to 64; the value's bits above it must
    // be zero.
    void write(unsigned bits, std::uint64_t value, std::string_view name);

    // Part 1 clause 4.2.2's variable_bits(n_bits): `value` as groups of
    // n_bits, the most significant first, each followed by a continuation bit.
    // One record, with the total width and the value.
    void write_variable_bits(unsigned n_bits, std::uint64_t value, std::string_view name);

    // The codeword of `index` in a codebook, as tables/huffman_codes.hpp holds
    // it, recorded with its length and the index (not the codeword).
    void write_codeword(std::span<const HuffCode> codebook, std::size_t index, std::string_view name);

    // Bits nothing records: fill_bits and padding.
    void write_unrecorded(unsigned bits, std::uint64_t value);

    // An element whose bits are not its value, such as ext_code: `raw` in
    // `bits` bits, recorded with `value`.
    void write_as(unsigned bits, std::uint64_t raw, std::uint64_t value, std::string_view name);

    // byte_align: zero bits to the next byte boundary of this writer's bits.
    void align();

    // Copies another writer's bits after this one's, and sends the records it
    // kept, their offsets moved by where its bits land here.
    void append(const BitWriter& other);

    [[nodiscard]] std::size_t bit_position() const noexcept { return bits_; }
    [[nodiscard]] std::size_t byte_size() const noexcept { return (bits_ + 7) / 8; }

    // The bytes written so far, the last one zero-padded.
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

    // Starts over, keeping the substream index and the sink.
    void clear() noexcept;

    // The records a buffered writer kept.
    [[nodiscard]] std::span<const SyntaxRecord> kept() const noexcept { return kept_; }

   private:
    void put(unsigned bits, std::uint64_t value);

    void emit(const SyntaxRecord& record);

    int substream_ = 0;
    SyntaxSink sink_{};
    bool buffering_ = false;
    std::vector<SyntaxRecord> kept_;
    std::vector<std::byte> bytes_;
    std::size_t bits_ = 0;
};

// The number of bits variable_bits(n_bits) takes for `value`.
[[nodiscard]] unsigned variable_bits_width(unsigned n_bits, std::uint64_t value) noexcept;

}  // namespace ac4::detail
