#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"

// psk_id, the one handshake-message value that needs a hash, apart from
// handshake.cpp so the message parsers build without a crypto backend.

namespace ac3::sendspin::handshake {

std::optional<crypto::Digest32> psk_id(const crypto::Key32& psk) {
    static constexpr std::string_view kLabel = "sendspin-psk-id-v1";
    const crypto::Bytes label(static_cast<const std::uint8_t*>(static_cast<const void*>(kLabel.data())),
                              kLabel.size());
    crypto::Digest32 id{};
    if (!crypto::sha256({label, crypto::Bytes(psk)}, id)) {
        return std::nullopt;
    }
    return id;
}

}  // namespace ac3::sendspin::handshake
