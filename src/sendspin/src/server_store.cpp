#include "ac3/sendspin/server_store.hpp"

#include <map>
#include <mutex>
#include <optional>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"

namespace ac3::sendspin {

handshake::PskChoice MemoryServerStore::choose(const crypto::Key32& client_key) const {
    const std::lock_guard lock(mutex_);
    if (const auto record = records_.find(client_key); record != records_.end()) {
        return {.psk = record->second, .category = handshake::PskCategory::kLongTerm};
    }
    if (const auto entered = pairing_psks_.find(client_key); entered != pairing_psks_.end()) {
        return {.psk = entered->second, .category = handshake::PskCategory::kPairing};
    }
    return handshake::sentinel_choice();
}

bool MemoryServerStore::store_record(const crypto::Key32& client_key, const crypto::Key32& long_term_psk) {
    const std::lock_guard lock(mutex_);
    records_[client_key] = long_term_psk;
    pairing_psks_.erase(client_key);
    approvals_.erase(client_key);
    return true;
}

void MemoryServerStore::remove_record(const crypto::Key32& client_key) {
    const std::lock_guard lock(mutex_);
    records_.erase(client_key);
}

bool MemoryServerStore::paired(const crypto::Key32& client_key) const {
    const std::lock_guard lock(mutex_);
    return records_.contains(client_key);
}

void MemoryServerStore::set_pairing_psk(const crypto::Key32& client_key, std::optional<crypto::Key32> pairing_psk) {
    const std::lock_guard lock(mutex_);
    if (pairing_psk) {
        pairing_psks_[client_key] = *pairing_psk;
    } else {
        pairing_psks_.erase(client_key);
    }
}

bool MemoryServerStore::has_pairing_psk(const crypto::Key32& client_key) const {
    const std::lock_guard lock(mutex_);
    return pairing_psks_.contains(client_key);
}

bool MemoryServerStore::approved(const crypto::Key32& client_key) const {
    const std::lock_guard lock(mutex_);
    const auto found = approvals_.find(client_key);
    return found != approvals_.end() && found->second;
}

void MemoryServerStore::set_approved(const crypto::Key32& client_key, bool approved) {
    const std::lock_guard lock(mutex_);
    approvals_[client_key] = approved;
}

}  // namespace ac3::sendspin
