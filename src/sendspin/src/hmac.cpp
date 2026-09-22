#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/sendspin/crypto.hpp"

// RFC 2104 HMAC over the backend's hashes, and the one wipe every secret-holding
// type here uses.

namespace ac3::sendspin::crypto {

namespace {

template <std::size_t kBlockBytes, class Digest, class Hash>
[[nodiscard]] bool hmac(Hash hash, Bytes key, std::span<const Bytes> parts, Digest& out) {
    if (parts.size() > kMaxHmacParts) {
        return false;
    }
    std::array<std::uint8_t, kBlockBytes> block{};
    Digest key_digest{};
    if (key.size() > kBlockBytes) {
        const std::array<Bytes, 1> whole{key};
        if (!hash(std::span<const Bytes>(whole), key_digest)) {
            return false;
        }
        std::copy(key_digest.begin(), key_digest.end(), block.begin());
    } else {
        std::copy(key.begin(), key.end(), block.begin());
    }

    std::array<std::uint8_t, kBlockBytes> pad{};
    for (std::size_t i = 0; i < kBlockBytes; ++i) {
        pad[i] = static_cast<std::uint8_t>(block[i] ^ 0x36U);
    }
    std::array<Bytes, kMaxHmacParts + 1> inner{};
    inner[0] = pad;
    std::copy(parts.begin(), parts.end(), inner.begin() + 1);
    Digest inner_digest{};
    bool ok = hash(std::span<const Bytes>(inner.data(), parts.size() + 1), inner_digest);

    if (ok) {
        for (std::size_t i = 0; i < kBlockBytes; ++i) {
            pad[i] = static_cast<std::uint8_t>(block[i] ^ 0x5CU);
        }
        const std::array<Bytes, 2> outer{Bytes(pad), Bytes(inner_digest)};
        ok = hash(std::span<const Bytes>(outer), out);
    }
    wipe(block);
    wipe(pad);
    wipe(key_digest);
    wipe(inner_digest);
    return ok;
}

}  // namespace

void wipe(std::span<std::uint8_t> bytes) {
    // Through a volatile pointer, so the writes survive dead-store elimination.
    auto* volatile const data = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        data[i] = 0;
    }
}

bool hmac_sha256(Bytes key, std::span<const Bytes> parts, Digest32& out) {
    return hmac<64>([](std::span<const Bytes> p, Digest32& d) { return sha256(p, d); }, key,
                    parts, out);
}

bool hmac_sha512(Bytes key, std::span<const Bytes> parts, Digest64& out) {
    return hmac<128>([](std::span<const Bytes> p, Digest64& d) { return sha512(p, d); }, key,
                     parts, out);
}

}  // namespace ac3::sendspin::crypto
