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

}  // namespace ac3::audio
