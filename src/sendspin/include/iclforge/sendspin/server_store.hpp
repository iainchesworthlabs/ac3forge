#pragma once

#include <map>
#include <mutex>
#include <optional>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"

// What a Sendspin server remembers about its clients (pairing.md, Pairing Records and Unpaired
// Access): the long-term PSK of each pairing, the pairing PSK an operator entered from a client's
// SP:0 token, and which clients the operator has approved for unpaired access. It is also the
// key ring the handshake names a PSK from: a pairing's long-term PSK, else an entered pairing PSK,
// else the Sentinel.
//
// MemoryServerStore keeps them for the life of the process, which is what the tests need; an
// application persists them by implementing this with its own storage.

namespace ac3::sendspin {

class ServerStore : public handshake::ServerKeyring {
   public:
    // Stores the record of a completed pairing, replacing any earlier one, and discards the
    // client's approval and entered pairing PSK. False when it could not be stored.
    [[nodiscard]] virtual bool store_record(const crypto::Key32& client_key, const crypto::Key32& long_term_psk) = 0;
    virtual void remove_record(const crypto::Key32& client_key) = 0;
    [[nodiscard]] virtual bool paired(const crypto::Key32& client_key) const = 0;

    // The pairing PSK from a client's token, to be named in the next handshake; none clears it.
    virtual void set_pairing_psk(const crypto::Key32& client_key, std::optional<crypto::Key32> pairing_psk) = 0;
    [[nodiscard]] virtual bool has_pairing_psk(const crypto::Key32& client_key) const = 0;

    [[nodiscard]] virtual bool approved(const crypto::Key32& client_key) const = 0;
    virtual void set_approved(const crypto::Key32& client_key, bool approved) = 0;
};

class MemoryServerStore final : public ServerStore {
   public:
    [[nodiscard]] handshake::PskChoice choose(const crypto::Key32& client_key) const override;

    [[nodiscard]] bool store_record(const crypto::Key32& client_key, const crypto::Key32& long_term_psk) override;
    void remove_record(const crypto::Key32& client_key) override;
    [[nodiscard]] bool paired(const crypto::Key32& client_key) const override;

    void set_pairing_psk(const crypto::Key32& client_key, std::optional<crypto::Key32> pairing_psk) override;
    [[nodiscard]] bool has_pairing_psk(const crypto::Key32& client_key) const override;

    [[nodiscard]] bool approved(const crypto::Key32& client_key) const override;
    void set_approved(const crypto::Key32& client_key, bool approved) override;

   private:
    mutable std::mutex mutex_;
    std::map<crypto::Key32, crypto::Key32> records_;
    std::map<crypto::Key32, crypto::Key32> pairing_psks_;
    std::map<crypto::Key32, bool> approvals_;
};

}  // namespace ac3::sendspin
