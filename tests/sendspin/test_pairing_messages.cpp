#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/pairing_messages.hpp"

// The pairing messages in both dialects: each written as pairing.md and aiosendspin 9.1.1's
// noise/models.py define it, read back, and refused when a value has the wrong length or a
// required one is missing.

namespace {

using ac3::sendspin::Dialect;
namespace pm = ac3::sendspin::pairing_messages;
namespace m = ac3::sendspin::messages;
namespace json = ac3::sendspin::json;

struct Parsed {
    std::string text;
    std::vector<json::Token> tokens;
    json::Document document;
    std::optional<m::Envelope> envelope;

    explicit Parsed(std::string message) : text(std::move(message)) {
        if (document.parse(text, tokens, 1024)) {
            envelope = m::read_envelope(document);
        }
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    Parsed(Parsed&&) = delete;
    Parsed& operator=(Parsed&&) = delete;
    ~Parsed() = default;

    [[nodiscard]] json::Value payload() const {
        REQUIRE(envelope.has_value());
        return envelope->payload;
    }
};

template <class T>
T filled(std::uint8_t first) {
    T bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(first + i);
    }
    return bytes;
}

}  // namespace

TEST_CASE("pairing messages: pair-pending and pair-init", "[sendspin][pairing_messages]") {
    const std::string pending = pm::write_pair_pending({.pairing_index = 2, .message = "Press the button"},
                                                       Dialect::kSpecification);
    CHECK(pending == R"({"type":"client/pair-pending","payload":{"pairing_index":2,"message":"Press the button"}})");
    CHECK(pm::write_pair_pending({.pairing_index = 2, .message = "Press the button"}, Dialect::kAiosendspin911) ==
          R"({"type":"client/pair-pending","payload":{"pairing_index":2}})");
    const auto read_pending = pm::read_pair_pending(Parsed(pending).payload());
    REQUIRE(read_pending.has_value());
    CHECK(read_pending->pairing_index == 2);
    CHECK(read_pending->message == "Press the button");

    const pm::Digest32 commit = filled<pm::Digest32>(0);
    const std::string init = pm::write_client_pair_init({.pairing_index = 1, .commit_b = commit});
    CHECK(init ==
          R"({"type":"client/pair-init","payload":{"pairing_index":1,"commit_B":"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"}})");
    const auto read_init = pm::read_client_pair_init(Parsed(init).payload());
    REQUIRE(read_init.has_value());
    CHECK(read_init->commit_b == commit);
    CHECK_FALSE(pm::read_client_pair_init(
                    Parsed(R"({"type":"client/pair-init","payload":{"pairing_index":1,"commit_B":"AAEC"}})").payload())
                    .has_value());
    CHECK_FALSE(pm::read_client_pair_init(Parsed(R"({"type":"client/pair-init","payload":{}})").payload()).has_value());

    // server/pair-init: nonce_A in the first round only, and always in 9.1.1.
    const std::string later = pm::write_server_pair_init({.nonce_a = std::nullopt});
    CHECK(later == R"({"type":"server/pair-init","payload":{}})");
    CHECK(pm::read_server_pair_init(Parsed(later).payload(), Dialect::kSpecification).has_value());
    CHECK_FALSE(pm::read_server_pair_init(Parsed(later).payload(), Dialect::kAiosendspin911).has_value());
    const pm::Key32 nonce = filled<pm::Key32>(100);
    const auto first = pm::read_server_pair_init(Parsed(pm::write_server_pair_init({.nonce_a = nonce})).payload(),
                                                 Dialect::kAiosendspin911);
    REQUIRE(first.has_value());
    CHECK(first->nonce_a == nonce);
}

TEST_CASE("pairing messages: shares, tags, confirm and finalize", "[sendspin][pairing_messages]") {
    const pm::Key32 ya = filled<pm::Key32>(1);
    const pm::Key32 yb = filled<pm::Key32>(2);
    CHECK(pm::read_server_pair_auth(Parsed(pm::write_server_pair_auth(ya)).payload()) == ya);
    CHECK(pm::read_client_pair_auth(Parsed(pm::write_client_pair_auth(yb)).payload()) == yb);
    CHECK_FALSE(pm::read_client_pair_auth(Parsed(pm::write_server_pair_auth(ya)).payload()).has_value());

    const pm::Digest64 tag = filled<pm::Digest64>(7);
    const std::string confirm = pm::write_server_pair_confirm(tag);
    // A 64-byte tag is 86 characters.
    CHECK(confirm.size() == std::string(R"({"type":"server/pair-confirm","payload":{"server_kc":""}})").size() + 86);
    CHECK(pm::read_server_pair_confirm(Parsed(confirm).payload()) == tag);

    const pm::Wrapped wrapped = filled<pm::Wrapped>(9);
    const pm::Key32 nonce_b = filled<pm::Key32>(40);
    const pm::ClientPairConfirm both{.client_kc = tag, .wrapped_nonce_b = wrapped, .nonce_b = nonce_b};
    const std::string spec = pm::write_client_pair_confirm(both, Dialect::kSpecification);
    CHECK(spec.find("wrapped_nonce_B") != std::string::npos);
    CHECK(spec.find("\"nonce_B\"") == std::string::npos);
    const auto spec_read = pm::read_client_pair_confirm(Parsed(spec).payload(), Dialect::kSpecification);
    REQUIRE(spec_read.has_value());
    CHECK(spec_read->client_kc == tag);
    CHECK(spec_read->wrapped_nonce_b == wrapped);
    CHECK_FALSE(spec_read->nonce_b.has_value());

    const std::string legacy = pm::write_client_pair_confirm(both, Dialect::kAiosendspin911);
    CHECK(legacy.find("wrapped_nonce_B") == std::string::npos);
    const auto legacy_read = pm::read_client_pair_confirm(Parsed(legacy).payload(), Dialect::kAiosendspin911);
    REQUIRE(legacy_read.has_value());
    CHECK(legacy_read->nonce_b == nonce_b);
    CHECK_FALSE(legacy_read->wrapped_nonce_b.has_value());

    CHECK(pm::write_client_pair_retry() == R"({"type":"client/pair-retry","payload":{}})");
    CHECK(pm::write_server_pair_finalize() == R"({"type":"server/pair-finalize","payload":{}})");

    const auto direct = pm::read_client_pair_finalize(
        Parsed(pm::write_client_pair_finalize({.long_term_psk = ya, .wrapped_psk = std::nullopt})).payload());
    REQUIRE(direct.has_value());
    CHECK(direct->long_term_psk == ya);
    const auto sealed = pm::read_client_pair_finalize(
        Parsed(pm::write_client_pair_finalize({.long_term_psk = std::nullopt, .wrapped_psk = wrapped})).payload());
    REQUIRE(sealed.has_value());
    CHECK(sealed->wrapped_psk == wrapped);
    CHECK_FALSE(pm::read_client_pair_finalize(Parsed(R"({"type":"client/pair-finalize","payload":{}})").payload())
                    .has_value());
}

TEST_CASE("pairing messages: abort reasons in both dialects", "[sendspin][pairing_messages]") {
    CHECK(pm::write_pair_abort(pm::AbortReason::kCodeMismatch, Dialect::kSpecification) ==
          R"({"type":"pair/abort","payload":{"reason":"pairing_code_mismatch"}})");
    CHECK(pm::write_pair_abort(pm::AbortReason::kCodeMismatch, Dialect::kAiosendspin911) ==
          R"({"type":"pair/abort","payload":{"reason":"pin_mismatch"}})");
    CHECK(pm::write_pair_abort(pm::AbortReason::kPinLengthUnacceptable, Dialect::kSpecification) ==
          R"({"type":"pair/abort","payload":{"reason":"user_cancelled"}})");

    for (const pm::AbortReason reason :
         {pm::AbortReason::kAttemptTimeout, pm::AbortReason::kConcurrentAttempt, pm::AbortReason::kMethodNotSupported,
          pm::AbortReason::kCodeMismatch, pm::AbortReason::kUserCancelled}) {
        for (const Dialect dialect : {Dialect::kSpecification, Dialect::kAiosendspin911}) {
            CHECK(pm::read_pair_abort(Parsed(pm::write_pair_abort(reason, dialect)).payload(), dialect) == reason);
        }
    }
    // Each dialect's mismatch name is unknown to the other.
    CHECK_FALSE(pm::read_pair_abort(Parsed(pm::write_pair_abort(pm::AbortReason::kCodeMismatch, Dialect::kAiosendspin911)).payload(),
                                    Dialect::kSpecification)
                    .has_value());
}
