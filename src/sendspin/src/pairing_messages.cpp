#include "ac3/sendspin/pairing_messages.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ac3/sendspin/base64url.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/messages.hpp"

namespace ac3::sendspin::pairing_messages {

namespace {

template <class Body>
[[nodiscard]] std::string envelope(std::string_view type, const Body& body) {
    std::string out;
    json::Writer w(out);
    w.begin_object().member("type", type).key("payload").begin_object();
    body(w);
    w.end_object().end_object();
    return out;
}

[[nodiscard]] std::unexpected<MessageError> malformed() {
    return std::unexpected(MessageError::kMalformed);
}

void write_bytes(json::Writer& w, std::string_view name, std::span<const std::uint8_t> bytes) {
    w.member(name, std::string_view{base64url::encode(bytes)});
}

// A fixed-size base64url value; false when the member is missing, not a string or not
// exactly `out.size()` bytes.
[[nodiscard]] bool read_bytes(json::Value value, std::span<std::uint8_t> out) {
    const std::optional<std::string_view> text = value.raw();
    return value.is_string() && text && base64url::decode_exact(*text, out);
}

[[nodiscard]] std::optional<std::uint32_t> read_index(json::Value value) {
    const std::optional<std::int64_t> number = value.as_int();
    if (!number || *number < 0 || *number > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*number);
}

struct ReasonName {
    AbortReason reason;
    std::string_view name;
};

constexpr std::array<ReasonName, 5> kReasons{{
    {AbortReason::kAttemptTimeout, "attempt_timeout"},
    {AbortReason::kConcurrentAttempt, "concurrent_attempt"},
    {AbortReason::kMethodNotSupported, "method_not_supported"},
    {AbortReason::kCodeMismatch, "pairing_code_mismatch"},
    {AbortReason::kUserCancelled, "user_cancelled"},
}};

constexpr std::array<ReasonName, 6> kReasons911{{
    {AbortReason::kAttemptTimeout, "attempt_timeout"},
    {AbortReason::kConcurrentAttempt, "concurrent_attempt"},
    {AbortReason::kMethodNotSupported, "method_not_supported"},
    {AbortReason::kCodeMismatch, "pin_mismatch"},
    {AbortReason::kUserCancelled, "user_cancelled"},
    {AbortReason::kPinLengthUnacceptable, "pin_length_unacceptable"},
}};

}  // namespace

std::string write_pair_pending(const PairPending& pending, Dialect dialect) {
    return envelope("client/pair-pending", [&](json::Writer& w) {
        w.key("pairing_index").unsigned_integer(pending.pairing_index);
        if (dialect == Dialect::kSpecification && !pending.message.empty()) {
            w.member("message", std::string_view{pending.message});
        }
    });
}

std::expected<PairPending, MessageError> read_pair_pending(json::Value payload) {
    const std::optional<std::uint32_t> index = read_index(payload["pairing_index"]);
    if (!index) {
        return malformed();
    }
    PairPending pending;
    pending.pairing_index = *index;
    if (const json::Value message = payload["message"]; message.exists()) {
        std::optional<std::string> text = message.as_string();
        if (!text) {
            return malformed();
        }
        // An unauthenticated peer's text: kept to the 200 characters a server shows, at most
        // 800 bytes of UTF-8, cut where a character starts.
        if (text->size() > 800) {
            std::size_t cut = 800;
            while (cut > 0 && (static_cast<unsigned char>((*text)[cut]) & 0xC0U) == 0x80U) {
                --cut;
            }
            text->resize(cut);
        }
        pending.message = std::move(*text);
    }
    return pending;
}

std::string write_client_pair_init(const ClientPairInit& init) {
    return envelope("client/pair-init", [&](json::Writer& w) {
        w.key("pairing_index").unsigned_integer(init.pairing_index);
        if (init.commit_b) {
            write_bytes(w, "commit_B", *init.commit_b);
        }
    });
}

std::expected<ClientPairInit, MessageError> read_client_pair_init(json::Value payload) {
    const std::optional<std::uint32_t> index = read_index(payload["pairing_index"]);
    if (!index) {
        return malformed();
    }
    ClientPairInit init;
    init.pairing_index = *index;
    if (const json::Value commit = payload["commit_B"]; commit.exists()) {
        Digest32 bytes{};
        if (!read_bytes(commit, bytes)) {
            return malformed();
        }
        init.commit_b = bytes;
    }
    return init;
}

std::string write_server_pair_init(const ServerPairInit& init) {
    return envelope("server/pair-init", [&](json::Writer& w) {
        if (init.nonce_a) {
            write_bytes(w, "nonce_A", *init.nonce_a);
        }
    });
}

std::expected<ServerPairInit, MessageError> read_server_pair_init(json::Value payload, Dialect dialect) {
    ServerPairInit init;
    if (const json::Value nonce = payload["nonce_A"]; nonce.exists()) {
        Key32 bytes{};
        if (!read_bytes(nonce, bytes)) {
            return malformed();
        }
        init.nonce_a = bytes;
    } else if (dialect == Dialect::kAiosendspin911) {
        return malformed();
    }
    return init;
}

std::string write_server_pair_auth(const Key32& pake_msg_1) {
    return envelope("server/pair-auth", [&](json::Writer& w) { write_bytes(w, "pake_msg_1", pake_msg_1); });
}

std::expected<Key32, MessageError> read_server_pair_auth(json::Value payload) {
    Key32 share{};
    if (!read_bytes(payload["pake_msg_1"], share)) {
        return malformed();
    }
    return share;
}

std::string write_client_pair_auth(const Key32& pake_msg_2) {
    return envelope("client/pair-auth", [&](json::Writer& w) { write_bytes(w, "pake_msg_2", pake_msg_2); });
}

std::expected<Key32, MessageError> read_client_pair_auth(json::Value payload) {
    Key32 share{};
    if (!read_bytes(payload["pake_msg_2"], share)) {
        return malformed();
    }
    return share;
}

std::string write_server_pair_confirm(const Digest64& server_kc) {
    return envelope("server/pair-confirm", [&](json::Writer& w) { write_bytes(w, "server_kc", server_kc); });
}

std::expected<Digest64, MessageError> read_server_pair_confirm(json::Value payload) {
    Digest64 tag{};
    if (!read_bytes(payload["server_kc"], tag)) {
        return malformed();
    }
    return tag;
}

std::string write_client_pair_retry() {
    return envelope("client/pair-retry", [](json::Writer&) {});
}

std::string write_client_pair_confirm(const ClientPairConfirm& confirm, Dialect dialect) {
    return envelope("client/pair-confirm", [&](json::Writer& w) {
        write_bytes(w, "client_kc", confirm.client_kc);
        if (dialect == Dialect::kSpecification && confirm.wrapped_nonce_b) {
            write_bytes(w, "wrapped_nonce_B", *confirm.wrapped_nonce_b);
        } else if (dialect == Dialect::kAiosendspin911 && confirm.nonce_b) {
            write_bytes(w, "nonce_B", *confirm.nonce_b);
        }
    });
}

std::expected<ClientPairConfirm, MessageError> read_client_pair_confirm(json::Value payload, Dialect dialect) {
    ClientPairConfirm confirm;
    if (!read_bytes(payload["client_kc"], confirm.client_kc)) {
        return malformed();
    }
    if (dialect == Dialect::kSpecification) {
        if (const json::Value wrapped = payload["wrapped_nonce_B"]; wrapped.exists()) {
            Wrapped bytes{};
            if (!read_bytes(wrapped, bytes)) {
                return malformed();
            }
            confirm.wrapped_nonce_b = bytes;
        }
    } else if (const json::Value nonce = payload["nonce_B"]; nonce.exists()) {
        Key32 bytes{};
        if (!read_bytes(nonce, bytes)) {
            return malformed();
        }
        confirm.nonce_b = bytes;
    }
    return confirm;
}

std::string write_client_pair_finalize(const ClientPairFinalize& finalize) {
    return envelope("client/pair-finalize", [&](json::Writer& w) {
        if (finalize.long_term_psk) {
            write_bytes(w, "long_term_psk", *finalize.long_term_psk);
        } else if (finalize.wrapped_psk) {
            write_bytes(w, "wrapped_psk", *finalize.wrapped_psk);
        }
    });
}

std::expected<ClientPairFinalize, MessageError> read_client_pair_finalize(json::Value payload) {
    const json::Value direct = payload["long_term_psk"];
    const json::Value wrapped = payload["wrapped_psk"];
    if (direct.exists() == wrapped.exists()) {
        return malformed();
    }
    ClientPairFinalize finalize;
    if (direct.exists()) {
        Key32 psk{};
        if (!read_bytes(direct, psk)) {
            return malformed();
        }
        finalize.long_term_psk = psk;
    } else {
        Wrapped bytes{};
        if (!read_bytes(wrapped, bytes)) {
            return malformed();
        }
        finalize.wrapped_psk = bytes;
    }
    return finalize;
}

std::string write_server_pair_finalize() {
    return envelope("server/pair-finalize", [](json::Writer&) {});
}

std::string write_pair_abort(AbortReason reason, Dialect dialect) {
    if (dialect == Dialect::kSpecification && reason == AbortReason::kPinLengthUnacceptable) {
        reason = AbortReason::kUserCancelled;
    }
    std::string_view name;
    if (dialect == Dialect::kSpecification) {
        for (const ReasonName& entry : kReasons) {
            if (entry.reason == reason) {
                name = entry.name;
            }
        }
    } else {
        for (const ReasonName& entry : kReasons911) {
            if (entry.reason == reason) {
                name = entry.name;
            }
        }
    }
    return envelope("pair/abort", [&](json::Writer& w) { w.member("reason", name); });
}

std::expected<AbortReason, MessageError> read_pair_abort(json::Value payload, Dialect dialect) {
    const json::Value reason = payload["reason"];
    if (dialect == Dialect::kSpecification) {
        for (const ReasonName& entry : kReasons) {
            if (reason.equals(entry.name)) {
                return entry.reason;
            }
        }
    } else {
        for (const ReasonName& entry : kReasons911) {
            if (reason.equals(entry.name)) {
                return entry.reason;
            }
        }
    }
    return malformed();
}

}  // namespace ac3::sendspin::pairing_messages
