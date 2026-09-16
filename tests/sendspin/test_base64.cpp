#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/sendspin/base64.hpp"

// Standard Base64 with padding, which player@v1 uses for codec_header: RFC 4648's own test
// vectors, and the strictness the decoder shares with base64url.

namespace {

std::vector<std::uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

}  // namespace

TEST_CASE("base64: RFC 4648 section 10 vectors", "[sendspin][base64]") {
    const std::vector<std::pair<std::string_view, std::string_view>> vectors{
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    for (const auto& [plain, encoded] : vectors) {
        CHECK(ac3::sendspin::base64::encode(bytes_of(plain)) == encoded);
        CHECK(ac3::sendspin::base64::encoded_size(plain.size()) == encoded.size());
        const auto decoded = ac3::sendspin::base64::decode(encoded);
        REQUIRE(decoded.has_value());
        CHECK(*decoded == bytes_of(plain));
    }
}

TEST_CASE("base64: the standard alphabet's last two characters", "[sendspin][base64]") {
    const std::vector<std::uint8_t> high{0xFB, 0xFF, 0xBF};
    CHECK(ac3::sendspin::base64::encode(high) == "+/+/");
    CHECK(ac3::sendspin::base64::decode("+/+/") == high);
    // base64url's characters are not in this alphabet.
    CHECK_FALSE(ac3::sendspin::base64::decode("-_-_").has_value());
}

TEST_CASE("base64: decoding is strict", "[sendspin][base64]") {
    using ac3::sendspin::base64::decode;
    CHECK_FALSE(decode("Zg").has_value());      // no padding
    CHECK_FALSE(decode("Zg=").has_value());     // length not a multiple of four
    CHECK_FALSE(decode("Zg===").has_value());
    CHECK_FALSE(decode("Z===").has_value());    // three padding characters
    CHECK_FALSE(decode("Zh==").has_value());    // bits past the last byte are not zero
    CHECK_FALSE(decode("Zm9=").has_value());
    CHECK_FALSE(decode("Zm=v").has_value());    // padding in the middle
    CHECK_FALSE(decode("Zm9v\n").has_value());  // whitespace
    CHECK_FALSE(decode("====").has_value());
}
