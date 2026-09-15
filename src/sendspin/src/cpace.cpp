#include "ac3/sendspin/cpace.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/sendspin/crypto.hpp"
#include "field25519.hpp"

namespace ac3::sendspin::cpace {

namespace {

namespace fe = field25519;

constexpr std::size_t kSha512BlockBytes = 128;
constexpr std::string_view kDsi = "CPace255";
constexpr std::string_view kDsiIsk = "CPace255_ISK";
constexpr std::string_view kMacLabel = "CPaceMac";

[[nodiscard]] Bytes bytes_of(std::string_view text) {
    return {static_cast<const std::uint8_t*>(static_cast<const void*>(text.data())), text.size()};
}

[[nodiscard]] std::size_t prepend_len_size(std::size_t length) {
    std::size_t bytes = 1;
    while (length >= 128) {
        length >>= 7U;
        ++bytes;
    }
    return bytes;
}

// Curve25519's A = 486662, as limbs of 16 bits.
constexpr fe::Element kA{0x6D06, 0x0007};

[[nodiscard]] bool constant_time_equal(Bytes a, Bytes b) {
    if (a.size() != b.size()) {
        return false;
    }
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return difference == 0;
}

}  // namespace

void append_prepend_len(std::vector<std::uint8_t>& out, Bytes data) {
    // LEB128: seven bits a byte, least significant first, bit 7 set while more follow.
    std::size_t length = data.size();
    do {
        auto byte = static_cast<std::uint8_t>(length & 0x7FU);
        length >>= 7U;
        if (length != 0) {
            byte |= 0x80U;
        }
        out.push_back(byte);
    } while (length != 0);
    out.insert(out.end(), data.begin(), data.end());
}

std::vector<std::uint8_t> lv_cat(std::span<const Bytes> parts) {
    std::vector<std::uint8_t> out;
    for (const Bytes part : parts) {
        append_prepend_len(out, part);
    }
    return out;
}

std::vector<std::uint8_t> generator_string(Bytes prs, Bytes ci, Bytes sid) {
    const Bytes dsi = bytes_of(kDsi);
    const std::size_t used = prepend_len_size(prs.size()) + prs.size() +
                             prepend_len_size(dsi.size()) + dsi.size() + 1;
    const std::size_t zero_pad =
        kSha512BlockBytes > used ? kSha512BlockBytes - used : std::size_t{0};
    // len_zpad = max(0, s_in_bytes - 1 - len(prepend_len(PRS)) - len(prepend_len(DSI))),
    // with len(prepend_len(x)) counting x itself.
    const std::vector<std::uint8_t> zeros(zero_pad, 0);
    const std::array<Bytes, 5> parts{dsi, prs, Bytes(zeros), ci, sid};
    return lv_cat(parts);
}

Key32 elligator2(const Key32& encoded_field_element) {
    const fe::Element r = fe::decode(encoded_field_element);
    // v = -A / (1 + 2 r^2). 1 + 2 r^2 is never 0: -1/2 is not a square modulo p.
    const fe::Element r2 = fe::square(r);
    const fe::Element denominator = fe::add(fe::add(r2, r2), fe::kOne);
    const fe::Element minus_a = fe::sub(fe::kZero, kA);
    const fe::Element v = fe::mul(minus_a, fe::invert(denominator));
    // gx = v^3 + A v^2 + v = v (v (v + A) + 1); x = v if gx is a square, else -v - A.
    const fe::Element gx = fe::mul(v, fe::add(fe::mul(v, fe::add(v, kA)), fe::kOne));
    const fe::Bytes32 symbol = fe::encode(fe::legendre(gx));
    const fe::Bytes32 one = fe::encode(fe::kOne);
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < symbol.size(); ++i) {
        difference |= static_cast<std::uint8_t>(symbol[i] ^ one[i]);
    }
    const std::int64_t is_square = ((std::int64_t{difference} - 1) >> 8U) & 1;
    fe::Element x = fe::sub(minus_a, v);
    fe::select(x, v, is_square);
    return fe::encode(x);
}

std::optional<Key32> calculate_generator(Bytes prs, Bytes ci, Bytes sid) {
    const std::vector<std::uint8_t> gen = generator_string(prs, ci, sid);
    crypto::Digest64 digest{};
    if (!crypto::sha512({Bytes(gen)}, digest)) {
        return std::nullopt;
    }
    Key32 prefix{};
    std::copy_n(digest.begin(), prefix.size(), prefix.begin());
    const Key32 g = elligator2(prefix);
    crypto::wipe(digest);
    crypto::wipe(prefix);
    return g;
}

Party::~Party() {
    crypto::wipe(scalar_);
    crypto::wipe(isk_);
    crypto::wipe(mac_key_);
}

std::optional<Party> Party::start(Role role, Bytes prs, Bytes ci, Bytes sid, Bytes ad) {
    Key32 scalar{};
    if (!crypto::random_bytes(scalar)) {
        return std::nullopt;
    }
    std::optional<Party> party = start_with_scalar(role, prs, ci, sid, ad, scalar);
    crypto::wipe(scalar);
    return party;
}

std::optional<Party> Party::start_with_scalar(Role role, Bytes prs, Bytes ci, Bytes sid,
                                              Bytes ad, const Key32& scalar) {
    std::optional<Key32> g = calculate_generator(prs, ci, sid);
    if (!g) {
        return std::nullopt;
    }
    Party party;
    party.role_ = role;
    party.sid_.assign(sid.begin(), sid.end());
    party.ad_.assign(ad.begin(), ad.end());
    party.scalar_ = scalar;
    // scalar_mult(y, g) = X25519(y, g); a generator of low order would make every
    // share the identity.
    if (!crypto::x25519(party.scalar_, *g, party.share_)) {
        return std::nullopt;
    }
    return party;
}

bool Party::derive(Bytes peer_share, Bytes peer_ad) {
    if (spent_ || peer_share.size() != peer_share_.size()) {
        spent_ = true;
        crypto::wipe(scalar_);
        return false;
    }
    spent_ = true;
    std::copy(peer_share.begin(), peer_share.end(), peer_share_.begin());
    peer_ad_.assign(peer_ad.begin(), peer_ad.end());

    Key32 k{};
    const bool shared = crypto::x25519(scalar_, peer_share_, k);
    crypto::wipe(scalar_);
    if (!shared) {
        crypto::wipe(k);
        return false;
    }

    const bool initiator = role_ == Role::kInitiator;
    const Bytes ya = initiator ? Bytes(share_) : Bytes(peer_share_);
    const Bytes ada = initiator ? Bytes(ad_) : Bytes(peer_ad_);
    const Bytes yb = initiator ? Bytes(peer_share_) : Bytes(share_);
    const Bytes adb = initiator ? Bytes(peer_ad_) : Bytes(ad_);
    const std::array<Bytes, 3> isk_head_parts{bytes_of(kDsiIsk), Bytes(sid_), Bytes(k)};
    std::vector<std::uint8_t> input = lv_cat(isk_head_parts);
    const std::array<Bytes, 4> transcript_parts{ya, ada, yb, adb};
    const std::vector<std::uint8_t> transcript = lv_cat(transcript_parts);
    input.insert(input.end(), transcript.begin(), transcript.end());

    const bool ok = crypto::sha512({Bytes(input)}, isk_) &&
                    crypto::sha512({bytes_of(kMacLabel), Bytes(sid_), Bytes(isk_)}, mac_key_);
    crypto::wipe(k);
    crypto::wipe(input);
    if (!ok) {
        crypto::wipe(isk_);
        crypto::wipe(mac_key_);
        return false;
    }
    derived_ = true;
    return true;
}

std::optional<Digest64> Party::side_tag(bool own) const {
    if (!derived_) {
        return std::nullopt;
    }
    const Bytes share = own ? Bytes(share_) : Bytes(peer_share_);
    const Bytes ad = own ? Bytes(ad_) : Bytes(peer_ad_);
    const std::array<Bytes, 2> parts{share, ad};
    const std::vector<std::uint8_t> message = lv_cat(parts);
    Digest64 tag{};
    if (!crypto::hmac_sha512(mac_key_, {Bytes(message)}, tag)) {
        return std::nullopt;
    }
    return tag;
}

std::optional<Digest64> Party::tag() const { return side_tag(true); }

bool Party::verify(Bytes peer_tag) const {
    if (!derived_) {
        return false;
    }
    // A reflected transcript, whose peer side is this side's own message, would let
    // this side's tag verify as the peer's.
    if (constant_time_equal(share_, peer_share_) && constant_time_equal(ad_, peer_ad_)) {
        return false;
    }
    const std::optional<Digest64> expected = side_tag(false);
    return expected && constant_time_equal(*expected, peer_tag);
}

}  // namespace ac3::sendspin::cpace
