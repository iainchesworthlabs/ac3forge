#pragma once

#include <string_view>

#include "bit_reader.hpp"
#include "huffman_codebook.hpp"

// Huffman decoding for every codebook of both parts' Annex A. The codebooks
// and their shape are the shared core's (src/ac4core/src/huffman_codebook.hpp
// and tables/huffman_tables.hpp); reading them is the decoder's.

namespace ac4::detail {

// Reads one codeword and returns its index, recording one SyntaxRecord named
// `element` with the codeword's length and that index. Returns -1, with
// nothing consumed, when no codeword matches within the codebook's longest
// length - a corrupt stream, since Annex A's codebooks are complete.
[[nodiscard]] int huff_decode(BitReader& reader, const Codebook& codebook, std::string_view element);

}  // namespace ac4::detail
