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

void SlotAllocator::add(AppId app) {
    if (find(app) != nullptr) {
        return;
    }
    apps_.push_back({.app = app, .fullscreen = fullscreen_ == app});
}

void SlotAllocator::remove(AppId app) {
    const auto it = std::ranges::find(apps_, app, &AppSlot::app);
    if (it == apps_.end()) {
        return;
    }
    if (it->positioned) {
        taken_[static_cast<std::size_t>(*it->positioned)] = false;
    }
    apps_.erase(it);
    reconcile();
}

std::optional<int> SlotAllocator::take_free_slot() {
    for (int i = 0; i < kPositionedSlots; ++i) {
        if (!taken_[static_cast<std::size_t>(i)]) {
            taken_[static_cast<std::size_t>(i)] = true;
            return i;
        }
    }
    return std::nullopt;
}

std::optional<int> SlotAllocator::position(AppId app) {
    AppSlot* slot = find(app);
    if (slot == nullptr) {
        add(app);
        slot = find(app);
    }
    slot->wants_position = true;
    reconcile();
    return slot->positioned;
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
            taken_[static_cast<std::size_t>(*slot.positioned)] = false;
            slot.positioned.reset();
        }
    }
    for (auto& slot : apps_) {
        const bool qualifies = slot.wants_position && !slot.fullscreen;
        if (qualifies && !slot.positioned) {
            slot.positioned = take_free_slot();
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
