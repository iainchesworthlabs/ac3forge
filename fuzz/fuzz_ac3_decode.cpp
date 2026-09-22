#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/decoder/decoder.hpp"
#include "crc_mutator.hpp"

// Mirrors ac3cli's own 'decode' path (apps/cli/main.cpp: run_decode): split the
// raw stream into syncframes, then decode each one with a single FrameDecoder
// so overlap-add state carries across frames exactly as it does for a real
// caller. A malformed differential exponent chain walking the reconstruction
// outside 0..24 (the bug fixed in 8386c8f) is exactly the class of input this
// is meant to keep catching.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};
    const auto frames = ac3::split_frames(bytes);
    if (!frames) {
        return 0;
    }
    ac3::FrameDecoder decoder;
    for (const auto& frame : *frames) {
        (void)decoder.decode_frame(frame);
    }
    return 0;
}

// Re-stamp crc1/crc2 after mutating, so a mutation aimed at the frame's
// contents is not thrown away by the checksum guarding them - see
// crc_mutator.hpp for the mechanism and for why one mutation in four is
// deliberately left unrepaired.
extern "C" std::size_t LLVMFuzzerCustomMutator(std::uint8_t* data, std::size_t size,
                                               std::size_t max_size, unsigned int seed) {
    return ac3fuzz::crc_repairing_mutate(data, size, max_size, seed);
}
