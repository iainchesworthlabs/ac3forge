// A Sendspin player's keys and pairing records in NVS. See
// ../include/ac3forge/sendspin_store.hpp.

#include "ac3forge/sendspin_store.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string_view>

#include "esp_err.h"
#include "nvs.h"

#include "ac3/sendspin/base64url.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/noise.hpp"

namespace ac3forge {
namespace {

namespace hs = ac3::sendspin::handshake;
using Key32 = SendspinStore::Key32;
using Names = SendspinStore::Names;
using Records = PairingRecords<SendspinStore::kRecordCapacity>;

// One NVS namespace for the player, beside the example's own settings
// namespace, so that forgetting the board's pairings and forgetting its
// network are two separate acts.
constexpr const char* kNamespace = "sendspin";
constexpr const char* kKeyIdentity = "identity";
constexpr const char* kKeyPairingPsk = "pairing_psk";
constexpr const char* kKeyRecords = "records";
constexpr const char* kKeyNames = "names";
constexpr const char* kKeyLastPlayback = "last_play";

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

// The names blob's bytes pass through the heap, not the caller's stack: the
// Sendspin server's task writes a name after a pairing, and the handshake
// before it leaves that stack little room.
using NameBytes = std::unique_ptr<std::uint8_t[]>;

[[nodiscard]] NameBytes name_bytes() { return NameBytes(new (std::nothrow) std::uint8_t[Names::kBlobBytes]); }

// The names NVS holds, into `names`. A board with none stored has none; false
// when NVS could not be read.
[[nodiscard]] bool read_names_blob(Names& names) {
    names = Names{};
    NameBytes blob = name_bytes();
    nvs_handle_t handle = 0;
    if (!blob || nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    std::size_t length = Names::kBlobBytes;
    const esp_err_t err = nvs_get_blob(handle, kKeyNames, blob.get(), &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        return false;
    }
    names.decode(std::span<const std::uint8_t>(blob.get(), length));
    return true;
}

[[nodiscard]] bool write_names_blob(const Names& names) {
    NameBytes blob = name_bytes();
    if (!blob) {
        std::printf("sendspin: no memory to write the servers' names\n");
        return false;
    }
    const std::size_t length = names.encode(std::span<std::uint8_t>(blob.get(), Names::kBlobBytes));
    return write_blob(kKeyNames, blob.get(), length);
}

// A server's key as the console shows it: its server_id's first eight
// characters, as the host's own lines do.
void print_evicted(const Key32& server_key) {
    std::printf("sendspin: every pairing record is taken; forgot server %s, the least recently used\n",
                ac3::sendspin::base64url::encode(server_key).substr(0, 8).c_str());
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
    records_.clear();
    last_playback_.reset();
    if (opened) {
        have_keys = read_key(handle, kKeyIdentity, private_key) && read_key(handle, kKeyPairingPsk, pairing_psk_);
        std::array<std::uint8_t, Records::kBlobBytes> blob{};
        std::size_t length = blob.size();
        if (nvs_get_blob(handle, kKeyRecords, blob.data(), &length) == ESP_OK) {
            records_.decode(std::span<const std::uint8_t>(blob).first(length));
        }
        ac3::sendspin::crypto::wipe(blob);
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
    records_.clear();
    last_playback_.reset();
    if (!create_keys()) {
        return false;
    }
    (void)write_blob(kKeyRecords, nullptr, 0);
    (void)write_blob(kKeyNames, nullptr, 0);
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
        for (std::size_t i = 0; i < records_.size(); ++i) {
            if (hs::psk_id(records_[i].psk) == id) {
                return hs::PskCandidate{
                    .psk = records_[i].psk, .category = hs::PskCategory::kLongTerm, .server_key = records_[i].server_key};
            }
        }
    }
    return std::nullopt;
}

bool SendspinStore::save_records() const {
    std::array<std::uint8_t, Records::kBlobBytes> blob{};
    const std::size_t length = records_.encode(blob);
    const bool saved = write_blob(kKeyRecords, blob.data(), length);
    ac3::sendspin::crypto::wipe(blob);
    return saved;
}

bool SendspinStore::add_record(const Key32& server_key, const Key32& long_term_psk, std::span<const Key32> in_use) {
    const std::lock_guard lock(mutex_);
    if (const std::optional<Key32> evicted = records_.add(server_key, long_term_psk, in_use)) {
        print_evicted(*evicted);
    }
    return save_records();
}

void SendspinStore::touch(const Key32& server_key) {
    const std::lock_guard lock(mutex_);
    if (records_.touch(server_key)) {
        (void)save_records();
    }
}

void SendspinStore::saw(const Key32& server_key) {
    const std::lock_guard lock(mutex_);
    records_.mark_seen(server_key);
}

bool SendspinStore::remove_record(const Key32& server_key) {
    const std::lock_guard lock(mutex_);
    if (!records_.remove(server_key)) {
        return false;
    }
    (void)save_records();
    if (auto names = std::unique_ptr<Names>(new (std::nothrow) Names())) {
        if (read_names_blob(*names) && names->set(server_key, {})) {
            (void)write_names_blob(*names);
        }
    }
    if (last_playback_ == server_key) {
        last_playback_.reset();
        (void)write_blob(kKeyLastPlayback, nullptr, 0);
    }
    return true;
}

bool SendspinStore::has_record(const Key32& server_key) const {
    const std::lock_guard lock(mutex_);
    return records_.find(server_key).has_value();
}

std::size_t SendspinStore::records() const {
    const std::lock_guard lock(mutex_);
    return records_.size();
}

std::size_t SendspinStore::list(std::span<Listed> out) const {
    const std::lock_guard lock(mutex_);
    const std::size_t n = std::min(out.size(), records_.size());
    for (std::size_t i = 0; i < n; ++i) {
        const Records::Record& record = records_[records_.size() - 1 - i];
        out[i] = Listed{.server_key = record.server_key, .seen = record.seen};
    }
    return n;
}

void SendspinStore::set_name(const Key32& server_key, std::string_view name) {
    const std::lock_guard lock(mutex_);
    if (!records_.find(server_key)) {
        return;
    }
    auto names = std::unique_ptr<Names>(new (std::nothrow) Names());
    if (!names || !read_names_blob(*names)) {
        std::printf("sendspin: could not read the servers' names to name one\n");
        return;
    }
    // The names of records gone since the last write go with this one.
    const bool pruned = names->keep([this](const Key32& key) { return records_.find(key).has_value(); });
    const bool named = names->set(server_key, name);
    if (pruned || named) {
        (void)write_names_blob(*names);
    }
}

bool SendspinStore::read_names(Names& out) const {
    const std::lock_guard lock(mutex_);
    return read_names_blob(out);
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
    records_.clear();
    last_playback_.reset();
    return create_keys() && write_blob(kKeyRecords, nullptr, 0) && write_blob(kKeyNames, nullptr, 0) &&
           write_blob(kKeyLastPlayback, nullptr, 0);
}

}  // namespace ac3forge
