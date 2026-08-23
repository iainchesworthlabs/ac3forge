#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/export.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/oamd.hpp"

// An object scene: what is in the room, where each thing goes, and when.
//
// AtmosEncoder::encode_frame takes one ObjectPlacement per object per frame
// and nothing more, so every caller that wanted a SCENE - objects with names,
// a bed assignment, automation, a file it can be saved to and reloaded from -
// built its own. ac3cli's atmos-path grew a keyframe-file grammar; the GUI's
// timeline grew a parallel one it exports in that grammar; the
// station-broadcast example hard-codes a cue table in C++; a live position
// source (ROADMAP UX4) would have grown a third. This is the one description
// they share.
//
// Scope, deliberately: this is METADATA AND AUTHORING. A scene says where an
// object is at a moment in time; turning that into speaker feeds is the
// encoder's job (and a room-corrected render is Cavern's, not this project's -
// see CONTRIBUTING.md). Orientation below is the same kind of thing: it
// rewrites the positions that go into OAMD, so what reaches the bitstream is
// an ordinary scene that happens to have been turned. Nothing here renders.
//
// Two ways to consume one, both first-class:
//   - a static timeline: ObjectScene::evaluate_into(t, out) once per frame,
//     which is what every batch encode above does today;
//   - a live stream: SceneCursor, which is that same timeline with per-object
//     overrides an external source pushes in as they arrive. UX4's OSC/MIDI/
//     controller source is meant to land on that seam without the scene type
//     itself changing shape.

namespace ac3::oba {

// How the segment that STARTS at an automation point reaches the next one.
// Stated per point rather than per object, so one object can hold a position,
// then slide, then ease, without being split into three.
enum class Interpolation : std::uint8_t {
    // Step. The value stays exactly this point's until the next point's
    // instant, then jumps. Position steps are audible as a click in the
    // panning, so this is mostly for gain gating and for cue-accurate
    // teleports (a cut, not a move).
    kHold = 0,
    // Straight line, component by component. What KeyframePath has always
    // done, and the default, so a scene built from a legacy keyframe file
    // evaluates to exactly the values that file produced before this type
    // existed.
    kLinear = 1,
    // Smoothstep: linear position along the segment shaped by f*f*(3-2f), so
    // the value leaves and arrives with zero slope. Use where a linear ramp
    // corners audibly - a long slow approach that suddenly stops dead.
    kSmooth = 2,
};

// One authored moment: where an object is, how loud, and how it travels from
// here to its next point.
struct AutomationPoint {
    double time_s = 0.0;
    Position position{};
    // Linear, not dB - the same convention ObjectPlacement::gain uses, since
    // that is what this evaluates to.
    double gain = 1.0;
    double lfe_send = 0.0;
    Interpolation interp = Interpolation::kLinear;
};

// One object in the scene.
struct SceneObject {
    // For humans and for round-tripping: it identifies the object in the
    // serialised form and in a GUI list. It is NOT a wire identity - OAMD
    // matches objects to essences BY POSITION IN THE ORDER (TS 103 420 §4.3
    // never pairs them by name), so the index of this object within the scene
    // is what the encoder actually contracts on, and renaming an object
    // changes nothing about the stream.
    std::string name;
    // 0 (the default) means a dynamic object: free to move, driven by the
    // automation below. Otherwise a mask of ac3::oba::bed:: labels, meaning
    // the object is speaker-anchored to those channels. A bed-assigned object
    // still carries automation - its gain is authorable like anything else -
    // but a renderer takes its position from the speaker label, not from here,
    // which is exactly §5.6.4.8's split between contained-in-a-bed objects and
    // dynamic ones.
    std::uint16_t bed = 0;
    // At least one point, sorted by time, no two at the same instant -
    // ObjectScene::create enforces all three. One point is legal and means an
    // object that never moves.
    std::vector<AutomationPoint> automation;
};

// A rotation of the whole scene about the room's centre, applied to positions
// on their way out of evaluate(). METADATA, not a render: it changes the
// coordinates OAMD carries, so a decoder sees a scene that was authored turned.
// Re-aiming a mix at a differently-oriented room, or spinning a scene under a
// fixed listener, without re-authoring every keyframe.
//
// Angles are radians, matching OrbitPath::phase_rad. The serialised form writes
// radians, so a scene survives a save/load bit-exactly, and reads either unit
// ("yaw_rad" or "yaw_deg") because 90 is easier to hand-author than 1.5707963.
// Rotation runs in a centred cube - x and y mapped from [0,1] to [-1,+1] about
// the room centre, z already [-1,+1] about ear height per §4.2.1 - applied yaw
// (about the vertical axis), then pitch (about the left-right axis), then roll
// (about the front-back axis), and mapped back with a clamp to the room. The
// clamp is why an extreme pitch on an already-high object flattens against the
// ceiling rather than leaving the room: OAMD has no coordinates outside it.
//
// An all-zero Orientation is an exact no-op, not a rotation by zero: evaluate()
// returns the authored doubles untouched, so a scene with no orientation
// produces bit-identical positions to one evaluated before this existed.
struct Orientation {
    double yaw_rad = 0.0;
    double pitch_rad = 0.0;
    double roll_rad = 0.0;
};

[[nodiscard]] AC3FORGE_EXPORT Orientation orientation_from_degrees(double yaw_deg,
                                                                   double pitch_deg,
                                                                   double roll_deg);

// One position through one orientation. Exposed because the GUI's room plan
// has to draw what the encoder will emit, not what was authored.
[[nodiscard]] AC3FORGE_EXPORT Position rotate(const Position& position,
                                              const Orientation& orientation);

enum class SceneErrorKind : std::uint8_t {
    // Malformed text: a line that is not the grammar, a JSON token where
    // another was due, a truncated file.
    kSyntax,
    // A required member is missing, or is present with the wrong JSON type.
    kBadField,
    // An object with no automation points at all. A scene has to say where
    // its objects are; a caller that wants a default has to choose it, since
    // the library has no way to know what that caller's default is (ac3cli's
    // and the GUI's differ).
    kEmptyObject,
    // Two automation points on one object share an instant, so which of them
    // is in force is undefined. Rejected rather than silently ordered.
    kDuplicateTime,
    // A recognised field with an unusable value: an unknown interpolation or
    // bed label, a non-finite time/position/gain, a negative duration.
    kBadValue,
};

// Errors carry a line where the format has lines (the keyframe grammar) and a
// message specific enough to print unchanged, because the CLI's existing
// diagnostics are and losing that would be a regression.
struct SceneError {
    SceneErrorKind kind = SceneErrorKind::kSyntax;
    // 1-based. 0 when the format has no line structure to point at.
    std::size_t line = 0;
    std::string message;
};

// The scene. Immutable in shape once created - objects and their automation
// are validated at construction and cannot then go out of order - except for
// the orientation, which is a view-level transform a caller is expected to
// turn while a session is open.
class AC3FORGE_EXPORT ObjectScene {
   public:
    // Sorts each object's automation by time and rejects the four ways a
    // scene can be unusable (see SceneErrorKind). An empty object LIST is
    // legal: a scene with nothing in it evaluates to nothing, which is a
    // sensible starting state for an editor.
    [[nodiscard]] static std::expected<ObjectScene, SceneError> create(
        std::vector<SceneObject> objects, const Orientation& orientation = {});

    [[nodiscard]] std::span<const SceneObject> objects() const { return objects_; }
    [[nodiscard]] std::size_t object_count() const { return objects_.size(); }

    [[nodiscard]] const Orientation& orientation() const { return orientation_; }
    void set_orientation(const Orientation& orientation) { orientation_ = orientation; }

    // The last authored instant in the scene, 0 for an empty one. What a
    // caller encoding "the whole scene" derives its frame count from.
    [[nodiscard]] double duration_s() const;

    // Where one object is at time_s.
    //
    // Ramp semantics, in full, because this is the contract every caller was
    // reimplementing: between two points the segment's own Interpolation
    // applies; BEFORE the first point and AFTER the last, the value HOLDS at
    // that end point - an object sits still before its first cue and stays put
    // after its last, rather than extrapolating off into the wall or vanishing.
    // An object with one point never moves. Callers evaluate at the frame's END
    // time (every encode loop in this repo does, and AtmosEncoder ramps its bed
    // between successive frames' placements, so the placement handed in is the
    // value that ramp arrives AT, not the one it leaves from).
    //
    // Out-of-range index returns a default ObjectPlacement rather than
    // trapping, so a caller iterating a stale count degrades to silence at
    // room centre instead of reading off the end.
    [[nodiscard]] ObjectPlacement evaluate(std::size_t object, double time_s) const;

    // Every object's placement at one instant, in scene order - the span
    // AtmosEncoder::encode_frame takes. The _into form writes min(out.size(),
    // object_count()) entries into caller-owned storage and allocates nothing,
    // which is what a per-frame encode loop wants.
    void evaluate_into(double time_s, std::span<ObjectPlacement> out) const;
    [[nodiscard]] std::vector<ObjectPlacement> evaluate(double time_s) const;

   private:
    ObjectScene(std::vector<SceneObject> objects, const Orientation& orientation)
        : objects_(std::move(objects)), orientation_(orientation) {}

    std::vector<SceneObject> objects_;
    Orientation orientation_{};
};

// --- Live -----------------------------------------------------------------

// One object's placement, from outside the timeline, now.
struct SceneUpdate {
    std::size_t object = 0;
    ObjectPlacement placement{};
};

// The scene as a live surface: the authored timeline underneath, with
// per-object overrides on top for as long as something is driving them.
//
// This is the seam a live position source (ROADMAP UX4 - OSC, MIDI, a game
// controller) and the GUI's live room plug into, and the reason the scene type
// is not just a static table. An overridden object ignores its automation and
// reports whatever was last pushed; release() hands it back to the timeline.
// Latest-value-wins with no interpolation between updates, deliberately: a
// controller's own update rate is not the frame rate, guessing an intermediate
// position would invent motion nobody authored, and AtmosEncoder already ramps
// its bed between the placements it is handed - which is exactly the right
// place for that smoothing to happen, since it is the thing that knows the
// frame boundary.
//
// The scene's orientation applies to pushed placements too. An external source
// reports a position in the same room coordinates the timeline is authored in,
// so turning the scene has to turn the live object with it or a live object and
// its authored neighbours would end up in different rooms.
class AC3FORGE_EXPORT SceneCursor {
   public:
    explicit SceneCursor(ObjectScene scene);

    [[nodiscard]] const ObjectScene& scene() const { return scene_; }
    [[nodiscard]] ObjectScene& scene() { return scene_; }

    // False (and nothing changed) for an object index the scene does not
    // have: an external source can be misconfigured and that must not be
    // fatal to a live session.
    bool push(const SceneUpdate& update);
    // Back to the authored timeline. Releasing an object that was not
    // overridden is a no-op.
    void release(std::size_t object);
    void release_all();
    [[nodiscard]] bool is_live(std::size_t object) const;

    // As ObjectScene::evaluate, with any override in force winning.
    void sample_into(double time_s, std::span<ObjectPlacement> out) const;
    [[nodiscard]] std::vector<ObjectPlacement> sample(double time_s) const;

   private:
    ObjectScene scene_;
    std::vector<std::optional<ObjectPlacement>> live_;
};

// What a scene file held, before any caller's policy is applied to it: objects
// in index order - with the keyframe form's gaps present as empty automation -
// and the orientation, which only the JSON form can carry.
//
// Two callers already disagree about what an index the file skipped should be
// (ac3cli's atmos-path fans it out across the ring, its atmos-encode keeps that
// channel's existing static placement) and a third will have its own answer, so
// the decision has to be theirs. Fill the gaps, then ObjectScene::create.
struct SceneContents {
    std::vector<SceneObject> objects;
    Orientation orientation{};
};

// --- Serialisation --------------------------------------------------------

// JSON, not YAML. The codec library takes no third-party dependencies
// (vcpkg.json says so of the whole target), so whatever format this is has to
// be read and written by code in this repository. RFC 8259 is a grammar small
// enough to implement completely and to be sure of; YAML 1.2's is not, and a
// hand-rolled "YAML subset" would accept and reject files no other YAML tool
// agrees with, which is worse than not offering YAML at all. Both other front
// ends already have a JSON reader to hand (Qt's, Python's) if they ever want to
// read a scene without linking this library.
//
// The written form is stable and diffable: members in a fixed order, one
// automation point per line, numbers short-round-tripped (the shortest decimal
// that reads back as the same double), so a scene under version control shows
// real edits rather than formatting churn.
[[nodiscard]] AC3FORGE_EXPORT std::string to_json(const ObjectScene& scene);

[[nodiscard]] AC3FORGE_EXPORT std::expected<ObjectScene, SceneError> scene_from_json(
    std::string_view text);

// The same read, stopping short of ObjectScene::create - for a caller that
// wants to apply its own policy to the objects first. See SceneContents above.
[[nodiscard]] AC3FORGE_EXPORT std::expected<SceneContents, SceneError> read_scene_json(
    std::string_view text);

// The keyframe grammar ac3cli's atmos-path and atmos-encode have always read,
// and the GUI's timeline has always exported: whitespace-separated columns
//
//     object_index time_s x y z gain lfe_send
//
// one per line, '#' starting a comment to end of line, blank lines skipped.
// Unchanged, byte for byte, including its diagnostics - it is a format users
// have files in.
//
// It returns raw objects rather than a scene because the format is INDEXED and
// SPARSE: a file may mention objects 0 and 2 and say nothing about 1, and what
// object 1 should then be is the caller's policy, not the library's (ac3cli's
// atmos-path and its atmos-encode already disagree about it). Objects come back
// indexed by object_index, unnamed, with gaps present as empty automation;
// fill those, then ObjectScene::create.
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<SceneObject>, SceneError>
scene_objects_from_keyframe_text(std::string_view text);

// The same grammar, written back out. Object indices are scene order; names,
// bed assignments, per-point interpolation and orientation have no column to
// live in and are dropped, which is the reason to prefer to_json() for
// anything a user will reload.
[[nodiscard]] AC3FORGE_EXPORT std::string to_keyframe_text(const ObjectScene& scene);

// The same, over raw objects rather than a validated scene - so a writer whose
// indices are SPARSE can keep them. An object with no automation contributes no
// lines, which is exactly how the format spells a skipped index; the GUI's
// export needs that, because a bed-pinned channel occupies an index that
// atmos-encode's own model has no object for.
[[nodiscard]] AC3FORGE_EXPORT std::string to_keyframe_text(std::span<const SceneObject> objects);

// Reads either form: JSON when the first non-whitespace character is '{',
// otherwise the keyframe grammar. Sniffing rather than trusting a file
// extension, because the CLI's argument has always just been a path and both
// forms have to keep working there.
[[nodiscard]] AC3FORGE_EXPORT std::expected<SceneContents, SceneError> read_scene(
    std::string_view text);

// read_scene() plus the simplest possible gap policy: an index the keyframe
// form skipped becomes an object that sits at `fallback` and never moves. For a
// caller that has no per-index policy of its own.
[[nodiscard]] AC3FORGE_EXPORT std::expected<ObjectScene, SceneError> scene_from_text(
    std::string_view text, const ObjectPlacement& fallback = {});

}  // namespace ac3::oba
