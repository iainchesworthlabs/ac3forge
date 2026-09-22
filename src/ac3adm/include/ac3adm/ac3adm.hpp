#pragma once

#include <cstdint>
#include <expected>
#include <iosfwd>
#include <string>
#include <string_view>

#include "ac3adm/export.hpp"
#include "ac3adm/model.hpp"

// Top-level entry point for ac3adm::ac3adm: parses a BW64/RF64 (or plain,
// sub-4 GB RIFF/WAVE carrying the same chunks) file into an AdmDocument -
// the ADM object graph plus the <chna> join table plus the decoded PCM.
//
// Roadmap item B1 phase 1 of 3 ('s "ADM BWF reader feeding the
// JOC encoder" entry): this module knows nothing about AC-3, E-AC-3 or the
// JOC/Atmos object layer - it is a container + XML metadata reader only, the
// same "codec-blind" shape as matroska::matroska, mp4::mp4 and
// mpegts::mpegts. Mapping the parsed graph onto ac3::oba::AtmosEncoder is a
// separate, later task (phase 2); a worked end-to-end example is phase 3.
//
// Implemented on top of two vendored third-party libraries rather than a
// hand-rolled parser (see src/ac3adm/CMakeLists.txt for how they're
// fetched): libbw64 (github.com/ebu/libbw64, Apache-2.0, header-only) for
// the container layer, and libadm (github.com/ebu/libadm, Apache-2.0) for
// the ADM XML object model. Both are maintained by the same BBC/IRT team
// that authored the underlying ITU-R Recommendations themselves. Neither
// library's own types appear in this header or model.hpp - see model.hpp's
// header comment for why (the namespace collision libadm's own `adm::`
// would otherwise cause is the main reason).
//
// Primary sources these two libraries themselves implement, and that this
// module's own translation code (src/ac3adm/src/adm_model.cpp) cites where
// it makes a choice beyond "trust the library"):
//   - Recommendation ITU-R BS.2088-1 (10/2019), Annex 1: the BW64 container
//     - <ds64>, <fmt>, <data>, <chna>, <axml> chunk layouts.
//   - Recommendation ITU-R BS.2076-2 (10/2019), Annex 1: the Audio
//     Definition Model XML schema (audioProgramme/audioContent/audioObject/
//     audioPackFormat/audioChannelFormat/audioBlockFormat/audioStreamFormat/
//     audioTrackFormat/audioTrackUID) and its ID/coordinate/time-format
//     conventions.

namespace ac3adm {

enum class AdmError : std::uint8_t {
    kCannotOpen,        // path could not be opened for reading, OR libbw64 opened it and then
                         // rejected it (not RIFF at all, a missing mandatory chunk, an
                         // unsupported <fmt > format) - all reported through the same untyped
                         // exception family, with no distinguishing type this module could map
                         // from, so every case in that family collapses to kCannotOpen rather
                         // than inventing false precision - see adm.cpp's own comment on
                         // parse_bw64_path
    kNotRiff,            // the file's chunk table is internally inconsistent - a chunk other
                         // than <data> declaring more bytes than the file contains, which
                         // adm.cpp's chunk_sizes_fit() refuses before libbw64 can allocate from
                         // that number
    kMalformedXml,       // <axml> content failed to parse: genuinely malformed XML (an
                         // unterminated tag, say), OR well-formed XML missing a mandatory ADM
                         // attribute/element - libadm's own parser reports both through the same
                         // plain, untyped std::runtime_error (confirmed directly: a missing
                         // audioObjectID surfaces exactly this way, not as one of libadm's own
                         // typed exceptions), so this module cannot reliably tell them apart
                         // and does not claim to.
    kMalformedAdm,       // libadm rejected the document with one of its own typed
                         // adm::error::AdmException diagnostics - duplicate IDs, an unresolved
                         // reference, an invalid enumerated value, the audioFormatExtended root
                         // not found, ...
    kOther,              // any other failure surfaced by libbw64/libadm; see the exception message
                         // this can't carry - kept broad deliberately since neither library's
                         // own exception hierarchy is exposed through this API (see this
                         // header's own top comment on why not).
};

[[nodiscard]] AC3ADM_EXPORT std::string_view describe(AdmError error);

// Parses a whole BW64/RF64/RIFF file from `path`.
[[nodiscard]] AC3ADM_EXPORT std::expected<AdmDocument, AdmError> parse_bw64(const std::string& path);

// Same parse, from an already-open stream - e.g. an in-memory buffer via
// std::istringstream for testing without touching a disk. Implemented by
// spooling the stream to a temporary file and delegating to the path
// overload: libbw64's own reader opens a file by path internally (it has no
// istream constructor), so there is no way to hand it an in-memory buffer
// directly.
[[nodiscard]] AC3ADM_EXPORT std::expected<AdmDocument, AdmError> parse_bw64(std::istream& in);

// Roadmap item IM2 ("JOC -> ADM BWF writer"): the write-side counterpart of parse_bw64, using the
// same two vendored libraries in the other direction - libadm's document-builder API
// (adm::AudioObject::create() and friends, see src/ac3adm/src/adm_model.cpp) to turn an AdmModel
// into a libadm adm::Document, adm::writeXml() to serialize it, and libbw64's Bw64Writer
// (bw64::writeFile()) to write the BW64 container (<fmt >, <chna>, <axml>, <data>).
enum class AdmWriteError : std::uint8_t {
    kInvalidDocument,  // an AdmModel cross-reference (a *_refs entry, or an AdmDocument::chna
                       // entry's uid) did not resolve to another element `document` itself
                       // carries, or named an element type this writer does not support (Matrix/
                       // HOA/Binaural channel/pack formats, nested audioObject/audioPackFormat
                       // references, a block whose `position` is polar rather than cartesian -
                       // this writer only emits the Dolby Atmos Master ADM Profile's cartesian
                       // shape). A caller bug, not a hostile-input case: unlike parse_bw64's
                       // AdmError, nothing here comes from an untrusted file.
    kCannotOpen,       // the output path could not be opened for writing
    kOther,            // any other failure surfaced by libbw64/libadm; see the exception message
                       // this can't carry - kept broad deliberately, same reasoning as AdmError::
                       // kOther above.
};

[[nodiscard]] AC3ADM_EXPORT std::string_view describe(AdmWriteError error);

// Writes `document` to `path` as a BW64 file carrying an <axml> chunk (the ADM XML built from
// `document.model`), a <chna> chunk (from `document.chna`) and the interleaved PCM `document.audio`
// carries, at 24-bit PCM - the bit depth real-world ADM BWF masters (EBU Tech 3306) almost always
// use, and the only integer width libbw64's own FormatInfoChunk accepts alongside 16/32 (it has no
// IEEE-float write path at all, matching its own read-side refusal - see model.hpp's PcmAudio
// comment).
//
// One asymmetry from the read side, worth stating plainly: `document.model`'s own ID strings
// (AudioObject::id, AudioPackFormat::id, ChnaEntry::uid, ...) are used here ONLY as correlation
// keys while this function wires the object graph together (matching a *_refs entry back to the
// element it names) - they never appear literally in the written file. Real, BS.2076-2-formatted
// IDs are assigned by libadm's own adm::reassignIds() once the whole graph is built, and it is
// THOSE that end up in the XML and in <chna>'s own AudioId rows. A caller building the AdmModel to
// pass here is therefore free to use any unique, stable strings it likes for `id`/`uid` fields -
// "obj0", "pack3", whatever is convenient - not just the "AO_1001"-style strings parse_bw64 itself
// produces. `ChnaEntry::track_ref`/`pack_ref` are read-path-only fields for this same reason: this
// function derives the real trackRef/packRef strings itself from the resolved AudioTrackUid's own
// references, so a caller populating an AdmDocument purely to write it may leave both empty.
[[nodiscard]] AC3ADM_EXPORT std::expected<void, AdmWriteError> write_bw64(const std::string& path,
                                                                          const AdmDocument& document);

}  // namespace ac3adm
