#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/server_store.hpp"
#include "settings_model.hpp"

// The pairing records Hearth keeps as a Sendspin server: the Settings page's
// "Pairing records", and the plan's Security section - "in the app's
// settings directory, readable by the user only".
//
// A record is written through a SettingsStore as soon as its pairing
// completes, and read back when the store is made. The window gives this a
// store of its own: a settings file in its settings folder that only the
// person's account can read. The keys sit under "pairing/", as QSettings
// writes an array: "pairing/size", then "pairing/1/client", "pairing/1/psk",
// "pairing/1/name" and "pairing/1/paired", with the keys in hex. The
// diagnostics file withholds everything under "pairing/".
//
// What pairing.md does not keep beyond a session is kept in memory only, as
// sendspin::MemoryServerStore keeps it: a pairing PSK typed in from a
// client's token, and approval for unpaired access.
//
// Thread-safe. The network's sessions look keys up while the window lists and
// forgets records. The settings store is used only under this object's lock,
// so it must be one that nothing else uses.

namespace ac3::hearth {

// A record as the Settings page lists it.
struct PairingRecordView {
    sendspin::crypto::Key32 client_key{};
    // What the client called itself when it paired.
    std::string name{};
    // When it paired, as `today` gave it.
    std::string paired_on{};

    friend bool operator==(const PairingRecordView&, const PairingRecordView&) = default;
};

class PairingStore final : public sendspin::ServerStore {
public:
    // `store` outlives this. `today` gives the date a new record is stamped
    // with, as the page shows it.
    PairingStore(SettingsStore& store, std::function<std::string()> today);
    // Wipes the keys it holds.
    ~PairingStore() override;
    PairingStore(const PairingStore&) = delete;
    PairingStore& operator=(const PairingStore&) = delete;
    PairingStore(PairingStore&&) = delete;
    PairingStore& operator=(PairingStore&&) = delete;

    [[nodiscard]] sendspin::handshake::PskChoice choose(
        const sendspin::crypto::Key32& client_key) const override;

    // Also false when the record could not be written. The store then keeps
    // what it had before, so a pairing that would not outlive the process is
    // not taken for one that will.
    [[nodiscard]] bool store_record(const sendspin::crypto::Key32& client_key,
                                    const sendspin::crypto::Key32& long_term_psk) override;
    void remove_record(const sendspin::crypto::Key32& client_key) override;
    [[nodiscard]] bool paired(const sendspin::crypto::Key32& client_key) const override;

    void set_pairing_psk(const sendspin::crypto::Key32& client_key,
                         std::optional<sendspin::crypto::Key32> pairing_psk) override;
    [[nodiscard]] bool has_pairing_psk(const sendspin::crypto::Key32& client_key) const override;

    [[nodiscard]] bool approved(const sendspin::crypto::Key32& client_key) const override;
    void set_approved(const sendspin::crypto::Key32& client_key, bool approved) override;

    // The name a client gave in its hello: its record, when the pairing
    // completes, carries it, and a record already made takes it.
    void set_client_name(const sendspin::crypto::Key32& client_key, const std::string& name);

    // The records, in the order they were made.
    [[nodiscard]] std::vector<PairingRecordView> records() const;

    // Forgets a pairing: the client has to pair again, with a new code.
    void forget(const sendspin::crypto::Key32& client_key) { remove_record(client_key); }

private:
    struct Record {
        sendspin::crypto::Key32 client_key{};
        sendspin::crypto::Key32 psk{};
        std::string name;
        std::string paired_on;
    };

    // Writes every record; false when the store did not take them.
    [[nodiscard]] bool save_locked();
    [[nodiscard]] std::vector<Record>::iterator find_locked(const sendspin::crypto::Key32& key);
    [[nodiscard]] std::vector<Record>::const_iterator find_locked(
        const sendspin::crypto::Key32& key) const;

    SettingsStore& store_;
    std::function<std::string()> today_;
    mutable std::mutex mutex_;
    std::vector<Record> records_;
    std::map<sendspin::crypto::Key32, std::string> names_;
    std::map<sendspin::crypto::Key32, sendspin::crypto::Key32> pairing_psks_;
    std::map<sendspin::crypto::Key32, bool> approvals_;
};

}  // namespace ac3::hearth
