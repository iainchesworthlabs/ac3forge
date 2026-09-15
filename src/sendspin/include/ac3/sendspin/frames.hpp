#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/sendspin/dialect.hpp"

// Sendspin's transport-mode framing (messaging.md, Binary Message ID Structure
// and Fragmentation): after the Noise handshake every WebSocket binary message
// is one Noise transport message, and its plaintext starts with a message ID.
//
// A plaintext message is `[id][payload]`. One Noise message holds at most
// 65,535 bytes, 16 of them the AEAD tag, so a message longer than 65,519 bytes
// travels in fragments.
//
// Two fragment forms exist. The specification's (ID 1):
//
//     first    [1][flags][orig_id][data]
//     later    [1][flags][data]
//
// with flag bit 1 on the first fragment, bit 0 on the last, and the other bits
// zero. aiosendspin 9.1.1, which Music Assistant pins, predates it and uses two
// IDs the specification now reserves:
//
//     first    [2][orig_id][data]
//     later    [2][data]
//     last     [3][data]
//
// Hearth accepts both from any peer, and sends each peer the form its dialect
// reads (planning/hearth-sendspin-extension.md, C1). A message is reassembled in
// whichever form its first fragment used; switching form part-way is malformed.

namespace ac3::sendspin {

namespace message_id {
inline constexpr std::uint8_t kJson = 0;
inline constexpr std::uint8_t kFragment = 1;
inline constexpr std::uint8_t kLegacyFragment = 2;
inline constexpr std::uint8_t kLegacyFragmentLast = 3;
inline constexpr std::uint8_t kPlayerAudio = 4;
inline constexpr std::uint8_t kArtworkFirst = 8;
inline constexpr std::uint8_t kArtworkLast = 11;
inline constexpr std::uint8_t kSourceFirst = 12;
inline constexpr std::uint8_t kSourceLast = 15;
inline constexpr std::uint8_t kVisualizerFirst = 16;
inline constexpr std::uint8_t kVisualizerLast = 23;
inline constexpr std::uint8_t kApplicationFirst = 192;
// _ac3forge_player@v1's burst chunk (planning/hearth-sendspin-extension.md).
inline constexpr std::uint8_t kAc3forgeBurst = 192;
}  // namespace message_id

inline constexpr std::size_t kMaxNoiseMessage = 65535;
inline constexpr std::size_t kAeadTagBytes = 16;
// One frame's plaintext, message ID included.
inline constexpr std::size_t kMaxFramePlaintext = kMaxNoiseMessage - kAeadTagBytes;
// One unfragmented message's payload after its ID.
inline constexpr std::size_t kMaxFramePayload = kMaxFramePlaintext - 1;

// How many frames a message of `message_bytes` bytes (ID included) takes in
// `dialect`'s form: 1 when it fits in one frame, which is then the message itself.
[[nodiscard]] std::size_t frame_count(std::size_t message_bytes, Dialect dialect);

// Writes frame `index` of `message` in `dialect`'s form into `frame`, which must
// hold kMaxFramePlaintext bytes, and returns the bytes written; 0 when `index` is
// past the last frame or `message` is empty.
std::size_t write_frame(std::span<const std::uint8_t> message, std::size_t index,
                        std::span<std::uint8_t> frame, Dialect dialect);

enum class FrameError : std::uint8_t {
    kNone,
    kEmpty,                   // a frame with no plaintext at all
    kTruncatedFragment,       // a fragment frame too short for its header
    kReservedFlags,           // flag bits 2 to 7 set
    kFirstWhileInFlight,      // a new fragmented message before the last one ended
    kContinuationWithNone,    // a later fragment with no message in flight
    kInterleaved,             // an unfragmented message while a fragmented one is in flight
    kMixedForms,              // a fragment of the other form while one is in flight
    kBadOriginalId,           // a fragment claiming to carry a fragment ID
    kTooLarge,                // the reassembled message would exceed the receiver's limit
};

[[nodiscard]] std::string_view describe(FrameError error);

// Turns decrypted frames back into messages. Every error is one the
// specification makes fatal to the connection; after one the reassembler is
// empty again, but the caller closes.
class Reassembler {
   public:
    // `max_message_bytes` bounds one reassembled message, ID included. The
    // specification sets no limit; aiosendspin 9.1.1 uses 64 MiB.
    explicit Reassembler(std::size_t max_message_bytes) : max_(max_message_bytes) {}

    struct Result {
        FrameError error = FrameError::kNone;
        // A whole message, ID first; empty while more fragments are needed. Valid
        // until the next push() or reset().
        std::span<const std::uint8_t> message;
    };

    Result push(std::span<const std::uint8_t> frame);

    [[nodiscard]] bool in_flight() const { return state_ != State::kIdle; }
    void reset();

   private:
    enum class State : std::uint8_t { kIdle, kSpecification, kAiosendspin911 };

    Result fail(FrameError error);
    Result append(std::span<const std::uint8_t> data, bool last);

    std::vector<std::uint8_t> buffer_;
    std::size_t max_;
    State state_ = State::kIdle;
    bool delivered_ = false;
};

}  // namespace ac3::sendspin
