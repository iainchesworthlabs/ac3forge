#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "esp_timer.h"

// What the two I2S sinks (sink/i2s/, sink/tdm/) share and the others do not
// need: the shape of the DMA queue, and the model of what the DAC heard.

namespace player {

// --- the DMA queue's shape ------------------------------------------------------
// Kconfig gives a depth as descriptors x frames (main/Kconfig.projbuild), sized
// for stereo. The driver caps a descriptor at 4,092 bytes and quietly shortens
// one that asks for more, so eight 32-bit slots at the stereo default of 240
// frames would silently get 127 and a queue a quarter as deep as configured.
// This keeps the DEPTH - the product - and splits it into as many descriptors
// as the bus width needs.
struct DmaPlan {
    int descriptors;
    int frames;  // per descriptor
};

inline DmaPlan dma_plan(int budget_descriptors, int budget_frames, std::size_t bytes_per_frame) {
    constexpr std::size_t kMaxDescriptorBytes = 4092;  // I2S_DMA_BUFFER_MAX_SIZE
    const std::size_t total =
        static_cast<std::size_t>(budget_descriptors) * static_cast<std::size_t>(budget_frames);
    std::size_t per = bytes_per_frame == 0 ? 1 : kMaxDescriptorBytes / bytes_per_frame;
    if (per > static_cast<std::size_t>(budget_frames)) {
        per = static_cast<std::size_t>(budget_frames);
    }
    if (per == 0) {
        per = 1;
    }
    std::size_t descriptors = (total + per - 1) / per;
    if (descriptors < 2) {
        descriptors = 2;
    }
    return {static_cast<int>(descriptors), static_cast<int>(per)};
}

// --- what the DAC heard, by the DAC's own clock ---------------------------------
// The driver says nothing when the DMA runs dry: with auto_clear it plays zeros
// and carries on, and the gap is audible and otherwise unrecorded. So the queue
// is modelled here from the one fact the hardware guarantees - it drains at
// exactly the sample rate, whatever the CPU does.
//
// When a write returns, every byte of it has been queued, so the DMA holds what
// it held on arrival plus this block, less what drained while the write waited,
// and never more than its capacity. When the NEXT block arrives, what remains
// is that figure less the drain since - and if that is zero or negative, the
// DAC has been playing silence for the difference. That is an underrun,
// counted and timed below.
//
// It is a model, and it is out by up to one descriptor: the write returns when
// its last bytes land in a descriptor buffer, not when the queue is full to the
// byte. A few milliseconds at the default depth, which is worth knowing when
// reading min_headroom and not enough to mistake a stall for a smooth run.
//
// And it is kept per play. Between two plays the queue drains and then plays
// zeros with nothing owed to it, so restart() starts the figures again and
// exempts the play's first block as open() exempts the first after boot.
// Before it did, a play started from the control surface counted the idle
// time ahead of it as one underrun: 55 seconds of it on a DevKitC-1, reported
// against a ten-minute play with no gaps in it, and the same block pinned
// min_headroom at zero.
class DacQueueModel {
   public:
    void open(std::uint32_t bytes_per_second, std::size_t capacity_bytes) {
        bytes_per_second_ = bytes_per_second;
        capacity_ = static_cast<std::int64_t>(capacity_bytes);
    }

    // A play is beginning. What the queue holds is left alone - whatever the
    // last play put in it is still draining, and the next arrival works out
    // how much of it is left - but the figures start again, and the next
    // block is the play's first.
    void restart() { play_ = Play{}; }

    // As a block arrives, before its write: where the queue stands, and
    // whether it ran dry since the last one. Not for a play's first block -
    // the DMA has been playing zeros since the channel started, or since the
    // last play ran out, and nothing was owed to it yet.
    void arriving() {
        arrived_us_ = esp_timer_get_time();
        // What the last write left, less what has drained since: in bytes
        // while the queue lasts, in time once it has run out. In bytes because
        // only the drain is rounded there, downwards, so the figure can err
        // high and never low, and the capacity clamp in queued() takes the
        // error out whenever a write fills the queue; taking the figure itself
        // through microseconds and back would round it down twice a block with
        // nothing to correct it. In time once the queue is empty because ahead
        // of a play's first block the gap is however long the player sat
        // idle, and five weeks of that as bytes at a sixteen-slot bus's rate
        // would overflow 64 bits.
        const std::int64_t elapsed_us = arrived_us_ - stamp_us_;
        const std::int64_t lasts_us = bytes_to_us(queue_bytes_);
        std::int64_t headroom = 0;
        std::int64_t dry_us = 0;
        if (elapsed_us < lasts_us) {
            headroom = queue_bytes_ - us_to_bytes(elapsed_us);
        } else {
            dry_us = elapsed_us - lasts_us;
        }
        queue_bytes_ = headroom;
        if (play_.writes == 0) {
            return;
        }
        if (headroom <= 0) {
            ++play_.underruns;
            play_.dry_us += static_cast<std::uint64_t>(dry_us);
        }
        const std::int64_t left_us = bytes_to_us(headroom);
        if (play_.min_headroom_us < 0 || left_us < play_.min_headroom_us) {
            play_.min_headroom_us = left_us;
        }
    }

    // After the write returned: every byte is queued now, so the DMA holds
    // what it had plus this block, less what drained while the write waited,
    // and never more than it can.
    void queued(std::size_t bytes) {
        const std::int64_t returned_us = esp_timer_get_time();
        std::int64_t queued_bytes = queue_bytes_ + static_cast<std::int64_t>(bytes) -
                                    us_to_bytes(returned_us - arrived_us_);
        if (queued_bytes > capacity_) {
            queued_bytes = capacity_;
        }
        if (queued_bytes < 0) {
            queued_bytes = 0;
        }
        queue_bytes_ = queued_bytes;
        stamp_us_ = returned_us;
        ++play_.writes;
        ++writes_;
    }

    // Blocks since open(), across every play: the seam's sink_frames_written().
    [[nodiscard]] std::uint64_t writes() const { return writes_; }
    [[nodiscard]] std::int64_t capacity_ms() const { return bytes_to_us(capacity_) / 1000; }

    // What IS visible from this side of the wire: whether the samples got
    // there in time, this play.
    void report() const {
        const std::int64_t least_ms = play_.min_headroom_us < 0 ? -1 : play_.min_headroom_us / 1000;
        std::printf("sink.writes=%lu sink.underruns=%lu sink.dry_ms=%lu sink.min_headroom_ms=%ld "
                    "sink.dma_ms=%ld\n",
                    static_cast<unsigned long>(play_.writes),
                    static_cast<unsigned long>(play_.underruns),
                    static_cast<unsigned long>(play_.dry_us / 1000), static_cast<long>(least_ms),
                    static_cast<long>(capacity_ms()));
    }

   private:
    // What report() prints: one play's, since restart() or open().
    struct Play {
        std::uint64_t writes = 0;
        std::uint64_t underruns = 0;        // blocks that arrived to an empty DMA
        std::uint64_t dry_us = 0;           // how long it had been empty, summed
        std::int64_t min_headroom_us = -1;  // least queued as a block arrived; -1 until then
    };

    [[nodiscard]] std::int64_t bytes_to_us(std::int64_t bytes) const {
        return bytes_per_second_ == 0
                   ? 0
                   : (bytes * 1000000) / static_cast<std::int64_t>(bytes_per_second_);
    }
    [[nodiscard]] std::int64_t us_to_bytes(std::int64_t us) const {
        return (us * static_cast<std::int64_t>(bytes_per_second_)) / 1000000;
    }

    std::uint32_t bytes_per_second_ = 0;
    std::int64_t capacity_ = 0;
    std::int64_t queue_bytes_ = 0;  // what the DMA held when the last write returned
    std::int64_t stamp_us_ = 0;     // and when that was
    std::int64_t arrived_us_ = 0;   // when the current block arrived
    std::uint64_t writes_ = 0;      // since open(), across every play
    Play play_;
};

}  // namespace player
