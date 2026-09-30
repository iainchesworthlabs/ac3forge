#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// The shape of every Huffman codebook of both parts' Annex A, shared by the
// decoder, which reads codewords (src/ac4dec/src/huffman.hpp), and the encoder,
// which writes them.
//
// Part 1 Annex A.0: each codebook is a table of codeword lengths and a table
// of codewords, indexed from 0, and huff_decode() returns the index of the
// codeword read (Part 1 clause 4.3.6.4.2); an encoder writes the codeword of
// an index, from tables/huffman_codes.hpp. The tables themselves are
// generated from the ETSI attachment by tools/generators/gen_ac4_tables.py
// into tables/huffman_tables.cpp, together with the per-codebook values Annex
// A prints beside them (cb_off, cb_mod, and for the ASF spectrum codebooks
// their dimension and signedness from Tables A.14 and A.15).
//
// The generator also sorts each codebook's entries by (length, codeword) and
// records where each length starts, and each codebook carries a shortcut for its
// short codewords, built at compile time from the sorted entries
// (make_fast_table): decoding reads the next kHuffFastBits bits and looks them up,
// which settles every codeword of that length or less at once. A longer codeword
// falls back to the search by length: at each length, look the leading bits up
// among the entries of that length. Nothing is built at run time.

namespace iclforge::ac4::detail {

struct HuffEntry {
    std::uint32_t code = 0;   // the codeword, right-aligned in `bits` bits, MSB first
    std::uint16_t index = 0;  // its position in the codebook: what huff_decode returns
    std::uint8_t bits = 0;
};

// One entry of a codebook in index order (tables/huffman_codes.hpp), which is
// how an encoder looks a codeword up.
struct HuffCode {
    std::uint32_t code = 0;  // right-aligned in `bits` bits, MSB first
    std::uint8_t bits = 0;
};

inline constexpr int kMaxHuffBits = 32;

// The bits a codebook's shortcut table is indexed by: 256 entries of 2 bytes.
inline constexpr int kHuffFastBits = 8;
inline constexpr std::size_t kHuffFastSize = std::size_t{1} << kHuffFastBits;

// Entry v of a shortcut table, for the kHuffFastBits bits that read as v, is
// (index << 4) | length of the codeword those bits begin, when that codeword is
// kHuffFastBits long or shorter; and 0, which no codeword's is (a length is at
// least 1), where the codeword is longer or the bits begin none. Annex A's codes
// are prefix codes, so no two codewords claim the same entry. An index is at most
// 288, and a length at most kHuffFastBits, so both fit.
template <std::size_t N>
[[nodiscard]] constexpr std::array<std::uint16_t, kHuffFastSize> make_fast_table(
    const std::array<HuffEntry, N>& entries) noexcept {
    std::array<std::uint16_t, kHuffFastSize> table{};
    for (const HuffEntry& entry : entries) {
        if (entry.bits == 0 || entry.bits > kHuffFastBits) {
            continue;
        }
        const int spare = kHuffFastBits - entry.bits;
        const std::size_t first = static_cast<std::size_t>(entry.code) << spare;
        const std::size_t count = std::size_t{1} << spare;
        for (std::size_t i = 0; i < count; ++i) {
            table[first + i] = static_cast<std::uint16_t>((entry.index << 4U) | entry.bits);
        }
    }
    return table;
}

struct Codebook {
    std::string_view name;               // Annex A's codebook name, e.g. "ASF_HCB_1"
    std::span<const HuffEntry> sorted;   // every entry, sorted by (bits, code)
    // sorted[length_start[L] .. length_start[L + 1]) are the entries of length L.
    std::array<std::uint16_t, kMaxHuffBits + 2> length_start{};
    std::uint16_t codebook_length = 0;   // Annex A's codebook_length
    std::uint8_t max_bits = 0;
    // Annex A's per-codebook values. Zero where the table prints none.
    std::int16_t cb_off = 0;
    std::int16_t cb_mod = 0;
    std::int16_t cb_mod2 = 0;
    std::int16_t cb_mod3 = 0;
    // The shortcut for codewords of kHuffFastBits or fewer (make_fast_table);
    // empty in a Codebook built without one, which decodes by search alone.
    std::span<const std::uint16_t> fast{};
};

}  // namespace iclforge::ac4::detail
