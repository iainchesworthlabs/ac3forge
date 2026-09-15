#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/sendspin/base64url.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "ac3/sendspin/handshake.hpp"
#include "ac3/sendspin/noise.hpp"
#include "sendspin/sendspin_test_support.hpp"

// Noise KKpsk2 for both Sendspin suites. The first two cases are the cacophony
// test vectors for Noise_KKpsk2_25519_ChaChaPoly_SHA256 and
// Noise_KKpsk2_25519_AESGCM_SHA256 (github.com/haskell-cryptography/cacophony,
// vectors/cacophony.txt), every handshake and transport message byte for byte.
// The rest are the Sendspin uses of the pattern: late PSK binding, the Sentinel
// fallback, the prologue binding the init texts, and a re-handshake.

namespace {

using ac3::sendspin::crypto::Digest32;
using ac3::sendspin::crypto::Key32;
using ac3::sendspin::noise::Handshake;
using ac3::sendspin::noise::KeyPair;
using ac3::sendspin::noise::Role;
using ac3::sendspin::noise::Suite;
using ac3::sendspin::test::bytes_of;
using ac3::sendspin::test::from_hex;
using ac3::sendspin::test::key_from_hex;
using ac3::sendspin::test::to_hex;

struct Vector {
    Suite suite;
    std::string_view handshake_hash;
    std::array<std::string_view, 6> ciphertexts;
};

constexpr std::string_view kPrologue = "4a6f686e2047616c74";
constexpr std::string_view kInitStatic = "e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1";
constexpr std::string_view kInitEphemeral = "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a";
constexpr std::string_view kRespStatic = "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893";
constexpr std::string_view kRespEphemeral = "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b";
constexpr std::string_view kInitStaticPublic = "6bc3822a2aa7f4e6981d6538692b3cdf3e6df9eea6ed269eb41d93c22757b75a";
constexpr std::string_view kRespStaticPublic = "31e0303fd6418d2f8c0e78b91f22e8caed0fbe48656dcf4767e4834f701b8f62";
constexpr std::string_view kPsk = "54686973206973206d7920417573747269616e20706572737065637469766521";
constexpr std::array<std::string_view, 6> kPayloads{
    "4c756477696720766f6e204d69736573", "4d757272617920526f746862617264", "462e20412e20486179656b",
    "4361726c204d656e676572",           "4a65616e2d426170746973746520536179",
    "457567656e2042f6686d20766f6e2042617765726b",
};

const std::array<Vector, 2> kVectors{{
    {Suite::kChaChaPolySha256,
     "7f3c5fdcdd3767e2835473a2683971490339f5bbeee82c3690bc606e14db70ed",
     {"ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c7944babf6443250c604872e33233c3b9a29df5c6d334ae2d53f1bd7f0b265a716b37",
      "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f14480884366a1f5f0d79fe93ae476bd1897a7a8ae92764898aa5d49e07b5849f35865ba",
      "2eb2686b8814a7c0178fe18bfeeafe3e07312d69486d45e6572546",
      "eea5791a890cd573a5c2e2345a8f98b0d1f0727acd24584fcddde5",
      "ab2e1a411abaaa3df9cb497dffe4cfb70af6c71f0815b3c33b35e22329dee72f3e",
      "ed22a0392c6afbfd6a6adea92b1faf13c4df24072f7060a20b1500609621c6957ac86d82f9"}},
    {Suite::kAesGcmSha256,
     "1d2be5b73c43962fa87db816e6b5a73a0d67053e29b3d89fcee6ae299b5a5c1f",
     {"ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c794483cf298f1bfed916ea20d8f528bd30670670c7245e236a68f021b5a521d8bf31",
      "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f14480884332b32e0f483ae58a9c36e93390a9c2238918bda6ae0ebfe48a400b8f565f5f",
      "f7cf1f47885fa5ff7cefecf3db3066f89b996834069b8f1517c55a",
      "ce7de28a6b54b559d3db2f7482f1be3264a62165f12a73e377ebde",
      "da4173c5aa84756d0eab64adfbe81f10538b099966897771a071502ee2899e2fbd",
      "1a1e3408acea689f5cced09b246e13e68e1ce146e8f1b3478482593b988587be99ba7595d2"}},
}};

KeyPair pair_from_hex(std::string_view hex) {
    std::optional<KeyPair> pair = KeyPair::from_private(key_from_hex(hex));
    REQUIRE(pair.has_value());
    return *pair;
}

struct Session {
    Handshake::Transport server;
    Handshake::Transport client;
};

// A whole handshake between fresh keys, with the client choosing its PSK from
// message 1's payload the way a Sendspin player does.
std::optional<Session> handshake(Suite suite, const Key32& server_psk, const Key32& client_psk,
                                 std::span<const std::uint8_t> server_prologue,
                                 std::span<const std::uint8_t> client_prologue) {
    const std::optional<KeyPair> server_key = KeyPair::generate();
    const std::optional<KeyPair> client_key = KeyPair::generate();
    REQUIRE(server_key.has_value());
    REQUIRE(client_key.has_value());
    Handshake server(suite, Role::kInitiator, *server_key, client_key->public_key(), server_prologue);
    Handshake client(suite, Role::kResponder, *client_key, server_key->public_key(), client_prologue);

    std::vector<std::uint8_t> message_1;
    const std::optional<Digest32> id = ac3::sendspin::handshake::psk_id(server_psk);
    REQUIRE(id.has_value());
    const std::string payload_1 = ac3::sendspin::handshake::write_message_1_payload(
        *id, ac3::sendspin::handshake::PskCategory::kLongTerm);
    REQUIRE(server.write_message_1(bytes_of(payload_1), message_1));

    std::vector<std::uint8_t> received_1;
    if (!client.read_message_1(message_1, received_1)) {
        return std::nullopt;
    }
    CHECK(std::string(received_1.begin(), received_1.end()) == payload_1);

    std::vector<std::uint8_t> message_2;
    REQUIRE(client.write_message_2(client_psk,
                                   bytes_of(ac3::sendspin::handshake::kMessage2Payload),
                                   message_2));
    std::vector<std::uint8_t> received_2;
    if (!server.read_message_2(server_psk, message_2, received_2)) {
        return std::nullopt;
    }
    std::optional<Handshake::Transport> server_transport = server.split();
    std::optional<Handshake::Transport> client_transport = client.split();
    REQUIRE(server_transport.has_value());
    REQUIRE(client_transport.has_value());
    return Session{std::move(*server_transport), std::move(*client_transport)};
}

}  // namespace

TEST_CASE("noise: KKpsk2 cacophony vectors, both suites", "[sendspin][noise]") {
    for (const Vector& v : kVectors) {
        INFO(ac3::sendspin::noise::protocol_name(v.suite));
        const KeyPair init_static = pair_from_hex(kInitStatic);
        const KeyPair resp_static = pair_from_hex(kRespStatic);
        CHECK(to_hex(init_static.public_key()) == kInitStaticPublic);
        CHECK(to_hex(resp_static.public_key()) == kRespStaticPublic);
        const std::vector<std::uint8_t> prologue = from_hex(kPrologue);
        const Key32 psk = key_from_hex(kPsk);

        Handshake init(v.suite, Role::kInitiator, init_static, resp_static.public_key(), prologue);
        Handshake resp(v.suite, Role::kResponder, resp_static, init_static.public_key(), prologue);
        init.set_ephemeral(pair_from_hex(kInitEphemeral));
        resp.set_ephemeral(pair_from_hex(kRespEphemeral));

        std::vector<std::uint8_t> message;
        std::vector<std::uint8_t> payload;
        REQUIRE(init.write_message_1(from_hex(kPayloads[0]), message));
        CHECK(to_hex(message) == v.ciphertexts[0]);
        REQUIRE(resp.read_message_1(message, payload));
        CHECK(to_hex(payload) == kPayloads[0]);

        message.clear();
        payload.clear();
        REQUIRE(resp.write_message_2(psk, from_hex(kPayloads[1]), message));
        CHECK(to_hex(message) == v.ciphertexts[1]);
        REQUIRE(init.read_message_2(psk, message, payload));
        CHECK(to_hex(payload) == kPayloads[1]);

        std::optional<Handshake::Transport> i = init.split();
        std::optional<Handshake::Transport> r = resp.split();
        REQUIRE(i.has_value());
        REQUIRE(r.has_value());
        CHECK(to_hex(i->handshake_hash) == v.handshake_hash);
        CHECK(i->handshake_hash == r->handshake_hash);

        // Transport messages alternate, initiator first.
        for (std::size_t m = 2; m < 6; ++m) {
            INFO(m);
            auto& sender = m % 2 == 0 ? i->send : r->send;
            auto& receiver = m % 2 == 0 ? r->receive : i->receive;
            message.clear();
            payload.clear();
            REQUIRE(sender.encrypt(from_hex(kPayloads[m]), message));
            CHECK(to_hex(message) == v.ciphertexts[m]);
            REQUIRE(receiver.decrypt(message, payload));
            CHECK(to_hex(payload) == kPayloads[m]);
        }
        CHECK(i->send.nonce() == 2);
        CHECK(r->receive.nonce() == 2);
    }
}

TEST_CASE("noise: a Sendspin handshake with late PSK binding", "[sendspin][noise]") {
    const Key32 psk = key_from_hex("0101010101010101010101010101010101010101010101010101010101010101");
    const std::string prologue = R"({"type":"client/init"}{"type":"server/init"})";
    for (const Suite suite : {Suite::kChaChaPolySha256, Suite::kAesGcmSha256}) {
        std::optional<Session> session = handshake(suite, psk, psk, bytes_of(prologue), bytes_of(prologue));
        REQUIRE(session.has_value());
        std::vector<std::uint8_t> sealed;
        std::vector<std::uint8_t> opened;
        REQUIRE(session->server.send.encrypt(bytes_of("server/hello"), sealed));
        REQUIRE(session->client.receive.decrypt(sealed, opened));
        CHECK(std::string(opened.begin(), opened.end()) == "server/hello");

        // A replayed or reordered frame fails: the receiver's nonce has moved on.
        opened.clear();
        CHECK_FALSE(session->client.receive.decrypt(sealed, opened));
    }
}

TEST_CASE("noise: message 2 under another PSK fails, and the Sentinel retry succeeds",
          "[sendspin][noise]") {
    const Key32 long_term = key_from_hex("0202020202020202020202020202020202020202020202020202020202020202");
    const Key32& sentinel = ac3::sendspin::handshake::sentinel_psk();
    const std::optional<KeyPair> server_key = KeyPair::generate();
    const std::optional<KeyPair> client_key = KeyPair::generate();
    REQUIRE(server_key.has_value());
    REQUIRE(client_key.has_value());
    const std::span<const std::uint8_t> prologue = bytes_of("init texts");
    Handshake server(Suite::kChaChaPolySha256, Role::kInitiator, *server_key, client_key->public_key(), prologue);
    Handshake client(Suite::kChaChaPolySha256, Role::kResponder, *client_key, server_key->public_key(), prologue);

    std::vector<std::uint8_t> message_1;
    std::vector<std::uint8_t> payload;
    REQUIRE(server.write_message_1(bytes_of("{}"), message_1));
    REQUIRE(client.read_message_1(message_1, payload));
    // The client has no record for the long-term PSK the server named, so it
    // completes with the Sentinel (connection.md, Sentinel Fallback).
    std::vector<std::uint8_t> message_2;
    REQUIRE(client.write_message_2(sentinel, bytes_of("{}"), message_2));

    Handshake retry = server;
    payload.clear();
    CHECK_FALSE(server.read_message_2(long_term, message_2, payload));
    CHECK(server.failed());
    CHECK_FALSE(server.split().has_value());
    payload.clear();
    REQUIRE(retry.read_message_2(sentinel, message_2, payload));
    std::optional<Handshake::Transport> server_side = retry.split();
    std::optional<Handshake::Transport> client_side = client.split();
    REQUIRE(server_side.has_value());
    REQUIRE(client_side.has_value());
    CHECK(server_side->handshake_hash == client_side->handshake_hash);
}

TEST_CASE("noise: a different prologue fails message 1", "[sendspin][noise]") {
    const Key32 psk{};
    CHECK_FALSE(handshake(Suite::kAesGcmSha256, psk, psk, bytes_of("client/init as sent"),
                          bytes_of("client/init tampered")).has_value());
}

TEST_CASE("noise: a re-handshake chains from the previous handshake hash",
          "[sendspin][noise]") {
    const std::optional<KeyPair> server_key = KeyPair::generate();
    const std::optional<KeyPair> client_key = KeyPair::generate();
    REQUIRE(server_key.has_value());
    REQUIRE(client_key.has_value());
    const Key32& sentinel = ac3::sendspin::handshake::sentinel_psk();
    const Key32 paired = key_from_hex("0303030303030303030303030303030303030303030303030303030303030303");

    const auto run = [&](const Key32& psk, std::span<const std::uint8_t> prologue) {
        Handshake server(Suite::kChaChaPolySha256, Role::kInitiator, *server_key, client_key->public_key(), prologue);
        Handshake client(Suite::kChaChaPolySha256, Role::kResponder, *client_key, server_key->public_key(), prologue);
        std::vector<std::uint8_t> m1;
        std::vector<std::uint8_t> m2;
        std::vector<std::uint8_t> p;
        REQUIRE(server.write_message_1(bytes_of("{}"), m1));
        REQUIRE(client.read_message_1(m1, p));
        REQUIRE(client.write_message_2(psk, bytes_of("{}"), m2));
        REQUIRE(server.read_message_2(psk, m2, p));
        std::optional<Handshake::Transport> t = server.split();
        REQUIRE(t.has_value());
        return t->handshake_hash;
    };
    const Digest32 first = run(sentinel, bytes_of("client/init server/init"));
    const Digest32 second = run(paired, first);
    CHECK(second != first);
    // The same inputs with a different prior hash give a different session.
    Digest32 other = first;
    other[0] ^= 1U;
    CHECK(run(paired, other) != second);
}

TEST_CASE("noise: messages out of turn and truncated messages fail", "[sendspin][noise]") {
    const std::optional<KeyPair> a = KeyPair::generate();
    const std::optional<KeyPair> b = KeyPair::generate();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> payload;

    Handshake responder(Suite::kChaChaPolySha256, Role::kResponder, *b, a->public_key(), {});
    CHECK_FALSE(responder.write_message_1({}, out));
    Handshake initiator(Suite::kChaChaPolySha256, Role::kInitiator, *a, b->public_key(), {});
    CHECK_FALSE(initiator.read_message_1(out, payload));
    CHECK_FALSE(initiator.split().has_value());

    Handshake fresh_initiator(Suite::kChaChaPolySha256, Role::kInitiator, *a, b->public_key(), {});
    Handshake fresh_responder(Suite::kChaChaPolySha256, Role::kResponder, *b, a->public_key(), {});
    REQUIRE(fresh_initiator.write_message_1({}, out));
    CHECK_FALSE(fresh_initiator.write_message_1({}, out));
    const std::vector<std::uint8_t> truncated(out.begin(), out.begin() + 40);
    CHECK_FALSE(fresh_responder.read_message_1(truncated, payload));
    CHECK(fresh_responder.failed());
}

TEST_CASE("noise: suite names", "[sendspin][noise]") {
    using ac3::sendspin::noise::parse_suite;
    using ac3::sendspin::noise::protocol_name;
    using ac3::sendspin::noise::suite_name;
    CHECK(suite_name(Suite::kChaChaPolySha256) == "25519_ChaChaPoly_SHA256");
    CHECK(suite_name(Suite::kAesGcmSha256) == "25519_AESGCM_SHA256");
    CHECK(parse_suite("25519_ChaChaPoly_SHA256") == Suite::kChaChaPolySha256);
    CHECK(parse_suite("25519_AESGCM_SHA256") == Suite::kAesGcmSha256);
    CHECK_FALSE(parse_suite("25519_aesgcm_sha256").has_value());
    CHECK_FALSE(parse_suite("448_ChaChaPoly_BLAKE2b").has_value());
    // The AESGCM protocol name is exactly HASHLEN bytes, the edge of
    // InitializeSymmetric's two branches.
    CHECK(protocol_name(Suite::kAesGcmSha256).size() == 32);
    CHECK(protocol_name(Suite::kChaChaPolySha256).size() == 36);
}

TEST_CASE("noise: the Sentinel PSK and psk_id match connection.md", "[sendspin][noise]") {
    Digest32 derived{};
    REQUIRE(ac3::sendspin::crypto::sha256({bytes_of("sendspin-sentinel-psk-v1")}, derived));
    CHECK(derived == ac3::sendspin::handshake::sentinel_psk());
    CHECK(to_hex(derived) == "1b5e24dbc1aed95fc2a5a338a90c05df44bd10f5ec1f4cd66cbf86272767b9d3");

    const std::optional<Digest32> id = ac3::sendspin::handshake::psk_id(derived);
    REQUIRE(id.has_value());
    CHECK(*id == ac3::sendspin::handshake::sentinel_psk_id());
    CHECK(to_hex(*id) == "185b15f6d2da4909bd1dc156a4ab206103abef0153bcd52d926170b95cf7ce8a");
    CHECK(ac3::sendspin::base64url::encode(*id) == "GFsV9tLaSQm9HcFWpKsgYQOr7wFTvNUtkmFwuVz3zoo");
}
