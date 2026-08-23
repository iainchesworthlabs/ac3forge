#pragma once

#include <expected>
#include <string>

// AdmError lives in ac3adm.hpp, not model.hpp - model.hpp is only the parsed
// object graph (AdmModel/AdmDocument/ChnaEntry), and this header's own
// std::expected<..., AdmError> return types need the error enum itself.
// ac3adm.hpp already includes model.hpp, so this is the one include either
// way.
#include "ac3adm/ac3adm.hpp"

// The one BW64/RF64/RIFF shape the vendored libbw64 refuses outright:
// WAVE_FORMAT_IEEE_FLOAT in <fmt > (formatTag 3, or 0xFFFE EXTENSIBLE
// wrapping the IEEE-float SubFormat GUID). libbw64's parser.hpp
// parseFormatInfoChunk accepts formatTag 1 / EXTENSIBLE-wrapped PCM and
// throws on everything else, during readFile() itself - so a float master
// never reaches any of its accessors and there is nothing to widen from the
// outside.
//
// Rather than patch a fetched third-party dependency (src/ac3adm/
// CMakeLists.txt pulls libbw64 straight from github.com/ebu at a pinned tag),
// this walks the container for that one case: BS.2088-1 Annex 1's chunk list
// is a flat RIFF walk, <chna> is a fixed-width record table (§8.2), and
// <axml> is a byte range handed to the same libadm parse the ordinary path
// uses. The ADM metadata therefore goes through exactly the same code either
// way; only the PCM conversion differs, and only in which sample format it
// reads.
//
// Deliberately NOT a general second reader: an integer-PCM file still goes to
// libbw64, which remains the reference for everything this project does not
// have to special-case.

namespace ac3adm::detail {

// The <axml> XML text through libadm's parser and this module's own
// translation layer. Defined in adm.cpp beside the libbw64 path that shares
// it, so a float master's ADM metadata goes through the identical parse -
// see that function's own comment.
[[nodiscard]] std::expected<AdmModel, AdmError> parse_axml(const std::string& xml);

// True when `path` opens and its <fmt > chunk names WAVE_FORMAT_IEEE_FLOAT.
// False for anything else, an unreadable file included - the caller's next
// move on false is to hand the path to libbw64 exactly as before.
[[nodiscard]] bool is_ieee_float_wave(const std::string& path);

// The whole document, for a file is_ieee_float_wave() said yes to. Reports
// the specific AdmError the container walk found (kNotRiff/kMissingFmt/
// kMissingData/kUnsupportedFormat), which the libbw64 path cannot distinguish
// - see ac3adm.hpp's own comment on those four.
[[nodiscard]] std::expected<AdmDocument, AdmError> parse_float_pcm_bw64(const std::string& path);

}  // namespace ac3adm::detail
