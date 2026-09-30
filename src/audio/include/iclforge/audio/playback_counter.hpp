#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "ac3/audio/monitor.hpp"

// The arithmetic behind MonitorPosition, which is the same on every platform
// even though nothing else about the backends is.
//
// Each backend can obtain two numbers, both counted from the last start or
// flush: how many sample-frames it has handed the device, and how many of
// those the device has not played yet. WASAPI reads the second from
// GetCurrentPadding, ALSA from snd_pcm_delay, PipeWire from the queued and
// delay figures in pw_stream_get_time_n, Core Audio from the lead between the
// IOProc's two timestamps, AAudio from the gap between the frames written and
// the presentation position. Getting them is all that is platform-specific;
// what follows from them is not, and lives here - so a fake device's clock in
// ac3tests drives the same code the real backends do rather than a
// reimplementation of it.
//
// The device thread calls report(); a caller asking where playback has got to
// calls position() on its own thread. Two relaxed atomics rather than a lock:
// a reader that catches a new frame count beside an old unplayed count is out
// by less than one device period, no thread ever waits for another (a render
// callback must not wait for anything), and a device's own clock is not that
// precise to begin with. What must not happen - and cannot, since report()
// stores the difference rather than the reader subtracting one from the
// other - is a played count that runs backwards or wraps.

namespace ac3::audio {

class PlaybackCounter {
public:
    // A start, or a flush that dropped everything: count from zero again.
    void restart() {
        played_.store(0, std::memory_order_relaxed);
        unplayed_.store(0, std::memory_order_relaxed);
    }

    // One period's report from the device side. A device claiming to hold
    // more than it has been given is taken to hold all of it, rather than to
    // have played a negative number of frames.
    void report(std::uint64_t handed_over, std::uint64_t unplayed) {
        const std::uint64_t held = std::min(handed_over, unplayed);
        played_.store(handed_over - held, std::memory_order_relaxed);
        unplayed_.store(held, std::memory_order_relaxed);
    }

    // The frames played since the last restart(), as the last report() had
    // it. For a backend that has to re-establish its own frame count after
    // dropping what the device held (ALSA pausing hardware that cannot).
    [[nodiscard]] std::uint64_t played() const {
        return played_.load(std::memory_order_relaxed);
    }

    // `queued` is what the sink itself still holds and has not handed over;
    // `latency` what the platform says its output path adds beyond the
    // device's own buffer (0 where it does not say).
    [[nodiscard]] MonitorPosition position(std::uint64_t queued, std::uint32_t latency) const {
        return MonitorPosition{
            .frames_played = played_.load(std::memory_order_relaxed),
            .frames_queued = queued + unplayed_.load(std::memory_order_relaxed),
            .latency_frames = latency};
    }

private:
    std::atomic<std::uint64_t> played_{0};
    std::atomic<std::uint64_t> unplayed_{0};
};

// A device's 32-bit play-head count, widened, and kept moving forward through
// what a real one does besides count. Android's
// AudioTrack.getPlaybackHeadPosition() is the case in point. For a direct
// track it is the HAL's render position, which:
//   * wraps at 2^32;
//   * reads 0 when the HAL does not answer;
//   * starts again from 0 when the output goes to standby;
//   * after a flush, goes on reading the old count until the flush reaches
//     the hardware.
// One thread's: the one asking where playback has got to.
class PlayHead {
public:
    // A flush: count from zero. Until a reading comes in below the last one
    // taken, readings are the old count still arriving, and read as nothing
    // played - for kStaleReadings of them at most, after which the count is
    // taken as it is.
    void restart() {
        stale_from_ = last_;
        stale_left_ = last_ > 0 ? kStaleReadings : 0;
        base_ = 0;
        last_ = 0;
    }

    // Takes one reading, and returns the frames played since the last
    // restart().
    std::uint64_t read(std::uint32_t reading) {
        if (stale_left_ > 0) {
            if (reading >= stale_from_) {
                --stale_left_;
                return base_ + last_;
            }
            stale_left_ = 0;
        } else if (reading < last_) {
            if (last_ >= kTopQuarter && reading < kBottomQuarter) {
                // Counted round.
                base_ += std::uint64_t{1} << 32U;
            } else if (reading == 0) {
                // No answer, or a count started again that has not moved yet.
                return base_ + last_;
            } else {
                // The count started again: carry on from where it was.
                base_ += last_ - reading;
            }
        }
        last_ = reading;
        return base_ + last_;
    }

    static constexpr int kStaleReadings = 100;

private:
    static constexpr std::uint32_t kTopQuarter = 0xC0000000U;
    static constexpr std::uint32_t kBottomQuarter = 0x40000000U;

    std::uint64_t base_ = 0;
    std::uint32_t last_ = 0;
    std::uint32_t stale_from_ = 0;
    int stale_left_ = 0;
};

// A passthrough link's figures, counted in its own frames, in the content's:
// `ratio` link frames make one content frame (carrier_ratio() in
// passthrough.hpp). A partial content frame is not yet a frame.
[[nodiscard]] inline MonitorPosition per_content_frame(const MonitorPosition& link,
                                                      std::uint32_t ratio) {
    const std::uint32_t by = std::max<std::uint32_t>(ratio, 1);
    return MonitorPosition{.frames_played = link.frames_played / by,
                           .frames_queued = link.frames_queued / by,
                           .latency_frames = link.latency_frames / by};
}

}  // namespace ac3::audio
