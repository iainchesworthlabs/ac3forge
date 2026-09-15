#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/frames.hpp"

// ac3::sendspin::Reassembler, parse_player_chunk and parse_burst_chunk
// (src/sendspin/src/frames.cpp, chunks.cpp) - everything a decrypted Sendspin
// frame meets before a session looks at it: fragment reassembly in both the
// specification's form and aiosendspin 9.1.1's, and the two audio chunk parsers,
// whose length fields a peer chooses.
//
// The input is a sequence of frames, each preceded by a two-byte little-endian
// length, after one control byte: its low two bits pick the reassembly limit.
// Every delivered message goes through both chunk parsers, player@v1's in both
// dialects' header forms. When bits 2 to 4 of
// the control byte are all set, the whole input is also treated as one message,
// repeated past the size that needs fragments, and split and reassembled in both
// forms; the result must be the message. That costs a megabyte of copying, so it
// is kept to one input in eight: on every input it held the harness to a few
// hundred executions a second.

namespace {

using ac3::sendspin::Dialect;
using ac3::sendspin::FrameError;
using ac3::sendspin::Reassembler;

void inspect(std::span<const std::uint8_t> message) {
    for (const Dialect dialect : {Dialect::kSpecification, Dialect::kAiosendspin911}) {
        if (const auto chunk = ac3::sendspin::parse_player_chunk(message, dialect)) {
            if (chunk->data.size() + ac3::sendspin::audio_chunk_header_bytes(dialect) !=
                message.size()) {
                std::abort();
            }
        }
    }
    if (const auto burst = ac3::sendspin::parse_burst_chunk(message)) {
        const auto payload = burst->chunk.data;
        if (payload.size() > ac3::sendspin::max_burst_payload(burst->data_type()) ||
            burst->pd != ac3::sendspin::burst_length_code(burst->data_type(), payload.size())) {
            std::abort();
        }
    }
}

void round_trip(std::span<const std::uint8_t> input, Dialect dialect) {
    // Past two frames' worth, so a middle fragment exists.
    const std::size_t target = (2 * ac3::sendspin::kMaxFramePlaintext) + 7;
    std::vector<std::uint8_t> message;
    message.reserve(target);
    while (message.size() < target) {
        const std::size_t n = std::min(input.size(), target - message.size());
        message.insert(message.end(), input.begin(), input.begin() + static_cast<std::ptrdiff_t>(n));
    }
    // A message whose own ID is a fragment ID cannot be sent.
    if (message[0] <= ac3::sendspin::message_id::kLegacyFragmentLast) {
        message[0] = ac3::sendspin::message_id::kJson;
    }

    static std::vector<std::uint8_t> frame(ac3::sendspin::kMaxFramePlaintext);
    Reassembler reassembler(message.size());
    const std::size_t count = ac3::sendspin::frame_count(message.size(), dialect);
    std::span<const std::uint8_t> delivered;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t written = ac3::sendspin::write_frame(message, i, frame, dialect);
        if (written == 0 || written > frame.size()) {
            std::abort();
        }
        const Reassembler::Result result =
            reassembler.push(std::span<const std::uint8_t>(frame.data(), written));
        if (result.error != FrameError::kNone || result.message.empty() != (i + 1 < count)) {
            std::abort();
        }
        delivered = result.message;
    }
    if (!std::equal(delivered.begin(), delivered.end(), message.begin(), message.end())) {
        std::abort();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> input{data, size};
    if (input.empty()) {
        return 0;
    }

    static constexpr std::array<std::size_t, 4> kLimits{16, 1024, 70000, 1 << 20};
    Reassembler reassembler(kLimits[input[0] & 3U]);
    std::span<const std::uint8_t> rest = input.subspan(1);
    while (rest.size() >= 2) {
        const std::size_t length = std::min<std::size_t>(
            rest.size() - 2, static_cast<std::size_t>(rest[0] | (rest[1] << 8U)));
        const std::span<const std::uint8_t> frame = rest.subspan(2, length);
        rest = rest.subspan(2 + length);
        const Reassembler::Result result = reassembler.push(frame);
        if (result.error != FrameError::kNone) {
            if (reassembler.in_flight()) {
                std::abort();
            }
            continue;
        }
        if (!result.message.empty()) {
            inspect(result.message);
        }
    }

    inspect(input);
    if ((input[0] & 0x1CU) == 0x1CU) {
        round_trip(input, Dialect::kSpecification);
        round_trip(input, Dialect::kAiosendspin911);
    }
    return 0;
}
