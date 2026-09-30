#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/export.hpp"

// Removing the object layer from a Dolby Digital Plus JOC stream, losslessly.
//
// A DD+ JOC stream is an ordinary 5.1 E-AC-3 stream that happens to carry an
// EMDF container - OAMD positions plus JOC side information - inside the
// per-block skip fields the standard already requires a decoder to step over
// (see ac3/emdf/emdf.hpp). The bed underneath is the FULL mix, every object
// already panned into it, which is precisely why an Atmos-unaware decoder can
// play the stream at all.
//
// So the 5.1 rendition of a JOC stream does not need re-encoding: it needs the
// container taken out. Everything that carries audio - exponents, mantissas,
// bit allocation, coupling, spectral extension - is copied bit for bit, and
// the result decodes to sample-identical PCM. What changes is the framing
// around it: the skip fields go, the TS 103 420 §8.3.1 object-audio marker in
// addbsi goes, frmsiz is re-derived for the shorter frame, the auxdata padding
// is re-laid, and crc2 is re-stamped over the result.
//
// Why "removed" and not "emptied": an EMDF container with no payloads still
// signals an object layer to everything downstream - the dec3 box's Atmos
// extension, a DASH ExtensionType property, an HLS CHANNELS="<N>/JOC"
// attribute - for a stream that no longer has one. The project's rule is that
// a stream carries objects or omits the container entirely, never an empty
// one (docs/concepts/atmos-joc.md), and that is what this implements.
//
// This is the inverse of ac3::signing's in-place EMDF rewrite and, like it,
// needs no key: taking a container out is not authenticating one. A stripped
// frame simply has nothing left for a decoder's authenticity gate to check.
//
// FRAME SIZE. A rewritten frame is sized to the content it actually holds -
// §E2.3.1.3 makes frmsiz an arbitrary per-frame word count, unlike AC-3's
// index into Table 5.18, so every frame is free to be exactly as long as it
// needs to be. That means the output is SMALLER than the input, which is the
// point for a delivery rendition, but it also means a constant-rate input
// does not stay constant-rate: alongside the container, whatever auxdata
// padding the encoder used to hit its target rate goes too. A frame with
// nothing to remove is copied byte for byte and keeps its original size.
//
// Note on CRCs: unlike an AC-3 syncframe, an E-AC-3 one has no crc1 - Annex E
// dropped it, and §E2.3.1 leaves syncinfo as the syncword alone. crc2 is the
// only check word, it sits at the very end of the frame, and it covers
// everything between the syncword and itself, so re-stamping it is an ordinary
// forward recompute over the finished frame rather than anything that has to
// be solved for.

namespace ac3::io {

enum class StripError : std::uint8_t {
    kEmpty,       // nothing to read
    kLostSync,    // expected 0x0B77 at a frame boundary
    kTruncated,   // the stream ends mid-frame
    kNotEac3,     // an AC-3 stream: no Annex E substreams, so no object layer to remove
    // A frame that carries a skip field or the addbsi object-audio marker -
    // so it MIGHT hold an object layer - but whose bit layout this project
    // cannot map (ac3/emdf/frame_layout.hpp's SCOPE note). Refused rather
    // than passed through: passing it through could hand back a stream still
    // carrying the objects this function promises to remove, and "I could not
    // confirm this frame is clean" is the honest answer. A skip field used as
    // plain padding rather than for an EMDF container lands here too, since
    // telling the two apart is exactly what the full map is for.
    kUnsupportedFrame,
    // A frame whose blkstrtinfo field is present. Its own width is derived
    // from frmsiz (§E2.3.3.2), and frmsiz necessarily changes here, so the
    // field would have to be re-encoded rather than copied. ac3forge never
    // writes one (eac3_frame.cpp sends blkstrtinfoe = 0).
    kFrameSizeDependentField,
};

[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(StripError error);

struct StrippedStream {
    std::vector<std::byte> bytes;
    // Every syncframe seen, and how many of them actually carried something to
    // remove. A stream where `frames_stripped` is 0 came back byte-identical.
    int frames_total = 0;
    int frames_stripped = 0;
    std::size_t bytes_removed = 0;
};

// Strips every frame in `stream`, returning the new elementary stream.
// Frames with no object layer are copied through untouched, byte for byte -
// including frames of a shape this project cannot map, since a frame with
// neither the addbsi object marker nor a skip field has nothing to strip
// whatever its shape.
[[nodiscard]] AC3FORGE_EXPORT std::expected<StrippedStream, StripError> strip_objects(
    std::span<const std::byte> stream);

}  // namespace ac3::io
