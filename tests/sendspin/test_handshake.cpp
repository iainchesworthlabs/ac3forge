#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/base64url.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "sendspin/sendspin_test_support.hpp"

// The handshake phase's messages. These parse bytes from a peer nobody has
// authenticated yet, so most cases are refusals, and the order in which a server
// picks a server/error reason is the specification's (connection.md, Failure
// Handling).

namespace {

using ac3::sendspin::handshake::ClientInit;
using ac3::sendspin::handshake::InitError;
using ac3::sendspin::handshake::PskCategory;
using ac3::sendspin::noise::Suite;
using ac3::sendspin::test::bytes_of;
using ac3::sendspin::test::key_from_hex;

constexpr std::string_view kKeyHex = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";

std::string client_init_text(std::string_view payload_members) {
    return std::string(R"({"type":"client/init","payload":{)") + std::string(payload_members) + "}}";
}

std::string key_text() { return ac3::sendspin::base64url::encode(key_from_hex(kKeyHex)); }

}  // namespace

TEST_CASE("handshake: client/init round-trips", "[sendspin][handshake]") {
    ClientInit init;
    init.client_key = key_from_hex(kKeyHex);
    init.suite = Suite::kAesGcmSha256;
    const std::string text = ac3::sendspin::handshake::write_client_init(init);
    CHECK(text == R"({"type":"client/init","payload":{"client_id":")" + key_text() +
                      R"(","version":1,"suite":"25519_AESGCM_SHA256"}})");
    const auto parsed = ac3::sendspin::handshake::parse_client_init(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->client_key == init.client_key);
    CHECK(parsed->suite == Suite::kAesGcmSha256);
}

TEST_CASE("handshake: client/init as aiosendspin 9.1.1 orders it", "[sendspin][handshake]") {
    // mashumaro writes the payload first; member order is not significant.
    const std::string text = R"({"payload":{"client_id":")" + key_text() +
                             R"(","version":1,"suite":"25519_ChaChaPoly_SHA256"},"type":"client/init"})";
    const auto parsed = ac3::sendspin::handshake::parse_client_init(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->suite == Suite::kChaChaPolySha256);
}

TEST_CASE("handshake: client/init failures pick the specification's reason",
          "[sendspin][handshake]") {
    using ac3::sendspin::handshake::parse_client_init;
    const std::string id = "\"client_id\":\"" + key_text() + "\"";
    const auto reason = [](const std::string& text) {
        const auto parsed = parse_client_init(text);
        REQUIRE_FALSE(parsed.has_value());
        return parsed.error();
    };

    // An integer version other than 1 wins over every other fault.
    CHECK(reason(client_init_text(R"("version":2,"suite":"nonsense")")) ==
          InitError::kUnsupportedVersion);
    CHECK(reason(client_init_text(R"("version":0)")) == InitError::kUnsupportedVersion);
    // Then the suite.
    CHECK(reason(client_init_text(R"("version":1,"suite":"25519_ChaChaPoly_BLAKE2s",)" + id)) ==
          InitError::kUnsupportedSuite);
    CHECK(reason(client_init_text(R"("version":1,)" + id)) == InitError::kMalformed);
    // Everything else is malformed.
    CHECK(reason("not json") == InitError::kMalformed);
    CHECK(reason(R"({"type":"server/init","payload":{"version":2}})") == InitError::kMalformed);
    CHECK(reason(R"({"type":"client/init"})") == InitError::kMalformed);
    CHECK(reason(client_init_text(R"("suite":"25519_AESGCM_SHA256",)" + id)) == InitError::kMalformed);
    CHECK(reason(client_init_text(R"("version":"1","suite":"25519_AESGCM_SHA256",)" + id)) ==
          InitError::kMalformed);
    CHECK(reason(client_init_text(R"("version":1.0,"suite":"25519_AESGCM_SHA256",)" + id)) ==
          InitError::kMalformed);
    CHECK(reason(client_init_text(R"("version":1,"suite":7,)" + id)) == InitError::kMalformed);
    CHECK(reason(client_init_text(R"("version":1,"suite":"25519_AESGCM_SHA256","client_id":"short")")) ==
          InitError::kMalformed);
    // 43 characters whose unused bits are set: not canonical base64url.
    std::string noncanonical = key_text();
    noncanonical.back() = 'b';
    CHECK(reason(client_init_text(R"("version":1,"suite":"25519_AESGCM_SHA256","client_id":")" +
                                  noncanonical + "\"")) == InitError::kMalformed);
    CHECK(reason(client_init_text(R"("version":1,"suite":"25519_AESGCM_SHA256","client_id":")" +
                                  key_text() + "=\"")) == InitError::kMalformed);
}

TEST_CASE("handshake: unknown client/init fields are ignored", "[sendspin][handshake]") {
    const std::string text = client_init_text(R"("version":1,"suite":"25519_AESGCM_SHA256","client_id":")" +
                                              key_text() + R"(","later":{"x":[1,2]})");
    CHECK(ac3::sendspin::handshake::parse_client_init(text).has_value());
}

TEST_CASE("handshake: server/init and server/error", "[sendspin][handshake]") {
    ac3::sendspin::handshake::ServerInit init;
    init.server_key = key_from_hex(kKeyHex);
    const std::string text = ac3::sendspin::handshake::write_server_init(init);
    const auto parsed = ac3::sendspin::handshake::parse_server_init(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->server_key == init.server_key);

    CHECK_FALSE(ac3::sendspin::handshake::parse_server_init(
                    R"({"type":"server/init","payload":{"server_id":")" + key_text() + R"(","version":2}})")
                    .has_value());
    CHECK_FALSE(ac3::sendspin::handshake::parse_server_init(
                    R"({"type":"client/init","payload":{"server_id":")" + key_text() + R"(","version":1}})")
                    .has_value());

    for (const InitError error :
         {InitError::kUnsupportedVersion, InitError::kUnsupportedSuite, InitError::kMalformed}) {
        const std::string error_text = ac3::sendspin::handshake::write_server_error(error);
        CHECK(ac3::sendspin::handshake::parse_server_error(error_text) == error);
    }
    CHECK(ac3::sendspin::handshake::write_server_error(InitError::kUnsupportedSuite) ==
          R"({"type":"server/error","payload":{"reason":"unsupported_suite"}})");
    CHECK_FALSE(ac3::sendspin::handshake::parse_server_error(
                    R"({"type":"server/error","payload":{"reason":"tired"}})")
                    .has_value());
}

TEST_CASE("handshake: noise/handshake carries base64url bytes", "[sendspin][handshake]") {
    const std::vector<std::uint8_t> bytes{0x00, 0xFF, 0x10, 0x80, 0x7F};
    const std::string text = ac3::sendspin::handshake::write_noise_handshake(bytes);
    CHECK(text == R"({"type":"noise/handshake","payload":{"data":"AP8QgH8"}})");
    CHECK(ac3::sendspin::handshake::parse_noise_handshake(text) == bytes);
    CHECK_FALSE(ac3::sendspin::handshake::parse_noise_handshake(
                    R"({"type":"noise/handshake","payload":{"data":"AP8QgH8="}})")
                    .has_value());
    CHECK_FALSE(ac3::sendspin::handshake::parse_noise_handshake(
                    R"({"type":"noise/handshake","payload":{"data":42}})")
                    .has_value());
    // Longer than any Noise message can be, refused before it is decoded.
    const std::string huge = R"({"type":"noise/handshake","payload":{"data":")" +
                             std::string(90000, 'A') + "\"}}";
    CHECK_FALSE(ac3::sendspin::handshake::parse_noise_handshake(huge).has_value());
}

TEST_CASE("handshake: message 1's payload names a PSK and its category",
          "[sendspin][handshake]") {
    const auto& sentinel_id = ac3::sendspin::handshake::sentinel_psk_id();
    for (const PskCategory category :
         {PskCategory::kLongTerm, PskCategory::kPairing, PskCategory::kSentinel}) {
        const std::string text = ac3::sendspin::handshake::write_message_1_payload(sentinel_id, category);
        const auto parsed = ac3::sendspin::handshake::parse_message_1_payload(bytes_of(text));
        REQUIRE(parsed.has_value());
        CHECK(parsed->psk_id == sentinel_id);
        CHECK(parsed->category == category);
    }
    CHECK(ac3::sendspin::handshake::write_message_1_payload(sentinel_id, PskCategory::kSentinel) ==
          R"({"psk_id":"GFsV9tLaSQm9HcFWpKsgYQOr7wFTvNUtkmFwuVz3zoo","psk_category":"sn"})");

    // aiosendspin 9.1.1 names no category.
    const auto legacy = ac3::sendspin::handshake::parse_message_1_payload(
        bytes_of(R"({"psk_id":"GFsV9tLaSQm9HcFWpKsgYQOr7wFTvNUtkmFwuVz3zoo"})"));
    REQUIRE(legacy.has_value());
    CHECK_FALSE(legacy->category.has_value());

    CHECK_FALSE(ac3::sendspin::handshake::parse_message_1_payload(
                    bytes_of(R"({"psk_id":"GFsV9tLaSQm9HcFWpKsgYQOr7wFTvNUtkmFwuVz3zoo","psk_category":"xx"})"))
                    .has_value());
    CHECK_FALSE(ac3::sendspin::handshake::parse_message_1_payload(
                    bytes_of(R"({"psk_id":"GFsV9tLaSQm9HcFWpKsgYQOr7wFTvNUtkmFwuVz3zo","psk_category":"sn"})"))
                    .has_value());
    CHECK_FALSE(ac3::sendspin::handshake::parse_message_1_payload(bytes_of("[]")).has_value());
    CHECK_FALSE(ac3::sendspin::handshake::parse_message_1_payload(bytes_of("")).has_value());
}

TEST_CASE("handshake: message 2's payload is an object", "[sendspin][handshake]") {
    CHECK(ac3::sendspin::handshake::parse_message_2_payload(bytes_of("{}")));
    CHECK(ac3::sendspin::handshake::parse_message_2_payload(bytes_of(R"({"future":true})")));
    CHECK_FALSE(ac3::sendspin::handshake::parse_message_2_payload(bytes_of("")));
    CHECK_FALSE(ac3::sendspin::handshake::parse_message_2_payload(bytes_of("[]")));
    CHECK_FALSE(ac3::sendspin::handshake::parse_message_2_payload(bytes_of("{")));
}
