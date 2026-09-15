#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/pairing.hpp"
#include "sendspin/sendspin_test_support.hpp"

// Sendspin's pairing values. The two token vectors are pairing.md's own; the
// derived values were computed with Python from pairing.md's formulas and from
// aiosendspin 9.1.1's noise/pin.py, with the cryptography package for the AEADs.

namespace {

using ac3::sendspin::crypto::Digest32;
using ac3::sendspin::crypto::Digest64;
using ac3::sendspin::crypto::Key32;
using ac3::sendspin::Dialect;
using ac3::sendspin::pairing::TokenVersion;
using ac3::sendspin::test::from_hex;
using ac3::sendspin::test::to_hex;

std::array<std::uint8_t, 32> counting(std::uint8_t first) {
    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(first + i);
    }
    return out;
}

constexpr std::string_view kToken0 =
    "SP:0AAAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFYYDENBWHA5DYP6BYPC4PSOLZXH5DU6V97M5XXO74HR6LZ7J5PW674PT6X37T6757Y";
constexpr std::string_view kToken1 = "SP:14DQ6FY7E4XTOP9HJ5LV6Z3PO57YPD4XT6T97N5Y";

}  // namespace

TEST_CASE("pairing: pairing.md's token vectors", "[sendspin][pairing]") {
    std::vector<std::uint8_t> payload0;
    const Key32 client_key = counting(0x00);
    const Key32 pairing_psk = counting(0xE0);
    payload0.insert(payload0.end(), client_key.begin(), client_key.end());
    payload0.insert(payload0.end(), pairing_psk.begin(), pairing_psk.end());
    CHECK(ac3::sendspin::pairing::encode_token(TokenVersion::kPairingPsk, payload0) == kToken0);

    const std::optional<ac3::sendspin::pairing::PairingPskToken> decoded =
        ac3::sendspin::pairing::decode_pairing_psk_token(kToken0);
    REQUIRE(decoded.has_value());
    CHECK(decoded->client_key == client_key);
    CHECK(decoded->pairing_psk == pairing_psk);

    std::vector<std::uint8_t> payload1(24);
    for (std::size_t i = 0; i < payload1.size(); ++i) {
        payload1[i] = static_cast<std::uint8_t>(0xE0 + i);
    }
    CHECK(ac3::sendspin::pairing::encode_token(TokenVersion::kDynamicCode, payload1) == kToken1);
    const auto token1 = ac3::sendspin::pairing::decode_token(kToken1);
    REQUIRE(token1.has_value());
    CHECK(token1->version == TokenVersion::kDynamicCode);
    CHECK(token1->payload == payload1);
}

TEST_CASE("pairing: token decoding is lenient with operator input", "[sendspin][pairing]") {
    using ac3::sendspin::pairing::decode_token;
    std::string lower(kToken1);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    const auto from_lower = decode_token("  " + lower + "\n");
    REQUIRE(from_lower.has_value());
    CHECK(from_lower->payload.size() == 24);
    // No SP: prefix, and 9 read back as 2.
    const auto bare = decode_token(kToken1.substr(3));
    REQUIRE(bare.has_value());
    CHECK(bare->payload == from_lower->payload);
    // Payload bytes past the version's are kept for the caller to ignore.
    std::vector<std::uint8_t> longer(30, 0x5A);
    const auto extended = decode_token(ac3::sendspin::pairing::encode_token(TokenVersion::kDynamicCode, longer));
    REQUIRE(extended.has_value());
    CHECK(extended->payload.size() == 30);
}

TEST_CASE("pairing: malformed tokens are refused", "[sendspin][pairing]") {
    using ac3::sendspin::pairing::decode_token;
    CHECK_FALSE(decode_token("").has_value());
    CHECK_FALSE(decode_token("SP:").has_value());
    CHECK_FALSE(decode_token("SP:2" + std::string(kToken1.substr(4))).has_value());  // version
    CHECK_FALSE(decode_token("SP:14DQ6FY7E4XTOP9HJ5LV6Z3PO57YPD4XT6T97N51").has_value());  // 1
    CHECK_FALSE(decode_token("SP:14DQ6FY7E4XTOP9HJ5LV6Z3PO57YPD4XT6T97N58").has_value());  // 8
    CHECK_FALSE(decode_token("SP:14DQ6FY7E4XTOP9HJ5LV6Z3PO57YPD4XT6T97N5").has_value());  // length
    CHECK_FALSE(decode_token("SP:14DQ6FY7E4XTOP").has_value());                             // short
    CHECK_FALSE(ac3::sendspin::pairing::decode_pairing_psk_token(kToken1).has_value());
}

TEST_CASE("pairing: the dynamic code in both dialects", "[sendspin][pairing]") {
    using ac3::sendspin::pairing::derive_digits;
    const Digest32 h = counting(0);
    const Key32 nonce_a = counting(32);
    const Key32 nonce_b = counting(64);
    CHECK(derive_digits(Dialect::kSpecification, h, nonce_a, nonce_b, 6) == "806729");
    CHECK_FALSE(derive_digits(Dialect::kSpecification, h, nonce_a, nonce_b, 8).has_value());

    CHECK(derive_digits(Dialect::kAiosendspin911, h, nonce_a, nonce_b, 4) == "2858");
    CHECK(derive_digits(Dialect::kAiosendspin911, h, nonce_a, nonce_b, 6) == "802858");
    CHECK(derive_digits(Dialect::kAiosendspin911, h, nonce_a, nonce_b, 8) == "60802858");
    CHECK(derive_digits(Dialect::kAiosendspin911, h, nonce_a, nonce_b, 12) == "652760802858");
    CHECK_FALSE(derive_digits(Dialect::kAiosendspin911, h, nonce_a, nonce_b, 3).has_value());
    CHECK_FALSE(derive_digits(Dialect::kAiosendspin911, h, nonce_a, nonce_b, 13).has_value());

    const auto qr = ac3::sendspin::pairing::derive_qr_code(h, nonce_a, nonce_b);
    REQUIRE(qr.has_value());
    CHECK(to_hex(*qr) == "9f26c48e69443b7fab8d1be2052017a0f69b4a1f0e49033d");

    const auto commitment = ac3::sendspin::pairing::commit(nonce_b);
    REQUIRE(commitment.has_value());
    CHECK(to_hex(*commitment) == "f6d431646ea17a4488f6008d0da4dbd79b7c0a898c3631453fb6ffc912d29c18");
}

TEST_CASE("pairing: the CPace session id with and without the round", "[sendspin][pairing]") {
    const Digest32 h = counting(0);
    const std::vector<std::uint8_t> spec = ac3::sendspin::pairing::pake_sid(Dialect::kSpecification, h, 2, 3);
    CHECK(spec.size() == 61);
    CHECK(to_hex(spec) ==
          "73656e647370696e2d706169722d70616b652d7631000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f0000000200000003");
    const std::vector<std::uint8_t> legacy = ac3::sendspin::pairing::pake_sid(Dialect::kAiosendspin911, h, 2, 3);
    CHECK(legacy.size() == 57);
    CHECK(to_hex(legacy) == to_hex(std::span<const std::uint8_t>(spec).first(57)));
}

TEST_CASE("pairing: wrapping the long-term PSK under K_wrap", "[sendspin][pairing]") {
    using ac3::sendspin::pairing::Wrapped;
    const Digest32 h = counting(0);
    const std::vector<std::uint8_t> sid = ac3::sendspin::pairing::pake_sid(Dialect::kSpecification, h, 2, 3);
    Digest64 isk{};
    for (std::size_t i = 0; i < isk.size(); ++i) {
        isk[i] = static_cast<std::uint8_t>(i);
    }
    const auto psk_key = ac3::sendspin::pairing::wrap_key(Wrapped::kLongTermPsk, sid, isk);
    const auto nonce_key = ac3::sendspin::pairing::wrap_key(Wrapped::kNonceB, sid, isk);
    REQUIRE(psk_key.has_value());
    REQUIRE(nonce_key.has_value());
    CHECK(to_hex(*psk_key) == "21e436f00cefc78881d5d7a8dfb014f2da98f5d066dd31068995a2f52e4d39e0");
    CHECK(to_hex(*nonce_key) == "bd7d8338c5875afc9fa6795f873619eb7788eb23ce648499057a7dcfa024fb38");

    const Key32 psk = counting(0xA0);
    const struct {
        ac3::sendspin::noise::Suite suite;
        std::string_view wrapped;
    } cases[] = {
        {ac3::sendspin::noise::Suite::kChaChaPolySha256,
         "c3eed9c184cb747b2950c98c1fe9827a3d739d2cc32682fa411762aa19c40b6497528027ec915193ad9e21921a0cea9e"},
        {ac3::sendspin::noise::Suite::kAesGcmSha256,
         "19ad3fb531731fe54c31fcb49d2470942ed169b4085c5056a00f31e93881477316a077e1d2145369e8dcd34484981776"},
    };
    for (const auto& c : cases) {
        INFO(c.wrapped);
        const auto wrapped = ac3::sendspin::pairing::wrap(c.suite, *psk_key, psk);
        REQUIRE(wrapped.has_value());
        CHECK(to_hex(*wrapped) == c.wrapped);
        const auto unwrapped = ac3::sendspin::pairing::unwrap(c.suite, *psk_key, *wrapped);
        REQUIRE(unwrapped.has_value());
        CHECK(*unwrapped == psk);

        auto tampered = *wrapped;
        tampered[0] ^= 1U;
        CHECK_FALSE(ac3::sendspin::pairing::unwrap(c.suite, *psk_key, tampered).has_value());
        CHECK_FALSE(ac3::sendspin::pairing::unwrap(c.suite, *nonce_key, *wrapped).has_value());
        CHECK_FALSE(ac3::sendspin::pairing::unwrap(c.suite, *psk_key, std::span<const std::uint8_t>(*wrapped).first(47)).has_value());
    }
}
