#include "store.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/noise.hpp"

namespace ac3::hearth::testsink {

namespace {

namespace fs = std::filesystem;
namespace hs = sendspin::handshake;

constexpr std::string_view kIdentityFile = "identity.key";
constexpr std::string_view kPairingPskFile = "pairing.psk";
constexpr std::string_view kRecordsFile = "records.txt";
constexpr std::string_view kLastPlaybackFile = "last-playback.txt";

[[nodiscard]] std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Writes `text` to a temporary file beside `path`, readable and writable by its owner only, and
// renames it over `path`, so a crash never leaves half a file.
[[nodiscard]] bool write_text(const fs::path& path, const std::string& text) {
    const fs::path temporary = fs::path(path).concat(".new");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << text;
        if (!out.flush()) {
            return false;
        }
    }
    std::error_code error;
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, error);
    fs::rename(temporary, path, error);
    return !error;
}

[[nodiscard]] std::string trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

// A key from `name` in `directory`, or a new random one written there.
[[nodiscard]] std::expected<Key32, std::string> load_or_create_key(const fs::path& directory, std::string_view name) {
    const fs::path path = directory / name;
    if (const std::optional<std::string> text = read_text(path)) {
        if (std::optional<Key32> key = key_from_hex(trimmed(*text))) {
            return *key;
        }
        return std::unexpected(path.string() + " does not hold 64 hex digits");
    }
    Key32 key{};
    if (!sendspin::crypto::random_bytes(key)) {
        return std::unexpected(std::string("no random bytes for a new key"));
    }
    if (!write_text(path, hex_of(key) + "\n")) {
        return std::unexpected("cannot write " + path.string());
    }
    return key;
}

}  // namespace

std::string hex_of(const Key32& bytes) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        text.push_back(kDigits[byte >> 4U]);
        text.push_back(kDigits[byte & 0x0FU]);
    }
    return text;
}

std::optional<Key32> key_from_hex(std::string_view text) {
    if (text.size() != 64) {
        return std::nullopt;
    }
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    Key32 key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        const int high = nibble(text[2 * i]);
        const int low = nibble(text[(2 * i) + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        key[i] = static_cast<std::uint8_t>((high * 16) + low);
    }
    return key;
}

std::expected<std::unique_ptr<Store>, std::string> Store::open(const fs::path& directory) {
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
        return std::unexpected("cannot create " + directory.string() + ": " + error.message());
    }
    std::unique_ptr<Store> store(new Store());
    store->directory_ = directory;

    const std::expected<Key32, std::string> private_key = load_or_create_key(directory, kIdentityFile);
    if (!private_key) {
        return std::unexpected(private_key.error());
    }
    std::optional<sendspin::noise::KeyPair> identity = sendspin::noise::KeyPair::from_private(*private_key);
    if (!identity) {
        return std::unexpected(std::string("the identity key is not a usable X25519 key"));
    }
    store->identity_ = std::move(*identity);

    const std::expected<Key32, std::string> pairing_psk = load_or_create_key(directory, kPairingPskFile);
    if (!pairing_psk) {
        return std::unexpected(pairing_psk.error());
    }
    store->pairing_psk_ = *pairing_psk;

    if (const std::optional<std::string> text = read_text(directory / kRecordsFile)) {
        std::istringstream lines(*text);
        std::string server;
        std::string psk;
        while (lines >> server >> psk) {
            const std::optional<Key32> server_key = key_from_hex(server);
            const std::optional<Key32> long_term_psk = key_from_hex(psk);
            if (server_key && long_term_psk) {
                store->records_.push_back({.server_key = *server_key, .psk = *long_term_psk});
            }
        }
    }
    if (const std::optional<std::string> text = read_text(directory / kLastPlaybackFile)) {
        store->last_playback_ = key_from_hex(trimmed(*text));
    }
    return store;
}

std::optional<hs::PskCandidate> Store::find(const sendspin::crypto::Digest32& id,
                                            std::optional<hs::PskCategory> category) const {
    const std::lock_guard lock(mutex_);
    if (!category || *category == hs::PskCategory::kPairing) {
        if (hs::psk_id(pairing_psk_) == id) {
            return hs::PskCandidate{.psk = pairing_psk_, .category = hs::PskCategory::kPairing, .server_key = {}};
        }
    }
    if (!category || *category == hs::PskCategory::kLongTerm) {
        for (const Record& record : records_) {
            if (hs::psk_id(record.psk) == id) {
                return hs::PskCandidate{
                    .psk = record.psk, .category = hs::PskCategory::kLongTerm, .server_key = record.server_key};
            }
        }
    }
    return std::nullopt;
}

bool Store::save_records() const {
    std::string text;
    for (const Record& record : records_) {
        text += hex_of(record.server_key) + " " + hex_of(record.psk) + "\n";
    }
    return write_text(directory_ / kRecordsFile, text);
}

bool Store::add_record(const Key32& server_key, const Key32& long_term_psk) {
    const std::lock_guard lock(mutex_);
    std::erase_if(records_, [&](const Record& record) { return record.server_key == server_key; });
    records_.push_back({.server_key = server_key, .psk = long_term_psk});
    if (records_.size() > kRecordCapacity) {
        records_.erase(records_.begin());
    }
    return save_records();
}

void Store::remove_record(const Key32& server_key) {
    const std::lock_guard lock(mutex_);
    std::erase_if(records_, [&](const Record& record) { return record.server_key == server_key; });
    (void)save_records();
}

std::size_t Store::records() const {
    const std::lock_guard lock(mutex_);
    return records_.size();
}

std::optional<Key32> Store::last_playback() const {
    const std::lock_guard lock(mutex_);
    return last_playback_;
}

void Store::set_last_playback(const Key32& server_key) {
    const std::lock_guard lock(mutex_);
    if (last_playback_ == server_key) {
        return;
    }
    last_playback_ = server_key;
    (void)write_text(directory_ / kLastPlaybackFile, hex_of(server_key) + "\n");
}

}  // namespace ac3::hearth::testsink
