#include <optional>
#include <string_view>

#include "iclforge/sendspin/noise.hpp"

// The Noise suite names, apart from noise.cpp so the handshake-message parsers
// build without a crypto backend (the fuzz build has none).

namespace iclforge::sendspin::noise {

std::string_view suite_name(Suite suite) {
    return suite == Suite::kChaChaPolySha256 ? "25519_ChaChaPoly_SHA256" : "25519_AESGCM_SHA256";
}

std::optional<Suite> parse_suite(std::string_view name) {
    if (name == suite_name(Suite::kChaChaPolySha256)) {
        return Suite::kChaChaPolySha256;
    }
    if (name == suite_name(Suite::kAesGcmSha256)) {
        return Suite::kAesGcmSha256;
    }
    return std::nullopt;
}

std::string_view protocol_name(Suite suite) {
    return suite == Suite::kChaChaPolySha256 ? "Noise_KKpsk2_25519_ChaChaPoly_SHA256"
                                             : "Noise_KKpsk2_25519_AESGCM_SHA256";
}

}  // namespace iclforge::sendspin::noise
