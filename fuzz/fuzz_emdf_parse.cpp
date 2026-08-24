#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/emdf/emdf.hpp"

// ac3::emdf::parse_container (src/forge/src/emdf/emdf.cpp) over raw bytes.
//
// This is the outermost of the three metadata parsers and the one the decoder
// hands attacker-controlled bytes to first: eac3_decoder.cpp reads skipl (9
// bits, so up to 511 bytes) out of every block's skip field and passes those
// bytes here verbatim, before anything has checked a single field inside
// them. Everything the container then decides - the 16-bit
// emdf_container_length, each payload's variable-bits size, how far to skip
// for the protection field - comes out of that same untrusted buffer.
//
// Reached only indirectly through fuzz_eac3_decode, and then only for inputs
// whose CRC survived mutation; this drives it directly, so a mutation lands
// on the container's own syntax rather than on the frame carrying it.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
    (void)ac3::emdf::parse_container(bytes);
    return 0;
}
