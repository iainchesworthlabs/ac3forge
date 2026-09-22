#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/decoder/decoder.hpp"

// Two things a player that decodes a whole elementary stream has to get right
// besides calling the decoders: which decoder reads the stream, and what
// becomes of the audio the E-AC-3 one is still holding when the stream ends.
// ac3cli's 'monitor' and 'spatial' use both. Compiled straight into ac3cli
// and ac3tests, the way container_input.cpp beside it is (see
// recording_sink.hpp for why apps/common has no library target), and kept
// out of apps/cli so a test can hold both without a render device - neither
// command gets past opening one on a headless CI leg.

namespace ac3::apps {

// True when `stream` is read an access unit at a time (split_access_units and
// Eac3Decoder); false when FrameDecoder reads it a frame at a time, which is
// plain AC-3, and for a stream too short to hold a syncframe (the caller's own
// stream_bsid() check reports that). bsid alone does not decide it: A/52
// §E2.3.1.2's legacy-core delivery opens with an AC-3 syncframe and carries
// Annex E dependents behind it, and FrameDecoder refuses the first dependent
// it reaches. 'ac3cli decode' makes the same test; see
// ac3::has_eac3_extension_substreams.
[[nodiscard]] bool reads_as_access_units(std::span<const std::byte> stream);

// Eac3Decoder::flush()'s substreams as one access unit, laid out the way
// decode_access_unit lays one out, or std::nullopt when nothing was held
// back. flush() releases what §3.7's transient pre-noise processing still
// holds when a stream ends - its last unit, whenever the stream's last frames
// used the tool - and releases it as raw substreams, because that unit never
// assembled. A player that stops at its last decode_access_unit call never
// plays it.
//
// `programme` is the layout the units before this one rendered, so this
// unit's slots line up with theirs even where it lacks a substream they had
// (that slot is silent). std::nullopt means no unit came out at all; the
// layout is then the union of the flushed substreams' own, as §E3.8.2 builds
// it. `folded` is whether the decoder's DecoderConfig::output folds, which
// flush() has already applied to every substream it returns.
//
// The assembly is decode_access_unit's, with three cases that flush()'s
// output needs spelled out:
// - flush() returns held frames before released ones, so a legacy core,
//   which is never held itself, comes out after the dependent that held its
//   unit back. §E3.8.2 is order-sensitive - a dependent's channel replaces
//   the bed's at the same location - so the bed is laid down first and every
//   dependent over it, in whatever order flush() returned them.
// - Under a fold, flush() has folded each substream on its own, so a
//   dependent's two channels are a fold of its own channels alone. The bed's
//   fold is the programme's compatible rendering (what a decoder that reads
//   no dependents plays), so the unit carries that, and no dependent.
// - Dual mono is one substream whose two channels have no location, so they
//   stay in coded order and `layout` stays empty, as decode_access_unit
//   leaves it.
//
// A flushed dependent with no bed beside it has nothing to extend, and gives
// std::nullopt.
[[nodiscard]] std::optional<ac3::DecodedAccessUnit> held_back_unit(
    std::vector<ac3::DecodedSubstream> flushed,
    const std::optional<ac3::eac3::chanmap::Layout>& programme, bool folded);

}  // namespace ac3::apps
