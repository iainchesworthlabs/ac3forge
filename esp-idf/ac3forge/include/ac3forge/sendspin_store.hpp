#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3forge/pairing_records.hpp"

// What a Sendspin player keeps across a reboot, in NVS: its X25519 identity,
// its pairing PSK, its pairing records, the names of the servers they are with,
// and the last-playback server (pairing.md, Pairing Records; connection.md,
// Multiple servers). It is the board's form of the test sink's state directory
// (apps/hearth/testsink/store.hpp), and like that one it is also the player's
// key ring.
//
// The identity and the pairing PSK come from the hardware RNG on a board's
// first boot and stay until forget(). A board's client_id is its identity's
// public key, and every server's pairing record names it, so a reflash that
// keeps the NVS partition keeps the board's pairings.
//
// The records are held in RAM once loaded, in a fixed-size array, least
// recently used first (pairing_records.hpp): a handshake looks a PSK up without
// touching flash or the heap. The servers' names are not: they are read from
// NVS when someone asks for the list, and written when a server pairs or comes
// back under a name the board does not have. A write goes to NVS at once, from
// whichever task made it, which must be one whose stack is in internal RAM -
// NVS runs with the flash cache off.
//
// NVS is not encrypted unless the project turns on NVS encryption, so anyone
// holding the board can read these keys. docs/threat-model.md says what that
// exposes.
//
// Thread-safe: a handshake on the Sendspin server's task looks a PSK up while
// the control surface's task lists the records.

namespace ac3forge {

class SendspinStore final : public ac3::sendspin::handshake::ClientKeyring {
   public:
    using Key32 = ac3::sendspin::crypto::Key32;

    // pairing.md's minimum is five. A board holds at most three connections at
    // once (sendspin_host.hpp), so there is always a record no connection rests
    // on to evict.
    static constexpr std::size_t kRecordCapacity = 8;
    using Names = ServerNames<kRecordCapacity>;

    // Reads everything from NVS, making and storing an identity and a pairing
    // PSK when there are none. nvs_flash_init() has run already. False, having
    // said why on the console, when NVS cannot be opened or the RNG fails.
    [[nodiscard]] bool load();

    [[nodiscard]] const ac3::sendspin::noise::KeyPair& identity() const { return identity_; }
    [[nodiscard]] const Key32& pairing_psk() const { return pairing_psk_; }

    [[nodiscard]] std::optional<ac3::sendspin::handshake::PskCandidate> find(
        const ac3::sendspin::crypto::Digest32& id,
        std::optional<ac3::sendspin::handshake::PskCategory> category) const override;

    // Stores the record for `server_key` as the most recently used, replacing
    // any earlier one. At the capacity the least recently used record goes,
    // bar any a connection still open rests on (`in_use`). False when NVS
    // refused it; the record is still held for this boot.
    bool add_record(const Key32& server_key, const Key32& long_term_psk, std::span<const Key32> in_use = {});
    // A connection `server_key`'s record authenticated, which the board
    // admitted: the record is now the most recently used, which is written to
    // NVS only when that moved it.
    void touch(const Key32& server_key);
    // One the board refused: the record is marked seen, in RAM only.
    void saw(const Key32& server_key);
    // True when there was a record for `server_key`. It goes with its
    // server's name, and so does the last-playback server when it was that one.
    bool remove_record(const Key32& server_key);
    [[nodiscard]] bool has_record(const Key32& server_key) const;
    [[nodiscard]] std::size_t records() const;

    // Each record's server, and whether the board has used the record since
    // it started, the most recently used first. Returns how many it wrote.
    struct Listed {
        Key32 server_key{};
        bool seen = false;
    };
    std::size_t list(std::span<Listed> out) const;

    // What `server_key`'s server calls itself, as its hello said, kept beside
    // its record. Written only when it changed, and not at all for a server
    // with no record.
    void set_name(const Key32& server_key, std::string_view name);
    // The names, from NVS. False when NVS could not be read; `out` is then
    // empty.
    bool read_names(Names& out) const;

    [[nodiscard]] std::optional<Key32> last_playback() const;
    void set_last_playback(const Key32& server_key);

    // Every pairing record and name dropped, and a new identity and pairing
    // PSK: the board is a stranger to every server that knew it. False when
    // NVS refused.
    [[nodiscard]] bool forget();

   private:
    [[nodiscard]] bool save_records() const;
    [[nodiscard]] bool create_keys();

    mutable std::mutex mutex_;
    ac3::sendspin::noise::KeyPair identity_;
    Key32 pairing_psk_{};
    PairingRecords<kRecordCapacity> records_;
    std::optional<Key32> last_playback_;
};

}  // namespace ac3forge
