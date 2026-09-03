#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ac3/oba/atmos.hpp"

// The slot plan: how the demo spends the encoder's fifteen dynamic objects
// (docs/platforms/windows-demo.md, "Objects and the bed").
//
// AtmosEncoder has no bed input of its own - its 5.1 bed is rendered FROM
// the objects - so the bed is five of the fifteen, pinned to the L, R, C, Ls
// and Rs speaker positions with snap set, and the other ten are what
// positioned applications get. The count is fixed at construction and never
// changes mid-stream: an application that leaves the bed takes the lowest
// free positioned slot, one that returns to the bed frees it, and idle slots
// carry silence.
//
// Platform-independent by construction (an AppId is just a number the
// platform layer chose - a process id on Windows), so it is tested on every
// CI leg, not only the one that can run the demo.

namespace ac3::windemo {

using AppId = std::uint32_t;

inline constexpr int kObjectSlots = 15;    // TS 103 420 §8.3.2.2: 16 with the bed's LFE
inline constexpr int kPositionedSlots = 10;
inline constexpr int kBedSlots = 5;
static_assert(kPositionedSlots + kBedSlots == kObjectSlots);

// Bed slot order and index. The order is the one the bed mixer writes in
// and the encoder is fed in; it is NOT AC-3's coded order (L C R Ls Rs LFE),
// which is the encoder's own business.
enum class BedChannel : std::uint8_t { kL = 0, kR = 1, kC = 2, kLs = 3, kRs = 4 };

[[nodiscard]] constexpr int bed_slot(BedChannel channel) {
    return kPositionedSlots + static_cast<int>(channel);
}

// Where each bed slot sits, in room coordinates (ac3::oba::Position: x 0 left
// to 1 right, y 0 front to 1 back, z 0 on the listener plane). Snapped, so a
// renderer sends each to its nearest speaker rather than panning.
[[nodiscard]] ac3::oba::ObjectPlacement bed_placement(BedChannel channel);

// One application's standing in the plan.
struct AppSlot {
    AppId app = 0;
    // A positioned slot index in [0, kPositionedSlots), or nullopt: this
    // application is in the bed. A split application (width 2) holds this
    // slot and the next one: left, then right.
    std::optional<int> positioned;
    int width = 1;
    // The user asked for this application to be positioned. Kept separately
    // from `positioned` because the full-screen rule can override it: a
    // full-screen foreground application is the bed whatever the user asked,
    // and gets its slot back when it stops being full-screen.
    bool wants_position = false;
    bool fullscreen = false;
};

class SlotAllocator {
public:
    // Registers an application in the bed, and hands back its slot.
    // Idempotent for a known id: the existing slot is returned.
    AppSlot& add(AppId app);
    // Forgets an application, freeing its slot if it held one.
    void remove(AppId app);

    // The user dragged it out of the bed. Returns the slot it got, or
    // nullopt when every positioned slot is taken (the request is remembered
    // as wants_position, and honoured the moment a slot frees up) or when it
    // is currently full-screen (likewise remembered).
    std::optional<int> position(AppId app);
    // The user dragged it back, or pressed reset.
    void unposition(AppId app);

    // Split (width 2: a stereo application becomes two objects, one per
    // channel) or mono (width 1). A positioned application that changes
    // width gives its slots back and takes the new count as one block; when
    // two consecutive slots are not free it waits in the bed, like any
    // other waiter, and gets them when they free up.
    void set_width(AppId app, int width);
    [[nodiscard]] int width_of(AppId app) const;

    // The platform layer's report of which application is full-screen in the
    // foreground, or nullopt. Only that application is forced into the bed;
    // everything else keeps what it has.
    void set_fullscreen(std::optional<AppId> app);

    [[nodiscard]] std::optional<int> slot_of(AppId app) const;
    [[nodiscard]] bool in_bed(AppId app) const;
    [[nodiscard]] bool known(AppId app) const;
    [[nodiscard]] int free_positioned_slots() const;
    [[nodiscard]] const std::vector<AppSlot>& apps() const { return apps_; }

private:
    AppSlot* find(AppId app);
    const AppSlot* find(AppId app) const;
    std::optional<int> take_free_slots(int width);
    void release(AppSlot& slot);
    void reconcile();

    std::vector<AppSlot> apps_;
    std::array<bool, kPositionedSlots> taken_{};
    std::optional<AppId> fullscreen_;
};

}  // namespace ac3::windemo
