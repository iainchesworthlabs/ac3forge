#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/noise.hpp"

// The messages of Sendspin's handshake phase (messaging.md, Communication;
// connection.md, Encryption): the cleartext client/init, server/init and
// server/error text frames, noise/handshake carrying each Noise message, the JSON
// payloads encrypted inside the two Noise messages, and the PSK identities.
//
// Everything that reads here reads bytes an unauthenticated peer chose, so every
// reader refuses what it does not recognise as the defined shape. Both sides
// keep the init texts exactly as they went over the wire: the prologue is those
// bytes, never a re-encoding.

namespace ac3::sendspin::handshake {

inline constexpr std::int64_t kCoreVersion = 1;

// Why a server refuses a client/init, in the order it checks: a JSON envelope
// whose integer version is not 1 is unsupported_version whatever else it holds,
// then the suite, then every other shape failure is malformed.
enum class InitError : std::uint8_t {
    kUnsupportedVersion,
    kUnsupportedSuite,
    kMalformed,
};

[[nodiscard]] std::string_view reason_text(InitError error);

struct ClientInit {
    crypto::Key32 client_key{};
    noise::Suite suite = noise::Suite::kChaChaPolySha256;
};

struct ServerInit {
    crypto::Key32 server_key{};
};

[[nodiscard]] std::string write_client_init(const ClientInit& init);
[[nodiscard]] std::expected<ClientInit, InitError> parse_client_init(std::string_view text);

[[nodiscard]] std::string write_server_init(const ServerInit& init);
// Nothing on a failure: the client closes silently.
[[nodiscard]] std::optional<ServerInit> parse_server_init(std::string_view text);

[[nodiscard]] std::string write_server_error(InitError error);
[[nodiscard]] std::optional<InitError> parse_server_error(std::string_view text);

// noise/handshake: `data` is the base64url of one Noise message. The same shape
// travels as a text frame in the first handshake and inside an encrypted JSON
// message in a re-handshake.
[[nodiscard]] std::string write_noise_handshake(std::span<const std::uint8_t> noise_message);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> parse_noise_handshake(
    std::string_view text);

enum class PskCategory : std::uint8_t {
    kLongTerm,
    kPairing,
    kSentinel,
};

// psk_id = base64url(SHA-256("sendspin-psk-id-v1" || PSK)), here as the digest.
[[nodiscard]] std::optional<crypto::Digest32> psk_id(const crypto::Key32& psk);

// SHA-256("sendspin-sentinel-psk-v1") and its psk_id, as connection.md publishes
// them.
[[nodiscard]] const crypto::Key32& sentinel_psk();
[[nodiscard]] const crypto::Digest32& sentinel_psk_id();

struct PskReference {
    crypto::Digest32 psk_id{};
    // Absent only when the peer is aiosendspin 9.1.1, whose message 1 names no
    // category (planning/hearth-sendspin-extension.md, C3).
    std::optional<PskCategory> category;
};

// Noise message 1's payload: {"psk_id":"...","psk_category":"lt"|"pr"|"sn"}.
[[nodiscard]] std::string write_message_1_payload(const crypto::Digest32& psk_id,
                                                  PskCategory category);
[[nodiscard]] std::optional<PskReference> parse_message_1_payload(
    std::span<const std::uint8_t> payload);

// Noise message 2's payload: the two bytes {}. The reader takes any JSON object,
// since fields a later revision adds are to be ignored.
inline constexpr std::string_view kMessage2Payload = "{}";
[[nodiscard]] bool parse_message_2_payload(std::span<const std::uint8_t> payload);

}  // namespace ac3::sendspin::handshake
