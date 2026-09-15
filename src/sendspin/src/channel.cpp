#include "ac3/sendspin/channel.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
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
      reassembler_(max_message_bytes),
      frame_(kMaxFramePlaintext) {}

bool Channel::seal(std::span<const std::uint8_t> message, std::vector<std::vector<std::uint8_t>>& out) {
    const std::size_t frames = frame_count(message.size(), dialect_);
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
