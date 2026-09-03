#include "placement.hpp"

#include <cmath>

namespace ac3::windemo {

namespace {

// Close enough to stop chasing: below this the residual is inaudible and
// letting it decay forever would keep every idle slot's arithmetic warm.
constexpr double kSettled = 1e-4;

double approach(double current, double target, double alpha) {
    const double next = current + alpha * (target - current);
    return std::abs(target - next) < kSettled ? target : next;
}

}  // namespace

PlacementSmoother::PlacementSmoother(double tau_frames)
    : alpha_(tau_frames <= 0.0 ? 1.0 : 1.0 - std::exp(-1.0 / tau_frames)) {}

void PlacementSmoother::set_target(int positioned_slot, const PlacementTarget& target) {
    target_[static_cast<std::size_t>(positioned_slot)] = target;
}

void PlacementSmoother::set_gain(int positioned_slot, double gain) {
    target_[static_cast<std::size_t>(positioned_slot)].gain = gain;
}

void PlacementSmoother::snap(int positioned_slot) {
    const auto index = static_cast<std::size_t>(positioned_slot);
    current_[index] = target_[index];
}

void PlacementSmoother::step(std::span<ac3::oba::ObjectPlacement> out) {
    for (int slot = 0; slot < kPositionedSlots && slot < static_cast<int>(out.size()); ++slot) {
        const auto index = static_cast<std::size_t>(slot);
        auto& now = current_[index];
        const auto& want = target_[index];
        now.position.x = approach(now.position.x, want.position.x, alpha_);
        now.position.y = approach(now.position.y, want.position.y, alpha_);
        now.position.z = approach(now.position.z, want.position.z, alpha_);
        now.gain = approach(now.gain, want.gain, alpha_);
        out[index].position = now.position;
        out[index].gain = now.gain;
        out[index].snap = false;
    }
    for (int channel = 0; channel < kBedSlots; ++channel) {
        const int slot = kPositionedSlots + channel;
        if (slot < static_cast<int>(out.size())) {
            out[static_cast<std::size_t>(slot)] = bed_placement(static_cast<BedChannel>(channel));
        }
    }
}

}  // namespace ac3::windemo
