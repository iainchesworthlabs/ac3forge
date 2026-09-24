#pragma once

#include <array>
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
// records where each length starts, so decoding needs no table built at run
// time: read one bit at a time, and at each length look the accumulated code
// up among the entries of that length.

namespace ac4::detail {

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
};

}  // namespace ac4::detail
