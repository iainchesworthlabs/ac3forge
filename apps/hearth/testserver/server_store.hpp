#pragma once

#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/server_store.hpp"

// What the test server keeps between runs, in its state directory: its X25519 identity and its
// pairing records, so that a board paired once plays again without pairing (pairing.md, Pairing
// Records). The pairing PSKs entered from tokens and the approvals of unpaired access last the run.
// One text file of hex, written readable by its owner only.

namespace ac3::hearth::testserver {

using sendspin::crypto::Key32;

class FileServerStore final : public sendspin::ServerStore {
   public:
    // Opens `directory`, creating it and a new identity when absent. An error says what could not
    // be read or written.
    [[nodiscard]] static std::expected<std::unique_ptr<FileServerStore>, std::string> open(
        const std::filesystem::path& directory);

    [[nodiscard]] const sendspin::noise::KeyPair& identity() const { return identity_; }

    [[nodiscard]] sendspin::handshake::PskChoice choose(const Key32& client_key) const override;
    [[nodiscard]] bool store_record(const Key32& client_key, const Key32& long_term_psk) override;
    void remove_record(const Key32& client_key) override;
    [[nodiscard]] bool paired(const Key32& client_key) const override;
    void set_pairing_psk(const Key32& client_key, std::optional<Key32> pairing_psk) override;
    [[nodiscard]] bool has_pairing_psk(const Key32& client_key) const override;
    [[nodiscard]] bool approved(const Key32& client_key) const override;
    void set_approved(const Key32& client_key, bool approved) override;

   private:
    FileServerStore() = default;
    [[nodiscard]] bool save() const;

    std::filesystem::path file_;
    sendspin::noise::KeyPair identity_{};
    // Holds the records, entered PSKs and approvals that handshakes read.
    sendspin::MemoryServerStore memory_;
    // The records again, as they are written out.
    mutable std::mutex mutex_;
    std::map<Key32, Key32> records_;
};

}  // namespace ac3::hearth::testserver
