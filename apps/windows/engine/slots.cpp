#include "slots.hpp"

#include <algorithm>

namespace ac3::windemo {

ac3::oba::ObjectPlacement bed_placement(BedChannel channel) {
    ac3::oba::ObjectPlacement placement;
    switch (channel) {
        case BedChannel::kL: placement.position = {0.0, 0.0, 0.0}; break;
        case BedChannel::kR: placement.position = {1.0, 0.0, 0.0}; break;
        case BedChannel::kC: placement.position = {0.5, 0.0, 0.0}; break;
        case BedChannel::kLs: placement.position = {0.0, 1.0, 0.0}; break;
        case BedChannel::kRs: placement.position = {1.0, 1.0, 0.0}; break;
    }
    placement.snap = true;
    placement.gain = 1.0;
    return placement;
}

AppSlot* SlotAllocator::find(AppId app) {
    const auto it = std::ranges::find(apps_, app, &AppSlot::app);
    return it == apps_.end() ? nullptr : &*it;
}

const AppSlot* SlotAllocator::find(AppId app) const {
    const auto it = std::ranges::find(apps_, app, &AppSlot::app);
    return it == apps_.end() ? nullptr : &*it;
}

AppSlot& SlotAllocator::add(AppId app) {
    if (AppSlot* existing = find(app); existing != nullptr) {
        return *existing;
    }
    apps_.push_back({.app = app,
                     .positioned = std::nullopt,
                     .width = 1,
                     .wants_position = false,
                     .fullscreen = fullscreen_ == app});
    return apps_.back();
}

void SlotAllocator::remove(AppId app) {
    const auto it = std::ranges::find(apps_, app, &AppSlot::app);
    if (it == apps_.end()) {
        return;
    }
    release(*it);
    apps_.erase(it);
    reconcile();
}

void SlotAllocator::release(AppSlot& slot) {
    if (slot.positioned) {
        for (int i = 0; i < slot.width; ++i) {
            taken_[static_cast<std::size_t>(*slot.positioned + i)] = false;
        }
        slot.positioned.reset();
    }
}

// The lowest run of `width` free slots, taken as one block.
std::optional<int> SlotAllocator::take_free_slots(int width) {
    for (int first = 0; first + width <= kPositionedSlots; ++first) {
        bool free = true;
        for (int i = 0; i < width; ++i) {
            free = free && !taken_[static_cast<std::size_t>(first + i)];
        }
        if (free) {
            for (int i = 0; i < width; ++i) {
                taken_[static_cast<std::size_t>(first + i)] = true;
            }
            return first;
        }
    }
    return std::nullopt;
}

void SlotAllocator::set_width(AppId app, int width) {
    AppSlot* slot = find(app);
    if (slot == nullptr) {
        add(app);
        slot = find(app);
    }
    width = width >= 2 ? 2 : 1;
    if (slot->width == width) {
        return;
    }
    release(*slot);
    slot->width = width;
    reconcile();
}

int SlotAllocator::width_of(AppId app) const {
    const AppSlot* slot = find(app);
    return slot == nullptr ? 1 : slot->width;
}

std::optional<int> SlotAllocator::position(AppId app) {
    AppSlot& slot = add(app);  // the slot it has, or a new one
    slot.wants_position = true;
    reconcile();
    return slot.positioned;
}

void SlotAllocator::unposition(AppId app) {
    AppSlot* slot = find(app);
    if (slot == nullptr) {
        return;
    }
    slot->wants_position = false;
    reconcile();
}

void SlotAllocator::set_fullscreen(std::optional<AppId> app) {
    fullscreen_ = app;
    for (auto& slot : apps_) {
        slot.fullscreen = fullscreen_ == slot.app;
    }
    reconcile();
}

// The one place slots move. Two passes: first take slots away from anything
// that no longer qualifies (asked back into the bed, or went full-screen),
// then hand free ones to whoever is waiting, in registration order so the
// outcome is deterministic. Handing out after freeing is what lets a
// full-screen application's slot go straight to the next waiter.
void SlotAllocator::reconcile() {
    for (auto& slot : apps_) {
        const bool qualifies = slot.wants_position && !slot.fullscreen;
        if (slot.positioned && !qualifies) {
            release(slot);
        }
    }
    for (auto& slot : apps_) {
        const bool qualifies = slot.wants_position && !slot.fullscreen;
        if (qualifies && !slot.positioned) {
            slot.positioned = take_free_slots(slot.width);
        }
    }
}

std::optional<int> SlotAllocator::slot_of(AppId app) const {
    const AppSlot* slot = find(app);
    return slot == nullptr ? std::nullopt : slot->positioned;
}

bool SlotAllocator::in_bed(AppId app) const {
    const AppSlot* slot = find(app);
    return slot != nullptr && !slot->positioned;
}

bool SlotAllocator::known(AppId app) const {
    return find(app) != nullptr;
}

int SlotAllocator::free_positioned_slots() const {
    return static_cast<int>(std::ranges::count(taken_, false));
}

}  // namespace ac3::windemo
