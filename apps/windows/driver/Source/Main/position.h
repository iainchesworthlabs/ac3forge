// The null sink's clock: where a stream's position is and which packet is
// being consumed, as a function of wall-clock time alone.
//
// A real device's position comes from its DMA engine; this device has none,
// so the position is what a device consuming at exactly the nominal rate
// would have reached: bytes = elapsed time since Run * bytes per second.
// Packet completions follow from that (packet i is complete when the
// position reaches the end of packet i), which is what the stream's timer
// reports to ACX. That is the one piece of logic in this driver that is not
// framework plumbing, so it lives here with no kernel dependencies and is
// tested in user mode (tests/windemo/test_nullsink_position.cpp) - kernel
// code cannot be coverage-measured, this can.
//
// Times are in 100 ns units ("hns", the kernel's), taken from the caller;
// nothing here reads a clock. Unsigned 64-bit throughout: at 768,000 bytes
// per second a position needs 2^64 bytes in about 760,000 years.

#pragma once

namespace ac3nullsink {

using u64 = unsigned long long;

constexpr u64 kHnsPerSecond = 10000000ULL;

class PositionClock {
public:
    // Rates for the stream about to run: the format's average bytes per
    // second, and the size of one RT packet in bytes. Resets everything.
    void configure(u64 bytes_per_second, u64 packet_bytes) noexcept {
        bytes_per_second_ = bytes_per_second;
        packet_bytes_ = packet_bytes;
        stop();
    }

    // Pause -> Run at `now`. The position carries on from where it paused.
    void run(u64 now_hns) noexcept {
        start_time_ = now_hns;
        start_position_ = position_;
        running_ = true;
    }

    // Run -> Pause at `now`: the position freezes where the clock puts it.
    void pause(u64 now_hns) noexcept {
        position_ = position_at(now_hns);
        running_ = false;
    }

    // Back to the beginning: a released stream starts from zero next time.
    void stop() noexcept {
        position_ = 0;
        start_position_ = 0;
        start_time_ = 0;
        completed_ = 0;
        running_ = false;
    }

    bool running() const noexcept { return running_; }

    // Bytes consumed since the stream started, at `now`.
    u64 position_at(u64 now_hns) const noexcept {
        if (!running_) {
            return position_;
        }
        const u64 elapsed = now_hns > start_time_ ? now_hns - start_time_ : 0;
        return start_position_ + elapsed * bytes_per_second_ / kHnsPerSecond;
    }

    // How many packets the clock says are complete at `now`, against how
    // many have been reported: the number the caller still owes ACX.
    u64 packets_due(u64 now_hns) const noexcept {
        if (packet_bytes_ == 0) {
            return 0;
        }
        const u64 complete = position_at(now_hns) / packet_bytes_;
        return complete > completed_ ? complete - completed_ : 0;
    }

    // The index of the packet being consumed now: the count completed.
    u64 current_packet() const noexcept { return completed_; }

    // One more packet reported complete; returns its index.
    u64 mark_completed() noexcept { return completed_++; }

    // When the packet being consumed will be complete, in the same clock
    // as `run` was given: the time the position reaches its end. Never
    // earlier than the start time.
    u64 next_completion_hns() const noexcept {
        const u64 end = (completed_ + 1) * packet_bytes_;
        if (end <= start_position_ || bytes_per_second_ == 0) {
            return start_time_;
        }
        return start_time_ + (end - start_position_) * kHnsPerSecond / bytes_per_second_;
    }

    u64 bytes_per_second() const noexcept { return bytes_per_second_; }
    u64 packet_bytes() const noexcept { return packet_bytes_; }

private:
    u64 bytes_per_second_ = 0;
    u64 packet_bytes_ = 0;
    u64 start_time_ = 0;      // when the current run began
    u64 start_position_ = 0;  // the position then
    u64 position_ = 0;        // the position while paused
    u64 completed_ = 0;       // packets reported complete
    bool running_ = false;
};

}  // namespace ac3nullsink
