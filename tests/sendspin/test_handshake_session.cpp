#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/sendspin/base64url.hpp"
#include "ac3/sendspin/channel.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/dialect.hpp"
#include "ac3/sendspin/frames.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/handshake_session.hpp"
#include "ac3/sendspin/json.hpp"
#include "ac3/sendspin/noise.hpp"

// The handshake phase run end to end between the two state machines: every way the PSK can
// be chosen, the Sentinel fallback and the mismatch signal it gives the server, the prologue
// catching a changed init message, aiosendspin 9.1.1's message 1, and a re-handshake carried
// through the channel it replaces the keys of.

namespace {

namespace hs = ac3::sendspin::handshake;
using ac3::sendspin::Channel;
using ac3::sendspin::Dialect;
using ac3::sendspin::crypto::Digest32;
using ac3::sendspin::crypto::Key32;
using ac3::sendspin::noise::KeyPair;
using ac3::sendspin::noise::Suite;

class ClientKeys final : public hs::ClientKeyring {
   public:
    std::vector<hs::PskCandidate> held;

    [[nodiscard]] std::optional<hs::PskCandidate> find(const Digest32& id,
                                                       std::optional<hs::PskCategory> category) const override {
        for (const hs::PskCandidate& candidate : held) {
            if ((!category || candidate.category == *category) && hs::psk_id(candidate.psk) == id) {
                return candidate;
            }
        }
        return std::nullopt;
    }
};

class ServerKeys final : public hs::ServerKeyring {
   public:
    hs::PskChoice choice = hs::sentinel_choice();

    [[nodiscard]] hs::PskChoice choose(const Key32& /*client_key*/) const override { return choice; }
};

KeyPair generated() {
    std::optional<KeyPair> pair = KeyPair::generate();
    REQUIRE(pair.has_value());
    return *pair;
}

Key32 random_key() {
    Key32 key{};
    REQUIRE(ac3::sendspin::crypto::random_bytes(key));
    return key;
}

std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {static_cast<const std::uint8_t*>(static_cast<const void*>(text.data())), text.size()};
}

// Passes each side's replies to the other until both have finished; true when both are
// established.
bool run(hs::Initiator& server, hs::Responder& client) {
    hs::Step from_server = server.receive(client.client_init());
    hs::Step from_client;
    for (const std::string& text : from_server.replies) {
        if (from_server.outcome == hs::Outcome::kFailed) {
            break;
        }
        from_client = client.receive(text);
        if (from_client.outcome == hs::Outcome::kFailed) {
            return false;
        }
    }
    if (from_server.outcome == hs::Outcome::kFailed) {
        return false;
    }
    for (const std::string& text : from_client.replies) {
        from_server = server.receive(text);
    }
    return from_server.outcome == hs::Outcome::kEstablished &&
           from_client.outcome == hs::Outcome::kEstablished;
}

// One message through two channels made from each side's keys, both ways.
void check_channels(Channel& server, Channel& client) {
    const std::vector<std::uint8_t> hello{0, '{', '}'};
    std::vector<std::vector<std::uint8_t>> sealed;
    REQUIRE(server.seal(hello, sealed));
    REQUIRE(sealed.size() == 1);
    Channel::Opened opened = client.open(sealed[0]);
    REQUIRE(opened.error == Channel::OpenError::kNone);
    CHECK(std::vector<std::uint8_t>(opened.message.begin(), opened.message.end()) == hello);

    sealed.clear();
    REQUIRE(client.seal(hello, sealed));
    opened = server.open(sealed[0]);
    REQUIRE(opened.error == Channel::OpenError::kNone);
    CHECK(opened.message.size() == hello.size());
}

struct Established {
    Channel server;
    Channel client;
};

Established channels(hs::Initiator& server, hs::Responder& client, Dialect dialect) {
    std::optional<ac3::sendspin::noise::Handshake::Transport> server_keys = server.take_keys();
    std::optional<ac3::sendspin::noise::Handshake::Transport> client_keys = client.take_keys();
    REQUIRE(server_keys.has_value());
    REQUIRE(client_keys.has_value());
    CHECK(server_keys->handshake_hash == client_keys->handshake_hash);
    return Established{
        .server = Channel(std::move(*server_keys), dialect, 1 << 20),
        .client = Channel(std::move(*client_keys), dialect, 1 << 20),
    };
}

}  // namespace

TEST_CASE("handshake session: an unpaired connection under the Sentinel, in both suites",
          "[sendspin][handshake_session]") {
    for (const Suite suite : {Suite::kChaChaPolySha256, Suite::kAesGcmSha256}) {
        const KeyPair server_identity = generated();
        const KeyPair client_identity = generated();
        const ServerKeys server_keys;
        const ClientKeys client_keys;
        hs::Initiator server(server_identity, server_keys);
        hs::Responder client(client_identity, suite, client_keys);

        REQUIRE(run(server, client));
        CHECK(server.client_key() == client_identity.public_key());
        CHECK(server.suite() == suite);
        CHECK(client.server_key() == server_identity.public_key());
        CHECK(server.category() == hs::PskCategory::kSentinel);
        CHECK(client.category() == hs::PskCategory::kSentinel);
        CHECK_FALSE(server.credential_mismatch());
        CHECK_FALSE(client.fell_back());
        CHECK(client.dialect() == Dialect::kSpecification);

        Established channel = channels(server, client, Dialect::kSpecification);
        check_channels(channel.server, channel.client);
    }
}

TEST_CASE("handshake session: long-term and pairing PSKs", "[sendspin][handshake_session]") {
    const KeyPair server_identity = generated();
    const KeyPair client_identity = generated();
    const Key32 psk = random_key();
    const hs::PskCategory category = GENERATE(hs::PskCategory::kLongTerm, hs::PskCategory::kPairing);

    ServerKeys server_keys;
    server_keys.choice = {.psk = psk, .category = category};
    ClientKeys client_keys;
    client_keys.held.push_back({.psk = random_key(), .category = hs::PskCategory::kLongTerm,
                                .server_key = random_key()});
    client_keys.held.push_back({.psk = psk, .category = category, .server_key = server_identity.public_key()});

    hs::Initiator server(server_identity, server_keys);
    hs::Responder client(client_identity, Suite::kChaChaPolySha256, client_keys);
    REQUIRE(run(server, client));
    CHECK(server.category() == category);
    CHECK(client.category() == category);
    CHECK_FALSE(server.credential_mismatch());
    CHECK_FALSE(client.fell_back());
    Established channel = channels(server, client, Dialect::kSpecification);
    check_channels(channel.server, channel.client);
}

TEST_CASE("handshake session: a client that lost its record falls back, and the server sees the mismatch",
          "[sendspin][handshake_session]") {
    const KeyPair server_identity = generated();
    const KeyPair client_identity = generated();
    ServerKeys server_keys;
    server_keys.choice = {.psk = random_key(), .category = hs::PskCategory::kLongTerm};

    SECTION("no record at all") {
        const ClientKeys client_keys;
        hs::Initiator server(server_identity, server_keys);
        hs::Responder client(client_identity, Suite::kAesGcmSha256, client_keys);
        REQUIRE(run(server, client));
        CHECK(client.fell_back());
        CHECK(client.category() == hs::PskCategory::kSentinel);
        CHECK(server.credential_mismatch());
        CHECK(server.category() == hs::PskCategory::kSentinel);
        Established channel = channels(server, client, Dialect::kSpecification);
        check_channels(channel.server, channel.client);
    }
    SECTION("the PSK held under another category is a miss") {
        ClientKeys client_keys;
        client_keys.held.push_back({.psk = server_keys.choice.psk, .category = hs::PskCategory::kPairing,
                                    .server_key = {}});
        hs::Initiator server(server_identity, server_keys);
        hs::Responder client(client_identity, Suite::kChaChaPolySha256, client_keys);
        REQUIRE(run(server, client));
        CHECK(client.fell_back());
        CHECK(server.credential_mismatch());
    }
}

TEST_CASE("handshake session: a long-term PSK bound to another server fails without falling back",
          "[sendspin][handshake_session]") {
    const KeyPair server_identity = generated();
    const KeyPair client_identity = generated();
    const Key32 psk = random_key();
    ServerKeys server_keys;
    server_keys.choice = {.psk = psk, .category = hs::PskCategory::kLongTerm};
    ClientKeys client_keys;
    client_keys.held.push_back({.psk = psk, .category = hs::PskCategory::kLongTerm, .server_key = random_key()});

    hs::Initiator server(server_identity, server_keys);
    hs::Responder client(client_identity, Suite::kChaChaPolySha256, client_keys);
    CHECK_FALSE(run(server, client));
    CHECK(client.failure() == hs::Failure::kServerMismatch);
    CHECK_FALSE(client.take_keys().has_value());
}

TEST_CASE("handshake session: server/error and the prologue", "[sendspin][handshake_session]") {
    const KeyPair server_identity = generated();
    const KeyPair client_identity = generated();
    const ServerKeys server_keys;
    const ClientKeys client_keys;

    SECTION("a client/init with another version gets server/error") {
        hs::Initiator server(server_identity, server_keys);
        hs::Responder client(client_identity, Suite::kChaChaPolySha256, client_keys);
        std::string init = client.client_init();
        const std::size_t at = init.find("\"version\":1");
        REQUIRE(at != std::string::npos);
        init.replace(at, 11, "\"version\":2");
        const hs::Step step = server.receive(init);
        CHECK(step.outcome == hs::Outcome::kFailed);
        CHECK(server.init_error() == hs::InitError::kUnsupportedVersion);
        REQUIRE(step.replies.size() == 1);
        const hs::Step answer = client.receive(step.replies[0]);
        CHECK(answer.outcome == hs::Outcome::kFailed);
        CHECK(client.failure() == hs::Failure::kServerError);
        CHECK(client.server_error() == hs::InitError::kUnsupportedVersion);
    }
    SECTION("a server/init changed in transit reads the same but breaks the handshake") {
        hs::Initiator server(server_identity, server_keys);
        hs::Responder client(client_identity, Suite::kChaChaPolySha256, client_keys);
        const hs::Step step = server.receive(client.client_init());
        REQUIRE(step.replies.size() == 2);
        // The same members in another order: the client reads the same server/init, but
        // hashes different bytes into its prologue than the server did.
        const std::string reordered = R"({"payload":{"server_id":")" +
                                      ac3::sendspin::base64url::encode(server_identity.public_key()) +
                                      R"(","version":1},"type":"server/init"})";
        REQUIRE(reordered != step.replies[0]);
        CHECK(client.receive(reordered).outcome == hs::Outcome::kContinue);
        CHECK(client.receive(step.replies[1]).outcome == hs::Outcome::kFailed);
        CHECK(client.failure() == hs::Failure::kBadHandshake);
    }
    SECTION("messages out of order") {
        hs::Initiator server(server_identity, server_keys);
        hs::Responder client(client_identity, Suite::kChaChaPolySha256, client_keys);
        const hs::Step step = server.receive(client.client_init());
        REQUIRE(step.replies.size() == 2);
        // Message 1 before server/init is not a server/init.
        CHECK(client.receive(step.replies[1]).outcome == hs::Outcome::kFailed);
        CHECK(client.failure() == hs::Failure::kBadInit);
        // A second client/init where message 2 belongs.
        CHECK(server.receive(client.client_init()).outcome == hs::Outcome::kFailed);
        CHECK(server.failure() == hs::Failure::kBadHandshake);
    }
}

TEST_CASE("handshake session: aiosendspin 9.1.1's message 1 names no category",
          "[sendspin][handshake_session]") {
    const KeyPair server_identity = generated();
    const KeyPair client_identity = generated();
    const Key32 psk = random_key();
    ClientKeys client_keys;
    client_keys.held.push_back({.psk = psk, .category = hs::PskCategory::kLongTerm,
                                .server_key = server_identity.public_key()});
    hs::Responder client(client_identity, Suite::kChaChaPolySha256, client_keys);

    // A server that writes message 1 the way aiosendspin 9.1.1's noise/driver.py does:
    // {"psk_id": ...} and nothing else.
    const std::expected<hs::ClientInit, hs::InitError> init = hs::parse_client_init(client.client_init());
    REQUIRE(init.has_value());
    const std::string server_init = hs::write_server_init({.server_key = server_identity.public_key()});
    std::vector<std::uint8_t> prologue(client.client_init().begin(), client.client_init().end());
    prologue.insert(prologue.end(), server_init.begin(), server_init.end());
    ac3::sendspin::noise::Handshake server(Suite::kChaChaPolySha256, ac3::sendspin::noise::Role::kInitiator,
                                           server_identity, init->client_key, prologue);
    const std::optional<Digest32> id = hs::psk_id(psk);
    REQUIRE(id.has_value());
    const std::string payload = R"({"psk_id":")" + ac3::sendspin::base64url::encode(*id) + R"("})";
    std::vector<std::uint8_t> message_1;
    REQUIRE(server.write_message_1(bytes_of(payload), message_1));

    CHECK(client.receive(server_init).outcome == hs::Outcome::kContinue);
    const hs::Step reply = client.receive(hs::write_noise_handshake(message_1));
    REQUIRE(reply.outcome == hs::Outcome::kEstablished);
    CHECK(client.dialect() == Dialect::kAiosendspin911);
    CHECK(client.category() == hs::PskCategory::kLongTerm);
    CHECK_FALSE(client.fell_back());

    REQUIRE(reply.replies.size() == 1);
    const std::optional<std::vector<std::uint8_t>> message_2 = hs::parse_noise_handshake(reply.replies[0]);
    REQUIRE(message_2.has_value());
    std::vector<std::uint8_t> received;
    CHECK(server.read_message_2(psk, *message_2, received));
    CHECK(hs::parse_message_2_payload(received));
}

TEST_CASE("handshake session: a re-handshake inside the channel swaps the keys",
          "[sendspin][handshake_session]") {
    const KeyPair server_identity = generated();
    const KeyPair client_identity = generated();
    const ServerKeys server_keys;
    ClientKeys client_keys;
    hs::Initiator first_server(server_identity, server_keys);
    hs::Responder first_client(client_identity, Suite::kAesGcmSha256, client_keys);
    REQUIRE(run(first_server, first_client));
    Established channel = channels(first_server, first_client, Dialect::kSpecification);
    const Digest32 first_hash = channel.server.handshake_hash();

    // Pairing produced a long-term PSK; the server re-handshakes to it.
    const Key32 psk = random_key();
    client_keys.held.push_back({.psk = psk, .category = hs::PskCategory::kLongTerm,
                                .server_key = server_identity.public_key()});

    // A noise/handshake text travels as a JSON message: ID 0, then the text.
    const auto carry = [](Channel& from, Channel& to, const std::string& text) {
        std::vector<std::uint8_t> message{ac3::sendspin::message_id::kJson};
        message.insert(message.end(), text.begin(), text.end());
        std::vector<std::vector<std::uint8_t>> sealed;
        REQUIRE(from.seal(message, sealed));
        REQUIRE(sealed.size() == 1);
        const Channel::Opened opened = to.open(sealed[0]);
        REQUIRE(opened.error == Channel::OpenError::kNone);
        REQUIRE(!opened.message.empty());
        return std::string(opened.message.begin() + 1, opened.message.end());
    };

    SECTION("to a PSK the client holds") {
        hs::Initiator server(server_identity, client_identity.public_key(), Suite::kAesGcmSha256, first_hash,
                             {.psk = psk, .category = hs::PskCategory::kLongTerm});
        hs::Responder client(client_identity, Suite::kAesGcmSha256, server_identity.public_key(), first_hash,
                             client_keys);
        CHECK(client.client_init().empty());
        const hs::Step start = server.start();
        REQUIRE(start.replies.size() == 1);
        const hs::Step answer = client.receive(carry(channel.server, channel.client, start.replies[0]));
        REQUIRE(answer.outcome == hs::Outcome::kEstablished);
        CHECK(client.category() == hs::PskCategory::kLongTerm);
        REQUIRE(answer.replies.size() == 1);

        // Message 2 goes under the old keys; each side then switches.
        const std::string message_2 = carry(channel.client, channel.server, answer.replies[0]);
        std::optional<ac3::sendspin::noise::Handshake::Transport> client_keys_new = client.take_keys();
        REQUIRE(client_keys_new.has_value());
        channel.client.rekey(std::move(*client_keys_new));
        REQUIRE(server.receive(message_2).outcome == hs::Outcome::kEstablished);
        CHECK(server.category() == hs::PskCategory::kLongTerm);
        std::optional<ac3::sendspin::noise::Handshake::Transport> server_keys_new = server.take_keys();
        REQUIRE(server_keys_new.has_value());
        channel.server.rekey(std::move(*server_keys_new));

        CHECK(channel.server.handshake_hash() == channel.client.handshake_hash());
        CHECK(channel.server.handshake_hash() != first_hash);
        check_channels(channel.server, channel.client);
    }
    SECTION("a miss fails instead of falling back") {
        const ClientKeys empty;
        hs::Initiator server(server_identity, client_identity.public_key(), Suite::kAesGcmSha256, first_hash,
                             {.psk = random_key(), .category = hs::PskCategory::kLongTerm});
        hs::Responder client(client_identity, Suite::kAesGcmSha256, server_identity.public_key(), first_hash,
                             empty);
        const hs::Step start = server.start();
        REQUIRE(start.replies.size() == 1);
        CHECK(client.receive(carry(channel.server, channel.client, start.replies[0])).outcome ==
              hs::Outcome::kFailed);
        CHECK(client.failure() == hs::Failure::kPskMiss);
    }
}

TEST_CASE("channel: fragments in both dialects, and what it refuses", "[sendspin][handshake_session][channel]") {
    const Dialect dialect = GENERATE(Dialect::kSpecification, Dialect::kAiosendspin911);
    const KeyPair server_identity = generated();
    const KeyPair client_identity = generated();
    const ServerKeys server_keys;
    const ClientKeys client_keys;
    hs::Initiator server(server_identity, server_keys);
    hs::Responder client(client_identity, Suite::kChaChaPolySha256, client_keys);
    REQUIRE(run(server, client));
    Established channel = channels(server, client, dialect);

    // Three frames' worth of artwork-sized message.
    std::vector<std::uint8_t> large(150000);
    large[0] = ac3::sendspin::message_id::kArtworkFirst;
    for (std::size_t i = 1; i < large.size(); ++i) {
        large[i] = static_cast<std::uint8_t>(i * 7);
    }
    std::vector<std::vector<std::uint8_t>> sealed;
    REQUIRE(channel.server.seal(large, sealed));
    CHECK(sealed.size() == 3);
    for (std::size_t i = 0; i < sealed.size(); ++i) {
        CHECK(sealed[i].size() <= ac3::sendspin::noise::kMaxMessageBytes);
        const Channel::Opened opened = channel.client.open(sealed[i]);
        REQUIRE(opened.error == Channel::OpenError::kNone);
        if (i + 1 < sealed.size()) {
            CHECK(opened.message.empty());
        } else {
            CHECK(std::vector<std::uint8_t>(opened.message.begin(), opened.message.end()) == large);
        }
    }

    // A replayed ciphertext, and a damaged one, fail to decrypt.
    std::vector<std::vector<std::uint8_t>> small;
    REQUIRE(channel.server.seal(std::vector<std::uint8_t>{0, '{', '}'}, small));
    REQUIRE(channel.client.open(small[0]).error == Channel::OpenError::kNone);
    CHECK(channel.client.open(small[0]).error == Channel::OpenError::kDecrypt);

    small.clear();
    REQUIRE(channel.server.seal(std::vector<std::uint8_t>{0, '{', '}'}, small));
    small[0][1] ^= 0x01U;
    CHECK(channel.client.open(small[0]).error == Channel::OpenError::kDecrypt);
}

