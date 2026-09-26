#pragma once

#include <expected>
#include <string_view>

#include "bit_reader.hpp"
#include "huffman_codebook.hpp"
#include "syntax/context.hpp"

// Huffman decoding for every codebook of both parts' Annex A. The codebooks
// and their shape are the shared core's (src/ac4core/src/huffman_codebook.hpp
// and tables/huffman_tables.hpp); reading them is the decoder's.

namespace ac4::detail {

// Reads one codeword and returns its index, recording one SyntaxRecord named
// `element` with the codeword's length and that index. Returns -1, with
// nothing consumed, when no codeword matches within the codebook's longest
// length - a corrupt stream, since Annex A's codebooks are complete.
[[nodiscard]] int huff_decode(BitReader& reader, const Codebook& codebook, std::string_view element);

// The reasons a tool gives when no codeword matches, string literals both.
struct CodewordReasons {
    std::string_view truncated;  // the substream ended inside a codeword
    std::string_view invalid;    // the bits are no codeword of the codebook
};

// huff_decode() as every tool reads a codeword, ASF, A-SPX, A-CPL, DRC and
// dialogue enhancement alike: its index, or where none matches one error for
// one failure. Annex A's codebooks are complete, so a miss with fewer bits left
// than the codebook's longest codeword is the substream ending inside one,
// kTruncated, as any other element running past the end is; a miss with more
// left is kInvalidStream.
[[nodiscard]] std::expected<int, SyntaxError> huff_codeword(BitReader& reader,
                                                            const Codebook& codebook,
                                                            std::string_view element,
                                                            const CodewordReasons& reasons);

}  // namespace ac4::detail
