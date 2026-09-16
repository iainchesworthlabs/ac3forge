#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/frames.hpp"
#include "ac3/sendspin/noise.hpp"

// Sendspin's transport mode (messaging.md, Communication and Fragmentation): once the Noise
// handshake is done, every WebSocket binary message is one Noise transport message, and its
// plaintext is one frame. Channel turns a message into ciphertexts, one per frame, and
// received ciphertexts back into messages, in the connection's dialect; and it holds the
// keys, which a re-handshake replaces.
//
// Any failure it reports is one the specification makes fatal: an AEAD failure, which is
// also what a replayed or reordered ciphertext produces, or a malformed fragment sequence.
// The session closes the connection silently on either.
//
// Not synchronised: a session seals from one thread at a time and opens from one thread at a
// time, which may be different threads.

namespace ac3::sendspin {

class Channel {
   public:
    // `max_message_bytes` bounds one reassembled message, ID included.
    Channel(noise::Handshake::Transport keys, Dialect dialect, std::size_t max_message_bytes);

    [[nodiscard]] Dialect dialect() const { return dialect_; }
    // A server learns its client's dialect only from client/hello, after the channel exists.
    // The dialect decides only how outgoing messages are fragmented; incoming ones are
    // reassembled in either form.
    void set_dialect(Dialect dialect) { dialect_ = dialect; }
    // h at the end of the handshake that made the current keys: the next re-handshake's
    // prologue.
    [[nodiscard]] const crypto::Digest32& handshake_hash() const { return handshake_hash_; }

    // Seals `message`, ID first, into one ciphertext per frame, appended to `out`. False
    // only when the cipher fails, after which nothing more can be sent.
    [[nodiscard]] bool seal(std::span<const std::uint8_t> message,
                            std::vector<std::vector<std::uint8_t>>& out);
    // Seals one JSON message: ID 0, then the text.
    [[nodiscard]] bool seal_json(std::string_view json, std::vector<std::vector<std::uint8_t>>& out);

    enum class OpenError : std::uint8_t {
        kNone,
        kDecrypt,  // AEAD failure: a forged, damaged, replayed or reordered ciphertext
        kFrame,    // a frame the reassembler refuses; reassembler_error() says which
    };

    struct Opened {
        OpenError error = OpenError::kNone;
        // A whole message, ID first; empty while more fragments are needed. Valid until the
        // next open().
        std::span<const std::uint8_t> message;
    };

    [[nodiscard]] Opened open(std::span<const std::uint8_t> ciphertext);
    [[nodiscard]] FrameError reassembler_error() const { return frame_error_; }

    // Switches both directions to a re-handshake's keys. The sides switch at the same point
    // in the exchange: the client once it has sent Noise message 2, the server once it has
    // read it (connection.md, Re-handshake).
    void rekey(noise::Handshake::Transport keys);

   private:
    noise::CipherState send_;
    noise::CipherState receive_;
    crypto::Digest32 handshake_hash_{};
    Dialect dialect_;
    Reassembler reassembler_;
    FrameError frame_error_ = FrameError::kNone;
    std::vector<std::uint8_t> frame_;
    std::vector<std::uint8_t> plaintext_;
};

}  // namespace ac3::sendspin
