#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "sendspin/sendspin_test_support.hpp"

// The crypto seam over mbedTLS's PSA Crypto API, and the HMAC built on its
// hashes. Expected values are the published ones (FIPS 180-4's "abc", RFC 4231,
// RFC 7748), each also recomputed with Python's hashlib, hmac and cryptography
// before being written down here.

namespace {

using ac3::sendspin::crypto::Aead;
using ac3::sendspin::crypto::AeadKey;
using ac3::sendspin::crypto::Bytes;
using ac3::sendspin::crypto::Digest32;
using ac3::sendspin::crypto::Digest64;
using ac3::sendspin::crypto::Key32;
using ac3::sendspin::test::bytes_of;
using ac3::sendspin::test::from_hex;
using ac3::sendspin::test::key_from_hex;
using ac3::sendspin::test::to_hex;

}  // namespace

TEST_CASE("crypto: SHA-256 and SHA-512 of abc, and of parts", "[sendspin][crypto]") {
    Digest32 d256{};
    REQUIRE(ac3::sendspin::crypto::sha256({bytes_of("abc")}, d256));
    CHECK(to_hex(d256) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Digest32 parts{};
    REQUIRE(ac3::sendspin::crypto::sha256({bytes_of("a"), Bytes{}, bytes_of("bc")}, parts));
    CHECK(parts == d256);
    Digest32 empty{};
    REQUIRE(ac3::sendspin::crypto::sha256(std::span<const Bytes>{}, empty));
    CHECK(to_hex(empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    Digest64 d512{};
    REQUIRE(ac3::sendspin::crypto::sha512({bytes_of("ab"), bytes_of("c")}, d512));
    CHECK(to_hex(d512) ==
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
}

TEST_CASE("crypto: HMAC against RFC 4231", "[sendspin][crypto]") {
    struct Case {
        std::vector<std::uint8_t> key;
        std::string_view data;
        std::string_view hmac256;
        std::string_view hmac512;
    };
    const std::array<Case, 3> cases{{
        {std::vector<std::uint8_t>(20, 0x0B), "Hi There",
         "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
         "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854"},
        {std::vector<std::uint8_t>{'J', 'e', 'f', 'e'}, "what do ya want for nothing?",
         "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
         "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea2505549758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737"},
        // A key longer than the block, hashed first.
        {std::vector<std::uint8_t>(131, 0xAA), "Test Using Larger Than Block-Size Key - Hash Key First",
         "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
         "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f3526b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598"},
    }};
    for (const Case& c : cases) {
        INFO(c.data);
        Digest32 d256{};
        REQUIRE(ac3::sendspin::crypto::hmac_sha256(c.key, {bytes_of(c.data)}, d256));
        CHECK(to_hex(d256) == c.hmac256);
        Digest64 d512{};
        REQUIRE(ac3::sendspin::crypto::hmac_sha512(c.key, {bytes_of(c.data)}, d512));
        CHECK(to_hex(d512) == c.hmac512);
        // The same message in parts.
        const std::string_view head = c.data.substr(0, 3);
        const std::string_view tail = c.data.substr(3);
        Digest32 split{};
        REQUIRE(ac3::sendspin::crypto::hmac_sha256(c.key, {bytes_of(head), bytes_of(tail)}, split));
        CHECK(split == d256);
    }
}

TEST_CASE("crypto: HMAC refuses more parts than it holds", "[sendspin][crypto]") {
    const std::array<Bytes, ac3::sendspin::crypto::kMaxHmacParts + 1> parts{};
    Digest32 out{};
    CHECK_FALSE(ac3::sendspin::crypto::hmac_sha256(Bytes{}, std::span<const Bytes>(parts), out));
    CHECK(ac3::sendspin::crypto::hmac_sha256(
        Bytes{}, std::span<const Bytes>(parts).first(ac3::sendspin::crypto::kMaxHmacParts), out));
}

TEST_CASE("crypto: X25519 against RFC 7748", "[sendspin][crypto]") {
    const Key32 alice = key_from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    const Key32 bob = key_from_hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    Key32 alice_public{};
    Key32 bob_public{};
    REQUIRE(ac3::sendspin::crypto::x25519_public_key(alice, alice_public));
    REQUIRE(ac3::sendspin::crypto::x25519_public_key(bob, bob_public));
    CHECK(to_hex(alice_public) == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    CHECK(to_hex(bob_public) == "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");

    Key32 shared_a{};
    Key32 shared_b{};
    REQUIRE(ac3::sendspin::crypto::x25519(alice, bob_public, shared_a));
    REQUIRE(ac3::sendspin::crypto::x25519(bob, alice_public, shared_b));
    CHECK(to_hex(shared_a) == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    CHECK(shared_a == shared_b);

    // §5.2's first vector: a scalar that needs clamping, and an arbitrary u.
    Key32 out{};
    REQUIRE(ac3::sendspin::crypto::x25519(
        key_from_hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
        key_from_hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"), out));
    CHECK(to_hex(out) == "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
}

TEST_CASE("crypto: X25519 refuses low-order peer points", "[sendspin][crypto]") {
    const Key32 alice = key_from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    Key32 out{};
    Key32 zero{};
    CHECK_FALSE(ac3::sendspin::crypto::x25519(alice, zero, out));
    Key32 one{};
    one[0] = 1;
    CHECK_FALSE(ac3::sendspin::crypto::x25519(alice, one, out));
    // A point of order 8.
    CHECK_FALSE(ac3::sendspin::crypto::x25519(
        alice, key_from_hex("e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800"),
        out));
}

TEST_CASE("crypto: random bytes differ call to call", "[sendspin][crypto]") {
    Key32 a{};
    Key32 b{};
    REQUIRE(ac3::sendspin::crypto::random_bytes(a));
    REQUIRE(ac3::sendspin::crypto::random_bytes(b));
    CHECK(a != b);
    CHECK(a != Key32{});
}

TEST_CASE("crypto: both AEADs round-trip, and refuse tampering", "[sendspin][crypto]") {
    for (const Aead aead : {Aead::kChaCha20Poly1305, Aead::kAes256Gcm}) {
        INFO(static_cast<int>(aead));
        const Key32 key = key_from_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        std::optional<AeadKey> handle = AeadKey::create(aead, key);
        REQUIRE(handle.has_value());
        REQUIRE(handle->valid());

        const std::array<std::uint8_t, 12> nonce{0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
        const std::vector<std::uint8_t> ad = from_hex("feedface");
        const std::span<const std::uint8_t> plaintext = bytes_of("stream/start, then bursts");
        std::vector<std::uint8_t> sealed(plaintext.size() + ac3::sendspin::crypto::kAeadTagBytes);
        REQUIRE(handle->encrypt(nonce, ad, plaintext, sealed));
        CHECK_FALSE(std::equal(plaintext.begin(), plaintext.end(), sealed.begin()));

        std::vector<std::uint8_t> opened(plaintext.size());
        REQUIRE(handle->decrypt(nonce, ad, sealed, opened));
        CHECK(std::equal(opened.begin(), opened.end(), plaintext.begin(), plaintext.end()));

        std::vector<std::uint8_t> tampered = sealed;
        tampered[3] ^= 0x01U;
        CHECK_FALSE(handle->decrypt(nonce, ad, tampered, opened));
        tampered = sealed;
        tampered.back() ^= 0x80U;
        CHECK_FALSE(handle->decrypt(nonce, ad, tampered, opened));
        CHECK_FALSE(handle->decrypt(nonce, from_hex("feedfacf"), sealed, opened));
        std::array<std::uint8_t, 12> other_nonce = nonce;
        other_nonce[11] = 9;
        CHECK_FALSE(handle->decrypt(other_nonce, ad, sealed, opened));
        CHECK_FALSE(handle->decrypt(nonce, ad, std::span<const std::uint8_t>(sealed).first(15), opened));

        // Moving the key keeps it usable; the moved-from one is empty.
        AeadKey moved = std::move(*handle);
        CHECK(moved.valid());
        CHECK_FALSE(handle->valid());
        CHECK(moved.decrypt(nonce, ad, sealed, opened));
    }
}

TEST_CASE("crypto: wipe zeroes", "[sendspin][crypto]") {
    Key32 secret = key_from_hex("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    ac3::sendspin::crypto::wipe(secret);
    CHECK(secret == Key32{});
}
