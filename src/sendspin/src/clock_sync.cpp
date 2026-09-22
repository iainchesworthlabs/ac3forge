#include "ac3/sendspin/clock_sync.hpp"

#include <sendspin_time_filter.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>

#include "ac3/sendspin/messages.hpp"

namespace ac3::sendspin {

ClockSync::ClockSync() : filter_(std::make_unique<SendspinTimeFilter>()) {}
ClockSync::~ClockSync() = default;
ClockSync::ClockSync(ClockSync&&) noexcept = default;
ClockSync& ClockSync::operator=(ClockSync&&) noexcept = default;

std::optional<messages::ClientTime> ClockSync::poll(std::int64_t now) {
    if (in_flight_) {
        if (now - sent_at_ < kReplyTimeout) {
            return std::nullopt;
        }
        // The reply is overdue: the burst ends with what it has.
        in_flight_.reset();
        finish_burst(now);
    }
    if (now < next_due_) {
        return std::nullopt;
    }
    if (!base_) {
        base_ = now - 1;
    }
    in_flight_ = now;
    sent_at_ = now;
    return messages::ClientTime{.client_transmitted = now};
}

void ClockSync::receive(const messages::ServerTime& time, std::int64_t now) {
    if (!in_flight_ || time.client_transmitted != *in_flight_) {
        return;
    }
    in_flight_.reset();
    // T1 client sent, T2 server received, T3 server sent, T4 client received.
    const std::int64_t t1 = time.client_transmitted;
    const std::int64_t t2 = time.server_received;
    const std::int64_t t3 = time.server_transmitted;
    const std::int64_t t4 = now;
    const std::int64_t measurement = ((t2 - t1) + (t3 - t4)) / 2;
    const std::int64_t max_error = ((t4 - t1) - (t3 - t2)) / 2;
    if (max_error >= 0 && (!best_measurement_ || max_error < best_max_error_)) {
        best_measurement_ = measurement;
        best_max_error_ = max_error;
        best_time_ = t4;
    }
    ++burst_count_;
    if (burst_count_ >= kBurstLength) {
        finish_burst(now);
    } else {
        next_due_ = now;
    }
}

bool ClockSync::take(std::int64_t max_error) {
    std::int64_t floor = max_error;
    for (std::size_t i = 0; i < floor_count_; ++i) {
        floor = std::min(floor, floor_window_[i]);
    }
    floor_window_[floor_next_] = max_error;
    floor_next_ = (floor_next_ + 1) % kFloorBursts;
    floor_count_ = std::min(floor_count_ + 1, kFloorBursts);
    return !converged_ || max_error <= floor_limit(floor);
}

void ClockSync::finish_burst(std::int64_t now) {
    if (best_measurement_) {
        if (take(best_max_error_)) {
            // On the filter's scale: local times less the base, so the offset grows by it.
            filter_->update(*best_measurement_ + *base_, best_max_error_, best_time_ - *base_);
            ++updates_;
            if (converged_) {
                learning_left_ = learning_left_ > 0 ? learning_left_ - 1 : 0;
            } else if (confirming_) {
                // The confirming burst, a learning interval after the run that first reached
                // kConvergedUpdates: only a raw measurement genuinely apart in time from that
                // run, not the filter's error estimate again (which a run of samples biased the
                // same way satisfies just as well as an accurate run), can catch that the run
                // was wrong.
                confirming_ = false;
                if (std::llabs(*best_measurement_ - confirm_measurement_) <= kConvergedError) {
                    converged_ = true;
                    learning_left_ = kLearningBursts;
                } else {
                    // Disagreed: a real bias, not the filter's own noise agreeing with itself.
                    // The run that looked converged is not trusted; start over.
                    under_threshold_ = 0;
                }
            } else {
                under_threshold_ = filter_->get_error() < kConvergedError ? under_threshold_ + 1 : 0;
                if (under_threshold_ >= kConvergedUpdates) {
                    confirming_ = true;
                    confirm_measurement_ = *best_measurement_;
                }
            }
        } else {
            ++rejected_;
        }
    }
    best_measurement_.reset();
    burst_count_ = 0;
    if (converged_) {
        next_due_ = now + (learning_left_ > 0 ? kLearningInterval : kBurstInterval);
    } else if (confirming_) {
        next_due_ = now + kLearningInterval;
    } else {
        next_due_ = now;
    }
}

std::int64_t ClockSync::error_us() const {
    return filter_->get_error();
}

std::int64_t ClockSync::to_local(std::int64_t server_time) const {
    return filter_->compute_client_time(server_time) + base_.value_or(0);
}

std::int64_t ClockSync::to_server(std::int64_t local_time) const {
    return filter_->compute_server_time(local_time - base_.value_or(0));
}

void ClockSync::reset() {
    filter_->reset();
    base_.reset();
    in_flight_.reset();
    sent_at_ = 0;
    burst_count_ = 0;
    best_measurement_.reset();
    best_max_error_ = 0;
    best_time_ = 0;
    next_due_ = std::numeric_limits<std::int64_t>::min();
    updates_ = 0;
    under_threshold_ = 0;
    converged_ = false;
    learning_left_ = 0;
    floor_window_.fill(0);
    floor_count_ = 0;
    floor_next_ = 0;
    rejected_ = 0;
    confirming_ = false;
    confirm_measurement_ = 0;
}

}  // namespace ac3::sendspin
