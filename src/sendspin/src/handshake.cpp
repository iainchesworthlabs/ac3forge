#include "ac3/sendspin/handshake.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/base64url.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/noise.hpp"

namespace ac3::sendspin::handshake {

namespace {

using crypto::Bytes;
using crypto::Digest32;
using crypto::Key32;

// The handshake-phase messages are small; anything that needs more than this is
// not one of them.
constexpr std::size_t kMaxTokens = 64;
constexpr json::Limits kLimits{.max_depth = 4, .max_members = 16};

[[nodiscard]] std::string_view text_of(std::span<const std::uint8_t> bytes) {
    return {static_cast<const char*>(static_cast<const void*>(bytes.data())), bytes.size()};
}

// {"type":"<type>","payload":{...}} with the payload's members written by `body`.
template <class Body>
[[nodiscard]] std::string envelope(std::string_view type, Body body) {
    std::string out;
    json::Writer w(out);
    w.begin_object().member("type", type).key("payload").begin_object();
    body(w);
    w.end_object().end_object();
    return out;
}

// The payload object of an envelope whose type is `type`, or an absent value.
[[nodiscard]] json::Value payload_of(const json::Document& doc, std::string_view type) {
    const json::Value root = doc.root();
    if (!root.is_object() || !root["type"].equals(type) || !root["payload"].is_object()) {
        return {};
    }
    return root["payload"];
}

[[nodiscard]] bool read_key(json::Value value, Key32& out) {
    const std::optional<std::string> text = value.as_string();
    return text && text->size() == base64url::encoded_size(out.size()) &&
           base64url::decode_exact(*text, out);
}

// SHA-256("sendspin-sentinel-psk-v1") and its psk_id, from connection.md. The
// tests derive both again through the crypto backend.
constexpr Key32 kSentinelPsk{0x1b, 0x5e, 0x24, 0xdb, 0xc1, 0xae, 0xd9, 0x5f, 0xc2, 0xa5, 0xa3,
                             0x38, 0xa9, 0x0c, 0x05, 0xdf, 0x44, 0xbd, 0x10, 0xf5, 0xec, 0x1f,
                             0x4c, 0xd6, 0x6c, 0xbf, 0x86, 0x27, 0x27, 0x67, 0xb9, 0xd3};
constexpr Digest32 kSentinelPskId{0x18, 0x5b, 0x15, 0xf6, 0xd2, 0xda, 0x49, 0x09, 0xbd, 0x1d, 0xc1,
                                  0x56, 0xa4, 0xab, 0x20, 0x61, 0x03, 0xab, 0xef, 0x01, 0x53, 0xbc,
                                  0xd5, 0x2d, 0x92, 0x61, 0x70, 0xb9, 0x5c, 0xf7, 0xce, 0x8a};

[[nodiscard]] std::string_view category_code(PskCategory category) {
    switch (category) {
        case PskCategory::kLongTerm:
            return "lt";
        case PskCategory::kPairing:
            return "pr";
        case PskCategory::kSentinel:
            return "sn";
    }
    return "sn";
}

}  // namespace

std::string_view reason_text(InitError error) {
    switch (error) {
        case InitError::kUnsupportedVersion:
            return "unsupported_version";
        case InitError::kUnsupportedSuite:
            return "unsupported_suite";
        case InitError::kMalformed:
            return "malformed";
    }
    return "malformed";
}

std::string write_client_init(const ClientInit& init) {
    return envelope("client/init", [&](json::Writer& w) {
        w.member("client_id", base64url::encode(init.client_key))
            .member("version", kCoreVersion)
            .member("suite", noise::suite_name(init.suite));
    });
}

std::expected<ClientInit, InitError> parse_client_init(std::string_view text) {
    std::array<json::Token, kMaxTokens> tokens{};
    json::Document doc;
    if (!doc.parse(text, tokens, kLimits)) {
        return std::unexpected(InitError::kMalformed);
    }
    const json::Value payload = payload_of(doc, "client/init");
    if (!payload.exists()) {
        return std::unexpected(InitError::kMalformed);
    }
    const json::Value version = payload["version"];
    if (version.is_number()) {
        const std::optional<std::int64_t> number = version.as_int();
        // An integer other than 1 is unsupported_version; a number that is not
        // an integer is not the defined shape.
        if (!number) {
            return std::unexpected(InitError::kMalformed);
        }
        if (*number != kCoreVersion) {
            return std::unexpected(InitError::kUnsupportedVersion);
        }
    } else {
        return std::unexpected(InitError::kMalformed);
    }
    const json::Value suite_value = payload["suite"];
    if (!suite_value.is_string()) {
        return std::unexpected(InitError::kMalformed);
    }
    const std::optional<std::string> suite_text = suite_value.as_string();
    const std::optional<noise::Suite> suite =
        suite_text ? noise::parse_suite(*suite_text) : std::nullopt;
    if (!suite) {
        return std::unexpected(InitError::kUnsupportedSuite);
    }
    ClientInit init;
    init.suite = *suite;
    if (!read_key(payload["client_id"], init.client_key)) {
        return std::unexpected(InitError::kMalformed);
    }
    return init;
}

std::string write_server_init(const ServerInit& init) {
    return envelope("server/init", [&](json::Writer& w) {
        w.member("server_id", base64url::encode(init.server_key)).member("version", kCoreVersion);
    });
}

std::optional<ServerInit> parse_server_init(std::string_view text) {
    std::array<json::Token, kMaxTokens> tokens{};
    json::Document doc;
    if (!doc.parse(text, tokens, kLimits)) {
        return std::nullopt;
    }
    const json::Value payload = payload_of(doc, "server/init");
    ServerInit init;
    if (!payload.exists() || payload["version"].as_int() != kCoreVersion ||
        !read_key(payload["server_id"], init.server_key)) {
        return std::nullopt;
    }
    return init;
}

std::string write_server_error(InitError error) {
    return envelope("server/error",
                    [&](json::Writer& w) { w.member("reason", reason_text(error)); });
}

std::optional<InitError> parse_server_error(std::string_view text) {
    std::array<json::Token, kMaxTokens> tokens{};
    json::Document doc;
    if (!doc.parse(text, tokens, kLimits)) {
        return std::nullopt;
    }
    const json::Value reason = payload_of(doc, "server/error")["reason"];
    for (const InitError error :
         {InitError::kUnsupportedVersion, InitError::kUnsupportedSuite, InitError::kMalformed}) {
        if (reason.equals(reason_text(error))) {
            return error;
        }
    }
    return std::nullopt;
}

std::string write_noise_handshake(std::span<const std::uint8_t> noise_message) {
    return envelope("noise/handshake", [&](json::Writer& w) {
        w.member("data", base64url::encode(noise_message));
    });
}

std::optional<std::vector<std::uint8_t>> parse_noise_handshake(std::string_view text) {
    std::array<json::Token, kMaxTokens> tokens{};
    json::Document doc;
    if (!doc.parse(text, tokens, kLimits)) {
        return std::nullopt;
    }
    const json::Value data = payload_of(doc, "noise/handshake")["data"];
    // The encoded length is bounded before decoding the JSON string at all.
    if (!data.is_string() ||
        data.source().size() > base64url::encoded_size(noise::kMaxMessageBytes) + 2) {
        return std::nullopt;
    }
    const std::optional<std::string> encoded = data.as_string();
    if (!encoded) {
        return std::nullopt;
    }
    return base64url::decode(*encoded);
}

const Key32& sentinel_psk() { return kSentinelPsk; }

const Digest32& sentinel_psk_id() { return kSentinelPskId; }

std::string write_message_1_payload(const Digest32& id, PskCategory category) {
    std::string out;
    json::Writer w(out);
    w.begin_object()
        .member("psk_id", base64url::encode(id))
        .member("psk_category", category_code(category))
        .end_object();
    return out;
}

std::optional<PskReference> parse_message_1_payload(std::span<const std::uint8_t> payload) {
    std::array<json::Token, kMaxTokens> tokens{};
    json::Document doc;
    if (!doc.parse(text_of(payload), tokens, kLimits) || !doc.root().is_object()) {
        return std::nullopt;
    }
    PskReference reference;
    if (!read_key(doc.root()["psk_id"], reference.psk_id)) {
        return std::nullopt;
    }
    const json::Value category = doc.root()["psk_category"];
    if (!category.exists()) {
        return reference;
    }
    for (const PskCategory candidate :
         {PskCategory::kLongTerm, PskCategory::kPairing, PskCategory::kSentinel}) {
        if (category.equals(category_code(candidate))) {
            reference.category = candidate;
            return reference;
        }
    }
    return std::nullopt;
}

bool parse_message_2_payload(std::span<const std::uint8_t> payload) {
    std::array<json::Token, kMaxTokens> tokens{};
    json::Document doc;
    return doc.parse(text_of(payload), tokens, kLimits) && doc.root().is_object();
}

}  // namespace ac3::sendspin::handshake
