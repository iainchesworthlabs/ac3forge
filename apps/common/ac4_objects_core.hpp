#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/encoder/assignment.hpp"
#include "iclforge/ac3/oba/atmos.hpp"
#include "iclforge/objects/scene.hpp"
#include "iclforge/ac4/ac4.hpp"
#include "iclforge/ac4enc/encoder.hpp"

// What forge's `atmos-encode codec=ac4` and `atmos-adm`/`atmos-iab` with
// codec=ac4 and forge-gui's AC-4 objects share, so that the command line the GUI
// echoes writes the same bytes the GUI does: which channels become which
// objects, where a channel pinned to a speaker sits, the audio each object
// carries, the metadata updates one a frame, and the call into E9's writer.
// Compiled straight into each application, as the rest of apps/common is
// (recording_sink.hpp says why there is no library target).

namespace iclforge::apps {

// --- Which channels are objects ---------------------------------------------

// One dynamic object's source taps: (flattened source channel, linear gain).
// The flattened space concatenates every source's channels in load order -
// source 0's first, then source 1's - which is the same numbering gather_frame()
// fills and the same one `live`'s two capture devices use.
//
// One tap is a plain `obj` row. Several are `objm`: a contiguous range of ONE
// source's channels folded to a single mono object, each tap already scaled by
// 1/n so several full-range channels summed together do not clip past what one
// alone would (iclforge::plan::DestinationKind::kObjectMono's own contract). A slot
// with no taps is allocated but unbound, and carried silent - the state
// `live objects=<N>` leaves a slot in when nothing is mapped onto it.
struct ObjectSlot {
    std::vector<std::pair<std::size_t, double>> taps;
};

// The object slots a map= assignment describes, over `shapes`' flattened
// channel space: every `obj` row its own slot first, in (source, channel)
// order, then each maximal contiguous run of `objm` rows within one source
// folded to one. Empty when the assignment names no object destination at all
// - which is a real answer (a purely location-mapped assignment), not an
// error, so the caller decides what to do about it.
//
// Shared by `atmos-encode`, `live mode=atmos` and the AC-4 objects below so
// that the objects a given map= produces are the same objects every way - a
// GUI assignment reproduced headlessly has to reproduce.
[[nodiscard]] std::vector<ObjectSlot> object_slots_from_assignment(
    const iclforge::plan::Assignment& assignment,
    std::span<const iclforge::plan::SourceShape> shapes);

// Where a Table E2.5 location sits on the soundfield plans, degrees CCW from
// the front: the ITU-R BS.775 ring's five positions (L +30, C 0, R -30, Ls +110,
// Rs -110) extended to the wider set of channels the general channel model can
// carry. Nothing for the LFEs, which have no direction. A convention the GUI's
// soundfield ring and the AC-4 pins both read.
[[nodiscard]] std::optional<double> location_azimuth_deg(
    iclforge::eac3::chanmap::Location location);

// --- AC-4 objects -----------------------------------------------------------

// The object substream is frame_rate_index 13 only (ac4enc/encoder.hpp,
// SubstreamConfig::objects): 2 048 samples a frame, one metadata update a
// frame.
inline constexpr int kAc4ObjectFrameRateIndex = 13;
inline constexpr std::int64_t kAc4ObjectFrameSamples = 2048;
// "more than 64 objects" (ac4enc's object_layout_of): the decoder keeps at most
// this many in one portion (src/ac4dec/ERRATA.md).
inline constexpr std::size_t kAc4MaxObjects = 64;
// The rates an object substream is written at.
[[nodiscard]] bool ac4_objects_take_rate(std::uint32_t sample_rate_hz);

// One AC-4 object of a scene, in the stream's order: what feeds it, and what
// kind it is. `atmos-encode codec=ac4` and the GUI make the same list of an
// assignment.
struct Ac4ObjectSlot {
    enum class Kind : std::uint8_t {
        // A dynamic object: `obj`, or an `objm` fold, driven by its own path.
        kDynamic,
        // A channel assigned to a speaker: a dynamic object held at the
        // speaker's place on the ring, at unity, as atmos-adm gives a bed
        // channel.
        kPinned,
        // A channel assigned to an LFE: AC-4's LFE object, at most one.
        kLfe,
    };
    Kind kind = Kind::kDynamic;
    std::vector<std::pair<std::size_t, double>> taps;
    // kPinned: the speaker's azimuth, degrees CCW from the front.
    double azimuth_deg = 0.0;
};

// The stream's objects for `assignment`: the dynamic objects first, in
// object_slots_from_assignment's order, then a channel assigned to a speaker
// each (in source, channel order), then an LFE's. A row's trim is its tap's
// gain.
[[nodiscard]] std::vector<Ac4ObjectSlot> ac4_object_slots(
    const iclforge::plan::Assignment& assignment,
    std::span<const iclforge::plan::SourceShape> shapes);

// Where a channel pinned to a speaker sits: on the ring of radius 0.5 about the
// room's centre, at the speaker's azimuth, the place ADM's polar coordinates
// give a bed channel (iclforge::admbridge), so that a pinned channel and an ADM bed
// channel come out at one position.
[[nodiscard]] iclforge::oba::Position ac4_pin_position(double azimuth_deg);

// One source file's channels as the encoder's flat input: `offset_samples` of
// leading silence, then its samples.
struct Ac4SourceView {
    std::span<const std::vector<float>> channels;
    std::size_t offset_samples = 0;
};

// Every source's channels concatenated in load order, each with its source's
// offset ahead of it and zeros after its end, all the length of the longest
// (offset and samples together). Empty for no sources.
[[nodiscard]] std::vector<std::vector<float>> ac4_flat_planes(
    std::span<const Ac4SourceView> sources);

// The audio each slot carries: its taps' channels summed, each scaled by its
// gain, in the order the slot lists them, in float as the encoders' own mixes
// are.
[[nodiscard]] std::vector<std::vector<float>> ac4_object_planes(
    std::span<const Ac4ObjectSlot> slots, std::span<const std::vector<float>> flat_planes);

// An object's ObjectProperties for a placement: the position unconverted, since
// iclforge::oba::Position and TS 103 190-2 Annex F share one room (X from the left
// wall to the right, Y from the front to the back, Z from the floor to the
// ceiling), and the gain in dB, where the placement's is linear (-infinity for
// none). The LFE send has no AC-4 counterpart and is dropped.
[[nodiscard]] iclforge::ac4::ObjectProperties ac4_object_properties(
    const iclforge::oba::ObjectPlacement& p);

// Every object's placement at a time, in the stream's order.
using Ac4Placements = std::function<std::vector<iclforge::oba::ObjectPlacement>(double time_s)>;

// What E9's writer is given besides the audio and the metadata.
struct Ac4ObjectsParams {
    std::uint32_t sample_rate_hz = 48000;
    int bitrate_kbps = 384;
    // The dialogue level in dB below full scale; the encoder takes its negative.
    double dialnorm_db = 31.0;
    iclforge::ac4::ObjectCoding coding = iclforge::ac4::ObjectCoding::kAjoc;
};

// The configuration E9's writer takes for `objects` objects, `lfe` marking the
// LFE's, each at `initial`: the one encode_ac4_objects encodes with, and what
// Encoder::refusal_reason() is asked about before anything is read.
[[nodiscard]] iclforge::ac4::EncoderConfig ac4_objects_config(
    const Ac4ObjectsParams& params, const std::vector<bool>& lfe,
    std::span<const iclforge::oba::ObjectPlacement> initial);

// Why an object encode of `slots` at `params` cannot be written, in the terms
// the page and the command both use: what is known from the counts, the rate
// and the coding without reading any audio, before the encoder's own refusal
// (iclforge::ac4::Encoder::refusal_reason). Nothing where it can.
[[nodiscard]] std::optional<std::string> ac4_objects_refusal(std::span<const Ac4ObjectSlot> slots,
                                                             const Ac4ObjectsParams& params);

struct Ac4ObjectsError {
    enum class Kind : std::uint8_t {
        // The configuration is one the encoder refuses (forge's usage error).
        kRefused,
        // encode() or flush() failed on its input.
        kEncode,
        kFlush,
    };
    Kind kind = Kind::kRefused;
    std::string message;
};

struct Ac4ObjectsEncoded {
    std::vector<iclforge::ac4::EncodedFrame> frames;
    iclforge::ac4::Toc toc;
    // Where an input sample comes out of the decoder: the encoder's delay and
    // the decoder's.
    int lag_samples = 0;
};

// The steps `atmos-adm codec=ac4` took, for any objects: one metadata update per
// object a frame, ramped over the whole frame from the last and evaluated at
// the frame's end (the convention every Atmos encode in this repository uses),
// then E9's writer over the objects' audio (`pcm`, one equal-length span an
// object). `lfe` marks the LFE object, and may be shorter than `pcm` or empty
// where there is none.
[[nodiscard]] std::expected<Ac4ObjectsEncoded, Ac4ObjectsError> encode_ac4_objects(
    const Ac4ObjectsParams& params, const std::vector<bool>& lfe,
    std::span<const std::span<const float>> pcm, const Ac4Placements& placements);

// The same for a scene of slots: the audio the slots' taps make of `flat_planes`,
// and the placements `motion` gives the dynamic objects, in slot order, with a
// pinned channel held at its ring position and the LFE at the room's centre.
[[nodiscard]] std::expected<Ac4ObjectsEncoded, Ac4ObjectsError> encode_ac4_scene(
    const Ac4ObjectsParams& params, std::span<const Ac4ObjectSlot> slots,
    std::span<const std::vector<float>> flat_planes, const iclforge::oba::ObjectScene& motion);

// The placements of a scene of slots at a time: the form encode_ac4_scene
// hands encode_ac4_objects, for a caller that wants the initial values.
[[nodiscard]] std::vector<iclforge::oba::ObjectPlacement> ac4_scene_placements(
    std::span<const Ac4ObjectSlot> slots, const iclforge::oba::ObjectScene& motion, double time_s);

}  // namespace iclforge::apps
