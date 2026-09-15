#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/cpace.hpp"
#include "ac3/sendspin/crypto.hpp"
#include "sendspin/sendspin_test_support.hpp"

// CPACE-X25519-SHA512 against draft-irtf-cfrg-cpace-21's own test vectors
// (Appendix B.1, "Test vector for CPace using group X25519 and hash SHA-512"), the
// string helpers' vectors from its appendix, and the low-order u coordinates every
// X25519 implementation of CPace must refuse.

namespace {

using ac3::sendspin::cpace::Party;
using ac3::sendspin::cpace::Role;
using ac3::sendspin::crypto::Bytes;
using ac3::sendspin::crypto::Key32;
using ac3::sendspin::test::bytes_of;
using ac3::sendspin::test::from_hex;
using ac3::sendspin::test::key_from_hex;
using ac3::sendspin::test::to_hex;

constexpr std::string_view kPrs = "Password";
constexpr std::string_view kCi = "0b415f696e69746961746f720b425f726573706f6e646572";
constexpr std::string_view kSid = "7e4b4791d6a8ef019b936c79fb7f2c57";
constexpr std::string_view kYa = "21b4f4bd9e64ed355c3eb676a28ebedaf6d8f17bdc365995b319097153044080";
constexpr std::string_view kYb = "848b0779ff415f0af4ea14df9dd1d3c29ac41d836c7808896c4eba19c51ac40a";

}  // namespace

TEST_CASE("cpace: prepend_len and lv_cat vectors", "[sendspin][cpace]") {
    std::vector<std::uint8_t> out;
    ac3::sendspin::cpace::append_prepend_len(out, {});
    CHECK(to_hex(out) == "00");
    out.clear();
    ac3::sendspin::cpace::append_prepend_len(out, bytes_of("1234"));
    CHECK(to_hex(out) == "0431323334");

    std::vector<std::uint8_t> range(128);
    for (std::size_t i = 0; i < range.size(); ++i) {
        range[i] = static_cast<std::uint8_t>(i);
    }
    out.clear();
    ac3::sendspin::cpace::append_prepend_len(out, std::span<const std::uint8_t>(range).first(127));
    CHECK(out.size() == 128);
    CHECK(out[0] == 0x7F);
    out.clear();
    ac3::sendspin::cpace::append_prepend_len(out, range);
    CHECK(out.size() == 130);
    CHECK(out[0] == 0x80);
    CHECK(out[1] == 0x01);
    CHECK(out[2] == 0x00);

    const std::array<Bytes, 4> parts{bytes_of("1234"), bytes_of("5"), Bytes{}, bytes_of("678")};
    CHECK(to_hex(ac3::sendspin::cpace::lv_cat(parts)) == "043132333401350003363738");

    const std::array<Bytes, 4> transcript{bytes_of("123"), bytes_of("PartyA"), bytes_of("234"),
                                          bytes_of("PartyB")};
    CHECK(to_hex(ac3::sendspin::cpace::lv_cat(transcript)) ==
          "03313233065061727479410332333406506172747942");
}

TEST_CASE("cpace: calculate_generator for X25519 and SHA-512", "[sendspin][cpace]") {
    const std::vector<std::uint8_t> ci = from_hex(kCi);
    const std::vector<std::uint8_t> sid = from_hex(kSid);
    const std::vector<std::uint8_t> gen = ac3::sendspin::cpace::generator_string(bytes_of(kPrs), ci, sid);
    CHECK(gen.size() == 170);
    // lv_cat(DSI, PRS, 109 zero bytes, CI, sid).
    const std::string expected = std::string("08") + "4350616365323535" + "08" + "50617373776f7264" +
                                 "6d" + std::string(2 * 109, '0') + "18" + std::string(kCi) + "10" +
                                 std::string(kSid);
    CHECK(to_hex(gen) == expected);

    // hash generator string, then Elligator 2 on its first 32 bytes.
    CHECK(to_hex(ac3::sendspin::cpace::elligator2(
              key_from_hex("03998087bdb1a2617bbe25ef5a7c18cd4f84f902328701790958755ee4aed1d3"))) ==
          "d04bf6d41f6a289632a2e929fa29bebd51092512a7829fdde7d314b62f05a73f");

    const std::optional<Key32> g = ac3::sendspin::cpace::calculate_generator(bytes_of(kPrs), ci, sid);
    REQUIRE(g.has_value());
    CHECK(to_hex(*g) == "d04bf6d41f6a289632a2e929fa29bebd51092512a7829fdde7d314b62f05a73f");
}

TEST_CASE("cpace: the draft's initiator-responder run", "[sendspin][cpace]") {
    const std::vector<std::uint8_t> ci = from_hex(kCi);
    const std::vector<std::uint8_t> sid = from_hex(kSid);
    std::optional<Party> a = Party::start_with_scalar(Role::kInitiator, bytes_of(kPrs), ci, sid,
                                                      bytes_of("ADa"), key_from_hex(kYa));
    std::optional<Party> b = Party::start_with_scalar(Role::kResponder, bytes_of(kPrs), ci, sid,
                                                      bytes_of("ADb"), key_from_hex(kYb));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(to_hex(a->share()) == "1d13c89278cdadd826f6d8d7f887701430f8380ddc17611cdd6dc989ce0c9f32");
    CHECK(to_hex(b->share()) == "248cccf6d5cdc3646f0ad593f9e6cef4e69d4945f8372e623512ecea32185623");

    REQUIRE(b->derive(a->share(), bytes_of("ADa")));
    REQUIRE(a->derive(b->share(), bytes_of("ADb")));
    const std::string_view isk_ir =
        "6e19b875f7a561d6b3ca3dbb9ef42ac55de3e717881018204b8922b4d5e53bb2aa82c300bea7b65d2b671da71922ddf6472301b79bc270adfa8bf413285f2263";
    CHECK(to_hex(a->isk()) == isk_ir);
    CHECK(to_hex(b->isk()) == isk_ir);

    // Mutual confirmation: each side's tag verifies at the other, and not at itself.
    const std::optional<ac3::sendspin::crypto::Digest64> ta = a->tag();
    const std::optional<ac3::sendspin::crypto::Digest64> tb = b->tag();
    REQUIRE(ta.has_value());
    REQUIRE(tb.has_value());
    CHECK(b->verify(*ta));
    CHECK(a->verify(*tb));
    CHECK_FALSE(a->verify(*ta));
    CHECK_FALSE(b->verify(*tb));
    ac3::sendspin::crypto::Digest64 bent = *ta;
    bent[63] ^= 1U;
    CHECK_FALSE(b->verify(bent));
    CHECK_FALSE(b->verify(std::span<const std::uint8_t>(*ta).first(63)));

    // mac_key = SHA-512("CPaceMac" || sid || ISK), Ta = HMAC-SHA-512(mac_key, lv_cat(Ya, ADa)).
    ac3::sendspin::crypto::Digest64 mac_key{};
    REQUIRE(ac3::sendspin::crypto::sha512({bytes_of("CPaceMac"), Bytes(sid), Bytes(a->isk())}, mac_key));
    const std::array<Bytes, 2> message{Bytes(a->share()), bytes_of("ADa")};
    ac3::sendspin::crypto::Digest64 expected{};
    REQUIRE(ac3::sendspin::crypto::hmac_sha512(mac_key, {Bytes(ac3::sendspin::cpace::lv_cat(message))}, expected));
    CHECK(*ta == expected);
}

TEST_CASE("cpace: derive once only, and not before", "[sendspin][cpace]") {
    std::optional<Party> a = Party::start(Role::kInitiator, bytes_of("12345678"), {}, bytes_of("sid"), bytes_of("server"));
    std::optional<Party> b = Party::start(Role::kResponder, bytes_of("12345678"), {}, bytes_of("sid"), bytes_of("client"));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK_FALSE(a->tag().has_value());
    CHECK_FALSE(a->verify(std::array<std::uint8_t, 64>{}));
    REQUIRE(a->derive(b->share(), bytes_of("client")));
    CHECK_FALSE(a->derive(b->share(), bytes_of("client")));
    CHECK(a->derived());
}

TEST_CASE("cpace: a different pairing code, sid or AD gives tags that do not verify",
          "[sendspin][cpace]") {
    const auto run = [](std::string_view prs_a, std::string_view prs_b, std::string_view sid_b,
                        std::string_view ad_b_seen_by_a) {
        std::optional<Party> a = Party::start(Role::kInitiator, bytes_of(prs_a), {}, bytes_of("sid"), bytes_of("server"));
        std::optional<Party> b = Party::start(Role::kResponder, bytes_of(prs_b), {}, bytes_of(sid_b), bytes_of("client"));
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE(a->derive(b->share(), bytes_of(ad_b_seen_by_a)));
        REQUIRE(b->derive(a->share(), bytes_of("server")));
        return b->verify(*a->tag()) && a->verify(*b->tag());
    };
    CHECK(run("123456", "123456", "sid", "client"));
    CHECK_FALSE(run("123456", "123457", "sid", "client"));
    CHECK_FALSE(run("123456", "123456", "sie", "client"));
    CHECK_FALSE(run("123456", "123456", "sid", "clienT"));
}

TEST_CASE("cpace: shares of the wrong length or of low order abort", "[sendspin][cpace]") {
    // The draft's u0 to u5 and u7 must abort when received as a share.
    const std::array<std::string_view, 7> low_order{
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    };
    for (const std::string_view u : low_order) {
        INFO(u);
        std::optional<Party> a = Party::start(Role::kInitiator, bytes_of("12345678"), {}, bytes_of("sid"), bytes_of("server"));
        REQUIRE(a.has_value());
        const Key32 share = key_from_hex(u);
        CHECK_FALSE(a->derive(share, bytes_of("client")));
        CHECK_FALSE(a->tag().has_value());
    }
    std::optional<Party> a = Party::start(Role::kInitiator, bytes_of("12345678"), {}, bytes_of("sid"), bytes_of("server"));
    REQUIRE(a.has_value());
    CHECK_FALSE(a->derive(std::vector<std::uint8_t>(31, 7), bytes_of("client")));
}

TEST_CASE("cpace: X25519 on the draft's non-canonical and twist points", "[sendspin][cpace]") {
    // u6, u8, u9, ua and ub have bit 255 set or lie past p; with it cleared they are
    // not low order, and s times them is the draft's q value.
    const Key32 s = key_from_hex("af46e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449aff");
    const std::array<std::array<std::string_view, 2>, 5> cases{{
        {"daffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "d8e2c776bbacd510d09fd9278b7edcd25fc5ae9adfba3b6e040e8d3b71b21806"},
        {"dbffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "c85c655ebe8be44ba9c0ffde69f2fe10194458d137f09bbff725ce58803cdb38"},
        {"d9ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "db64dafa9b8fdd136914e61461935fe92aa372cb056314e1231bc4ec12417456"},
        {"cdeb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b880",
         "e062dcd5376d58297be2618c7498f55baa07d7e03184e8aada20bca28888bf7a"},
        {"4c9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f11d7",
         "993c6ad11c4c29da9a56f7691fd0ff8d732e49de6250b6c2e80003ff4629a175"},
    }};
    for (const auto& [u, q] : cases) {
        INFO(u);
        Key32 out{};
        REQUIRE(ac3::sendspin::crypto::x25519(s, key_from_hex(u), out));
        CHECK(to_hex(out) == q);
    }
}

TEST_CASE("cpace: Elligator 2 against Python's big-integer arithmetic", "[sendspin][cpace]") {
    // Inputs that exercise both branches of the map and values at or above p,
    // with outputs computed by cpace-py 0.1.0's _elligator2 over Python integers.
    const std::array<std::array<std::string_view, 2>, 6> cases{{
        {"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "1e5942dd97c756040d27755f1e5b11349cd47d796c45d07052f7e5b11541c349"},
        {"edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
         "0000000000000000000000000000000000000000000000000000000000000000"},
        {"f2ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
         "e967a8afafafafafafafafafafafafafafafafafafafafafafafafafafafaf2f"},
        {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
         "5f3520001c6c9936a31206afe7c7ac224e8861619bf98872444915899d95f46e"},
        // The one input here whose gx is a square.
        {"0100000000000000000000000000000000000000000000000000000000000000",
         "9cdb525555555555555555555555555555555555555555555555555555555555"},
        {"0000000000000000000000000000000000000000000000000000000000000000",
         "0000000000000000000000000000000000000000000000000000000000000000"},
    }};
    for (const auto& [in, out] : cases) {
        INFO(in);
        CHECK(to_hex(ac3::sendspin::cpace::elligator2(key_from_hex(in))) == out);
    }
}
