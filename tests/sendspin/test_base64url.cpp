#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/base64url.hpp"

// Sendspin's identities are base64url text on the wire and 32-byte keys
// everywhere else, and a pairing record is looked up by one or the other. The
// strict decoder is what makes the two lookups agree.

namespace {

std::vector<std::uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

}  // namespace

TEST_CASE("base64url: RFC 4648 test vectors, unpadded", "[sendspin][base64url]") {
    using ac3::sendspin::base64url::decode;
    using ac3::sendspin::base64url::encode;
    const std::array<std::array<std::string_view, 2>, 7> vectors{{
        {"", ""},
        {"f", "Zg"},
        {"fo", "Zm8"},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg"},
        {"fooba", "Zm9vYmE"},
        {"foobar", "Zm9vYmFy"},
    }};
    for (const auto& [plain, coded] : vectors) {
        INFO(plain);
        CHECK(encode(bytes_of(plain)) == coded);
        CHECK(decode(coded) == bytes_of(plain));
        CHECK(ac3::sendspin::base64url::encoded_size(plain.size()) == coded.size());
    }
}

TEST_CASE("base64url: the URL alphabet's last two characters", "[sendspin][base64url]") {
    const std::array<std::uint8_t, 2> bytes{0xFB, 0xFF};
    CHECK(ac3::sendspin::base64url::encode(bytes) == "-_8");
    CHECK(ac3::sendspin::base64url::decode("-_8") ==
          std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

TEST_CASE("base64url: every byte value round-trips", "[sendspin][base64url]") {
    std::vector<std::uint8_t> all(256);
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = static_cast<std::uint8_t>(i);
    }
    for (std::size_t length = 0; length <= all.size(); ++length) {
        const std::span<const std::uint8_t> part(all.data(), length);
        const std::string coded = ac3::sendspin::base64url::encode(part);
        const auto back = ac3::sendspin::base64url::decode(coded);
        REQUIRE(back.has_value());
        CHECK(std::vector<std::uint8_t>(part.begin(), part.end()) == *back);
    }
}

TEST_CASE("base64url: non-canonical and foreign texts are refused", "[sendspin][base64url]") {
    using ac3::sendspin::base64url::decode;
    CHECK_FALSE(decode("Z").has_value());         // a length of 1 mod 4
    CHECK_FALSE(decode("Zh").has_value());        // unused bits set
    CHECK_FALSE(decode("Zm9=").has_value());      // unused bits set
    CHECK_FALSE(decode("Zg==").has_value());      // padding
    CHECK_FALSE(decode("Zm9v\n").has_value());    // whitespace
    CHECK_FALSE(decode("+/8").has_value());       // the standard alphabet
    CHECK_FALSE(decode("Zm 9v").has_value());
    CHECK(decode("Zg").has_value());
}

TEST_CASE("base64url: a 32-byte key is 43 characters and decodes exactly",
          "[sendspin][base64url]") {
    std::array<std::uint8_t, 32> key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(0xA5 ^ (i * 37));
    }
    const std::string id = ac3::sendspin::base64url::encode(key);
    CHECK(id.size() == 43);

    std::array<std::uint8_t, 32> back{};
    CHECK(ac3::sendspin::base64url::decode_exact(id, back));
    CHECK(back == key);

    std::array<std::uint8_t, 31> short_key{};
    CHECK_FALSE(ac3::sendspin::base64url::decode_exact(id, short_key));
    CHECK_FALSE(ac3::sendspin::base64url::decode_exact(id + "A", back));
    CHECK_FALSE(ac3::sendspin::base64url::decode_exact(id.substr(0, 42), back));
}
