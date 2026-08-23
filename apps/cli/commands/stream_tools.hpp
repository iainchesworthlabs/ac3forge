#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "../support.hpp"

// The stream tools (roadmap DC9): commands that operate on an ALREADY-encoded
// AC-3/E-AC-3 elementary stream rather than on PCM.
//
// Two of the three never touch the audio at all. 'metadata' and 'normalize'
// rewrite bsi fields in place and re-stamp the CRCs (ac3::io::metadata_edit),
// so a delivery correction costs no coding generation; 'cut' and 'cat' work
// on whole access units, so the bytes they keep are byte-for-byte the bytes
// that went in. 'transcode' is the one that does re-encode, because DD+ and
// DD are different codecs and nothing else can bridge them - it carries the
// metadata across rather than resetting it.
//
// Split into its own file rather than added to decode.hpp/containers.hpp for
// the reason the H4 monolith split gives generally: these five share a
// subject (an encoded stream in, an encoded stream out) that none of the
// existing groups has.
namespace ac3cli::commands {

// Decode and re-encode, preserving dialnorm, DRC and the mix metadata the
// target codec has room for. The output codec comes from out_path's suffix
// (.ac3/.ec3), or from codec= when the suffix cannot say (stdout, "-").
int run_transcode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                  std::string_view layout, const ac3cli::Options& meta);

// Rewrite dialnorm/compr/bsmod/dsurmod on an existing stream, audio
// untouched. The values come from the same metadata options every encoding
// command takes.
int run_metadata(std::string_view in_path, std::string_view out_path,
                 const ac3cli::Options& meta);

// The measurement-driven special case of the above: decode to measure
// BS.1770-4 integrated loudness, derive dialnorm from it (ATSC A/85 §8), and
// rewrite that one field. Nothing else changes.
int run_normalize(std::string_view in_path, std::string_view out_path,
                  const ac3cli::Options& meta);

// A frame-aligned extract. `start_seconds`/`duration_seconds` are snapped to
// access-unit boundaries - an E-AC-3 access unit being an independent
// substream plus its dependents, never one syncframe.
int run_cut(std::string_view in_path, std::string_view out_path, std::string_view start_seconds,
            std::string_view duration_seconds);

// Concatenation, output path FIRST because the input list is variadic.
// Refuses inputs whose codec, sample rate, layout or substream structure
// differ - a decoder cannot follow a stream that changes those mid-file.
int run_cat(std::string_view out_path, std::span<const std::string_view> in_paths);

}  // namespace ac3cli::commands
