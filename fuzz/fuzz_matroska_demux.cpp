#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "matroska/reader.hpp"

// matroska::demux and matroska::Reader are the first thing to touch a
// container nobody has vetted: a disc rip, a broadcast capture, an HTTP
// download. Every length in an EBML file is self-declared, so this is the
// entry point where a hostile input gets to ask for an out-of-bounds read
// (a lace whose declared sizes overrun its block), an unbounded allocation
// (an element claiming to be gigabytes) or unbounded recursion (masters
// nested until a walker's stack gives out). None of those may do anything
// but return an error.
//
// Both entry points run, on the same bytes, because they are NOT the same
// code path at the boundaries: demux() sees the whole input at once, while
// Reader::push() has to keep parse state across chunk edges that can fall
// anywhere - inside an id, inside a size vint, inside a frame. The
// chunking below is driven by the input's own first byte so the mutation
// engine can steer where those edges land.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};

    // Batch, zero-copy: frames come back as views into `bytes`.
    if (const auto out = matroska::demux(bytes)) {
        for (const auto& frame : out->frames) {
            // Touch every frame: a span that escaped its buffer is only a
            // bug if something reads it, and ASan only reports it then.
            volatile std::byte sink{};
            for (const auto b : frame) {
                sink = b;
            }
            (void)sink;
        }
    }

    // Incremental, over chunk boundaries the input itself picks.
    const std::size_t chunk = size == 0 ? 1 : (static_cast<std::size_t>(data[0]) % 64) + 1;
    matroska::Reader reader{};
    const auto sink = [](std::span<const std::byte> frame) {
        volatile std::byte last{};
        for (const auto b : frame) {
            last = b;
        }
        (void)last;
    };
    for (std::size_t offset = 0; offset < size; offset += chunk) {
        const auto take = chunk < size - offset ? chunk : size - offset;
        if (!reader.push(bytes.subspan(offset, take), sink)) {
            return 0;  // a rejected file is the expected outcome, not a finding
        }
    }
    (void)reader.finish();
    return 0;
}
