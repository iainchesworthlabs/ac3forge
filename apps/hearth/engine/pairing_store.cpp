#include "pairing_store.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// See pairing_store.hpp.

namespace ac3::hearth {

namespace {

using sendspin::crypto::Key32;

constexpr std::string_view kGroup = "pairing";
constexpr std::string_view kSize = "pairing/size";
// A damaged count must not keep the read going for ever; the specification's
// minimum is five records, and a person pairs a handful of sinks.
constexpr std::uint64_t kMaxRecords = 1000;

[[nodiscard]] std::string field(std::size_t index, std::string_view name) {
    return fmt::format("{}/{}/{}", kGroup, index + 1, name);
}

[[nodiscard]] std::string hex_of(const Key32& key) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(key.size() * 2);
    for (const std::uint8_t byte : key) {
        out.push_back(kDigits[byte >> 4U]);
        out.push_back(kDigits[byte & 0x0FU]);
    }
    return out;
}

// 64 hex digits, either case, and nothing else.
[[nodiscard]] std::optional<Key32> key_of(const std::optional<std::string>& text) {
    Key32 key{};
    if (!text || text->size() != key.size() * 2) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        const char* const first = text->data() + (i * 2);
        const auto [at, error] = std::from_chars(first, first + 2, key[i], 16);
        if (error != std::errc{} || at != first + 2) {
            return std::nullopt;
        }
    }
    return key;
}

template <typename Records>
void wipe_keys(Records& records) {
    for (auto& record : records) {
        sendspin::crypto::wipe(record.psk);
    }
}

[[nodiscard]] std::optional<std::uint64_t> count_of(const std::optional<std::string>& text) {
    if (!text || text->empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char* const end = text->data() + text->size();
    const auto [at, error] = std::from_chars(text->data(), end, value);
    if (error != std::errc{} || at != end) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

PairingStore::PairingStore(SettingsStore& store, std::function<std::string()> today)
    : store_(store), today_(std::move(today)) {
    // Records that do not read - a key that is not 64 hex digits - are left
    // out, and so is a second record for the same client.
    const auto size = count_of(store_.value(kSize));
    const std::uint64_t count = size ? std::min(*size, kMaxRecords) : 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto client = key_of(store_.value(field(i, "client")));
        const auto psk = key_of(store_.value(field(i, "psk")));
        if (!client || !psk || find_locked(*client) != records_.end()) {
            continue;
        }
        records_.push_back(Record{.client_key = *client,
                                  .psk = *psk,
                                  .name = store_.value(field(i, "name")).value_or(std::string{}),
                                  .paired_on = store_.value(field(i, "paired")).value_or(std::string{})});
    }
}

PairingStore::~PairingStore() {
    const std::scoped_lock lock(mutex_);
    wipe_keys(records_);
    for (auto& [client, psk] : pairing_psks_) {
        sendspin::crypto::wipe(psk);
    }
}

std::vector<PairingStore::Record>::iterator PairingStore::find_locked(const Key32& key) {
    return std::ranges::find(records_, key, &Record::client_key);
}

std::vector<PairingStore::Record>::const_iterator PairingStore::find_locked(const Key32& key) const {
    return std::ranges::find(records_, key, &Record::client_key);
}

bool PairingStore::save_locked() {
    store_.remove_group(kGroup);
    store_.set_value(kSize, std::to_string(records_.size()));
    for (std::size_t i = 0; i < records_.size(); ++i) {
        const Record& record = records_[i];
        store_.set_value(field(i, "client"), hex_of(record.client_key));
        store_.set_value(field(i, "psk"), hex_of(record.psk));
        store_.set_value(field(i, "name"), record.name);
        store_.set_value(field(i, "paired"), record.paired_on);
    }
    return store_.sync();
}

sendspin::handshake::PskChoice PairingStore::choose(const Key32& client_key) const {
    const std::scoped_lock lock(mutex_);
    if (const auto record = find_locked(client_key); record != records_.end()) {
        return {.psk = record->psk, .category = sendspin::handshake::PskCategory::kLongTerm};
    }
    if (const auto entered = pairing_psks_.find(client_key); entered != pairing_psks_.end()) {
        return {.psk = entered->second, .category = sendspin::handshake::PskCategory::kPairing};
    }
    return sendspin::handshake::sentinel_choice();
}

bool PairingStore::store_record(const Key32& client_key, const Key32& long_term_psk) {
    const std::scoped_lock lock(mutex_);
    // Kept, so that a record the file did not take is not one the process
    // goes on using: server/pair-finalize says the record is persisted.
    std::vector<Record> before = records_;
    const auto name = names_.find(client_key);
    Record record{.client_key = client_key,
                  .psk = long_term_psk,
                  .name = name != names_.end() ? name->second : std::string{},
                  .paired_on = today_ ? today_() : std::string{}};
    if (auto existing = find_locked(client_key); existing != records_.end()) {
        sendspin::crypto::wipe(existing->psk);
        records_.erase(existing);
    }
    records_.push_back(std::move(record));
    if (!save_locked()) {
        wipe_keys(records_);
        records_ = std::move(before);
        return false;
    }
    wipe_keys(before);
    if (const auto entered = pairing_psks_.find(client_key); entered != pairing_psks_.end()) {
        sendspin::crypto::wipe(entered->second);
        pairing_psks_.erase(entered);
    }
    approvals_.erase(client_key);
    return true;
}

void PairingStore::remove_record(const Key32& client_key) {
    const std::scoped_lock lock(mutex_);
    const auto existing = find_locked(client_key);
    if (existing == records_.end()) {
        return;
    }
    sendspin::crypto::wipe(existing->psk);
    records_.erase(existing);
    // Forgotten for this process whatever the write does; a write that fails
    // leaves the record for the next start, which is no worse than the
    // person asking again.
    static_cast<void>(save_locked());
}

bool PairingStore::paired(const Key32& client_key) const {
    const std::scoped_lock lock(mutex_);
    return find_locked(client_key) != records_.end();
}

void PairingStore::set_pairing_psk(const Key32& client_key, std::optional<Key32> pairing_psk) {
    const std::scoped_lock lock(mutex_);
    if (pairing_psk) {
        pairing_psks_[client_key] = *pairing_psk;
    } else if (const auto entered = pairing_psks_.find(client_key); entered != pairing_psks_.end()) {
        sendspin::crypto::wipe(entered->second);
        pairing_psks_.erase(entered);
    }
}

bool PairingStore::has_pairing_psk(const Key32& client_key) const {
    const std::scoped_lock lock(mutex_);
    return pairing_psks_.contains(client_key);
}

bool PairingStore::approved(const Key32& client_key) const {
    const std::scoped_lock lock(mutex_);
    const auto found = approvals_.find(client_key);
    return found != approvals_.end() && found->second;
}

void PairingStore::set_approved(const Key32& client_key, bool approved) {
    const std::scoped_lock lock(mutex_);
    approvals_[client_key] = approved;
}

void PairingStore::set_client_name(const Key32& client_key, const std::string& name) {
    const std::scoped_lock lock(mutex_);
    names_[client_key] = name;
    if (auto existing = find_locked(client_key);
        existing != records_.end() && existing->name != name) {
        existing->name = name;
        static_cast<void>(save_locked());
    }
}

std::vector<PairingRecordView> PairingStore::records() const {
    const std::scoped_lock lock(mutex_);
    std::vector<PairingRecordView> out;
    out.reserve(records_.size());
    for (const Record& record : records_) {
        out.push_back(PairingRecordView{
            .client_key = record.client_key, .name = record.name, .paired_on = record.paired_on});
    }
    return out;
}

}  // namespace ac3::hearth
