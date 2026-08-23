#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/meta/mixing.hpp"

// Changing a stream's bsi metadata WITHOUT re-encoding the audio.
//
// dialnorm, compr, bsmod and dsurmod are all delivery decisions - what a
// receiver is told the dialogue level is, how hard to compress on an RF
// output, what kind of service this is, whether the surrounds were matrixed.
// Every one of them lands in bsi, ahead of the first audblk, and none of them
// changes a single coded coefficient. Re-encoding a programme to correct one
// costs a whole generation of lossy coding for no reason; this rewrites the
// bits in place and re-stamps the frame's CRCs instead, so the audio comes
// back out of a decoder bit-identical to what went in.
//
// The CRCs are the part that is not obvious. crc2 is an ordinary trailing
// CRC - recompute over the covered region and store it. crc1 is not: A/52
// §7.10.1 puts it BEFORE the region it protects and requires the register to
// read zero once the first 5/8 of the syncframe has been shifted through, so
// it has to be SOLVED rather than computed. ac3::solve_leading_crc
// (ac3/core/crc16.hpp) does that with a GF(2) polynomial inverse, and is the
// same function the encoder itself uses - see its own comment.
//
// Scope, stated as limits rather than left to be discovered:
//
//   * Only fields ALREADY ON THE WIRE can change. compr lives behind compre,
//     bsmod and dsurmod behind E-AC-3's infomdate; a frame that did not
//     transmit one has no bits to overwrite, and inserting them would move
//     every bit after and re-frame the syncframe - which is a re-encode by
//     another name. Asking for such a field is kFieldAbsent, and the answer
//     is to encode (or transcode) the stream with it enabled.
//   * A DEPENDENT E-AC-3 substream reports no compr at all, whatever its
//     compre bit says: §E3.8.5 repurposes compre there to mark the last
//     dependent of the programme rather than to announce a compression word
//     (see decoder.hpp's DecodedSubstream::compr). Its 8 bits are still on
//     the wire and are still skipped correctly; they are simply not a compr
//     word, so this refuses to write one into them.
//   * strmtyp 2 (a "convertible" substream, §E2.3.1.1) is refused outright,
//     matching ac3::plan::validate's own stance - its bsi carries an extra
//     blkid/frmsizecod branch nothing in this project produces or consumes.

namespace ac3::io {

enum class EditError : std::uint8_t {
    kBadSyncWord,
    kTruncated,        // the span is shorter than the syncframe's own declared size
    kUnsupportedBsid,  // not AC-3 (<= 10) or E-AC-3 (16)
    kReservedValue,    // a reserved fscod/frmsizecod, or strmtyp 2
    kFieldAbsent,      // asked to change a field this frame does not transmit
    kOutOfRange,       // a value the field cannot hold
};

[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(EditError error);

// E-AC-3's mixmdate group (Table E1.2), as transmitted. Every member is
// optional because acmod and lfeon decide which of them are sent at all -
// std::nullopt means "not on the wire", never "sent as zero".
//
// Read, not applied: this exists so a transcode can carry a DD+ stream's
// downmix intent across to the two coarse levels AC-3 has room for. See
// ac3::meta (ac3/meta/mixing.hpp) for what the values mean.
struct WireMixMetadata {
    std::optional<meta::DownmixMode> dmixmod;
    std::optional<meta::MixLevel> ltrtcmixlev;
    std::optional<meta::MixLevel> lorocmixlev;
    std::optional<meta::MixLevel> ltrtsurmixlev;
    std::optional<meta::MixLevel> lorosurmixlev;
    std::optional<int> lfemixlevcod;  // Table E1.3, 0..31
};

// One syncframe's rewritable metadata, and enough of its shape to know which
// of those fields exist. std::nullopt on any of dialnorm2/compr/compr2/bsmod/
// dsurmod means the frame does not transmit it.
struct FrameMetadata {
    StreamKind kind = StreamKind::kAc3;
    int bsid = 0;
    std::size_t bytes = 0;  // the whole syncframe, from its own size field
    SampleRate sample_rate = SampleRate::k48000;
    // E-AC-3 only. AC-3 reports strmtyp 0, substreamid 0 and numblkscod 3,
    // which is what an AC-3 syncframe amounts to in Annex E's vocabulary.
    int strmtyp = 0;
    int substreamid = 0;
    int numblkscod = 3;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;

    int dialnorm = 31;
    std::optional<std::uint8_t> compr;
    std::optional<int> dialnorm2;  // 1+1 dual mono only
    std::optional<std::uint8_t> compr2;
    std::optional<int> bsmod;
    std::optional<int> dsurmod;

    // AC-3's two bsi downmix levels (§5.4.2.4/§5.4.2.5), present only when
    // acmod brought the channels they describe.
    std::optional<meta::CentreMixLevel> cmixlev;
    std::optional<meta::SurroundMixLevel> surmixlev;
    // E-AC-3's richer group, when mixmdate was set. AC-3 never has one.
    std::optional<WireMixMetadata> mix;
};

// What to change. Every field is optional; an unset one is left exactly as it
// was, so an empty edit is a no-op that still re-stamps the CRCs identically.
struct MetadataEdit {
    std::optional<int> dialnorm;            // 1..31 (§5.4.2.8)
    std::optional<int> dialnorm2;           // 1..31, 1+1 only
    std::optional<std::uint8_t> compr;      // §7.7.2's 8-bit word, needs compre set
    std::optional<std::uint8_t> compr2;     // ditto, Ch2's own
    std::optional<int> bsmod;               // 0..7 (Table 5.5)
    std::optional<int> dsurmod;             // 0..3 (Table 5.11), acmod 2/0 only
};

// Reads one syncframe's metadata without changing anything. `frame` may be
// longer than the syncframe (the trailing bytes are ignored) but not shorter.
[[nodiscard]] AC3FORGE_EXPORT std::expected<FrameMetadata, EditError> read_frame_metadata(
    std::span<const std::byte> frame);

// Applies `edit` to one syncframe in place and re-stamps its CRC word(s).
// Returns the frame's metadata AFTER the edit. Nothing outside the named
// fields and the CRCs is touched: the audblks, the aux bits and every other
// bsi field keep their exact bits.
//
// Fails without modifying anything when a named field is not on the wire
// (kFieldAbsent) or a value is out of range (kOutOfRange) - a partially
// applied edit would leave a frame claiming metadata nobody asked for.
[[nodiscard]] AC3FORGE_EXPORT std::expected<FrameMetadata, EditError> edit_frame_metadata(
    std::span<std::byte> frame, const MetadataEdit& edit);

// Re-stamps crc1 (AC-3 only) and crc2 for one syncframe, for a caller that
// changed bsi bits itself. edit_frame_metadata already does this; this is
// exposed because the CRCs are the non-obvious half of any in-place rewrite
// and a caller doing its own (ac3::signing::sign_atmos_frame is the
// in-project precedent) should not have to reimplement crc1's solve.
[[nodiscard]] AC3FORGE_EXPORT std::expected<void, EditError> restamp_crc(
    std::span<std::byte> frame);

struct EditSummary {
    std::size_t syncframes = 0;
    // Syncframes whose bytes actually changed. An edit that asks for the
    // value a frame already carries leaves it identical, CRCs included.
    std::size_t changed = 0;
};

// Applies `edit` to every syncframe of a whole elementary stream, in place.
//
// Every substream's own dialnorm is rewritten, because Table E1.2 gives each
// one its own and a decoder reads the one belonging to whichever substream it
// is decoding. Every other field is applied to the syncframes that carry it
// and skipped on those that do not - compr and compr2 belong to the
// independent substream alone (§E3.8.5), dialnorm2 to a 1+1 programme,
// dsurmod to acmod 2/0 - so a stream whose substreams differ is rewritten
// correctly rather than refused.
//
// A field named in `edit` that NO syncframe in the stream carries is
// kFieldAbsent, checked before anything is written: the stream is either
// fully rewritten or left byte-for-byte alone, and a metadata option that
// silently did nothing is indistinguishable from one that does not work.
[[nodiscard]] AC3FORGE_EXPORT std::expected<EditSummary, EditError> edit_stream_metadata(
    std::span<std::byte> stream, const MetadataEdit& edit);

}  // namespace ac3::io
