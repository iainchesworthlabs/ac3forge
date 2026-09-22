#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "ac3iab/ac3iab.hpp"
#include "ac3iab/export.hpp"

// Roadmap item IM1 phase 2 of 3 (see ROADMAP.md): minimal SMPTE ST 336:2017 KLV extraction for an
// IAB Track File, the way a real IMF/Dolby Atmos cinema master actually delivers ST 2098-2's
// Immersive Audio Bitstream - a bare elementary `.iab` file is the exception, not the rule.
//
// SMPTE ST 2098-2 itself has no MXF content at all; the wrapping is defined by a separate, much
// shorter standard, SMPTE ST 2067-201:2021 ("IMF - Immersive Audio Bitstream Level 0 Plug-in"),
// which in turn references the base MXF standards (ST 377-1 file format, ST 379-1/-2 Generic/
// Constrained Generic Container, ST 336 KLV/BER encoding). Every citation in mxf_reader.cpp names
// one of those five documents' own clause/table numbers - all five are free from
// https://pub.smpte.org, confirming ROADMAP.md's claim.
//
// The one fact that makes this "minimal" rather than "a general MXF library": ST 2067-201 §5.5
// clip-wraps the Immersive Audio Bitstream - "the entire duration of the essence container shall
// be contained within a single Content Package... comprised of a single KLV" (ST 379-2 §8.4.2).
// An IAB Track File's audio essence therefore lives in exactly ONE Generic Container KLV triplet,
// and that KLV's Value is byte-identical to ST 2098-2 Clause 7's IABitstream syntax - the same
// `while(true){Preamble;IAFrame;}` run an elementary `.iab` file already has. So this reader does
// not need Index Tables (legally absent per ST 377-1 §11.5.3, and only useful for random-access
// seeking inside that one KLV even when present - ST 379-2 §8.4.4 Table 1), a System Item (never
// required by ST 2067-201), or any of Header Metadata's Preface/ContentStorage/Package object
// graph (locating essence is a KLV-Key matter, not an object-graph one) - it only has to walk
// top-level KLV triplets from the start of the file, skip everything whose Key does not match ST
// 2067-201 Table 4.2's registered IAB Essence Element Key, and hand the one KLV that does match
// straight to ac3iab::parse_iabitstream(std::istream&) unmodified (see that function's own updated
// doc comment in ac3iab.hpp) - zero duplication of the Preamble/IAFrame framing logic phase 1
// already implements.
//
// Deliberately out of scope, since ST 2067-201 §5.3-5.5 already constrains a compliant IAB Track
// File to exactly one Essence Track and one Sound Element, closing off most of what ST 377-1
// otherwise permits: multi-Package Header Metadata graphs, Operational Pattern logic, external or
// segmented essence (BodySID = 0), the Footer Partition's repeated Header Metadata, and a nonzero
// Run-In before the Header Partition Pack (ST 377-1 §7.2.1's own "default case of a Run-In
// sequence length of zero" is assumed; a file that does not open with a KLV Key at byte 0 is
// reported as kMxfBadKlv rather than scanned for one, matching this reader's "minimal" scope).
//
// Consulted DTSProAudio/iab-validator (MIT) as an external oracle per usual - it turned out to
// have no MXF-related code or sample .mxf files at all, so there was nothing to cross-check this
// specific piece against beyond one corroborating doc comment (IABParserAPI.h, on its own
// no-stream frame-buffer entry point: "A typical application example is from MXF-unwrapped
// frames") confirming the same "external MXF-unwrap hands a frame buffer to the existing parser"
// split this reader uses.

namespace ac3iab {

// Reads an IAB Track File (SMPTE ST 2067-201) and returns the same IABitstreamFrame sequence
// parse_iabitstream() returns for a bare elementary `.iab` file - see this header's own top
// comment for why the two converge on identical output. kMxfNoIabEssence: no top-level KLV's Key
// matched ST 2067-201 Table 4.2's registered value. kMxfBadKlv: a Length field violated ST 336's
// BER encoding rules. kTruncated: fewer bytes remained than a Key or a declared Length/Value
// needed.
[[nodiscard]] AC3IAB_EXPORT std::expected<std::vector<IABitstreamFrame>, IabError> parse_mxf_iab(
    const std::string& path);
[[nodiscard]] AC3IAB_EXPORT std::expected<std::vector<IABitstreamFrame>, IabError> parse_mxf_iab(
    std::istream& in);

}  // namespace ac3iab
