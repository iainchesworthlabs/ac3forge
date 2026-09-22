#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/encoder/plan.hpp"
#include "ac3/io/elementary.hpp"
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

// The whole input, scanned. Every command in this file needs the same two
// things first - the bytes, and what the bitstream says it is - and reports
// the same two failures, so this is read once and shared rather than each
// command re-reading and re-scanning its own file. 'play' (roadmap UX9)
// shares it too, for its sink-following fallback - see decode_and_render.
struct LoadedStream {
    std::vector<std::byte> bytes;
    ac3::io::ScannedStream scan;
};

// Reads the whole file at `path` and scans it, reporting either failure
// (unreadable, or not a scannable AC-3/E-AC-3 elementary stream) to stderr
// itself.
[[nodiscard]] std::optional<LoadedStream> load_stream(std::string_view path);

// Tallies decode_and_render() hands back for a caller's own status line -
// run_transcode's only consumer today, kept as an out-of-band return rather
// than folded into on_frame() so a caller that does not want them ('play's
// sink-following fallback) is not forced to thread a status stream through
// just to discard the printf.
struct DecodeRenderStats {
    std::size_t units_in = 0;
    double dynrng_min_db = 0.0;
    double dynrng_max_db = 0.0;
    std::size_t dynrng_words = 0;
};

// Decodes the whole of `loaded` and renders it through `routing` into
// complete ac3::kSamplesPerFrame blocks (the last one held-sample-padded, not
// silence-dropped - see SampleQueue::take in the .cpp), calling `on_frame`
// for each one. `coded_channels` sizes the buffers `on_frame` receives -
// independent of the SOURCE's own channel count, which this reads off
// `loaded.scan` itself.
//
// `on_frame` does whatever the caller wants with the rendered PCM - encode
// it (run_transcode), re-encode and passthrough it, or decode straight to
// MonitorSink ('play', roadmap UX9) - and returns false to abort, already
// reported by then; this just unwinds without reporting anything further.
// `on_abort` is called instead for the DECODE side's own failure (a
// mid-stream channel count change no one routing can describe), before
// `on_frame` would run again for it - the default no-op suits a caller with
// nothing partially written that needs unwinding, unlike run_transcode's own
// EncodedStreamSink.
//
// Returns nullopt on any failure (already reported to stderr, from here or
// from `on_frame`/`on_abort`'s own caller).
[[nodiscard]] std::optional<DecodeRenderStats> decode_and_render(
    std::string_view in_path, const LoadedStream& loaded, const ac3::plan::Routing& routing,
    std::size_t coded_channels,
    const std::function<bool(std::span<const std::span<const float>>)>& on_frame,
    const std::function<void()>& on_abort = [] {});

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
