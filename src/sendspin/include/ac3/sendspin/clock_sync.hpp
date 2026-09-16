#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

#include "ac3/sendspin/messages.hpp"

class SendspinTimeFilter;

// A client's side of Sendspin's clock synchronisation (messaging.md, Clock Synchronization):
// client/time out, server/time back, and the time filter that maps server time to local time
// and back.
//
// Exchanges run in bursts, as the time filter's Recommended Usage describes: a burst sends
// kBurstLength exchanges one after another, each waiting for its reply, and feeds the filter
// only the sample with the smallest max_error. Until the clock has converged the bursts follow
// one another at once. The next kLearningBursts run kLearningInterval apart, since the bursts
// before them all came within a second or two and say little about drift; after that one runs
// every kBurstInterval. A reply that has not come within kReplyTimeout ends the burst with the
// samples it has.
//
// Once converged, a burst whose best sample has a max_error well above the least of the last
// kFloorBursts bursts' (floor_limit()) is left out of the filter. Measured on ESP32-S3 boards
// over Wi-Fi, the replies to a player that is being sent a stream wait behind its chunks: every
// sample of a burst can then come back several milliseconds late, and the filter would take
// that one-sided delay for a change of offset. Every burst counts towards the floor, so a
// network that has become slower for good is followed once the window has passed.
//
// Converged means the filter's own error estimate has stayed under kConvergedError for
// kConvergedUpdates updates in a row (planning/hearth-sendspin-extension.md, Q4). A player
// reports available: true only once converged.
//
// Every time is a local monotonic microsecond count the caller passes in; nothing here reads
// a clock. The filter itself ignores any update not later than its starting point of zero, so
// local times reach it counted from just before the first exchange, and come back out of it
// on the caller's scale: a clock that reads negative works like any other.

namespace ac3::sendspin {

class ClockSync {
   public:
    static constexpr std::size_t kBurstLength = 8;
    static constexpr std::int64_t kBurstInterval = 10'000'000;
    static constexpr std::size_t kLearningBursts = 30;
    static constexpr std::int64_t kLearningInterval = 1'000'000;
    static constexpr std::int64_t kReplyTimeout = 5'000'000;
    static constexpr std::int64_t kConvergedError = 1'000;
    static constexpr std::size_t kConvergedUpdates = 8;
    static constexpr std::size_t kFloorBursts = 30;

    // The largest max_error a converged burst's best sample may have for the filter to take
    // it, given `floor`, the least of the recent bursts': half as much again, and never less
    // than a millisecond over it.
    [[nodiscard]] static constexpr std::int64_t floor_limit(std::int64_t floor) {
        return floor + (floor / 2 > 1'000 ? floor / 2 : 1'000);
    }

    ClockSync();
    ~ClockSync();
    ClockSync(const ClockSync&) = delete;
    ClockSync& operator=(const ClockSync&) = delete;
    ClockSync(ClockSync&&) noexcept;
    ClockSync& operator=(ClockSync&&) noexcept;

    // The client/time to send at `now`, when one is due. At most one exchange is ever in
    // flight.
    [[nodiscard]] std::optional<messages::ClientTime> poll(std::int64_t now);

    // A server/time received at `now`. Replies that do not answer the exchange in flight
    // are ignored.
    void receive(const messages::ServerTime& time, std::int64_t now);

    // When poll() next has something to send; the caller's timer can sleep until then.
    [[nodiscard]] std::int64_t next_due() const { return next_due_; }

    [[nodiscard]] bool converged() const { return converged_; }
    [[nodiscard]] std::size_t updates() const { return updates_; }
    // Bursts left out of the filter for their best sample's max_error.
    [[nodiscard]] std::size_t rejected() const { return rejected_; }
    [[nodiscard]] std::int64_t error_us() const;

    // Mappings through the filter. Meaningful once at least one update has been made.
    [[nodiscard]] std::int64_t to_local(std::int64_t server_time) const;
    [[nodiscard]] std::int64_t to_server(std::int64_t local_time) const;

    // Forgets everything, as after a reconnection to another server.
    void reset();

   private:
    void finish_burst(std::int64_t now);
    // Whether a converged burst whose best sample has `max_error` goes to the filter; the
    // burst's max_error joins the floor's window either way.
    [[nodiscard]] bool take(std::int64_t max_error);

    std::unique_ptr<SendspinTimeFilter> filter_;
    // Subtracted from local times on the way into the filter and added back on the way out.
    std::optional<std::int64_t> base_;
    std::optional<std::int64_t> in_flight_;  // client_transmitted of the exchange awaiting a reply
    std::int64_t sent_at_ = 0;
    std::size_t burst_count_ = 0;
    std::optional<std::int64_t> best_measurement_;
    std::int64_t best_max_error_ = 0;
    std::int64_t best_time_ = 0;
    // Due at once, whatever the caller's clock reads: local times can be negative.
    std::int64_t next_due_ = std::numeric_limits<std::int64_t>::min();
    std::size_t updates_ = 0;
    std::size_t under_threshold_ = 0;
    bool converged_ = false;
    // Learning bursts still to run after convergence.
    std::size_t learning_left_ = 0;
    // The best max_error of each of the last kFloorBursts bursts, oldest overwritten first.
    std::array<std::int64_t, kFloorBursts> floor_window_{};
    std::size_t floor_count_ = 0;
    std::size_t floor_next_ = 0;
    std::size_t rejected_ = 0;
};

}  // namespace ac3::sendspin
