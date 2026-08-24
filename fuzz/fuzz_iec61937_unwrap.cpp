#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/sinks/iec61937.hpp"

// ac3::iec61937::BurstReader, driven the way ac3cli's 'unspdif' drives it
// (src/forge/src/sinks/iec61937.cpp).
//
// This is the one entry point in the project whose input is, by definition,
// something that came off a wire: an S/PDIF or HDMI capture, or a file
// somebody saved from one. Two of its four preamble words are attacker-
// chosen - Pc picks the data type and Pd states a length - and the length is
// exactly the number a naive parser would hand to resize(). Bounding that
// against the data type's repetition period is the property this harness
// exists to keep honest.
//
// Split into two chunks at a mutation-chosen point rather than pushed whole:
// the parser's state machine has to carry a preamble, a header or a payload
// across a chunk boundary, and a fuzzer that only ever fed it one buffer
// would never reach the carry paths at all.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    // First byte steers the split; the rest is carrier. Deriving the split
    // from the input keeps the harness deterministic, which a corpus entry
    // that reproduces a crash depends on.
    const std::size_t body = size - 1;
    const std::size_t split = body == 0 ? 0 : (static_cast<std::size_t>(data[0]) * body) / 256;
    const std::span<const std::byte> carrier{reinterpret_cast<const std::byte*>(data + 1), body};

    ac3::iec61937::BurstReader reader;
    std::vector<std::byte> out;
    if (reader.push(carrier.first(split), out)) {
        // Drained between pushes, exactly as the CLI drains it into its sink:
        // a harness that let `out` grow would be measuring the vector rather
        // than the parser.
        out.clear();
        if (reader.push(carrier.subspan(split), out)) {
            (void)reader.finish();
        }
    }

    // The batch form too: same parser, but it accumulates the whole
    // elementary stream, so a payload length that escaped its bound would
    // show up here as the allocation it is.
    (void)ac3::iec61937::unwrap_stream(carrier);
    return 0;
}
