#include <catch2/catch_test_macros.hpp>

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

// The pairing flows' refusals, beside test_pairing_flow.cpp's successful and mistyped runs:
// activations the client cannot take up, and at each stage of a real attempt a message of the
// wrong type, the right type with nothing in it, or one from a superseded attempt - each of
// which ends the attempt as a protocol error (close the connection), is discarded, or ends it
// with a pair/abort, as pairing.md says for that stage. Both sides are driven message by message
// with real messages up to the stage under test, so each injected message meets exactly the
// state it is aimed at.

namespace {

using ac3::sendspin::Dialect;
using ac3::sendspin::crypto::Digest32;
using ac3::sendspin::crypto::Key32;
namespace flow = ac3::sendspin::pairing_flow;
namespace m = ac3::sendspin::messages;
namespace hs = ac3::sendspin::handshake;
namespace json = ac3::sendspin::json;

// One message, parsed, with the storage its values point into.
struct Message {
    std::string text;
    std::vector<json::Token> tokens;
    json::Document document;
    std::optional<m::Envelope> envelope;

    explicit Message(std::string source) : text(std::move(source)) {
        if (document.parse(text, tokens, 1024)) {
            envelope = m::read_envelope(document);
        }
        REQUIRE(envelope.has_value());
    }
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;
    Message(Message&&) = delete;
    Message& operator=(Message&&) = delete;
    ~Message() = default;
};

// A message of `type` with `payload`, written by hand.
std::string made(const std::string& type, const std::string& payload = "{}") {
    return R"({"type":")" + type + R"(","payload":)" + payload + "}";
}

struct Events final : flow::ClientPairingEvents {
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
        h[i] = static_cast<std::uint8_t>(seed + (i * 5));
    }
    return h;
}

const m::PairMethodDescriptor kPsk{
    .method = m::PairMethod::kPairingPsk, .locations = {}, .out_channels = {}, .formats = {}, .min_pin_length = 0};
const m::PairMethodDescriptor kDigitsOnly{.method = m::PairMethod::kDynamicCode,
                                          .locations = {},
                                          .out_channels = {m::OutChannel::kDisplay},
                                          .formats = {m::CodeFormat::kDigits},
                                          .min_pin_length = 6};
const m::PairMethodDescriptor kStatic{.method = m::PairMethod::kStaticCode,
                                      .locations = {m::SecretLocation::kDevice},
                                      .out_channels = {},
                                      .formats = {},
                                      .min_pin_length = 0};

m::PairingActivation dynamic_digits() {
    return {.method = m::PairMethod::kDynamicCode, .format = m::CodeFormat::kDigits, .pin_length = 0, .languages = {}};
}
m::PairingActivation static_code() {
    return {.method = m::PairMethod::kStaticCode, .format = std::nullopt, .pin_length = 0, .languages = {}};
}
m::PairingActivation pairing_psk() {
    return {.method = m::PairMethod::kPairingPsk, .format = std::nullopt, .pin_length = 0, .languages = {}};
}

// A client and a server on one connection, advanced stage by stage with real messages.
struct Duo {
    flow::ClientPairingState state;
    Events events;
    std::unique_ptr<flow::ClientPairing> client;
    std::unique_ptr<flow::ServerPairing> server;
    std::vector<std::string> to_server;  // the client's last messages
    std::vector<std::string> to_client;  // the server's last messages

    Duo(Dialect dialect, hs::PskCategory matched, std::vector<m::PairMethodDescriptor> offered,
        std::string code = "12345678") {
        client = std::make_unique<flow::ClientPairing>(
            flow::ClientPairingConfig{.suite = ac3::sendspin::noise::Suite::kAesGcmSha256,
                                      .dialect = dialect,
                                      .handshake_hash = hash_of(3),
                                      .matched = matched,
                                      .offered = std::move(offered),
                                      .static_code = std::move(code),
                                      .connection = 1},
            state, events);
        server = std::make_unique<flow::ServerPairing>(flow::ServerPairingConfig{
            .suite = ac3::sendspin::noise::Suite::kAesGcmSha256, .dialect = dialect, .handshake_hash = hash_of(3),
            .matched = matched});
    }

    flow::Step start(const m::PairingActivation& activation, std::uint32_t index = 1) {
        server->activated(activation, index);
        flow::Step step = client->start(activation, index, 0);
        to_server = step.messages;
        return step;
    }

    flow::Step client_gets(const std::string& text) {
        const Message message(text);
        flow::Step step = client->receive(message.envelope->type, message.envelope->payload, 0);
        to_server = step.messages;
        return step;
    }

    flow::Step server_gets(const std::string& text) {
        const Message message(text);
        flow::Step step = server->receive(message.envelope->type, message.envelope->payload);
        to_client = step.messages;
        return step;
    }

    // Delivers the client's pending messages to the server, one by one.
    void deliver_to_server() {
        const std::vector<std::string> pending = to_server;
        for (const std::string& text : pending) {
            REQUIRE(server_gets(text).after == flow::After::kContinue);
        }
    }
    void deliver_to_client() {
        const std::vector<std::string> pending = to_client;
        for (const std::string& text : pending) {
            REQUIRE(client_gets(text).after == flow::After::kContinue);
        }
    }

    // A dynamic-code attempt, as far as the code being shown and wanted.
    void to_code_shown() {
        start(dynamic_digits());
        deliver_to_server();  // client/pair-init -> server/pair-init
        deliver_to_client();  // the code is shown
        REQUIRE(events.codes.size() == 1);
        REQUIRE(server->wants_code());
    }
    // ... and the operator has entered it: the client has answered server/pair-auth.
    void to_client_confirming() {
        to_code_shown();
        const flow::Step entered = server->enter_code(events.codes[0]);
        to_client = entered.messages;
        deliver_to_client();  // server/pair-auth -> client/pair-auth
    }
    // ... and the server has sent its confirmation: the client is waiting for server/pair-finalize.
    void to_client_finalizing() {
        to_client_confirming();
        deliver_to_server();  // client/pair-auth -> server/pair-confirm
        deliver_to_client();  // -> client/pair-confirm, client/pair-finalize
    }
};

}  // namespace

TEST_CASE("pairing flow errors: activations the client cannot take up end the attempt", "[sendspin][pairing_flow]") {
    SECTION("no method") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        const auto step =
            duo.client->start({.method = std::nullopt, .format = std::nullopt, .pin_length = 0, .languages = {}}, 1, 0);
        CHECK(step.after == flow::After::kEnded);
        CHECK(duo.client->aborted() == ac3::sendspin::pairing_messages::AbortReason::kMethodNotSupported);
    }
    SECTION("a code format the client does not offer, or none") {
        for (const std::optional<m::CodeFormat> format : {std::optional{m::CodeFormat::kQrCode}, std::optional<m::CodeFormat>{}}) {
            Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
            const auto step = duo.client->start(
                {.method = m::PairMethod::kDynamicCode, .format = format, .pin_length = 0, .languages = {}}, 1, 0);
            CHECK(step.after == flow::After::kEnded);
            CHECK(duo.client->aborted() == ac3::sendspin::pairing_messages::AbortReason::kMethodNotSupported);
        }
    }
    SECTION("aiosendspin's pin length outside what the client allows") {
        for (const int length : {4, 13}) {
            Duo duo(Dialect::kAiosendspin911, hs::PskCategory::kSentinel, {kDigitsOnly});
            const auto step = duo.client->start(
                {.method = m::PairMethod::kDynamicCode, .format = std::nullopt, .pin_length = length, .languages = {}}, 1, 0);
            CHECK(step.after == flow::After::kEnded);
            CHECK(duo.client->aborted() == ac3::sendspin::pairing_messages::AbortReason::kPinLengthUnacceptable);
        }
    }
    SECTION("a static code that is not eight digits") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kStatic}, "1234");
        const auto step = duo.client->start(static_code(), 1, 0);
        CHECK(step.after == flow::After::kEnded);
        CHECK(duo.client->aborted() == ac3::sendspin::pairing_messages::AbortReason::kMethodNotSupported);
    }
}

TEST_CASE("pairing flow errors: the client refuses a message its stage does not expect", "[sendspin][pairing_flow]") {
    SECTION("waiting for server/pair-init") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.start(dynamic_digits());
        CHECK(duo.client_gets(made("server/pair-auth")).after == flow::After::kClose);
        CHECK(duo.client->finished());
        // Once finished, anything still in flight is discarded.
        CHECK(duo.client_gets(made("server/pair-init")).messages.empty());
    }
    SECTION("a first server/pair-init without its nonce") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.start(dynamic_digits());
        CHECK(duo.client_gets(made("server/pair-init")).after == flow::After::kClose);
    }
    SECTION("waiting for server/pair-auth") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.to_code_shown();
        CHECK(duo.client_gets(made("server/pair-confirm")).after == flow::After::kClose);
    }
    SECTION("an empty server/pair-auth") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.to_code_shown();
        CHECK(duo.client_gets(made("server/pair-auth")).after == flow::After::kClose);
    }
    SECTION("waiting for server/pair-confirm") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.to_client_confirming();
        CHECK(duo.client_gets(made("server/pair-finalize")).after == flow::After::kClose);
    }
    SECTION("an empty server/pair-confirm") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.to_client_confirming();
        CHECK(duo.client_gets(made("server/pair-confirm")).after == flow::After::kClose);
    }
    SECTION("waiting for server/pair-finalize") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.to_client_finalizing();
        CHECK(duo.client_gets(made("server/pair-init")).after == flow::After::kClose);
    }
    SECTION("held back by the round limit") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.state.rounds_since_verified = flow::ClientPairingState::kRoundLimit;
        duo.start(dynamic_digits());
        REQUIRE(duo.client->held_back());
        // A tick does nothing to an attempt that has not begun, nor does a resume the limit still holds.
        CHECK(duo.client->tick(1'000'000'000).messages.empty());
        CHECK(duo.client->resume(0).messages.empty());
        CHECK(duo.client_gets(made("server/pair-init")).after == flow::After::kClose);
    }
    SECTION("a pair/abort for a concurrent attempt closes the connection") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.start(dynamic_digits());
        CHECK(duo.client_gets(made("pair/abort", R"({"reason":"concurrent_attempt"})")).after == flow::After::kClose);
        CHECK(duo.client->aborted() == ac3::sendspin::pairing_messages::AbortReason::kConcurrentAttempt);
        // Cancelling what has already ended says nothing.
        CHECK(duo.client->cancel().messages.empty());
    }
}

TEST_CASE("pairing flow errors: the server refuses a message its stage does not expect", "[sendspin][pairing_flow]") {
    SECTION("the Pairing PSK Flow: anything but client/pair-finalize, or one without the PSK") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kPairing, {kPsk});
        duo.server->activated(pairing_psk(), 1);
        CHECK(duo.server_gets(made("client/pair-init", R"({"pairing_index":1})")).after == flow::After::kClose);
        Duo again(Dialect::kSpecification, hs::PskCategory::kPairing, {kPsk});
        again.server->activated(pairing_psk(), 1);
        CHECK(again.server_gets(made("client/pair-finalize")).after == flow::After::kClose);
    }
    SECTION("a code flow's first message is not client/pair-init") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.server->activated(dynamic_digits(), 1);
        CHECK(duo.server_gets(made("client/pair-auth")).after == flow::After::kClose);
        CHECK(duo.server->finished());
        CHECK(duo.server_gets(made("client/pair-init")).messages.empty());  // after the end: discarded
        CHECK(duo.server->cancel().messages.empty());
    }
    SECTION("a client/pair-init from a later attempt than this one") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.server->activated(dynamic_digits(), 2);
        CHECK(duo.server_gets(made("client/pair-init", R"({"pairing_index":3})")).after == flow::After::kClose);
    }
    SECTION("a client/pair-init from a superseded attempt is discarded, and so are its other messages") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.server->activated(dynamic_digits(), 2);
        CHECK(duo.server_gets(made("client/pair-auth")).after == flow::After::kContinue);
        const auto leftover = duo.server_gets(made("client/pair-init", R"({"pairing_index":1})"));
        CHECK(leftover.after == flow::After::kContinue);
        CHECK(leftover.messages.empty());
        CHECK_FALSE(duo.server->started());
    }
    SECTION("a dynamic code's client/pair-init without its commitment") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.server->activated(dynamic_digits(), 1);
        CHECK(duo.server_gets(made("client/pair-init", R"({"pairing_index":1})")).after == flow::After::kClose);
    }
    SECTION("a static code's client/pair-init with a commitment") {
        Duo real(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        real.start(dynamic_digits());  // a genuine client/pair-init that carries commit_b
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kStatic});
        duo.server->activated(static_code(), 1);
        CHECK(duo.server_gets(real.to_server.at(0)).after == flow::After::kClose);
    }
    SECTION("a client/pair-pending: kept with its message, and refused from a later attempt") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.server->activated(dynamic_digits(), 1);
        CHECK(duo.server_gets(made("client/pair-pending", R"({"pairing_index":1,"message":"press the button"})"))
                  .after == flow::After::kContinue);
        CHECK(duo.server->held_back());
        CHECK(duo.server->pending() == std::optional<std::string>{"press the button"});
        CHECK(duo.server_gets(made("client/pair-pending", R"({"pairing_index":7})")).after == flow::After::kClose);
    }
    SECTION("waiting for the operator's code") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.to_code_shown();
        // A code of the wrong shape is not taken; the operator can type it again.
        CHECK(duo.server->enter_code(std::string("12")).messages.empty());
        CHECK(duo.server->enter_code(std::array<std::uint8_t, 24>{}).messages.empty());
        CHECK(duo.server->wants_code());
        CHECK(duo.server_gets(made("client/pair-auth")).after == flow::After::kClose);
    }
    SECTION("waiting for client/pair-auth") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.to_code_shown();
        static_cast<void>(duo.server->enter_code(duo.events.codes[0]));
        CHECK(duo.server->enter_code(duo.events.codes[0]).messages.empty());  // not wanted any more
        CHECK(duo.server_gets(made("client/pair-confirm")).after == flow::After::kClose);
    }
    SECTION("an empty client/pair-auth") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.to_code_shown();
        static_cast<void>(duo.server->enter_code(duo.events.codes[0]));
        CHECK(duo.server_gets(made("client/pair-auth")).after == flow::After::kClose);
    }
    SECTION("waiting for client/pair-confirm: another type, an empty one, or one without its wrapped nonce") {
        for (const std::string& wrong : {made("client/pair-finalize"), made("client/pair-confirm")}) {
            Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
            duo.to_client_confirming();
            duo.deliver_to_server();  // the server is now waiting for client/pair-confirm
            CHECK(duo.server_gets(wrong).after == flow::After::kClose);
        }
    }
    SECTION("waiting for client/pair-finalize") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.to_client_finalizing();
        REQUIRE(duo.to_server.size() == 2);
        REQUIRE(duo.server_gets(duo.to_server[0]).after == flow::After::kContinue);  // the confirm
        CHECK(duo.server_gets(made("client/pair-confirm")).after == flow::After::kClose);
        Duo again(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        again.to_client_finalizing();
        REQUIRE(again.server_gets(again.to_server[0]).after == flow::After::kContinue);
        CHECK(again.server_gets(made("client/pair-finalize")).after == flow::After::kClose);
    }
    SECTION("a pair/abort whose reason cannot be read counts as a cancel") {
        Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kDigitsOnly});
        duo.server->activated(dynamic_digits(), 1);
        CHECK(duo.server_gets(made("pair/abort")).after == flow::After::kEnded);
        CHECK(duo.server->aborted() == ac3::sendspin::pairing_messages::AbortReason::kUserCancelled);
    }
}

TEST_CASE("pairing flow errors: a static code's retry request is a protocol error", "[sendspin][pairing_flow]") {
    Duo duo(Dialect::kSpecification, hs::PskCategory::kSentinel, {kStatic});
    duo.state.open_window(0);
    duo.start(static_code());
    duo.deliver_to_server();  // client/pair-init: the server now wants the code
    REQUIRE(duo.server->wants_code());
    const flow::Step entered = duo.server->enter_code(std::string("12345678"));
    duo.to_client = entered.messages;
    duo.deliver_to_client();  // server/pair-auth -> client/pair-auth
    duo.deliver_to_server();  // -> server/pair-confirm: the server waits for client/pair-confirm
    CHECK(duo.server_gets(made("client/pair-retry")).after == flow::After::kClose);
}
