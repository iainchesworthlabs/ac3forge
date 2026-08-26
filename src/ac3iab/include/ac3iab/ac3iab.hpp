#pragma once

#include <cstdint>
#include <expected>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3iab/export.hpp"
#include "ac3iab/model.hpp"

// Top-level entry points for ac3iab::ac3iab: parses SMPTE ST 2098-2:2022's Immersive Audio
// Bitstream into an IaFrame per frame (see model.hpp for the full element graph).
//
// Roadmap item IM1 phase 1 of 3 (see ROADMAP.md's "IAB (SMPTE ST 2098-2) reader" entry): a
// standalone bitstream reader, the "codec-blind" shape matroska::matroska, mp4::mp4 and
// mpegts::mpegts already use for their own containers (bare `include/ac3iab/` prefix, not
// `ac3/ac3iab/` - see CONTRIBUTING.md's repository-layout section on what that prefix means).
// AudioDataDLC's lossless coder (Annex B) is read only by identity in this phase, not decoded
// - see model.hpp's AudioDataDlc comment. Phase 2 (MXF/KLV extraction for IAB track files - see
// mxf.hpp) is implemented alongside this header. Phase 3 (mapping onto ac3::admbridge's
// ObjectPath layer, the `atmos-iab` CLI command) is separate, later work.
//
// Every table and algorithm this module implements is transcribed directly from the published
// standard (SMPTE ST 2098-2:2022, a free PDF since SMPTE opened its catalogue on 2026-06-17),
// with the section/table number cited at each call site, per CONTRIBUTING.md's clean-room
// rule. DTSProAudio/iab-validator (MIT) was consulted only as an external oracle to check this
// reader's output against a second, independent implementation - never as a source to
// transcribe from.

namespace ac3iab {

enum class IabError : std::uint8_t {
    kCannotOpen,          // path could not be opened for reading
    kTruncated,            // fewer bytes remained than a field, ElementSize, DLCSize,
                           // PreambleLength or IAFrameLength declared
    kBadEscape,             // a Plex(n) escape chain (§5.2) grew past this format's own 32-bit
                           // ceiling (symbols are guaranteed <= 0xFFFFFFFE) without terminating -
                           // either a corrupt stream or one using the reserved 0xFFFFFFFF value
    kBadPreambleTag,        // PreambleTag != 0x01 (§8.1.1)
    kBadFrameTag,           // IAFrameTag != 0x02 (§8.1.4)
    kReservedVersion,       // IAFrame.Version is not 1 - 0 and 2 are explicitly forbidden (§10.2.1)
    kReservedSampleRate,    // SampleRate code 0x2/0x3 (§10.2.2, Table 15 - Reserved)
    kReservedBitDepth,      // BitDepth code 0x2/0x3 (§10.2.3, Table 16 - Reserved)
    kReservedFrameRate,     // FrameRate code 0xA-0xF (§10.2.4, Table 17 - Reserved)
    kUnterminatedString,    // a NUL-terminated ASCII field (AudioDescriptionText,
                           // AuthoringToolURI) ran off the end of its element without a
                           // terminating 0x00 byte
    kMxfBadKlv,             // mxf.hpp: a KLV Length field violated SMPTE ST 336:2017's BER
                           // encoding rules (the reserved 0x80 "indefinite length" token, or a
                           // long form needing more than the 8 following bytes SMPTE ST 377-1's
                           // 9-byte-total cap on a KLV Length field allows)
    kMxfNoIabEssence,       // mxf.hpp: walked every top-level KLV in the file without finding one
                           // whose Key matched SMPTE ST 2067-201 Table 4.2's registered IAB
                           // Essence Element Key
};

[[nodiscard]] AC3IAB_EXPORT std::string_view describe(IabError error);

// §7 Table 2, §8.1: one IABitstream frame - the Preamble segment (opaque; §8.1.3 says its
// content is "outside the scope of this specification") plus the decoded IAFrame that follows
// it.
struct IABitstreamFrame {
    std::vector<std::byte> preamble;
    IaFrame frame;
};

// Parses a whole IABitstream (§7): a sequential run of Preamble+IAFrame segment pairs read
// until end of file/stream, per Table 2's `while(true)` framing. This is the container an
// elementary `.iab` file actually uses; it is also what mxf.hpp's parse_mxf_iab() hands this
// exact function (the istream overload) once it has stripped an IAB track file's own KLV
// wrapper away - SMPTE ST 2067-201 clip-wraps the whole IABitstream as a single Generic
// Container KLV Value, so there is no reframing to do, only extraction (see mxf.hpp's own
// header comment).
[[nodiscard]] AC3IAB_EXPORT std::expected<std::vector<IABitstreamFrame>, IabError> parse_iabitstream(
    const std::string& path);
[[nodiscard]] AC3IAB_EXPORT std::expected<std::vector<IABitstreamFrame>, IabError> parse_iabitstream(
    std::istream& in);

// Parses a single already-extracted IAFrame element's payload (§9.1 Table 5) directly -
// `payload` is exactly the element's own ElementSize bytes, with no ElementID/ElementSize
// header of its own (that header belongs to the IAElement wrapper one level up, per §9's own
// IAElement/IAFrame split). This is the lower-level entry point parse_iabitstream() above
// uses internally once it has stripped the IAFrameTag/IAFrameLength TLV wrapper (§8.1.4-6);
// exposed directly since it is what a caller already holding one extracted frame - a
// synthetic test vector, or a future MXF track reader - actually has in hand.
[[nodiscard]] AC3IAB_EXPORT std::expected<IaFrame, IabError> parse_iaframe(std::span<const std::byte> payload);

}  // namespace ac3iab
