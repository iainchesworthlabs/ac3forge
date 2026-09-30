#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ac3/sendspin/crypto.hpp"

// CPace, CPACE-X25519-SHA512 (draft-irtf-cfrg-cpace-21), in the initiator-responder
// setting with explicit mutual key confirmation, as Sendspin's code-based pairing
// uses it (pairing.md, PAKE): the server is A and the client B.
//
// The confirmation tags follow the draft's §9.4 suggestion with HMAC-SHA-512,
// mac_key = SHA-512("CPaceMac" || sid || ISK) and Ta = HMAC(mac_key, lv_cat(Ya, ADa)),
// which is also what cpace-py 0.1.0, aiosendspin 9.1.1's CPace, computes
// (planning/hearth-sendspin-extension.md, Q6).
//
// What the peer sends is checked here: a share that is not 32 bytes, or that X25519
// maps to the identity (a low-order point on the curve or its twist), aborts the
// run, as do tags that do not verify. K is never exposed.

namespace ac3::sendspin::cpace {

using crypto::Bytes;
using crypto::Digest64;
using crypto::Key32;

// The draft's string helpers, public for their test vectors.
void append_prepend_len(std::vector<std::uint8_t>& out, Bytes data);
[[nodiscard]] std::vector<std::uint8_t> lv_cat(std::span<const Bytes> parts);
[[nodiscard]] std::vector<std::uint8_t> generator_string(Bytes prs, Bytes ci, Bytes sid);

// RFC 9380's map_to_curve_elligator2 for Curve25519, u coordinate only, from the
// 32 bytes decodeUCoordinate reads.
[[nodiscard]] Key32 elligator2(const Key32& encoded_field_element);

// G_X25519.calculate_generator with SHA-512.
[[nodiscard]] std::optional<Key32> calculate_generator(Bytes prs, Bytes ci, Bytes sid);

enum class Role : std::uint8_t {
    kInitiator,
    kResponder,
};

// One side of one CPace run. Holds its scalar until derive(), and ISK after.
class Party {
   public:
    ~Party();
    Party(const Party&) = delete;
    Party& operator=(const Party&) = delete;
    Party(Party&&) noexcept = default;
    Party& operator=(Party&&) noexcept = default;

    // Samples a scalar from the CSPRNG and computes this side's share.
    [[nodiscard]] static std::optional<Party> start(Role role, Bytes prs, Bytes ci, Bytes sid,
                                                    Bytes ad);
    // The same with a given scalar, for test vectors.
    [[nodiscard]] static std::optional<Party> start_with_scalar(Role role, Bytes prs, Bytes ci,
                                                                Bytes sid, Bytes ad,
                                                                const Key32& scalar);

    // Ya for the initiator, Yb for the responder.
    [[nodiscard]] const Key32& share() const { return share_; }

    // Takes the peer's share and associated data, and derives ISK and the
    // confirmation key. Once only. False aborts the run.
    [[nodiscard]] bool derive(Bytes peer_share, Bytes peer_ad);

    [[nodiscard]] bool derived() const { return derived_; }
    [[nodiscard]] const Digest64& isk() const { return isk_; }

    // This side's confirmation tag: Ta for the initiator, Tb for the responder.
    [[nodiscard]] std::optional<Digest64> tag() const;
    // Whether `peer_tag` is the peer's tag, compared in constant time. False before
    // derive(), and for a reflected transcript whose two sides are equal.
    [[nodiscard]] bool verify(Bytes peer_tag) const;

   private:
    Party() = default;

    [[nodiscard]] std::optional<Digest64> side_tag(bool own) const;

    Role role_ = Role::kInitiator;
    std::vector<std::uint8_t> sid_;
    std::vector<std::uint8_t> ad_;
    std::vector<std::uint8_t> peer_ad_;
    Key32 scalar_{};
    Key32 share_{};
    Key32 peer_share_{};
    Digest64 isk_{};
    Digest64 mac_key_{};
    bool derived_ = false;
    bool spent_ = false;
};

}  // namespace ac3::sendspin::cpace
