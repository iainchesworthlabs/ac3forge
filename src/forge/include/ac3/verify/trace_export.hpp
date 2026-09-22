#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "ac3/export.hpp"
#include "ac3/verify/eac3_mirror.hpp"
#include "ac3/verify/mirror.hpp"

// Roadmap AP12: getting the encoder/decoder mirror trace (ac3/verify/mirror.hpp,
// ac3/verify/eac3_mirror.hpp) out in a form someone doing codec research can
// load into a notebook, not a new way to generate the data - see those headers
// for how a FrameTrace/Eac3AccessUnitTrace gets filled (DecoderConfig::trace/
// eac3_trace) and CONTRIBUTING.md's oracle table for what the trace is for.
//
// Per-frame bap, exponent, SNR-offset and masking-curve values, in TIDY (one
// observation per row) form rather than one cell per curve: `bap`/`exponent`
// are indexed per BIN, `mask` per Table 7.13 BAND - two different index
// spaces the trace itself never claims line up (see StreamTrace::mask's own
// comment) - and a tidy table sidesteps that by naming which is which per row
// (`kind`) instead of forcing both into one fixed-width record. `snr_offset`
// is a per-stream scalar, carried as one row with `index` 0. This is the
// schema both append_trace_csv and append_trace_json_lines write, so a
// caller mixing the two formats (or mixing AC-3 and E-AC-3 output - see
// `substream` below) gets one consistent table either way:
//
//   frame, substream, block, stream, kind, index, value
//
// `substream` is always 0 for AC-3 (FrameTrace has no substream layer) and
// the E-AC-3 access unit's substream POSITION for Eac3AccessUnitTrace -
// ac3::verify::Eac3Mismatch::substream's own convention, not the wire
// strmtyp/substreamid identity. `stream` is the internal numbering
// StreamTrace/Eac3StreamTrace already document: full-bandwidth channels
// first, then the LFE, then the coupling channel, when in use.
//
// A block the decoder never reached an allocation for (BlockTrace::allocated/
// Eac3BlockTrace::allocated clear - a refused or truncated frame) contributes
// no rows at all, rather than a row of zeros that would misread as a real
// silent measurement.
//
// No Parquet writer here: Python already has one (pyarrow/pandas), and
// pulling Arrow into this library for a research-only export path would cost
// every other consumer a dependency they have no use for. `append_trace_csv`
// or `append_trace_json_lines`, read into a DataFrame
// (`pandas.read_csv`/`pandas.read_json(lines=True)`), then `.to_parquet()` is
// how "reachable from Python" is met - see python/ for the binding that hands
// a caller these same strings without a C++ toolchain at all.
//
// Each function APPENDS (never clears `out` first), so a caller walking a
// whole file writes trace_csv_header() once and then one append call per
// frame, without holding the file in memory - the same incremental spirit
// DecoderConfig::trace/syntax already follow.

namespace ac3::verify {

// "frame,substream,block,stream,kind,index,value\n" - write this once, before
// the first append_trace_csv call, for a self-describing file.
[[nodiscard]] AC3FORGE_EXPORT std::string_view trace_csv_header();

AC3FORGE_EXPORT void append_trace_csv(const FrameTrace& trace, std::uint64_t frame_index,
                                      std::string& out);
AC3FORGE_EXPORT void append_trace_csv(const Eac3AccessUnitTrace& trace, std::uint64_t frame_index,
                                      std::string& out);

// JSON Lines (one compact object per row, same columns as the CSV form) -
// not one JSON document per file, so a caller can append and flush frame by
// frame the same way the CSV form does, without a closing bracket to manage.
AC3FORGE_EXPORT void append_trace_json_lines(const FrameTrace& trace, std::uint64_t frame_index,
                                             std::string& out);
AC3FORGE_EXPORT void append_trace_json_lines(const Eac3AccessUnitTrace& trace,
                                             std::uint64_t frame_index, std::string& out);

}  // namespace ac3::verify
