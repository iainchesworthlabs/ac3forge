#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/oba/joc.hpp"

// ac3::oba::joc::parse_payload (src/forge/src/oba/joc.cpp) - the joc() payload of
// TS 103 420 §6, as recovered from an EMDF payload with id 14.
//
// The widest of the three metadata parsers by a distance, and the only one
// that Huffman-decodes: joc_num_objects, joc_num_bands_idx and the
// per-object/per-band quantized coefficients all drive variable-length table
// walks and a matrix sized from the stream's own numbers, over bytes that
// arrived through a container carrying no checksum (see fuzz_oamd_parse.cpp
// for that reasoning in full).
//
// parse_payload only, not oba::joc::reconstruct: reconstruct takes an already-
// parsed FrameParameters plus PCM, so it is reached from a decoded frame
// rather than from a byte span, and fuzz_eac3_decode already drives that
// path end to end.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
    (void)ac3::oba::joc::parse_payload(bytes);
    return 0;
}
