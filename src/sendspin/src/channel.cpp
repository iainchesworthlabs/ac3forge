#include "ac3/sendspin/channel.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/frames.hpp"
#include "ac3/sendspin/noise.hpp"

namespace ac3::sendspin {

Channel::Channel(noise::Handshake::Transport keys, Dialect dialect, std::size_t max_message_bytes)
    : send_(std::move(keys.send)),
      receive_(std::move(keys.receive)),
      handshake_hash_(keys.handshake_hash),
      dialect_(dialect),
      reassembler_(max_message_bytes) {}

bool Channel::seal(std::span<const std::uint8_t> message, std::vector<std::vector<std::uint8_t>>& out) {
    if (message.empty()) {
        return false;
    }
    const std::size_t frames = frame_count(message.size(), dialect_);
    if (frames == 1) {
        // A message that fits one frame is that frame, sealed as it stands. The scratch frame
        // below is only for fragments: allocated with the channel, it cost a board 64 KB per
        // connection for messages no longer than a client/state.
        std::vector<std::uint8_t> ciphertext;
        ciphertext.reserve(message.size() + kAeadTagBytes);
        if (!send_.encrypt(message, ciphertext)) {
            return false;
        }
        out.push_back(std::move(ciphertext));
        return true;
    }
    frame_.resize(kMaxFramePlaintext);
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t written = write_frame(message, i, frame_, dialect_);
        std::vector<std::uint8_t> ciphertext;
        ciphertext.reserve(written + kAeadTagBytes);
        if (written == 0 ||
            !send_.encrypt(std::span<const std::uint8_t>(frame_.data(), written), ciphertext)) {
            return false;
        }
        out.push_back(std::move(ciphertext));
    }
    return true;
}

bool Channel::seal_json(std::string_view json, std::vector<std::vector<std::uint8_t>>& out) {
    // Built from its ID and then the text: GCC 15 misreads a reserve() followed by push_back()
    // here as freeing a pointer that is not the allocation's start, and a vector sized
    // json.size() + 1 as possibly empty.
    std::vector<std::uint8_t> message(1, message_id::kJson);
    message.insert(message.end(), json.begin(), json.end());
    return seal(message, out);
}

Channel::Opened Channel::open(std::span<const std::uint8_t> ciphertext) {
    plaintext_.clear();
    if (!receive_.decrypt(ciphertext, plaintext_)) {
        return {.error = OpenError::kDecrypt, .message = {}};
    }
    const Reassembler::Result result = reassembler_.push(plaintext_);
    if (result.error != FrameError::kNone) {
        frame_error_ = result.error;
        return {.error = OpenError::kFrame, .message = {}};
    }
    return {.error = OpenError::kNone, .message = result.message};
}

void Channel::rekey(noise::Handshake::Transport keys) {
    send_ = std::move(keys.send);
    receive_ = std::move(keys.receive);
    handshake_hash_ = keys.handshake_hash;
}

}  // namespace ac3::sendspin
