#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <optional>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/noise.hpp"

// What a Sendspin player keeps across a reboot, in NVS: its X25519 identity,
// its pairing PSK, its pairing records and the last-playback server
// (pairing.md, Pairing Records; connection.md, Multiple servers). It is the
// board's form of the test sink's state directory (apps/hearth/testsink/
// store.hpp), and like that one it is also the player's key ring.
//
// The identity and the pairing PSK come from the hardware RNG on a board's
// first boot and stay until forget(). A board's client_id is its identity's
// public key, and every server's pairing record names it, so a reflash that
// keeps the NVS partition keeps the board's pairings.
//
// Everything is held in RAM once loaded, in fixed-size arrays: a handshake
// looks a PSK up without touching flash or the heap. A write goes to NVS at
// once, from whichever task made it, which must be one whose stack is in
// internal RAM - NVS runs with the flash cache off.
//
// NVS is not encrypted unless the project turns on NVS encryption, so anyone
// holding the board can read these keys. docs/threat-model.md says what that
// exposes.
//
// Thread-safe: a handshake on the Sendspin server's task looks a PSK up while
// the control surface's task counts the records.

namespace ac3forge {

class SendspinStore final : public ac3::sendspin::handshake::ClientKeyring {
   public:
    using Key32 = ac3::sendspin::crypto::Key32;

    // pairing.md's minimum is five. A board holds at most three connections at
    // once (sendspin_host.hpp), so a record backing one is never evicted.
    static constexpr std::size_t kRecordCapacity = 8;

    // Reads everything from NVS, making and storing an identity and a pairing
    // PSK when there are none. nvs_flash_init() has run already. False, having
    // said why on the console, when NVS cannot be opened or the RNG fails.
    [[nodiscard]] bool load();

    [[nodiscard]] const ac3::sendspin::noise::KeyPair& identity() const { return identity_; }
    [[nodiscard]] const Key32& pairing_psk() const { return pairing_psk_; }

    [[nodiscard]] std::optional<ac3::sendspin::handshake::PskCandidate> find(
        const ac3::sendspin::crypto::Digest32& id,
        std::optional<ac3::sendspin::handshake::PskCategory> category) const override;

    // Stores the record for `server_key`, replacing any earlier one and
    // evicting the oldest past the capacity. False when NVS refused it; the
    // record is still held for this boot.
    bool add_record(const Key32& server_key, const Key32& long_term_psk);
    void remove_record(const Key32& server_key);
    [[nodiscard]] std::size_t records() const;

    [[nodiscard]] std::optional<Key32> last_playback() const;
    void set_last_playback(const Key32& server_key);

    // Every pairing record dropped, and a new identity and pairing PSK: the
    // board is a stranger to every server that knew it. False when NVS
    // refused.
    [[nodiscard]] bool forget();

   private:
    struct Record {
        Key32 server_key{};
        Key32 psk{};
    };

    [[nodiscard]] bool save_records() const;
    [[nodiscard]] bool create_keys();

    mutable std::mutex mutex_;
    ac3::sendspin::noise::KeyPair identity_;
    Key32 pairing_psk_{};
    std::array<Record, kRecordCapacity> records_{};
    std::size_t record_count_ = 0;
    std::optional<Key32> last_playback_;
};

}  // namespace ac3forge
