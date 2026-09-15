#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/messages.hpp"

// The pairing messages (pairing.md, Messages), each a struct with a writer and a reader, in
// both dialects where they differ (planning/hearth-sendspin-extension.md, C22 to C26). The
// binary values - nonces, shares, tags, wrapped PSKs - are fixed-size and base64url, and a
// reader refuses any of the wrong length: a pairing message that no conformant peer produces
// is a protocol error (pairing.md, Protocol Errors).
//
// The type names are the same in both dialects.

namespace ac3::sendspin::pairing_messages {

using crypto::Digest32;
using crypto::Digest64;
using crypto::Key32;
using messages::MessageError;

inline constexpr std::size_t kWrappedBytes = 48;
using Wrapped = std::array<std::uint8_t, kWrappedBytes>;

// client/pair-pending. aiosendspin 9.1.1 has no `message` (C26).
struct PairPending {
    std::uint32_t pairing_index = 0;
    std::string message;  // empty when absent; at most 200 characters are shown
};

// client/pair-init: commit_B in the dynamic code flow only.
struct ClientPairInit {
    std::uint32_t pairing_index = 0;
    std::optional<Digest32> commit_b;
};

// server/pair-init: nonce_A in the first round only; aiosendspin 9.1.1 requires it (C22).
struct ServerPairInit {
    std::optional<Key32> nonce_a;
};

// client/pair-confirm: the specification seals nonce_B; aiosendspin 9.1.1 sends it in the
// clear (C23). Dynamic code flow only.
struct ClientPairConfirm {
    Digest64 client_kc{};
    std::optional<Wrapped> wrapped_nonce_b;
    std::optional<Key32> nonce_b;
};

// client/pair-finalize: exactly one of the two.
struct ClientPairFinalize {
    std::optional<Key32> long_term_psk;
    std::optional<Wrapped> wrapped_psk;
};

enum class AbortReason : std::uint8_t {
    kAttemptTimeout,
    kConcurrentAttempt,
    kMethodNotSupported,
    // aiosendspin 9.1.1's `pin_mismatch` (C24).
    kCodeMismatch,
    kUserCancelled,
    // aiosendspin 9.1.1 only.
    kPinLengthUnacceptable,
};

[[nodiscard]] std::string write_pair_pending(const PairPending& pending, Dialect dialect);
[[nodiscard]] std::expected<PairPending, MessageError> read_pair_pending(json::Value payload);

[[nodiscard]] std::string write_client_pair_init(const ClientPairInit& init);
[[nodiscard]] std::expected<ClientPairInit, MessageError> read_client_pair_init(json::Value payload);

[[nodiscard]] std::string write_server_pair_init(const ServerPairInit& init);
// In aiosendspin 9.1.1's dialect a missing nonce_A is malformed.
[[nodiscard]] std::expected<ServerPairInit, MessageError> read_server_pair_init(json::Value payload,
                                                                               Dialect dialect);

// server/pair-auth and client/pair-auth: one CPace share each.
[[nodiscard]] std::string write_server_pair_auth(const Key32& pake_msg_1);
[[nodiscard]] std::expected<Key32, MessageError> read_server_pair_auth(json::Value payload);
[[nodiscard]] std::string write_client_pair_auth(const Key32& pake_msg_2);
[[nodiscard]] std::expected<Key32, MessageError> read_client_pair_auth(json::Value payload);

// server/pair-confirm: one tag.
[[nodiscard]] std::string write_server_pair_confirm(const Digest64& server_kc);
[[nodiscard]] std::expected<Digest64, MessageError> read_server_pair_confirm(json::Value payload);

// client/pair-retry: no payload fields, specification only.
[[nodiscard]] std::string write_client_pair_retry();

// Writes wrapped_nonce_b in the specification's dialect and nonce_b in 9.1.1's, whichever the
// struct holds for that dialect.
[[nodiscard]] std::string write_client_pair_confirm(const ClientPairConfirm& confirm, Dialect dialect);
// Reads the dialect's nonce field when present; which the flow requires is the flow's check.
[[nodiscard]] std::expected<ClientPairConfirm, MessageError> read_client_pair_confirm(json::Value payload,
                                                                                     Dialect dialect);

[[nodiscard]] std::string write_client_pair_finalize(const ClientPairFinalize& finalize);
// Malformed unless exactly one field is present.
[[nodiscard]] std::expected<ClientPairFinalize, MessageError> read_client_pair_finalize(json::Value payload);

// server/pair-finalize: no payload fields.
[[nodiscard]] std::string write_server_pair_finalize();

// kPinLengthUnacceptable is not written in the specification's dialect, which has no such
// reason; kUserCancelled stands in.
[[nodiscard]] std::string write_pair_abort(AbortReason reason, Dialect dialect);
[[nodiscard]] std::expected<AbortReason, MessageError> read_pair_abort(json::Value payload, Dialect dialect);

}  // namespace ac3::sendspin::pairing_messages
