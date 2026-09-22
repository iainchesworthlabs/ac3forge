#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/oba/scene_osc.hpp"

// ac3::oba::parse_osc_packet (src/forge/src/oba/scene_osc.cpp) - one UDP
// datagram's worth of OSC 1.0, as ac3::audio::LivePositionSource hands it
// over the instant a byte arrives on the socket it opened for
// `positions=osc:<port>`. No CRC, no container, no length field checked by
// anything upstream: this is the project's first parser whose input reaches
// it straight off the network rather than from a bitstream this project (or
// a cooperating encoder) produced, and its own header comment's bounds -
// never reading past the packet, never recursing past a hard bundle-depth
// cap - are exactly what this harness exists to keep honest.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
    (void)ac3::oba::parse_osc_packet(bytes);
    return 0;
}
