#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "ac3/export.hpp"
#include "ac3/oba/atmos.hpp"

// The OSC 1.0 wire form of a live scene update - the third reader of a
// per-object placement, beside the JSON and keyframe-text forms in scene.hpp
// (scene_json.cpp, scene.cpp's own scene_objects_from_keyframe_text). Where
// those two AUTHOR a timeline up front, this one feeds ac3::oba::SceneCursor
// while a session is running: parse one UDP datagram from a show-control rig
// or a DAW into zero or more updates, merge each onto the object's current
// placement, push the result.
//
// Deliberately narrow, per CONTRIBUTING.md's "say so" rule for behaviour
// short of the standard - this is not a general OSC client:
//   - UDP only. No SLIP/TCP framing, no OSC 1.1's mid-stream length prefix.
//   - Address patterns match LITERALLY. No '?'/'*'/'[]'/'{}' glob matching -
//     nothing a live position source needs sends a pattern, and implementing
//     the matcher is the single largest piece of an OSC 1.0 implementation.
//   - Argument types 'f' (float32) and 'i' (int32, widened losslessly to
//     double) only. 'd'/'s'/'b'/anything else drops the message.
//   - A message with no OSC Type Tag String (OSC 1.0 permits this for
//     pre-1.0 compatibility) drops rather than guessing argument types.
// No third-party OSC library (oscpack, liblo) is linked or consulted for
// this - the ROADMAP.md JSON-not-YAML argument applies verbatim: OSC 1.0's
// binary grammar is small enough to transcribe completely from the published
// specification (opensoundcontrol.org, "OSC 1.0 Specification"), so there is
// never a need to borrow an implementation of it.
//
// Untrusted input (docs/threat-model.md): this is the project's first
// network-facing parser. It cannot crash, read out of bounds, or loop
// unboundedly on any input - see parse_osc_packet's own comment for the
// specific bounds - and is covered by fuzz/fuzz_osc_parse.cpp accordingly.

namespace ac3::oba {

// One object's fields as one OSC message (or one bundle's worth of them)
// carried them. Per-field optional, DELIBERATELY: SceneCursor::push replaces
// a WHOLE ObjectPlacement (scene.cpp), so routing a position-only message
// through a default-constructed ObjectPlacement would silently reset gain to
// 1.0 and drop lfe_send/size/snap/zone - destroying whatever gain law the
// caller (ac3cli's, the GUI's) computes per frame. apply() below is how a
// caller turns this into a real placement without that trap.
struct SceneOscUpdate {
    std::size_t object = 0;
    std::optional<Position> position{};
    std::optional<double> gain{};      // linear, matching ObjectPlacement::gain - not dB
    std::optional<double> lfe_send{};
    // /object/<n>/release: hand this to SceneCursor::release(object) instead
    // of apply()/push() - there is no placement to merge, only a request to
    // stop overriding. Never set alongside position/gain/lfe_send.
    bool release = false;
};

// Malformed-input counters a caller may want for a status line. Never a
// reason to stop listening - see LivePositionSource, which is the only
// thing that owns a socket and therefore the only place a bad datagram from
// the network can otherwise do anything at all.
struct OscParseStats {
    // A whole datagram that was not "/..." or "#bundle\0...", or a bundle
    // element whose framing (a negative or non-multiple-of-4 int32 size, or
    // more bytes than remain) could not be trusted enough to keep reading.
    std::size_t packets_rejected = 0;
    // A recognised message shape (starts '/', has a Type Tag String) that
    // was still unusable: an unknown address, an argument count/type
    // mismatch, or a non-finite float/int argument.
    std::size_t messages_dropped = 0;
};

// Parses one OSC packet (the payload of one UDP datagram) into zero or more
// updates, in the order their messages appear - depth-first through any
// bundle nesting, sibling order preserved, TIME TAGS READ AND DISCARDED
// (SceneCursor is latest-value-wins with no interpolation - scene.hpp's own
// "The live half" comment - so only arrival order at the caller's drain
// matters, never an OSC timestamp). A bundle nested deeper than 8 levels is
// dropped at the point the cap is hit rather than walked further - no real
// console nests remotely that deep, and it is what keeps
// docs/threat-model.md's "no recursive descent anywhere in the parsers"
// true of an iterative, depth-capped walk rather than an exception to it.
//
// Never throws, never asserts on malformed input, never reads past `packet`.
// `stats`, if non-null, is incremented (not reset) for whatever this call
// found - a caller accumulating across many packets passes the same struct
// through repeatedly.
[[nodiscard]] AC3FORGE_EXPORT std::vector<SceneOscUpdate> parse_osc_packet(
    std::span<const std::byte> packet, OscParseStats* stats = nullptr);

// As parse_osc_packet, writing into caller-owned storage instead of
// allocating - the form LivePositionSource's drain path uses, since nothing
// on that path may allocate. Stops once `out` is full rather than continuing
// to parse messages it has nowhere to put; returns the count written (never
// more than out.size()). A single OSC packet carrying more live messages
// than `out` holds is not expected in practice - a fader move is one
// message - so stopping early rather than growing `out` costs nothing real.
[[nodiscard]] AC3FORGE_EXPORT std::size_t parse_osc_packet_into(
    std::span<const std::byte> packet, std::span<SceneOscUpdate> out,
    OscParseStats* stats = nullptr);

// Merges `update` onto `base`, returning the placement to push - or nullopt
// when `update` carries no position, meaning nothing is ready to push yet
// (a gain/lfe-only message arriving before this object's first position).
//
// `base`'s position is NEVER reused as the merged position, even when
// `update.position` is absent: ObjectScene::evaluate has ALREADY rotated the
// position it returns (scene.cpp), and SceneCursor::sample_into rotates a
// PUSHED placement's position again (scene.cpp) - so a position that has
// been rotated once must never be pushed, or it is rotated twice under any
// non-identity Orientation. The caller (LivePositionSource) is therefore
// responsible for remembering a gain/lfe-only update against this object and
// re-applying it once a position update finally arrives; apply() itself only
// ever merges the single update it was given.
//
// `base`'s gain/lfe_send/size/snap/zone/enable_elevation carry through
// unchanged for whichever fields `update` did not set - an object's authored
// gain automation keeps running underneath a network-driven position.
[[nodiscard]] AC3FORGE_EXPORT std::optional<ObjectPlacement> apply(
    const SceneOscUpdate& update, const ObjectPlacement& base);

}  // namespace ac3::oba
