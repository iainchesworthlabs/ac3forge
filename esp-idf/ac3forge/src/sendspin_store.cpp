// A Sendspin player's keys and pairing records in NVS. See
// ../include/ac3forge/sendspin_store.hpp.

#include "ac3forge/sendspin_store.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>

#include "esp_err.h"
#include "nvs.h"

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/noise.hpp"

namespace ac3forge {
namespace {

namespace hs = ac3::sendspin::handshake;
using Key32 = SendspinStore::Key32;

// One NVS namespace for the player, beside the example's own settings
// namespace, so that forgetting the board's pairings and forgetting its
// network are two separate acts.
constexpr const char* kNamespace = "sendspin";
constexpr const char* kKeyIdentity = "identity";
constexpr const char* kKeyPairingPsk = "pairing_psk";
constexpr const char* kKeyRecords = "records";
constexpr const char* kKeyLastPlayback = "last_play";

// A record on flash: the server's key, then the long-term PSK.
constexpr std::size_t kRecordBytes = 64;

[[nodiscard]] bool read_key(nvs_handle_t handle, const char* key, Key32& out) {
    std::size_t length = out.size();
    return nvs_get_blob(handle, key, out.data(), &length) == ESP_OK && length == out.size();
}

[[nodiscard]] bool write_blob(const char* key, const std::uint8_t* data, std::size_t length) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        std::printf("sendspin: could not open NVS to write %s\n", key);
        return false;
    }
    esp_err_t err = length == 0 ? nvs_erase_key(handle, key) : nvs_set_blob(handle, key, data, length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;  // erasing what was never written
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        std::printf("sendspin: NVS refused %s (%s)\n", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

}  // namespace

bool SendspinStore::create_keys() {
    Key32 private_key{};
    if (!ac3::sendspin::crypto::random_bytes(private_key) ||
        !ac3::sendspin::crypto::random_bytes(pairing_psk_)) {
        std::printf("sendspin: no random bytes for the board's keys\n");
        return false;
    }
    std::optional<ac3::sendspin::noise::KeyPair> identity =
        ac3::sendspin::noise::KeyPair::from_private(private_key);
    ac3::sendspin::crypto::wipe(private_key);
    if (!identity) {
        std::printf("sendspin: the new identity is not a usable X25519 key\n");
        return false;
    }
    identity_ = *identity;
    return write_blob(kKeyIdentity, identity_.private_key().data(), identity_.private_key().size()) &&
           write_blob(kKeyPairingPsk, pairing_psk_.data(), pairing_psk_.size());
}

bool SendspinStore::load() {
    const std::lock_guard lock(mutex_);
    nvs_handle_t handle = 0;
    const bool opened = nvs_open(kNamespace, NVS_READONLY, &handle) == ESP_OK;
    Key32 private_key{};
    bool have_keys = false;
    record_count_ = 0;
    last_playback_.reset();
    if (opened) {
        have_keys = read_key(handle, kKeyIdentity, private_key) && read_key(handle, kKeyPairingPsk, pairing_psk_);
        std::array<std::uint8_t, kRecordCapacity * kRecordBytes> blob{};
        std::size_t length = blob.size();
        if (nvs_get_blob(handle, kKeyRecords, blob.data(), &length) == ESP_OK) {
            record_count_ = std::min(length / kRecordBytes, kRecordCapacity);
            for (std::size_t i = 0; i < record_count_; ++i) {
                std::copy_n(blob.begin() + static_cast<std::ptrdiff_t>(i * kRecordBytes), 32,
                            records_[i].server_key.begin());
                std::copy_n(blob.begin() + static_cast<std::ptrdiff_t>((i * kRecordBytes) + 32), 32,
                            records_[i].psk.begin());
            }
            ac3::sendspin::crypto::wipe(blob);
        }
        Key32 last{};
        if (read_key(handle, kKeyLastPlayback, last)) {
            last_playback_ = last;
        }
        nvs_close(handle);
    }
    if (have_keys) {
        std::optional<ac3::sendspin::noise::KeyPair> identity =
            ac3::sendspin::noise::KeyPair::from_private(private_key);
        ac3::sendspin::crypto::wipe(private_key);
        if (identity) {
            identity_ = *identity;
            return true;
        }
        std::printf("sendspin: the stored identity is not a usable X25519 key; making a new one\n");
    }
    // A board's first boot, or an identity that cannot be used: a new pair of
    // keys, and no pairing can outlive them.
    record_count_ = 0;
    last_playback_.reset();
    if (!create_keys()) {
        return false;
    }
    (void)write_blob(kKeyRecords, nullptr, 0);
    (void)write_blob(kKeyLastPlayback, nullptr, 0);
    std::printf("sendspin: a new identity for this board\n");
    return true;
}

std::optional<hs::PskCandidate> SendspinStore::find(const ac3::sendspin::crypto::Digest32& id,
                                                    std::optional<hs::PskCategory> category) const {
    const std::lock_guard lock(mutex_);
    if (!category || *category == hs::PskCategory::kPairing) {
        if (hs::psk_id(pairing_psk_) == id) {
            return hs::PskCandidate{.psk = pairing_psk_, .category = hs::PskCategory::kPairing, .server_key = {}};
        }
    }
    if (!category || *category == hs::PskCategory::kLongTerm) {
        for (std::size_t i = 0; i < record_count_; ++i) {
            if (hs::psk_id(records_[i].psk) == id) {
                return hs::PskCandidate{
                    .psk = records_[i].psk, .category = hs::PskCategory::kLongTerm, .server_key = records_[i].server_key};
            }
        }
    }
    return std::nullopt;
}

bool SendspinStore::save_records() const {
    std::array<std::uint8_t, kRecordCapacity * kRecordBytes> blob{};
    for (std::size_t i = 0; i < record_count_; ++i) {
        std::copy(records_[i].server_key.begin(), records_[i].server_key.end(),
                  blob.begin() + static_cast<std::ptrdiff_t>(i * kRecordBytes));
        std::copy(records_[i].psk.begin(), records_[i].psk.end(),
                  blob.begin() + static_cast<std::ptrdiff_t>((i * kRecordBytes) + 32));
    }
    const bool saved = write_blob(kKeyRecords, blob.data(), record_count_ * kRecordBytes);
    ac3::sendspin::crypto::wipe(blob);
    return saved;
}

bool SendspinStore::add_record(const Key32& server_key, const Key32& long_term_psk) {
    const std::lock_guard lock(mutex_);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < record_count_; ++i) {
        if (records_[i].server_key != server_key) {
            records_[kept++] = records_[i];
        }
    }
    record_count_ = kept;
    if (record_count_ == kRecordCapacity) {
        // The oldest goes. No record backs an open connection here: a board
        // has at most three, and eight records.
        std::copy(records_.begin() + 1, records_.end(), records_.begin());
        --record_count_;
    }
    records_[record_count_++] = Record{.server_key = server_key, .psk = long_term_psk};
    return save_records();
}

void SendspinStore::remove_record(const Key32& server_key) {
    const std::lock_guard lock(mutex_);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < record_count_; ++i) {
        if (records_[i].server_key != server_key) {
            records_[kept++] = records_[i];
        }
    }
    if (kept == record_count_) {
        return;
    }
    for (std::size_t i = kept; i < record_count_; ++i) {
        ac3::sendspin::crypto::wipe(records_[i].psk);
    }
    record_count_ = kept;
    (void)save_records();
}

std::size_t SendspinStore::records() const {
    const std::lock_guard lock(mutex_);
    return record_count_;
}

std::optional<Key32> SendspinStore::last_playback() const {
    const std::lock_guard lock(mutex_);
    return last_playback_;
}

void SendspinStore::set_last_playback(const Key32& server_key) {
    const std::lock_guard lock(mutex_);
    if (last_playback_ == server_key) {
        return;
    }
    last_playback_ = server_key;
    (void)write_blob(kKeyLastPlayback, server_key.data(), server_key.size());
}

bool SendspinStore::forget() {
    const std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < record_count_; ++i) {
        ac3::sendspin::crypto::wipe(records_[i].psk);
    }
    record_count_ = 0;
    last_playback_.reset();
    return create_keys() && write_blob(kKeyRecords, nullptr, 0) && write_blob(kKeyLastPlayback, nullptr, 0);
}

}  // namespace ac3forge
