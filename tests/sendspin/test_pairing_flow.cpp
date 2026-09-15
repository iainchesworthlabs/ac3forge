#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/messages.hpp"
#include "ac3/sendspin/noise.hpp"
#include "ac3/sendspin/pairing_flow.hpp"
#include "ac3/sendspin/pairing_messages.hpp"

// The pairing flows run between ClientPairing and ServerPairing directly, message by message:
// the Pairing PSK Flow, the dynamic code in digits and as a QR token with a mistyped code and
// its retry round, aiosendspin 9.1.1's dynamic code with no rounds, the static code behind its
// window, the round limit, and the attempt timeout.

namespace {

using ac3::sendspin::Dialect;
using ac3::sendspin::crypto::Digest32;
using ac3::sendspin::crypto::Key32;
namespace flow = ac3::sendspin::pairing_flow;
namespace m = ac3::sendspin::messages;
namespace hs = ac3::sendspin::handshake;
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
        REQUIRE(envelope.has_value());
    }
    Parsed(const Parsed&) = delete;
    Parsed& operator=(const Parsed&) = delete;
    Parsed(Parsed&&) = delete;
    Parsed& operator=(Parsed&&) = delete;
    ~Parsed() = default;
};

struct ClientEvents final : flow::ClientPairingEvents {
    std::vector<flow::Code> codes;
    int held_back = 0;
    std::optional<Key32> paired;

    void on_code(const flow::Code& code) override { codes.push_back(code); }
    void on_held_back() override { ++held_back; }
    void on_paired(const Key32& psk) override { paired = psk; }
};

Digest32 hash_of(std::uint8_t seed) {
    Digest32 h{};
    for (std::size_t i = 0; i < h.size(); ++i) {
        h[i] = static_cast<std::uint8_t>(seed + (i * 3));
    }
    return h;
}

const m::PairMethodDescriptor kPskDescriptor{
    .method = m::PairMethod::kPairingPsk, .locations = {}, .out_channels = {}, .formats = {}, .min_pin_length = 0};
const m::PairMethodDescriptor kDynamicDescriptor{.method = m::PairMethod::kDynamicCode,
                                                 .locations = {},
                                                 .out_channels = {m::OutChannel::kDisplay},
                                                 .formats = {m::CodeFormat::kDigits, m::CodeFormat::kQrCode},
                                                 .min_pin_length = 6};
const m::PairMethodDescriptor kStaticDescriptor{
    .method = m::PairMethod::kStaticCode, .locations = {m::SecretLocation::kDevice}, .out_channels = {}, .formats = {},
    .min_pin_length = 0};

// A client and a server pairing on one connection.
struct Pairing {
    Dialect dialect;
    ac3::sendspin::noise::Suite suite = ac3::sendspin::noise::Suite::kAesGcmSha256;
    flow::ClientPairingState state;
    ClientEvents events;
    std::unique_ptr<flow::ClientPairing> client;
    std::unique_ptr<flow::ServerPairing> server;
    std::int64_t now = 0;
    flow::After client_after = flow::After::kContinue;
    flow::After server_after = flow::After::kContinue;

    Pairing(Dialect d, hs::PskCategory matched, std::vector<m::PairMethodDescriptor> offered,
            std::string static_code = "12345678", Digest32 client_hash = hash_of(1), Digest32 server_hash = hash_of(1))
        : dialect(d) {
        client = std::make_unique<flow::ClientPairing>(
            flow::ClientPairingConfig{.suite = suite,
                                      .dialect = dialect,
                                      .handshake_hash = client_hash,
                                      .matched = matched,
                                      .offered = std::move(offered),
                                      .static_code = std::move(static_code)},
            state, events);
        server = std::make_unique<flow::ServerPairing>(flow::ServerPairingConfig{
            .suite = suite, .dialect = dialect, .handshake_hash = server_hash, .matched = matched});
    }

    // Delivers `messages` to the server, and what the server answers to the client, until
    // neither has anything more to say.
    void to_server(std::vector<std::string> messages) {
        while (!messages.empty()) {
            std::vector<std::string> answers;
            for (const std::string& text : messages) {
                const Parsed parsed(text);
                flow::Step step = server->receive(parsed.envelope->type, parsed.envelope->payload);
                server_after = step.after;
                for (std::string& answer : step.messages) {
                    answers.push_back(std::move(answer));
                }
            }
            messages.clear();
            for (const std::string& text : answers) {
                const Parsed parsed(text);
                flow::Step step = client->receive(parsed.envelope->type, parsed.envelope->payload, now);
                client_after = step.after;
                for (std::string& reply : step.messages) {
                    messages.push_back(std::move(reply));
                }
            }
        }
    }

    // The server's own step (the operator entered a code, or cancelled): its messages go to the
    // client, and the client's answers back.
    void from_server(flow::Step step) {
        server_after = step.after;
        std::vector<std::string> replies;
        for (const std::string& text : step.messages) {
            const Parsed parsed(text);
            flow::Step answer = client->receive(parsed.envelope->type, parsed.envelope->payload, now);
            client_after = answer.after;
            for (std::string& reply : answer.messages) {
                replies.push_back(std::move(reply));
            }
        }
        to_server(std::move(replies));
    }

    void start(const m::PairingActivation& activation, std::uint32_t index = 1) {
        server->activated(activation, index);
        flow::Step step = client->start(activation, index, now);
        client_after = step.after;
        to_server(std::move(step.messages));
    }
};

std::string digits_of(const flow::Code& code) {
    REQUIRE(std::holds_alternative<std::string>(code));
    return std::get<std::string>(code);
}

}  // namespace

TEST_CASE("pairing flow: the Pairing PSK Flow", "[sendspin][pairing_flow]") {
    const Dialect dialect = GENERATE(Dialect::kSpecification, Dialect::kAiosendspin911);
    Pairing pairing(dialect, hs::PskCategory::kPairing, {kPskDescriptor});
    pairing.start({.method = m::PairMethod::kPairingPsk, .format = std::nullopt, .pin_length = 0, .languages = {}});
    CHECK(pairing.server_after == flow::After::kPaired);
    CHECK(pairing.client_after == flow::After::kPaired);
    REQUIRE(pairing.events.paired.has_value());
    CHECK(*pairing.events.paired == pairing.server->long_term_psk());
    CHECK(pairing.client->finished());
}

TEST_CASE("pairing flow: pairing_psk needs the pairing PSK to have matched", "[sendspin][pairing_flow]") {
    Pairing pairing(Dialect::kSpecification, hs::PskCategory::kSentinel, {kPskDescriptor, kDynamicDescriptor});
    const flow::Step step = pairing.client->start(
        {.method = m::PairMethod::kPairingPsk, .format = std::nullopt, .pin_length = 0, .languages = {}}, 1, 0);
    CHECK(step.after == flow::After::kEnded);
    REQUIRE(step.messages.size() == 1);
    CHECK(step.messages[0] == R"({"type":"pair/abort","payload":{"reason":"method_not_supported"}})");
    // And a code method is refused on the pairing PSK.
    Pairing other(Dialect::kSpecification, hs::PskCategory::kPairing, {kPskDescriptor, kDynamicDescriptor});
    CHECK(other.client
              ->start({.method = m::PairMethod::kDynamicCode, .format = m::CodeFormat::kDigits, .pin_length = 0, .languages = {}},
                      1, 0)
              .after == flow::After::kEnded);
}

TEST_CASE("pairing flow: the dynamic code in digits, with a mistyped code and a second round",
          "[sendspin][pairing_flow]") {
    Pairing pairing(Dialect::kSpecification, hs::PskCategory::kSentinel, {kPskDescriptor, kDynamicDescriptor});
    pairing.start({.method = m::PairMethod::kDynamicCode, .format = m::CodeFormat::kDigits, .pin_length = 0, .languages = {}});
    REQUIRE(pairing.events.codes.size() == 1);
    const std::string code = digits_of(pairing.events.codes[0]);
    CHECK(code.size() == 6);
    REQUIRE(pairing.server->wants_code());

    // The operator mistypes: the client's check of server_kc fails and it asks for another
    // round, which emits the same code again.
    std::string wrong = code;
    wrong[0] = wrong[0] == '9' ? '0' : static_cast<char>(wrong[0] + 1);
    pairing.from_server(pairing.server->enter_code(wrong));
    REQUIRE(pairing.server->wants_code());
    REQUIRE(pairing.events.codes.size() == 2);
    CHECK(digits_of(pairing.events.codes[1]) == code);
    CHECK(pairing.state.rounds_since_verified == 1);

    pairing.from_server(pairing.server->enter_code(code));
    CHECK(pairing.server_after == flow::After::kPaired);
    CHECK(pairing.client_after == flow::After::kPaired);
    REQUIRE(pairing.events.paired.has_value());
    CHECK(*pairing.events.paired == pairing.server->long_term_psk());
    CHECK(pairing.state.rounds_since_verified == 0);
}

TEST_CASE("pairing flow: the dynamic code as a QR token", "[sendspin][pairing_flow]") {
    Pairing pairing(Dialect::kSpecification, hs::PskCategory::kSentinel, {kPskDescriptor, kDynamicDescriptor});
    pairing.start({.method = m::PairMethod::kDynamicCode, .format = m::CodeFormat::kQrCode, .pin_length = 0, .languages = {}});
    REQUIRE(pairing.events.codes.size() == 1);
    REQUIRE(std::holds_alternative<std::array<std::uint8_t, 24>>(pairing.events.codes[0]));
    // Digits are not what this attempt takes.
    CHECK(pairing.server->enter_code(std::string("123456")).messages.empty());
    pairing.from_server(pairing.server->enter_code(pairing.events.codes[0]));
    CHECK(pairing.server_after == flow::After::kPaired);
    CHECK(pairing.client_after == flow::After::kPaired);
}

TEST_CASE("pairing flow: aiosendspin 9.1.1's dynamic code, and no second round", "[sendspin][pairing_flow]") {
    Pairing pairing(Dialect::kAiosendspin911, hs::PskCategory::kSentinel, {kPskDescriptor, kDynamicDescriptor});

    SECTION("a pin_length below the client's minimum is refused") {
        const flow::Step step = pairing.client->start(
            {.method = m::PairMethod::kDynamicCode, .format = std::nullopt, .pin_length = 4, .languages = {}}, 1, 0);
        REQUIRE(step.messages.size() == 1);
        CHECK(step.messages[0] == R"({"type":"pair/abort","payload":{"reason":"pin_length_unacceptable"}})");
    }
    SECTION("eight digits") {
        pairing.start({.method = m::PairMethod::kDynamicCode, .format = std::nullopt, .pin_length = 8, .languages = {"en"}});
        REQUIRE(pairing.events.codes.size() == 1);
        const std::string code = digits_of(pairing.events.codes[0]);
        CHECK(code.size() == 8);
        pairing.from_server(pairing.server->enter_code(code));
        CHECK(pairing.server_after == flow::After::kPaired);
        CHECK(pairing.client_after == flow::After::kPaired);
        CHECK(*pairing.events.paired == pairing.server->long_term_psk());
    }
    SECTION("a mistyped code ends the attempt with pin_mismatch") {
        pairing.start({.method = m::PairMethod::kDynamicCode, .format = std::nullopt, .pin_length = 6, .languages = {}});
        std::string wrong = digits_of(pairing.events.codes[0]);
        wrong[5] = wrong[5] == '0' ? '1' : '0';
        pairing.from_server(pairing.server->enter_code(wrong));
        CHECK(pairing.client_after == flow::After::kEnded);
        CHECK(pairing.server_after == flow::After::kEnded);
        CHECK(pairing.server->aborted() == ac3::sendspin::pairing_messages::AbortReason::kCodeMismatch);
        CHECK_FALSE(pairing.events.paired.has_value());
    }
}

TEST_CASE("pairing flow: two different handshakes cannot pair, whatever code is typed", "[sendspin][pairing_flow]") {
    // A relay in the middle holds two handshakes with different hashes: the client derives its
    // code from one, and the server's CPace session id comes from the other.
    Pairing pairing(Dialect::kSpecification, hs::PskCategory::kSentinel, {kPskDescriptor, kDynamicDescriptor}, "12345678",
                    hash_of(1), hash_of(2));
    pairing.start({.method = m::PairMethod::kDynamicCode, .format = m::CodeFormat::kDigits, .pin_length = 0, .languages = {}});
    const std::string code = digits_of(pairing.events.codes[0]);
    for (int round = 0; round < 3; ++round) {
        pairing.from_server(pairing.server->enter_code(code));
        CHECK_FALSE(pairing.events.paired.has_value());
    }
    CHECK(pairing.state.rounds_since_verified == 3);
}

TEST_CASE("pairing flow: the static code behind its window", "[sendspin][pairing_flow]") {
    const Dialect dialect = GENERATE(Dialect::kSpecification, Dialect::kAiosendspin911);
    Pairing pairing(dialect, hs::PskCategory::kSentinel, {kPskDescriptor, kStaticDescriptor}, "20260915");
    pairing.now = 1'000'000;
    pairing.start({.method = m::PairMethod::kStaticCode, .format = std::nullopt, .pin_length = 0, .languages = {}});
    // No window yet: the client holds the attempt back and says so.
    CHECK(pairing.events.held_back == 1);
    CHECK_FALSE(pairing.server->wants_code());

    flow::Step opened = pairing.client->operator_action(pairing.now);
    pairing.to_server(std::move(opened.messages));
    REQUIRE(pairing.server->wants_code());
    CHECK(pairing.state.window_open(pairing.now));

    pairing.from_server(pairing.server->enter_code(std::string("20260915")));
    CHECK(pairing.server_after == flow::After::kPaired);
    CHECK(pairing.client_after == flow::After::kPaired);
    CHECK(*pairing.events.paired == pairing.server->long_term_psk());
    // A completed pairing closes the window.
    CHECK_FALSE(pairing.state.window_open(pairing.now));
}

TEST_CASE("pairing flow: five wrong static codes close the window", "[sendspin][pairing_flow]") {
    flow::ClientPairingState shared;
    shared.window_opened_at = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        Pairing pairing(Dialect::kSpecification, hs::PskCategory::kSentinel, {kPskDescriptor, kStaticDescriptor}, "20260915");
        pairing.state = shared;
        pairing.start({.method = m::PairMethod::kStaticCode, .format = std::nullopt, .pin_length = 0, .languages = {}},
                      static_cast<std::uint32_t>(attempt + 1));
        REQUIRE(pairing.server->wants_code());
        pairing.from_server(pairing.server->enter_code(std::string("00000000")));
        CHECK(pairing.client_after == flow::After::kEnded);
        shared = pairing.state;
    }
    CHECK(shared.window_failures == 5);
    CHECK_FALSE(shared.window_open(1'000));
}

TEST_CASE("pairing flow: the round limit holds attempts back until the operator acts", "[sendspin][pairing_flow]") {
    Pairing pairing(Dialect::kSpecification, hs::PskCategory::kSentinel, {kPskDescriptor, kDynamicDescriptor});
    pairing.state.rounds_since_verified = flow::ClientPairingState::kRoundLimit - 1;
    pairing.start({.method = m::PairMethod::kDynamicCode, .format = m::CodeFormat::kDigits, .pin_length = 0, .languages = {}});
    std::string wrong = digits_of(pairing.events.codes[0]);
    wrong[0] = wrong[0] == '1' ? '2' : '1';
    pairing.from_server(pairing.server->enter_code(wrong));
    CHECK(pairing.client_after == flow::After::kEnded);
    CHECK(pairing.state.held_back);

    // The next attempt waits for the operator.
    Pairing next(Dialect::kSpecification, hs::PskCategory::kSentinel, {kPskDescriptor, kDynamicDescriptor});
    next.state = pairing.state;
    next.start({.method = m::PairMethod::kDynamicCode, .format = m::CodeFormat::kDigits, .pin_length = 0, .languages = {}}, 2);
    CHECK(next.events.held_back == 1);
    CHECK(next.events.codes.empty());
    flow::Step resumed = next.client->operator_action(0);
    next.to_server(std::move(resumed.messages));
    REQUIRE(next.events.codes.size() == 1);
    next.from_server(next.server->enter_code(next.events.codes[0]));
    CHECK(next.client_after == flow::After::kPaired);
}

TEST_CASE("pairing flow: the attempt timeout, a cancel, and a leftover pairing_index", "[sendspin][pairing_flow]") {
    Pairing pairing(Dialect::kSpecification, hs::PskCategory::kSentinel, {kPskDescriptor, kDynamicDescriptor});
    pairing.start({.method = m::PairMethod::kDynamicCode, .format = m::CodeFormat::kDigits, .pin_length = 0, .languages = {}}, 3);
    CHECK(pairing.client->tick(flow::ClientPairing::kAttemptTimeout - 1).messages.empty());
    const flow::Step timeout = pairing.client->tick(flow::ClientPairing::kAttemptTimeout);
    CHECK(timeout.after == flow::After::kEnded);
    REQUIRE(timeout.messages.size() == 1);
    CHECK(timeout.messages[0] == R"({"type":"pair/abort","payload":{"reason":"attempt_timeout"}})");

    Pairing cancelled(Dialect::kSpecification, hs::PskCategory::kSentinel, {kPskDescriptor, kDynamicDescriptor});
    cancelled.start({.method = m::PairMethod::kDynamicCode, .format = m::CodeFormat::kDigits, .pin_length = 0, .languages = {}});
    cancelled.from_server(cancelled.server->cancel());
    CHECK(cancelled.client_after == flow::After::kEnded);
    CHECK(cancelled.client->finished());

    // A client/pair-init from an earlier activation is ignored; one from a later is an error.
    flow::ServerPairing server({.suite = ac3::sendspin::noise::Suite::kChaChaPolySha256,
                                .dialect = Dialect::kSpecification,
                                .handshake_hash = hash_of(1),
                                .matched = hs::PskCategory::kSentinel});
    server.activated({.method = m::PairMethod::kStaticCode, .format = std::nullopt, .pin_length = 0, .languages = {}}, 2);
    const Parsed old(R"({"type":"client/pair-init","payload":{"pairing_index":1}})");
    CHECK(server.receive(old.envelope->type, old.envelope->payload).after == flow::After::kContinue);
    CHECK_FALSE(server.wants_code());
    const Parsed ahead(R"({"type":"client/pair-init","payload":{"pairing_index":3}})");
    CHECK(server.receive(ahead.envelope->type, ahead.envelope->payload).after == flow::After::kClose);
}
