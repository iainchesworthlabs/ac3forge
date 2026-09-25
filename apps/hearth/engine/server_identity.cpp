#include "server_identity.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "ac3/sendspin/crypto.hpp"

// See server_identity.hpp.

namespace ac3::hearth {

namespace {

using sendspin::crypto::Key32;

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

}  // namespace

std::optional<sendspin::noise::KeyPair> load_or_make_server_identity(SettingsStore& store) {
    if (std::optional<Key32> kept = key_of(store.value(kServerIdentityKey))) {
        std::optional<sendspin::noise::KeyPair> identity = sendspin::noise::KeyPair::from_private(*kept);
        sendspin::crypto::wipe(*kept);
        if (identity) {
            return identity;
        }
    }
    std::optional<sendspin::noise::KeyPair> made = sendspin::noise::KeyPair::generate();
    if (!made) {
        return std::nullopt;
    }
    std::string text = hex_of(made->private_key());
    store.set_value(kServerIdentityKey, text);
    // A failed write leaves the new identity this run's alone (the header's comment).
    static_cast<void>(store.sync());
    sendspin::crypto::wipe(std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(text.data()), text.size()));
    return made;
}

}  // namespace ac3::hearth
