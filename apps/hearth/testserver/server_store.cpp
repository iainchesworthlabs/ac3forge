#include "server_store.hpp"

#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/noise.hpp"
#include "store.hpp"

namespace ac3::hearth::testserver {

namespace {

namespace fs = std::filesystem;
using testsink::hex_of;
using testsink::key_from_hex;

constexpr std::string_view kStateFile = "server.txt";

}  // namespace

std::expected<std::unique_ptr<FileServerStore>, std::string> FileServerStore::open(const fs::path& directory) {
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
        return std::unexpected("cannot create " + directory.string() + ": " + error.message());
    }
    std::unique_ptr<FileServerStore> store(new FileServerStore());
    store->file_ = directory / kStateFile;

    std::optional<Key32> private_key;
    std::map<Key32, Key32> records;
    if (std::ifstream in(store->file_, std::ios::binary); in) {
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            std::string kind;
            std::string first;
            std::string second;
            fields >> kind >> first >> second;
            if (kind == "identity") {
                private_key = key_from_hex(first);
                if (!private_key) {
                    return std::unexpected(store->file_.string() + ": the identity is not 64 hex digits");
                }
            } else if (kind == "record") {
                const std::optional<Key32> client = key_from_hex(first);
                const std::optional<Key32> psk = key_from_hex(second);
                if (!client || !psk) {
                    return std::unexpected(store->file_.string() + ": a record is not two keys of 64 hex digits");
                }
                records[*client] = *psk;
            }
        }
    }
    std::optional<sendspin::noise::KeyPair> identity =
        private_key ? sendspin::noise::KeyPair::from_private(*private_key) : sendspin::noise::KeyPair::generate();
    if (!identity) {
        return std::unexpected(std::string("the identity is not a usable X25519 key"));
    }
    store->identity_ = std::move(*identity);
    for (const auto& [client, psk] : records) {
        (void)store->memory_.store_record(client, psk);
    }
    store->records_ = std::move(records);
    if (!private_key && !store->save()) {
        return std::unexpected("cannot write " + store->file_.string());
    }
    return store;
}

sendspin::handshake::PskChoice FileServerStore::choose(const Key32& client_key) const {
    return memory_.choose(client_key);
}

bool FileServerStore::store_record(const Key32& client_key, const Key32& long_term_psk) {
    {
        const std::lock_guard lock(mutex_);
        records_[client_key] = long_term_psk;
    }
    return memory_.store_record(client_key, long_term_psk) && save();
}

void FileServerStore::remove_record(const Key32& client_key) {
    {
        const std::lock_guard lock(mutex_);
        records_.erase(client_key);
    }
    memory_.remove_record(client_key);
    (void)save();
}

bool FileServerStore::paired(const Key32& client_key) const { return memory_.paired(client_key); }

void FileServerStore::set_pairing_psk(const Key32& client_key, std::optional<Key32> pairing_psk) {
    memory_.set_pairing_psk(client_key, pairing_psk);
}

bool FileServerStore::has_pairing_psk(const Key32& client_key) const { return memory_.has_pairing_psk(client_key); }

bool FileServerStore::approved(const Key32& client_key) const { return memory_.approved(client_key); }

void FileServerStore::set_approved(const Key32& client_key, bool approved) {
    memory_.set_approved(client_key, approved);
}

// Written to a temporary file beside the state file, readable and writable by its owner only, and
// renamed over it, so a crash never leaves half a file.
bool FileServerStore::save() const {
    std::ostringstream text;
    text << "identity " << hex_of(identity_.private_key()) << "\n";
    {
        const std::lock_guard lock(mutex_);
        for (const auto& [client, psk] : records_) {
            text << "record " << hex_of(client) << " " << hex_of(psk) << "\n";
        }
    }
    const fs::path temporary = fs::path(file_).concat(".new");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << text.str();
        if (!out.flush()) {
            return false;
        }
    }
    std::error_code error;
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, error);
    fs::rename(temporary, file_, error);
    return !error;
}

}  // namespace ac3::hearth::testserver
