#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ac3/export.hpp"

// Where things ARE in an E-AC-3 syncframe that carries an EMDF object
// container - a bit-accurate map of the frame, with no opinion about what to
// do with it.
//
// Two very different jobs need the same map, which is why it lives here
// rather than inside either of them:
//
//   - ac3::signing (src/signing) authenticates a frame by hashing everything
//     EXCEPT the regions a licensed decoder is allowed to rewrite - the
//     framing words, the metadata flags, the skip fields and the CRC tail.
//     It needs those regions ("holes") and the container's own position.
//   - ac3::io::strip_objects (ac3/io/object_strip.hpp) removes the object
//     layer without touching the audio. It needs the skip fields' exact bit
//     ranges, the frame-level skipflde flag, the addbsi object-audio marker,
//     and where the last mantissa ends.
//
// Walking a syncframe to bit accuracy means re-deriving every field width
// that depends on content - exponent strategies, spectral extension geometry,
// delta bit allocation, the bit allocation the mantissa widths come from -
// so having two copies of this walk would mean two copies of every one of
// those subtleties, drifting apart one bug fix at a time.
//
// SCOPE. The full map covers the shape ac3forge's own Atmos encoder emits
// (see src/forge/src/encoder/eac3_frame.cpp): one independent substream, 3/2
// with LFE, six blocks, frame-level exponent strategy and SNR, no coupling.
// That is exactly the shape both callers were written against. A frame
// outside it comes back with `supported` false rather than with a map
// derived from assumptions that do not hold - a bit offset wrong by one is
// not a smaller error here than a missing one, because both callers WRITE
// using these offsets.
//
// The object-layer SIGNALS are the exception, and are read for any E-AC-3
// syncframe whatever its shape: everything up to and including skipflde is
// reachable without a single content-dependent field width, so "does this
// frame carry an object layer at all?" always has an answer. That is what
// lets ac3::io::strip_objects pass an ordinary stereo stream through
// untouched instead of refusing it for being out of scope. See
// FrameLayout::object_signals.

namespace ac3::emdf {

// A closed bit range [first, last], counted from the frame's first bit.
struct BitRange {
    std::size_t first = 0;
    std::size_t last = 0;

    [[nodiscard]] constexpr std::size_t bits() const { return last - first + 1; }
};

// §5.4.3.58-60's skip field as it appears in one audio block: the skiple flag
// bit, plus - when that flag is set - the 9-bit skipl length and the skipl
// bytes themselves. `range` covers the whole thing either way, so removing it
// removes the field entirely rather than leaving an empty one behind.
struct SkipField {
    BitRange range{};
    int block = 0;
    bool present = false;      // skiple was set, so there is data, not just the flag bit
    bool carries_container = false;  // the EMDF sync word was found inside this one
};

struct FrameLayout {
    // False for a frame this walker declines to map - one outside the scope
    // in the header comment above, or one whose fields stopped making sense
    // part-way through (a spectral-extension band range that cannot exist,
    // say, which means bit tracking has already desynced). Everything below
    // is meaningless when this is false, EXCEPT the object-layer signals
    // that `object_signals` covers.
    bool supported = false;

    // True once bsi and the front of audfrm have been walked successfully -
    // which is everything up to and including skipflde, and needs no
    // content-dependent field width at all, so it holds for ANY E-AC-3
    // syncframe, in scope or not. It is what lets a caller answer "does this
    // frame carry an object layer?" for a frame this walker will not fully
    // map: `addbsi_object_extension` and `skipflde` (with `skipflde_bit`,
    // `addbsi` and `oba_complexity_index`) are filled in whenever this is
    // true. A frame with neither signal set has no object layer in it, no
    // matter what shape it is.
    bool object_signals = false;

    std::size_t frame_bits = 0;
    // First bit after the last block's mantissas - i.e. where auxdata begins
    // (§5.4.4). Everything from here to the end of the frame is padding,
    // auxdatae, crcrsv and crc2.
    std::size_t audio_end_bits = 0;

    // --- object layer -------------------------------------------------------
    bool has_container = false;
    std::size_t container_start = 0;        // bit offset of the emdf_sync word
    int container_len = 0;                  // content bytes, after the 32-bit header
    std::size_t container_parsed_bits = 0;  // from container_start, through the protection bits
    int protection_primary_code = 0;
    int protection_secondary_code = 0;

    // --- excisable regions --------------------------------------------------
    // Table E1.3's frame-level skipflde flag: one bit, whose position is
    // recorded so a rewriter can clear it, and whose value says whether the
    // per-block skip fields below exist at all.
    std::size_t skipflde_bit = 0;
    bool skipflde = false;
    std::vector<SkipField> skip_fields{};
    // Table E1.2's addbsi element (addbsie, addbsil and the payload), when
    // one is present. `object_extension` is true only when that payload is
    // EXACTLY TS 103 420 §8.3.1's object-audio marker - seven zero reserved
    // bits, flag_ec3_extension_type_a set, and (when addbsil allows it) the
    // §8.3.2.2 complexity index - and never for an addbsi carrying something
    // else, which is not this project's to interpret or remove.
    std::optional<BitRange> addbsi = std::nullopt;
    bool addbsi_object_extension = false;
    int oba_complexity_index = 0;
    // Table E1.3's blkstrtinfoe. Recorded because blkstrtinfo's own field
    // width is derived from frmsiz (E2.3.3.2's "bit_length(frmsiz+1)"), so a
    // rewriter that re-derives the frame size cannot leave this field alone -
    // which is why ac3::io::strip_objects refuses a frame that carries one
    // rather than shifting the bits under it.
    bool blkstrtinfoe = false;

    // Every region ac3::signing excludes from the authenticated message,
    // ascending, non-overlapping: the framing words, the infomdate flag, the
    // addbsi element, the skipflde flag, every skip field, and the auxdata +
    // CRC tail.
    std::vector<BitRange> holes{};
};

// Maps one complete E-AC-3 syncframe. `frame` must be exactly the syncframe -
// syncframe_size() below is what sizes it out of a stream.
[[nodiscard]] AC3FORGE_EXPORT FrameLayout walk_frame(std::span<const std::byte> frame);

// §E2.3.1.3: frmsiz (bits 16..26) is the frame's length in 16-bit words minus
// one. Undefined for fewer than 4 bytes, which is not a syncframe.
[[nodiscard]] AC3FORGE_EXPORT std::size_t syncframe_size(std::span<const std::byte> at);

}  // namespace ac3::emdf
