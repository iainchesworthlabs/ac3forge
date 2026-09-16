#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/noise.hpp"

// What a test sink keeps between runs, in its state directory: its X25519 identity, its pairing
// PSK, its pairing records and the last-playback server (pairing.md, Pairing Records;
// connection.md, Multiple servers). Each is a small text file of hex, written readable by its
// owner only. The store is also the player's key ring.
//
// Thread-safe: sessions on different threads look up PSKs while another stores a pairing.

namespace ac3::hearth::testsink {

using sendspin::crypto::Key32;

class Store final : public sendspin::handshake::ClientKeyring {
   public:
    // The specification's minimum is five records; a sink never has more open connections
    // than this, so a record backing one is never evicted.
    static constexpr std::size_t kRecordCapacity = 16;

    // Opens `directory`, creating it and a new identity and pairing PSK when absent. An error
    // says what could not be read or written.
    [[nodiscard]] static std::expected<std::unique_ptr<Store>, std::string> open(const std::filesystem::path& directory);

    [[nodiscard]] const sendspin::noise::KeyPair& identity() const { return identity_; }
    [[nodiscard]] const Key32& pairing_psk() const { return pairing_psk_; }

    [[nodiscard]] std::optional<sendspin::handshake::PskCandidate> find(
        const sendspin::crypto::Digest32& id, std::optional<sendspin::handshake::PskCategory> category) const override;

    // Stores the record for `server_key`, replacing any earlier one and evicting the oldest past
    // the capacity. False when the file could not be written.
    bool add_record(const Key32& server_key, const Key32& long_term_psk);
    void remove_record(const Key32& server_key);
    [[nodiscard]] std::size_t records() const;

    [[nodiscard]] std::optional<Key32> last_playback() const;
    void set_last_playback(const Key32& server_key);

   private:
    struct Record {
        Key32 server_key{};
        Key32 psk{};
    };

    Store() = default;
    [[nodiscard]] bool save_records() const;

    std::filesystem::path directory_;
    sendspin::noise::KeyPair identity_;
    Key32 pairing_psk_{};
    mutable std::mutex mutex_;
    std::vector<Record> records_;
    std::optional<Key32> last_playback_;
};

// Lower-case hex of `bytes`, and back; nothing when the text is not 64 hex digits.
[[nodiscard]] std::string hex_of(const Key32& bytes);
[[nodiscard]] std::optional<Key32> key_from_hex(std::string_view text);

}  // namespace ac3::hearth::testsink
