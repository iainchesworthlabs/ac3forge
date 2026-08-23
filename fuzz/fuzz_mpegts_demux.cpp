#include <cstddef>
#include <cstdint>
#include <span>

#include "mpegts/reader.hpp"

// mpegts::demux and mpegts::Reader over bytes nobody has vetted.
//
// A transport stream is the container most likely to arrive damaged - it is
// designed to be tuned into mid-flight and to survive bit errors - so
// "malformed" here is the ordinary case rather than the exceptional one, and
// the reader's whole job is to keep making progress through it. That makes
// this the harness most likely to find a real hang rather than a crash: the
// sync search, the PSI section reassembly and the PES reassembly are all
// loops driven by self-declared lengths, and each has a way to be told to
// never finish. The unbounded PES_packet_length form is the sharpest of
// them - it ends only when the next payload_unit_start_indicator arrives,
// which a hostile stream simply never sends.
//
// Both entry points run on the same bytes: demux() sees everything at once,
// while Reader::push() has to hold partial packets, partial sections and a
// partial PES across chunk edges that can fall anywhere. The chunk size
// comes from the input's own first byte so the mutation engine can steer
// where those edges land.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(data), size};

    if (const auto out = mpegts::demux(bytes)) {
        for (const auto& payload : out->payloads) {
            volatile std::byte sink{};
            for (const auto b : payload) {
                sink = b;
            }
            (void)sink;
        }
    }

    const std::size_t chunk = size == 0 ? 1 : (static_cast<std::size_t>(data[0]) % 64) + 1;
    mpegts::Reader reader{};
    const auto sink = [](std::span<const std::byte> payload) {
        volatile std::byte last{};
        for (const auto b : payload) {
            last = b;
        }
        (void)last;
    };
    for (std::size_t offset = 0; offset < size; offset += chunk) {
        const auto take = chunk < size - offset ? chunk : size - offset;
        if (!reader.push(bytes.subspan(offset, take), sink)) {
            return 0;  // a rejected stream is the expected outcome, not a finding
        }
    }
    (void)reader.finish(sink);
    return 0;
}
