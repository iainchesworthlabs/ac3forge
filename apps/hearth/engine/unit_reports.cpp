#include "unit_reports.hpp"

#include <algorithm>

// See unit_reports.hpp.

namespace ac3::hearth {

void UnitReports::add(const UnitReport& report, std::uint64_t output_frame) {
    if (count_ == ring_.size()) {
        // Full: turn the ring so its oldest entry comes first, then grow it.
        std::rotate(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(head_), ring_.end());
        head_ = 0;
        ring_.resize(ring_.empty() ? 16 : ring_.size() * 2);
    }
    Entry& entry = ring_[(head_ + count_) % ring_.size()];
    ++count_;
    entry.output_frame = output_frame;
    entry.report = report;
}

void UnitReports::clear() {
    head_ = 0;
    count_ = 0;
}

bool UnitReports::release(std::uint64_t heard, UnitReport& latest) {
    const Entry* newest = nullptr;
    // A unit is playing once the clock has passed its first frame.
    while (count_ != 0 && ring_[head_].output_frame < heard) {
        newest = &ring_[head_];
        head_ = (head_ + 1) % ring_.size();
        --count_;
    }
    if (newest == nullptr) {
        return false;
    }
    latest = newest->report;
    return true;
}

}  // namespace ac3::hearth
