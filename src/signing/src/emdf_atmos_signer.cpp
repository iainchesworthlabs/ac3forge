#include "ac3/signing/emdf_atmos_signer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ac3/emdf/frame_layout.hpp"
#include "hmac_sha256.hpp"

namespace ac3::signing {
namespace {

// Where the frame's fields are is ac3::emdf::walk_frame's job (see
// ac3/emdf/frame_layout.hpp): one bit-accurate walk of the syncframe, shared
// with the object-layer strip in ac3::io, so the two cannot drift apart. What
// is left here is the part that is actually about signing - which of those
// regions are excluded from the authenticated message, and what is hashed
// over the rest.
using emdf::BitRange;
using emdf::FrameLayout;

int prot_bits(int code) { return (code == 0) ? 0 : (code == 1) ? 8 : (code == 2) ? 32 : 128; }

std::uint16_t crc16(const std::byte* p, std::size_t n) {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= std::uint16_t(std::to_integer<std::uint32_t>(p[i]) << 8);
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? std::uint16_t((crc << 1) ^ 0x8005) : std::uint16_t(crc << 1);
    }
    return crc;
}

bool bit_at(std::span<const std::byte> f, std::size_t p) {
    // Matches BitReader::read_bit()'s own contract: past the end reads as
    // zero rather than indexing out of bounds. Every caller here derives its
    // range from the layout's own container_len/container_start, so this
    // should never actually trip for a well-formed frame - it is
    // defence in depth for a function that indexes `f` directly, outside
    // BitReader.
    if ((p >> 3) >= f.size()) return false;
    return (std::to_integer<std::uint32_t>(f[p >> 3]) >> (7 - (p & 7))) & 1;
}

// Everything sign_atmos_frame and verify_atmos_frame both need: the parsed
// frame, the tag it computes from A||B, and where in the frame that tag
// belongs. Signing writes `digest` into the frame at `prim_off`; verifying
// reads what is already there at `prim_off` and compares. Neither the parse
// nor the HMAC construction differs between the two operations - only what
// happens with the result - so this is the one place that logic lives.
// nullopt means "no container to sign/verify", the same as the layout's own
// has_container.
struct TagContext {
    std::array<std::byte, 32> digest;
    int np;
    std::size_t prim_off;
};

std::optional<TagContext> compute_tag_context(std::span<const std::byte> frame,
                                               const SigningKey& key) {
    const FrameLayout p = emdf::walk_frame(frame);
    // A frame outside the walker's scope, or one whose fields stopped making
    // sense part-way through, reports no container - so it is left unsigned
    // rather than signed over a bit range that was never confirmed.
    if (!p.supported || !p.has_container) return std::nullopt;

    // Reconstruct A: excise holes, pack MSB-first, round to nearest 16-bit word.
    const std::size_t total = frame.size() * 8;
    std::vector<std::uint8_t> kept;
    kept.reserve(total);
    std::size_t pos = 0;
    // holes are recorded in walk order, which is ascending; sort defensively.
    auto holes = p.holes;
    std::sort(holes.begin(), holes.end(),
              [](const BitRange& x, const BitRange& y) { return x.first < y.first; });
    for (const auto& h : holes) {
        for (std::size_t q = pos; q < h.first; ++q) kept.push_back(bit_at(frame, q) ? 1 : 0);
        if (h.last + 1 > pos) pos = h.last + 1;
    }
    for (std::size_t q = pos; q < total; ++q) kept.push_back(bit_at(frame, q) ? 1 : 0);
    const std::size_t words = (kept.size() + 8) / 16;
    const std::size_t target = words * 16;
    if (target > kept.size()) kept.resize(target, 0);
    else kept.resize(target);
    std::vector<std::uint8_t> a_bytes((kept.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < kept.size(); ++i)
        if (kept[i]) a_bytes[i >> 3] |= std::uint8_t(1u << (7 - (i & 7)));

    // Build B: container content, primary+secondary tag bits zeroed - always,
    // regardless of what those bits currently hold, so verifying reproduces
    // exactly the message signing itself hashed.
    const int np = prot_bits(p.protection_primary_code);
    const int ms = prot_bits(p.protection_secondary_code);
    const std::size_t clen = std::size_t(p.container_len);
    std::vector<std::uint8_t> content(clen * 8);
    for (std::size_t k = 0; k < clen * 8; ++k)
        content[k] = bit_at(frame, p.container_start + 32 + k) ? 1 : 0;
    const std::size_t pb = p.container_parsed_bits;
    for (std::size_t k = (pb - std::size_t(np) - std::size_t(ms) - 32); k < pb - 32; ++k)
        if (k < content.size()) content[k] = 0;
    std::vector<std::uint8_t> b_bytes((content.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < content.size(); ++i)
        if (content[i]) b_bytes[i >> 3] |= std::uint8_t(1u << (7 - (i & 7)));

    // tag = HMAC(key, A||B)[:np/8], with the key supplied by the operator.
    std::vector<std::byte> msg;
    msg.reserve(a_bytes.size() + b_bytes.size());
    for (std::uint8_t x : a_bytes) msg.push_back(std::byte{x});
    for (std::uint8_t x : b_bytes) msg.push_back(std::byte{x});

    const std::size_t prim_off = p.container_start + pb - std::size_t(np) - std::size_t(ms);
    return TagContext{.digest = hmac_sha256(key.bytes(), msg), .np = np, .prim_off = prim_off};
}

}  // namespace

bool has_authenticity_tag(std::span<const std::byte> frame) {
    // Deliberately key-free: where the tag LIVES is fixed by the container's
    // own protection-length codes, and only whether it matches needs a key.
    // So an inspection tool can answer "is this stream signed at all" - the
    // question `ac3cli probe` asks - without holding anything secret, which
    // is the whole point of keeping the key out of this tool (see
    // docs/concepts/object-signing.md).
    // walk_frame screens the frame's shape itself and reports no container
    // for anything outside this signer's subset - an ordinary non-Atmos
    // frame included - so nothing here has to pre-qualify what it is handed.
    const FrameLayout p = emdf::walk_frame(frame);
    if (!p.supported || !p.has_container) {
        return false;
    }
    const int np = prot_bits(p.protection_primary_code);
    if (np <= 0) {
        // No primary protection field at all - the container declared it
        // absent, so there is nowhere for a tag to be.
        return false;
    }
    const std::size_t prim_off =
        p.container_start + p.container_parsed_bits - std::size_t(np) -
        std::size_t(prot_bits(p.protection_secondary_code));
    // An all-zero field is what an unsigned container carries: §H.2.2.4 leaves
    // the content implementation-defined, and this project's own writer emits
    // zeros until sign_atmos_frame replaces them. A real HMAC truncation
    // being all-zero is a 2^-np coincidence.
    for (int i = 0; i < np; ++i) {
        const std::size_t q = prim_off + static_cast<std::size_t>(i);
        if ((q >> 3) >= frame.size()) {
            return false;
        }
        if (bit_at(frame, q)) {
            return true;
        }
    }
    return false;
}

bool sign_atmos_frame(std::span<std::byte> frame, const SigningKey& key) {
    if (key.empty()) return false;
    const auto ctx = compute_tag_context(frame, key);
    if (!ctx) return false;

    // write protection_bits_primary (np bits) at prim_off. Bounds-checked for
    // the same reason bit_at() is: this indexes `frame` directly. A
    // well-formed match from walk_frame's single-container-per-frame rule
    // should never actually reach the out-of-range branch, but a write past
    // the end would corrupt the wrong memory rather than just read garbage,
    // so this one fails safe by skipping instead of clamping.
    for (int i = 0; i < ctx->np; ++i) {
        std::size_t q = ctx->prim_off + std::size_t(i);
        if ((q >> 3) >= frame.size()) continue;
        const bool bit = (std::to_integer<std::uint32_t>(ctx->digest[std::size_t(i / 8)]) >>
                          (7 - (i & 7))) &
                         1;
        std::byte& byte = frame[q >> 3];
        if (bit)
            byte |= static_cast<std::byte>(1u << (7 - (q & 7)));
        else
            byte &= static_cast<std::byte>(~(1u << (7 - (q & 7))) & 0xFFu);
    }
    // recompute crc2 (last two bytes; covers everything after the 16-bit sync)
    const std::uint16_t c = crc16(frame.data() + 2, frame.size() - 4);
    frame[frame.size() - 2] = std::byte(c >> 8);
    frame[frame.size() - 1] = std::byte(c & 0xFF);
    return true;
}

int sign_atmos_stream(std::span<std::byte> stream, const SigningKey& key) {
    if (key.empty()) return 0;
    int signed_count = 0;
    std::size_t off = 0;
    while (off + 6 <= stream.size()) {
        const std::size_t size = emdf::syncframe_size(stream.subspan(off));
        if (off + size > stream.size()) break;
        if (sign_atmos_frame(stream.subspan(off, size), key)) ++signed_count;
        off += size;
    }
    return signed_count;
}

VerifyResult verify_atmos_frame(std::span<const std::byte> frame, const SigningKey& key) {
    const auto ctx = compute_tag_context(frame, key);
    if (!ctx) return VerifyResult::kNoContainer;

    // Compare the digest just computed against whatever tag bits the frame
    // already carries at prim_off - unlike sign_atmos_frame, nothing here is
    // written back.
    for (int i = 0; i < ctx->np; ++i) {
        const std::size_t q = ctx->prim_off + std::size_t(i);
        const bool actual = bit_at(frame, q);
        const bool expected = (std::to_integer<std::uint32_t>(ctx->digest[std::size_t(i / 8)]) >>
                               (7 - (i & 7))) &
                              1;
        if (actual != expected) return VerifyResult::kMismatch;
    }
    return VerifyResult::kValid;
}

VerifySummary verify_atmos_stream(std::span<const std::byte> stream, const SigningKey& key) {
    VerifySummary summary;
    std::size_t off = 0;
    while (off + 6 <= stream.size()) {
        const std::size_t size = emdf::syncframe_size(stream.subspan(off));
        if (off + size > stream.size()) break;
        switch (verify_atmos_frame(stream.subspan(off, size), key)) {
            case VerifyResult::kValid: ++summary.valid; break;
            case VerifyResult::kMismatch: ++summary.mismatch; break;
            case VerifyResult::kNoContainer: ++summary.no_container; break;
        }
        off += size;
    }
    return summary;
}

}  // namespace ac3::signing
