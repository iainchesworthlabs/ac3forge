#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/export.hpp"

// The spatial/object layer: applications place and move mono sources around
// the listener; the renderer turns the scene into a 5.1 channel bed that
// feeds the AC-3 encoder (or any other sink — nothing here knows about
// AC-3 except the bed's channel order).
//
// Design:
// - 2D pairwise amplitude panning (VBAP on the horizontal ring — 5.1 has no
//   height): pick the adjacent speaker pair around the target azimuth, solve
//   the 2x2 system, clamp, and normalize to Σg² = 1 (energy preservation).
// - Speaker geometry per ITU-R BS.775: C 0°, L +30°, R −30°, SL +110°,
//   SR −110° (azimuth counterclockwise from front, degrees).
// - Objects never feed the LFE implicitly; an explicit lfe_send exists.
// - Automation is clocked at the 256-sample block: targets set between
//   blocks, applied with per-sample linear ramps (no zipper noise).
// - The render path performs no allocation.

namespace ac3::spatial {

inline constexpr int kBedChannels = 5;  // AC-3 3/2 order: L, C, R, SL, SR
inline constexpr int kBlockSamples = 256;

// The ITU-R BS.775 ring, in AC-3 3/2 channel order, degrees counterclockwise
// from front. Everything spatial — the panner here, and the soundfield
// analysis the front ends draw — is defined against this one array so the
// geometry cannot drift between them.
inline constexpr std::array<double, kBedChannels> kSpeakerAzimuthDeg = {
    30.0,    // L
    0.0,     // C
    -30.0,   // R
    110.0,   // SL
    -110.0,  // SR
};

// Per-speaker gains for one source direction, AC-3 3/2 channel order.
using PanGains = std::array<double, kBedChannels>;

// Energy-normalized pairwise pan of a direction (degrees, CCW from front,
// any value; normalized internally) onto the 5.1 ring.
[[nodiscard]] AC3FORGE_EXPORT PanGains pan_azimuth(double azimuth_deg);

// The same pan onto an ARBITRARY horizontal ring, which is what any layout
// wider than 5.1 needs: 7.1 puts its side surrounds at 90° and its rears at
// 150°, so a source at 110° belongs to a different pair there than it does on
// the 5.1 ring. `ring_azimuth_deg` may be in any order and any range; `gains`
// takes one entry per ring member and is OVERWRITTEN, with Sum(g^2) == 1 for a
// non-empty ring.
//
// Two speakers more than 180° apart leave an arc no pair can enclose - the
// hole behind a front-only pair being the obvious case, where the VBAP system
// is singular and both gains solve negative. Across such an arc this
// crossfades at constant power instead, which agrees with the pairwise
// solution at both edges and never drops the source into silence.
AC3FORGE_EXPORT void pan_ring(double azimuth_deg, std::span<const double> ring_azimuth_deg,
                              std::span<double> gains);

// The same pan, addressed by a room-anchored position instead of an angle:
// x runs 0 at the left wall to 1 at the right and y 0 at the front wall to 1
// at the back, which is TS 103 420 §4.2.1's system. A source at the exact
// centre of the room has no direction at all and stays at the front.
//
// There is no z. A 5.1 ring has no height speakers, so elevation cannot be
// rendered and a raised source folds onto the ring at its azimuth, at full
// level - a legacy 5.1 decoder has to hear everything, or backward
// compatibility means nothing. The height survives in the object metadata
// instead, which is the entire reason the object layer exists.
//
// The consequence is worth stating plainly: two sources at the same azimuth
// and different heights get IDENTICAL bed gains, and nothing downstream can
// tell them apart from the bed alone.
[[nodiscard]] AC3FORGE_EXPORT PanGains pan_room(double x, double y);

// --- arbitrary-layout, height-aware panning ---------------------------------
//
// Everything above targets the fixed 5.1 ring. A source with real elevation -
// a Table E2.5 height location, or an object whose z lifts it toward the
// ceiling - needs a second, upper ring and a crossfade between the two, which
// is what ac3::plan's channel-layout renderer already built to move a bed's
// channels between differently-shaped layouts (5.1 to 7.1.4 and back). It is
// promoted here rather than duplicated because IO12's object-based loudness
// measurement needs the identical geometry: an object panned onto a wide
// layout by its own position must agree with what the encoder itself would
// have rendered that position to.

// A source or speaker direction: azimuth counterclockwise from front (ITU-R
// BS.775, any range), elevation above the listener's plane in degrees (0 on
// the ring, positive toward the ceiling).
struct Direction {
    double azimuth_deg = 0.0;
    double elevation_deg = 0.0;
};

// The nominal elevation of the upper layer - TS 103 420 renders heights well
// above the ring, and 45 degrees is the conventional Atmos ceiling angle.
inline constexpr double kHeightElevationDeg = 45.0;
// Everything at or above this counts as the upper layer. Half way to the
// nominal height angle, so no real location is ambiguous.
inline constexpr double kHeightThresholdDeg = kHeightElevationDeg / 2.0;
// A gain below this is not a quiet signal, it is arithmetic (cos(pi/2) lands
// near 6e-17 rather than on zero).
inline constexpr double kNegligibleGain = 1e-9;

// Where a Table E2.5 location sits, in the same (azimuth, elevation) terms as
// `Direction` above. `has_rears`/`has_side_discrete` disambiguate the two
// locations whose direction depends on what else is in the same layout - see
// the .cpp for why: without a discrete rear pair, Ls/Rs sit at the 5.1 ring's
// own +-110 degrees; with one, they move to the side (+-90) and the rear pair
// takes +-135/180 instead. The two LFE-type locations have no direction and
// return {0, 0}; a caller that means to pan a source should exclude them
// first (see PanTargets below), since an LFE-type entry here says nothing
// about where the LFE speaker is.
[[nodiscard]] AC3FORGE_EXPORT Direction direction_of(eac3::chanmap::Location location,
                                                      bool has_rears, bool has_side_discrete);

// A layout's full-bandwidth locations and the direction each one sits at,
// LFE-type locations excluded - the set pan_direction below actually spreads
// a source over.
struct PanTargets {
    std::vector<eac3::chanmap::Location> locations;
    std::vector<Direction> directions;

    // Where a location sits in this set, or -1 if it takes no panned audio.
    [[nodiscard]] int index_of(eac3::chanmap::Location location) const {
        for (std::size_t i = 0; i < locations.size(); ++i) {
            if (locations[i] == location) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
};

[[nodiscard]] AC3FORGE_EXPORT PanTargets pan_targets(
    std::span<const eac3::chanmap::Location> locations);

// One source direction spread over a target speaker set. Two rings - the
// listener's plane and the ceiling - each panned by azimuth (via pan_ring
// above), crossfaded by elevation at constant power. `gains` is sized to
// `targets` and OVERWRITTEN.
//
// A target with no upper layer takes the whole source at full level rather
// than a cosine-attenuated share: a 5.1 ring has no height speakers, and a
// legacy decoder has to hear everything or backward compatibility means
// nothing - the same rule pan_room states for the 5.1 bed.
AC3FORGE_EXPORT void pan_direction(Direction source, std::span<const Direction> targets,
                                    std::span<double> gains);

// The direction an object-audio-metadata room position (TS 103 420 §4.2.1:
// x/y in [0, 1] as pan_room above, z -1 at the floor to +1 at the ceiling)
// sits at, for panning it with pan_direction rather than folding it onto the
// flat 5.1 ring the way pan_room does.
//
// Azimuth is exactly pan_room's own atan2(left, forward) - the two must agree
// or an object would point one way in a 5.1 fold and another in a wider
// render. Elevation reads z against the horizontal distance from room centre,
// atan2(z, horizontal): an object at the room's centre height (z = 0)
// measures 0 degrees whatever its azimuth, matching a DynamicObject's own
// default position, and one directly overhead (x = y = 0.5, z = 1) measures a
// full 90 regardless of the elevation angle this file otherwise treats as
// "the ceiling" - the two are independent numbers that only happen to agree
// at kHeightElevationDeg for a source at the room's outer edge, which is
// where every named height location in Table E2.5 sits.
[[nodiscard]] AC3FORGE_EXPORT Direction position_direction(double x, double y, double z);

struct ObjectState {
    double azimuth_deg = 0.0;
    double gain = 1.0;      // linear
    double lfe_send = 0.0;  // linear; the only way an object reaches the LFE
};

// Renders mono objects into a 5.1 bed (5 fullbw channels + LFE), one
// 256-sample block at a time, ramping each object's channel gains linearly
// from the previous block's values to the current targets.
class AC3FORGE_EXPORT BedRenderer {
   public:
    // Returns the object's index. Call before rendering starts (allocates).
    std::size_t add_object(const ObjectState& initial);

    void set_target(std::size_t object, const ObjectState& target);

    // audio: one 256-sample mono span per object, same order as add_object.
    // bed: 6 spans (L, C, R, SL, SR, LFE) of 256 samples each, OVERWRITTEN.
    void render_block(std::span<const std::span<const float>> audio,
                      std::span<const std::span<float>> bed);

   private:
    struct Slot {
        ObjectState target;
        PanGains current_gains{};
        double current_lfe = 0.0;
        bool primed = false;  // first block jumps to target instead of ramping
    };
    std::vector<Slot> slots_;
};

}  // namespace ac3::spatial
