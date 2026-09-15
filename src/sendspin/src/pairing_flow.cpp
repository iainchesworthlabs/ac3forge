#include "ac3/sendspin/pairing_flow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ac3/sendspin/cpace.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/pairing.hpp"
#include "ac3/sendspin/pairing_messages.hpp"

namespace ac3::sendspin::pairing_flow {

namespace {

namespace m = messages;
namespace pm = pairing_messages;
using handshake::PskCategory;

[[nodiscard]] std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {static_cast<const std::uint8_t*>(static_cast<const void*>(text.data())), text.size()};
}

[[nodiscard]] const m::PairMethodDescriptor* offered(const std::vector<m::PairMethodDescriptor>& list,
                                                     m::PairMethod method) {
    const auto found = std::find_if(list.begin(), list.end(),
                                    [method](const m::PairMethodDescriptor& d) { return d.method == method; });
    return found == list.end() ? nullptr : &*found;
}

// PRS: the code's ASCII digits, or the qr_code format's 24 raw bytes (pairing.md, PAKE).
[[nodiscard]] std::vector<std::uint8_t> code_bytes(const Code& code) {
    if (const auto* digits = std::get_if<std::string>(&code)) {
        const std::span<const std::uint8_t> bytes = bytes_of(*digits);
        return {bytes.begin(), bytes.end()};
    }
    const auto& raw = std::get<std::array<std::uint8_t, 24>>(code);
    return {raw.begin(), raw.end()};
}

[[nodiscard]] bool all_digits(std::string_view text, std::size_t length) {
    return text.size() == length && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

// --- ClientPairing ---------------------------------------------------------------------

ClientPairing::ClientPairing(ClientPairingConfig config, ClientPairingState& state, ClientPairingEvents& events)
    : config_(std::move(config)), shared_(&state), events_(&events) {}

ClientPairing::~ClientPairing() {
    crypto::wipe(nonce_b_);
    crypto::wipe(long_term_psk_);
}

Step ClientPairing::abort(pm::AbortReason reason) {
    state_ = State::kDone;
    aborted_ = reason;
    return {.after = reason == pm::AbortReason::kConcurrentAttempt ? After::kClose : After::kEnded,
            .messages = {pm::write_pair_abort(reason, config_.dialect)}};
}

Step ClientPairing::protocol_error() {
    state_ = State::kDone;
    return {.after = After::kClose, .messages = {}};
}

std::vector<std::uint8_t> ClientPairing::prs() const {
    return code_ ? code_bytes(*code_) : std::vector<std::uint8_t>{};
}

Step ClientPairing::start(const m::PairingActivation& activation, std::uint32_t pairing_index, std::int64_t now) {
    pairing_index_ = pairing_index;
    if (!activation.method) {
        return abort(pm::AbortReason::kMethodNotSupported);
    }
    method_ = *activation.method;
    const m::PairMethodDescriptor* descriptor = offered(config_.offered, method_);
    // pairing.method is pairing_psk exactly when the matched PSK is the pairing PSK
    // (messaging.md, server/activate).
    const bool pairing_psk_matched = config_.matched == PskCategory::kPairing;
    if (descriptor == nullptr || (method_ == m::PairMethod::kPairingPsk) != pairing_psk_matched) {
        return abort(pm::AbortReason::kMethodNotSupported);
    }

    switch (method_) {
        case m::PairMethod::kPairingPsk:
            if (!crypto::random_bytes(long_term_psk_)) {
                return protocol_error();
            }
            attempt_started_ = now;
            state_ = State::kFinalize;
            return {.after = After::kContinue,
                    .messages = {pm::write_client_pair_finalize({.long_term_psk = long_term_psk_, .wrapped_psk = std::nullopt})}};

        case m::PairMethod::kDynamicCode:
            if (config_.dialect == Dialect::kSpecification) {
                if (!activation.format || std::find(descriptor->formats.begin(), descriptor->formats.end(),
                                                    *activation.format) == descriptor->formats.end()) {
                    return abort(pm::AbortReason::kMethodNotSupported);
                }
                format_ = activation.format;
                digits_ = pairing::kSpecificationCodeDigits;
            } else {
                if (activation.pin_length < descriptor->min_pin_length ||
                    activation.pin_length > pairing::kMaximumCodeDigits) {
                    return abort(pm::AbortReason::kPinLengthUnacceptable);
                }
                format_ = m::CodeFormat::kDigits;
                digits_ = activation.pin_length;
            }
            break;

        case m::PairMethod::kStaticCode:
            if (!all_digits(config_.static_code, 8)) {
                return abort(pm::AbortReason::kMethodNotSupported);
            }
            break;
    }
    return may_start(now) ? begin_attempt(now) : hold_back();
}

bool ClientPairing::may_start(std::int64_t now) const {
    if (method_ == m::PairMethod::kStaticCode) {
        return shared_->window_open(now, config_.connection);
    }
    return !shared_->holding_back();
}

Step ClientPairing::hold_back() {
    state_ = State::kPending;
    events_->on_held_back();
    // One client/pair-pending an attempt: Music Assistant disconnects on a second (C26).
    return {.after = After::kContinue,
            .messages = {pm::write_pair_pending({.pairing_index = pairing_index_, .message = {}}, config_.dialect)}};
}

Step ClientPairing::begin_attempt(std::int64_t now) {
    attempt_started_ = now;
    round_ = 1;
    if (method_ == m::PairMethod::kDynamicCode) {
        const std::optional<Digest32> commitment =
            crypto::random_bytes(nonce_b_) ? pairing::commit(nonce_b_) : std::nullopt;
        if (!commitment) {
            return protocol_error();
        }
        state_ = State::kServerInit;
        return {.after = After::kContinue,
                .messages = {pm::write_client_pair_init({.pairing_index = pairing_index_, .commit_b = *commitment})}};
    }
    // The window's first attempt binds it to this connection.
    if (!shared_->window_connection) {
        shared_->window_connection = config_.connection;
    }
    code_ = config_.static_code;
    state_ = State::kAuth;
    return {.after = After::kContinue,
            .messages = {pm::write_client_pair_init({.pairing_index = pairing_index_, .commit_b = std::nullopt})}};
}

Step ClientPairing::resume(std::int64_t now) {
    if (state_ != State::kPending || !may_start(now)) {
        return {};
    }
    return begin_attempt(now);
}

Step ClientPairing::cancel() {
    if (state_ == State::kDone) {
        return {};
    }
    return abort(pm::AbortReason::kUserCancelled);
}

void ClientPairing::abandon() {
    // A round already counted when its server/pair-init arrived, which is the spec's "only
    // when the code was already being emitted".
    state_ = State::kDone;
}

Step ClientPairing::tick(std::int64_t now) {
    if (state_ == State::kDone || state_ == State::kPending || !attempt_started_) {
        return {};
    }
    if (now - *attempt_started_ >= kAttemptTimeout) {
        return abort(pm::AbortReason::kAttemptTimeout);
    }
    return {};
}

Step ClientPairing::receive(std::string_view type, json::Value payload, std::int64_t /*now*/) {
    const Dialect dialect = config_.dialect;
    if (state_ == State::kDone) {
        // Messages still in flight after the attempt ended are discarded.
        return {};
    }
    if (type == "pair/abort") {
        const auto reason = pm::read_pair_abort(payload, dialect);
        state_ = State::kDone;
        if (reason) {
            aborted_ = *reason;
        }
        return {.after = reason && *reason == pm::AbortReason::kConcurrentAttempt ? After::kClose : After::kEnded,
                .messages = {}};
    }

    switch (state_) {
        case State::kPending:
            return protocol_error();

        case State::kServerInit: {
            if (type != "server/pair-init") {
                return protocol_error();
            }
            const auto init = pm::read_server_pair_init(payload, dialect);
            if (!init || (round_ == 1) != init->nonce_a.has_value()) {
                return protocol_error();
            }
            if (round_ == 1) {
                if (format_ == m::CodeFormat::kQrCode) {
                    const auto qr = pairing::derive_qr_code(config_.handshake_hash, *init->nonce_a, nonce_b_);
                    if (!qr) {
                        return protocol_error();
                    }
                    code_ = *qr;
                } else {
                    std::optional<std::string> digits =
                        pairing::derive_digits(dialect, config_.handshake_hash, *init->nonce_a, nonce_b_, digits_);
                    if (!digits) {
                        return protocol_error();
                    }
                    code_ = std::move(*digits);
                }
            }
            // The round begins, and the code is emitted: it counts toward the round limit
            // however it ends.
            ++shared_->rounds_since_verified;
            events_->on_code(*code_);
            state_ = State::kAuth;
            return {};
        }

        case State::kAuth: {
            if (type != "server/pair-auth") {
                return protocol_error();
            }
            const auto ya = pm::read_server_pair_auth(payload);
            if (!ya) {
                return protocol_error();
            }
            sid_ = pairing::pake_sid(dialect, config_.handshake_hash, pairing_index_, round_);
            const std::vector<std::uint8_t> code = prs();
            std::optional<cpace::Party> party =
                cpace::Party::start(cpace::Role::kResponder, code, {}, sid_, bytes_of(pairing::kClientAd));
            if (!party || !party->derive(*ya, bytes_of(pairing::kServerAd))) {
                return protocol_error();
            }
            party_ = std::make_unique<cpace::Party>(std::move(*party));
            state_ = State::kConfirm;
            return {.after = After::kContinue, .messages = {pm::write_client_pair_auth(party_->share())}};
        }

        case State::kConfirm: {
            if (type != "server/pair-confirm") {
                return protocol_error();
            }
            const auto ta = pm::read_server_pair_confirm(payload);
            if (!ta || !party_) {
                return protocol_error();
            }
            if (!party_->verify(*ta)) {
                if (method_ == m::PairMethod::kStaticCode) {
                    ++shared_->window_failures;
                    return abort(pm::AbortReason::kCodeMismatch);
                }
                // At the round limit the attempt ends, and later ones wait for the operator.
                // aiosendspin 9.1.1 has no rounds: a failed round ends the attempt (C22).
                if (shared_->holding_back() || dialect == Dialect::kAiosendspin911) {
                    return abort(pm::AbortReason::kCodeMismatch);
                }
                ++round_;
                party_.reset();
                state_ = State::kServerInit;
                return {.after = After::kContinue, .messages = {pm::write_client_pair_retry()}};
            }
            if (method_ == m::PairMethod::kDynamicCode) {
                shared_->rounds_since_verified = 0;
            }

            const std::optional<crypto::Digest64> tb = party_->tag();
            const std::optional<Key32> psk_key =
                pairing::wrap_key(pairing::Wrapped::kLongTermPsk, sid_, party_->isk());
            if (!tb || !psk_key || !crypto::random_bytes(long_term_psk_)) {
                return protocol_error();
            }
            const auto wrapped_psk = pairing::wrap(config_.suite, *psk_key, long_term_psk_);
            if (!wrapped_psk) {
                return protocol_error();
            }
            pm::ClientPairConfirm confirm{.client_kc = *tb, .wrapped_nonce_b = std::nullopt, .nonce_b = std::nullopt};
            if (method_ == m::PairMethod::kDynamicCode) {
                if (dialect == Dialect::kSpecification) {
                    const std::optional<Key32> nonce_key =
                        pairing::wrap_key(pairing::Wrapped::kNonceB, sid_, party_->isk());
                    const auto wrapped_nonce =
                        nonce_key ? pairing::wrap(config_.suite, *nonce_key, nonce_b_) : std::nullopt;
                    if (!wrapped_nonce) {
                        return protocol_error();
                    }
                    confirm.wrapped_nonce_b = *wrapped_nonce;
                } else {
                    // aiosendspin 9.1.1 reveals nonce_B in the clear (C23).
                    confirm.nonce_b = nonce_b_;
                }
            }
            state_ = State::kFinalize;
            // client/pair-finalize follows client/pair-confirm without waiting (pairing.md).
            return {.after = After::kContinue,
                    .messages = {pm::write_client_pair_confirm(confirm, dialect),
                                 pm::write_client_pair_finalize({.long_term_psk = std::nullopt, .wrapped_psk = *wrapped_psk})}};
        }

        case State::kFinalize: {
            if (type != "server/pair-finalize") {
                return protocol_error();
            }
            events_->on_paired(long_term_psk_);
            if (method_ == m::PairMethod::kStaticCode) {
                shared_->close_window();
            }
            state_ = State::kDone;
            return {.after = After::kPaired, .messages = {}};
        }

        case State::kDone:
            return {};
    }
    return protocol_error();
}

// --- ServerPairing ---------------------------------------------------------------------

ServerPairing::ServerPairing(ServerPairingConfig config) : config_(config) {}

ServerPairing::~ServerPairing() {
    crypto::wipe(nonce_a_);
    crypto::wipe(long_term_psk_);
}

void ServerPairing::activated(const m::PairingActivation& activation, std::uint32_t pairing_index) {
    method_ = activation.method.value_or(m::PairMethod::kPairingPsk);
    format_ = activation.format;
    pairing_index_ = pairing_index;
    round_ = 0;
    state_ = State::kInit;
    if (method_ == m::PairMethod::kStaticCode) {
        digits_ = 8;
    } else if (config_.dialect == Dialect::kSpecification) {
        digits_ = pairing::kSpecificationCodeDigits;
    } else {
        digits_ = activation.pin_length;
    }
}

Step ServerPairing::protocol_error() {
    state_ = State::kDone;
    return {.after = After::kClose, .messages = {}};
}

Step ServerPairing::mismatch() {
    state_ = State::kDone;
    aborted_ = pm::AbortReason::kCodeMismatch;
    return {.after = After::kEnded, .messages = {pm::write_pair_abort(pm::AbortReason::kCodeMismatch, config_.dialect)}};
}

std::vector<std::uint8_t> ServerPairing::prs() const {
    return code_ ? code_bytes(*code_) : std::vector<std::uint8_t>{};
}

Step ServerPairing::cancel() {
    if (state_ == State::kDone) {
        return {};
    }
    state_ = State::kDone;
    aborted_ = pm::AbortReason::kUserCancelled;
    return {.after = After::kEnded, .messages = {pm::write_pair_abort(pm::AbortReason::kUserCancelled, config_.dialect)}};
}

Step ServerPairing::enter_code(const Code& code) {
    if (state_ != State::kCode) {
        return {};
    }
    const bool qr = std::holds_alternative<std::array<std::uint8_t, 24>>(code);
    const bool wants_qr = method_ == m::PairMethod::kDynamicCode && format_ == m::CodeFormat::kQrCode;
    if (qr != wants_qr ||
        (!qr && !all_digits(std::get<std::string>(code), static_cast<std::size_t>(digits_)))) {
        // Not a code of the shape this attempt uses; the operator can enter it again.
        return {};
    }
    code_ = code;
    sid_ = pairing::pake_sid(config_.dialect, config_.handshake_hash, pairing_index_, round_);
    const std::vector<std::uint8_t> bytes = prs();
    std::optional<cpace::Party> party =
        cpace::Party::start(cpace::Role::kInitiator, bytes, {}, sid_, bytes_of(pairing::kServerAd));
    if (!party) {
        return protocol_error();
    }
    party_ = std::make_unique<cpace::Party>(std::move(*party));
    state_ = State::kClientAuth;
    return {.after = After::kContinue, .messages = {pm::write_server_pair_auth(party_->share())}};
}

Step ServerPairing::finalize(const pm::ClientPairFinalize& finalize) {
    if (finalize.long_term_psk) {
        long_term_psk_ = *finalize.long_term_psk;
    } else {
        const std::optional<Key32> key =
            party_ ? pairing::wrap_key(pairing::Wrapped::kLongTermPsk, sid_, party_->isk()) : std::nullopt;
        const std::optional<Key32> psk =
            key && finalize.wrapped_psk ? pairing::unwrap(config_.suite, *key, *finalize.wrapped_psk) : std::nullopt;
        if (!psk) {
            return protocol_error();
        }
        long_term_psk_ = *psk;
    }
    state_ = State::kDone;
    return {.after = After::kPaired, .messages = {pm::write_server_pair_finalize()}};
}

Step ServerPairing::receive(std::string_view type, json::Value payload) {
    const Dialect dialect = config_.dialect;
    if (state_ == State::kDone) {
        return {};
    }
    if (type == "pair/abort") {
        const auto reason = pm::read_pair_abort(payload, dialect);
        aborted_ = reason ? *reason : pm::AbortReason::kUserCancelled;
        state_ = State::kDone;
        return {.after = reason && *reason == pm::AbortReason::kConcurrentAttempt ? After::kClose : After::kEnded,
                .messages = {}};
    }
    if (type == "client/pair-pending") {
        const auto pending = pm::read_pair_pending(payload);
        if (!pending || pending->pairing_index > pairing_index_ || state_ != State::kInit) {
            return protocol_error();
        }
        if (pending->pairing_index == pairing_index_) {
            held_back_ = true;
            if (pending->message.empty()) {
                pending_.reset();
            } else {
                pending_ = pending->message;
            }
        }
        return {};
    }

    switch (state_) {
        case State::kInit: {
            if (method_ != m::PairMethod::kPairingPsk && pairing_index_ > 1 && type != "client/pair-init") {
                // Until this activation's client/pair-init, a code flow's other messages can
                // only be ones the client sent in the attempt this activation superseded, which
                // are discarded (pairing.md, Entering and leaving pairing).
                return {};
            }
            if (method_ == m::PairMethod::kPairingPsk) {
                if (type != "client/pair-finalize") {
                    return protocol_error();
                }
                const auto finalize_message = pm::read_client_pair_finalize(payload);
                if (!finalize_message || !finalize_message->long_term_psk ||
                    config_.matched != PskCategory::kPairing) {
                    return protocol_error();
                }
                return finalize(*finalize_message);
            }
            if (type != "client/pair-init") {
                return protocol_error();
            }
            const auto init = pm::read_client_pair_init(payload);
            if (!init || init->pairing_index > pairing_index_) {
                return protocol_error();
            }
            if (init->pairing_index < pairing_index_) {
                // A leftover from a superseded pairing (pairing.md, Pairing index).
                return {};
            }
            held_back_ = false;
            pending_.reset();
            round_ = 1;
            if (method_ == m::PairMethod::kDynamicCode) {
                if (!init->commit_b || !crypto::random_bytes(nonce_a_)) {
                    return protocol_error();
                }
                commit_b_ = init->commit_b;
                state_ = State::kCode;
                return {.after = After::kContinue, .messages = {pm::write_server_pair_init({.nonce_a = nonce_a_})}};
            }
            if (init->commit_b) {
                return protocol_error();
            }
            state_ = State::kCode;
            return {};
        }

        case State::kCode:
            return protocol_error();

        case State::kClientAuth: {
            if (type != "client/pair-auth") {
                return protocol_error();
            }
            const auto yb = pm::read_client_pair_auth(payload);
            if (!yb || !party_ || !party_->derive(*yb, bytes_of(pairing::kClientAd))) {
                return protocol_error();
            }
            const std::optional<crypto::Digest64> ta = party_->tag();
            if (!ta) {
                return protocol_error();
            }
            state_ = State::kConfirm;
            return {.after = After::kContinue, .messages = {pm::write_server_pair_confirm(*ta)}};
        }

        case State::kConfirm: {
            if (type == "client/pair-retry") {
                if (method_ != m::PairMethod::kDynamicCode || dialect != Dialect::kSpecification) {
                    return protocol_error();
                }
                // Another round: the same code, entered again, under a new sid.
                ++round_;
                party_.reset();
                code_.reset();
                state_ = State::kCode;
                return {.after = After::kContinue, .messages = {pm::write_server_pair_init({.nonce_a = std::nullopt})}};
            }
            if (type != "client/pair-confirm") {
                return protocol_error();
            }
            const auto confirm = pm::read_client_pair_confirm(payload, dialect);
            if (!confirm || !party_) {
                return protocol_error();
            }
            const bool tag_ok = party_->verify(confirm->client_kc);
            if (method_ == m::PairMethod::kStaticCode) {
                if (!tag_ok) {
                    return mismatch();
                }
                state_ = State::kFinalize;
                return {};
            }

            // The dynamic code: the tag, the commitment, and the binding of the entered code to
            // this handshake. The specification checks in that order (pairing.md, Server
            // verification); aiosendspin 9.1.1 checks the commitment first and the other two
            // together (C24).
            std::optional<Key32> nonce_b;
            if (dialect == Dialect::kSpecification) {
                if (!confirm->wrapped_nonce_b) {
                    return protocol_error();
                }
                if (!tag_ok) {
                    return mismatch();
                }
                const std::optional<Key32> key = pairing::wrap_key(pairing::Wrapped::kNonceB, sid_, party_->isk());
                nonce_b = key ? pairing::unwrap(config_.suite, *key, *confirm->wrapped_nonce_b) : std::nullopt;
            } else {
                if (!confirm->nonce_b) {
                    return protocol_error();
                }
                nonce_b = confirm->nonce_b;
            }
            const std::optional<Digest32> opened = nonce_b ? pairing::commit(*nonce_b) : std::nullopt;
            if (!opened || !commit_b_ || *opened != *commit_b_) {
                return protocol_error();
            }
            bool bound = false;
            if (format_ == m::CodeFormat::kQrCode) {
                const auto expected = pairing::derive_qr_code(config_.handshake_hash, nonce_a_, *nonce_b);
                bound = expected && code_ && std::holds_alternative<std::array<std::uint8_t, 24>>(*code_) &&
                        *expected == std::get<std::array<std::uint8_t, 24>>(*code_);
            } else {
                const auto expected = pairing::derive_digits(dialect, config_.handshake_hash, nonce_a_, *nonce_b, digits_);
                bound = expected && code_ && std::holds_alternative<std::string>(*code_) &&
                        *expected == std::get<std::string>(*code_);
            }
            if (dialect == Dialect::kAiosendspin911) {
                if (!tag_ok || !bound) {
                    return mismatch();
                }
            } else if (!bound) {
                return protocol_error();
            }
            state_ = State::kFinalize;
            return {};
        }

        case State::kFinalize: {
            if (type != "client/pair-finalize") {
                return protocol_error();
            }
            const auto finalize_message = pm::read_client_pair_finalize(payload);
            if (!finalize_message || !finalize_message->wrapped_psk) {
                return protocol_error();
            }
            return finalize(*finalize_message);
        }

        case State::kDone:
            return {};
    }
    return protocol_error();
}

}  // namespace ac3::sendspin::pairing_flow
