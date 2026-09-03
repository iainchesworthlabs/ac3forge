#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "ac3/oba/atmos.hpp"
#include "slots.hpp"

// Where each object is this frame (docs/platforms/windows-demo.md, "Objects
// and the bed", last paragraph).
//
// The UI hands over targets in room coordinates whenever the user drags; the
// encoder wants a placement per slot per frame. Between the two sits this
// smoother, because a 32 ms step in position is a click: each positioned
// slot's live position approaches its target with a first-order lag whose
// time constant is a few frames, and the bed slots never move. Gains are
// smoothed the same way, so a slot that just got an application fades in
// rather than switching on.

namespace ac3::windemo {

struct PlacementTarget {
    ac3::oba::Position position{0.5, 0.5, 0.0};
    double gain = 0.0;  // 0 = silent slot
    // Isotropic extent, 0 (a point) to 1 (the whole room), carried to the
    // OAMD payload for the receiver's renderer (TS 103 420 §5.6.1.2); the
    // encoder's own bed render treats every object as a point.
    double size = 0.0;
};

class PlacementSmoother {
public:
    // `tau_frames` is the lag's time constant in frames (the 1/e settling
    // time); 3 at 32 ms is about 100 ms to settle, quick enough to feel
    // direct under a drag and slow enough not to click.
    explicit PlacementSmoother(double tau_frames = 3.0);

    void set_target(int positioned_slot, const PlacementTarget& target);
    // Sets the gain target alone, leaving the position where it was - what
    // freeing a slot does, so the object fades out in place.
    void set_gain(int positioned_slot, double gain);
    // Jumps to the target with no lag - the first placement of a slot that
    // was silent, so it does not glide in from wherever the last occupant
    // left it.
    void snap(int positioned_slot);

    // Advances every slot one frame and writes all kObjectSlots placements:
    // the positioned ones as smoothed, the bed ones pinned.
    void step(std::span<ac3::oba::ObjectPlacement> out);

    [[nodiscard]] const PlacementTarget& current(int positioned_slot) const {
        return current_[static_cast<std::size_t>(positioned_slot)];
    }

private:
    double alpha_;
    std::array<PlacementTarget, kPositionedSlots> target_{};
    std::array<PlacementTarget, kPositionedSlots> current_{};
};

}  // namespace ac3::windemo
