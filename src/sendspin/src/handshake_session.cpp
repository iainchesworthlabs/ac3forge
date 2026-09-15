#include "ac3/sendspin/handshake_session.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/noise.hpp"

namespace ac3::sendspin::handshake {

namespace {

[[nodiscard]] std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {static_cast<const std::uint8_t*>(static_cast<const void*>(text.data())), text.size()};
}

// The prologue of a first handshake: client/init's bytes, then server/init's, exactly as
// they went over the wire (connection.md, Prologue).
[[nodiscard]] std::vector<std::uint8_t> init_prologue(std::string_view client_init,
                                                      std::string_view server_init) {
    std::vector<std::uint8_t> prologue;
    prologue.reserve(client_init.size() + server_init.size());
    const std::span<const std::uint8_t> client = bytes_of(client_init);
    const std::span<const std::uint8_t> server = bytes_of(server_init);
    prologue.insert(prologue.end(), client.begin(), client.end());
    prologue.insert(prologue.end(), server.begin(), server.end());
    return prologue;
}

}  // namespace

PskChoice sentinel_choice() {
    return PskChoice{.psk = sentinel_psk(), .category = PskCategory::kSentinel};
}

// --- Responder ------------------------------------------------------------------------

Responder::Responder(noise::KeyPair identity, noise::Suite suite, const ClientKeyring& keyring)
    : identity_(std::move(identity)),
      suite_(suite),
      keyring_(&keyring),
      client_init_(write_client_init({.client_key = identity_.public_key(), .suite = suite_})) {}

Responder::Responder(noise::KeyPair identity, noise::Suite suite, const crypto::Key32& server_key,
                     const crypto::Digest32& previous_hash, const ClientKeyring& keyring)
    : identity_(std::move(identity)),
      suite_(suite),
      keyring_(&keyring),
      rehandshake_(true),
      state_(State::kMessage1),
      server_key_(server_key) {
    handshake_.emplace(suite_, noise::Role::kResponder, identity_, server_key_, previous_hash);
}

Step Responder::fail(Failure failure) {
    failure_ = failure;
    state_ = State::kDone;
    return {.outcome = Outcome::kFailed, .replies = {}};
}

Step Responder::receive(std::string_view text) {
    if (state_ == State::kServerInit) {
        if (const std::optional<ServerInit> init = parse_server_init(text)) {
            server_key_ = init->server_key;
            handshake_.emplace(suite_, noise::Role::kResponder, identity_, server_key_,
                               init_prologue(client_init_, text));
            state_ = State::kMessage1;
            return {};
        }
        if (const std::optional<InitError> error = parse_server_error(text)) {
            server_error_ = *error;
            return fail(Failure::kServerError);
        }
        return fail(Failure::kBadInit);
    }
    if (state_ != State::kMessage1 || !handshake_) {
        return fail(Failure::kUnexpected);
    }

    const std::optional<std::vector<std::uint8_t>> message = parse_noise_handshake(text);
    std::vector<std::uint8_t> payload;
    if (!message || !handshake_->read_message_1(*message, payload)) {
        return fail(Failure::kBadHandshake);
    }
    const std::optional<PskReference> reference = parse_message_1_payload(payload);
    if (!reference) {
        return fail(Failure::kBadHandshake);
    }
    dialect_ = reference->category ? Dialect::kSpecification : Dialect::kAiosendspin911;

    crypto::Key32 psk = sentinel_psk();
    category_ = PskCategory::kSentinel;
    const bool names_sentinel =
        reference->psk_id == sentinel_psk_id() &&
        (!reference->category || *reference->category == PskCategory::kSentinel);
    if (!names_sentinel) {
        const std::optional<PskCandidate> candidate = keyring_->find(reference->psk_id, reference->category);
        if (candidate) {
            // A long-term PSK is bound to the server its pairing record names; a match
            // under another server_id is a misbinding, which never falls back.
            if (candidate->category == PskCategory::kLongTerm && candidate->server_key != server_key_) {
                return fail(Failure::kServerMismatch);
            }
            psk = candidate->psk;
            category_ = candidate->category;
        } else if (rehandshake_) {
            return fail(Failure::kPskMiss);
        } else {
            fell_back_ = true;
        }
    }

    std::vector<std::uint8_t> reply;
    const bool written = handshake_->write_message_2(psk, bytes_of(kMessage2Payload), reply);
    crypto::wipe(psk);
    if (!written) {
        return fail(Failure::kCrypto);
    }
    state_ = State::kDone;
    return {.outcome = Outcome::kEstablished, .replies = {write_noise_handshake(reply)}};
}

std::optional<noise::Handshake::Transport> Responder::take_keys() {
    if (state_ != State::kDone || failure_ != Failure::kNone || !handshake_) {
        return std::nullopt;
    }
    return handshake_->split();
}

// --- Initiator ------------------------------------------------------------------------

Initiator::Initiator(noise::KeyPair identity, const ServerKeyring& keyring)
    : identity_(std::move(identity)), keyring_(&keyring) {}

Initiator::Initiator(noise::KeyPair identity, const crypto::Key32& client_key, noise::Suite suite,
                     const crypto::Digest32& previous_hash, PskChoice choice)
    : identity_(std::move(identity)),
      state_(State::kMessage2),
      client_key_(client_key),
      suite_(suite),
      choice_(choice) {
    handshake_.emplace(suite_, noise::Role::kInitiator, identity_, client_key_, previous_hash);
}

Step Initiator::fail(Failure failure) {
    failure_ = failure;
    state_ = State::kDone;
    return {.outcome = Outcome::kFailed, .replies = {}};
}

std::optional<std::string> Initiator::message_1() {
    const std::optional<crypto::Digest32> id = psk_id(choice_.psk);
    if (!id || !handshake_) {
        return std::nullopt;
    }
    const std::string payload = write_message_1_payload(*id, choice_.category);
    std::vector<std::uint8_t> message;
    if (!handshake_->write_message_1(bytes_of(payload), message)) {
        return std::nullopt;
    }
    return write_noise_handshake(message);
}

Step Initiator::start() {
    // Only a re-handshake starts from the server's side.
    if (keyring_ != nullptr || state_ != State::kMessage2) {
        return {};
    }
    std::optional<std::string> message = message_1();
    if (!message) {
        return fail(Failure::kCrypto);
    }
    return {.outcome = Outcome::kContinue, .replies = {std::move(*message)}};
}

Step Initiator::receive(std::string_view text) {
    if (state_ == State::kClientInit) {
        const std::expected<ClientInit, InitError> init = parse_client_init(text);
        if (!init) {
            init_error_ = init.error();
            Step step = fail(Failure::kBadInit);
            step.replies.push_back(write_server_error(init.error()));
            return step;
        }
        client_key_ = init->client_key;
        suite_ = init->suite;
        choice_ = keyring_->choose(client_key_);
        std::string server_init = write_server_init({.server_key = identity_.public_key()});
        handshake_.emplace(suite_, noise::Role::kInitiator, identity_, client_key_,
                           init_prologue(text, server_init));
        std::optional<std::string> message = message_1();
        if (!message) {
            return fail(Failure::kCrypto);
        }
        state_ = State::kMessage2;
        return {.outcome = Outcome::kContinue, .replies = {std::move(server_init), std::move(*message)}};
    }
    if (state_ != State::kMessage2 || !handshake_) {
        return fail(Failure::kUnexpected);
    }

    const std::optional<std::vector<std::uint8_t>> message = parse_noise_handshake(text);
    if (!message) {
        return fail(Failure::kBadHandshake);
    }
    // A copy before message 2 is read, for the Sentinel retry (connection.md, Sentinel
    // Fallback).
    noise::Handshake retry = *handshake_;
    std::vector<std::uint8_t> payload;
    bool accepted = handshake_->read_message_2(choice_.psk, *message, payload) &&
                    parse_message_2_payload(payload);
    category_ = choice_.category;
    if (!accepted && choice_.category != PskCategory::kSentinel) {
        payload.clear();
        if (retry.read_message_2(sentinel_psk(), *message, payload) && parse_message_2_payload(payload)) {
            handshake_ = std::move(retry);
            accepted = true;
            mismatch_ = true;
            category_ = PskCategory::kSentinel;
        }
    }
    if (!accepted) {
        return fail(Failure::kBadHandshake);
    }
    state_ = State::kDone;
    return {.outcome = Outcome::kEstablished, .replies = {}};
}

std::optional<noise::Handshake::Transport> Initiator::take_keys() {
    if (state_ != State::kDone || failure_ != Failure::kNone || !handshake_) {
        return std::nullopt;
    }
    return handshake_->split();
}

}  // namespace ac3::sendspin::handshake
