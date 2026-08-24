#include <cstddef>
#include <cstdint>
#include <span>

#include "mp4/reader.hpp"

// mp4::demux and mp4::Reader over bytes nobody has vetted. An MP4 is a
// harder target than the Matroska sibling for one reason: its sample table
// is an INDEX rather than an in-line framing. stsc names chunks, stco names
// absolute file offsets and stsz names sizes, all self-declared and all
// resolved against each other before a single byte of audio is touched - so
// a hostile file gets to point a sample at an offset that does not exist,
// claim four billion of them, or describe a chunk map that walks off the end
// of the size table. None of that may do anything but return an error.
//
// Both entry points run on the same bytes, because they differ exactly where
// the bugs would be: demux() can reach any offset and resolves the table
// against the whole file, while Reader::push() carries parse state across
// chunk edges and has to decide, per sample, whether the bytes it names have
// arrived yet or are already gone. The chunk size comes from the input's own
// first byte so the mutation engine can steer where those edges land.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};

    if (const auto out = mp4::demux(bytes)) {
        for (const auto& sample : out->samples) {
            // Touch every byte: a span that escaped its buffer is only a bug
            // if something reads it, and ASan only reports it then.
            volatile std::byte sink{};
            for (const auto b : sample) {
                sink = b;
            }
            (void)sink;
        }
        // The parsed dac3/dec3 payload is attacker-controlled too.
        volatile int complexity = out->track.codec_config.oba_complexity_index.value_or(-1);
        (void)complexity;
    }

    const std::size_t chunk = size == 0 ? 1 : (static_cast<std::size_t>(data[0]) % 64) + 1;
    mp4::Reader reader{};
    const auto sink = [](std::span<const std::byte> sample) {
        volatile std::byte last{};
        for (const auto b : sample) {
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
