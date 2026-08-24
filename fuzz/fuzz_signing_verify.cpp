#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "ac3/signing/emdf_atmos_signer.hpp"
#include "ac3/signing/signing_key.hpp"

// ac3::signing::verify_atmos_stream / verify_atmos_frame (src/signing/src/
// emdf_atmos_signer.cpp).
//
// Verification is the one signing operation that runs on input the operator
// did NOT produce: `ac3cli decode ... verify-objects` points it at whatever
// stream arrived, and both entry points walk the E-AC-3 bitstream themselves
// - frmsiz-delimited framing, the whole bsi/audblk field walk that locates
// the EMDF container's bit offset, then the container's own header - with no
// CRC check anywhere in front of them. Signing is deliberately NOT fuzzed
// here: it mutates the caller's own buffer, and a caller signs a stream it
// just encoded.
//
// Input layout: one length byte, that many key bytes, then the stream.
//     [0]            key length, masked to 0..63
//     [1 .. 1+n)     the key, handed to SigningKey verbatim
//     [1+n .. )      the E-AC-3 stream to verify
// The key is part of the fuzzed input because it is part of the untrusted
// surface too (a key file is operator-supplied and may be any length,
// including empty - verify_atmos_stream, unlike sign_atmos_stream, has no
// key.empty() early-out), and because keying the HMAC differently is what
// makes the kValid and kMismatch branches both reachable. Nothing here
// derives, forges or reconstructs a key: an arbitrary key simply produces an
// arbitrary tag, which is the whole point.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::size_t key_len = static_cast<std::size_t>(data[0]) & 0x3F;
    if (size < 1 + key_len) {
        return 0;
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(data);
    std::vector<std::byte> key_bytes(bytes + 1, bytes + 1 + key_len);
    const ac3::signing::SigningKey key{std::move(key_bytes)};
    const std::span<const std::byte> stream{bytes + 1 + key_len, size - 1 - key_len};

    (void)ac3::signing::verify_atmos_stream(stream, key);
    // ... and the single-frame entry directly, not only through the stream
    // walk above: verify_atmos_frame is public, so a caller that has already
    // split the stream itself reaches it with a span the stream walk's own
    // frmsiz arithmetic never would have produced.
    (void)ac3::signing::verify_atmos_frame(stream, key);
    return 0;
}
