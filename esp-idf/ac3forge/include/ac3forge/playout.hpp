#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>

// When a sample leaves the audio port, and how a Sendspin player makes that
// the time its server asked for (planning/hearth-reference-player.md, B3;
// planning/hearth-sendspin-extension.md, Timing and row R9).
//
// Three parts, free of ESP-IDF like dac_queue_model.hpp beside them, so that
// the arithmetic runs on the host (tests/io/test_playout.cpp):
//
//   DmaRing and DmaClock, which say when a buffer written to an ESP32 I2S
//   channel plays, from the channel's own end-of-frame interrupts;
//   VirtualDac, which paces and times a sink that has no peripheral (the
//   capture sink under QEMU) as a DMA ring would;
//   Playout, which takes decoded frames with the time each should play and
//   writes whole blocks to a sink, padding with silence, skipping, and
//   dropping or repeating single frames so that each frame plays when it
//   should. Corrections touch decoded PCM only, never a burst.
//
// Times are local microseconds (esp_timer on a board).

namespace ac3forge {

// --- The DMA ring's clock ------------------------------------------------------
//
// ESP-IDF v6.1's I2S TX channel transmits a ring of `descriptors` DMA buffers,
// one after another, whether or not anything was written to them: a buffer
// nobody wrote plays as zeros when the channel clears buffers after sending
// them, as the example's does. Each buffer that finishes raises an
// end-of-frame interrupt and joins a queue of buffers free to write, which
// holds descriptors - 1 of them and drops its oldest when full;
// i2s_channel_write takes the oldest, and waits for one when the queue is
// empty. So a buffer that joined the queue at the k-th end-of-frame plays
// again when the ring comes round to it: from the (k + descriptors - 1)-th
// end-of-frame to the next, and every end-of-frame fires one buffer's length
// after the one before, by the part's own clock
// (components/esp_driver_i2s/i2s_common.c, i2s_dma_tx_callback and
// i2s_channel_write).
//
// DmaRing is the interrupt's side: the channel's on_sent callback records each
// end-of-frame. DmaClock is the writer's: after each write of whole buffers it
// works out which end-of-frames' buffers the driver took, and from a line
// fitted through the end-of-frame times, when they play.

struct DmaRecord {
    std::uint32_t index = 0;
    std::int64_t time_us = 0;
};

class DmaRing {
   public:
    // The driver's own ceiling on descriptors.
    static constexpr std::size_t kCapacity = 64;

    // A new channel: counting starts again. Not while the channel runs.
    void reset() { head_.store(0, std::memory_order_relaxed); }

    // The interrupt's side, from the channel's on_sent callback: a buffer
    // finished at `now_us`. Plain loads and stores, and nothing that waits.
    // A channel whose interrupt runs while the flash cache is off
    // (CONFIG_I2S_ISR_IRAM_SAFE) would need this in IRAM as well.
    void sent(std::int64_t now_us) {
        const std::uint32_t head = head_.load(std::memory_order_relaxed);
        records_[head % kCapacity] = DmaRecord{.index = head, .time_us = now_us};
        head_.store(head + 1, std::memory_order_relaxed);
    }

    // The writer's side. End-of-frames so far.
    [[nodiscard]] std::uint32_t count() const { return head_.load(std::memory_order_relaxed); }
    // The record of end-of-frame `index`, which must be among the last
    // kCapacity.
    [[nodiscard]] DmaRecord at(std::uint32_t index) const { return records_[index % kCapacity]; }

   private:
    std::array<DmaRecord, kCapacity> records_{};
    std::atomic<std::uint32_t> head_{0};
};

class DmaClock {
   public:
    struct Taken {
        // When the first buffer just written starts to play.
        std::int64_t play_us = 0;
        // The write returned after that: some of it may have played as
        // whatever the buffer held before.
        bool late = false;
        // Buffers the ring sent since the last write that no write took:
        // the queue was full and dropped them, and they played as silence.
        std::uint32_t skipped = 0;
    };

    // A channel (re)opened with `descriptors` buffers of `frames` frames each
    // at `sample_rate`. The ring's counting starts again with it.
    void open(std::uint32_t descriptors, std::uint32_t frames, std::uint32_t sample_rate) {
        descriptors_ = std::max<std::uint32_t>(descriptors, 2);
        nominal_q16_ = (static_cast<std::int64_t>(frames) * 1'000'000 << 16) / std::max<std::uint32_t>(sample_rate, 1);
        period_q16_ = nominal_q16_;
        taken_ = false;
        fitted_ = false;
    }

    [[nodiscard]] std::int64_t buffer_us() const { return period_q16_ >> 16; }
    // The least time between a write and its buffer starting to play.
    [[nodiscard]] std::int64_t latency_us() const {
        return (static_cast<std::int64_t>(descriptors_ - 1) * period_q16_) >> 16;
    }

    // After a write of exactly `buffers` whole buffers returned at `now_us`:
    // which end-of-frames' buffers the driver took for it, and when the first
    // plays. Nothing until the ring has sent a buffer.
    //
    // The driver takes one buffer at a time, the oldest in its queue, and the
    // takes of one write are consecutive. The queue holds the end-of-frames
    // since the last take, the newest descriptors - 1 of them at most, so the
    // first buffer taken is the one after the last write's, or the oldest a
    // full queue holds if the ring has sent more than that since; for a write
    // of more buffers than the queue holds, the takes past the queue wait for
    // the ring, which moves the newest end-of-frame on by as many. Worked out
    // after the write: an end-of-frame that fires during a write that found
    // the queue full makes the answer one buffer later than the truth, for a
    // write whose first buffer started playing while it was written. A later
    // write that waits for its buffers takes the newest there are, which
    // brings the answer back.
    [[nodiscard]] std::optional<Taken> took(const DmaRing& ring, std::int64_t now_us, std::uint32_t buffers = 1) {
        const std::uint32_t count = ring.count();
        if (count == 0 || buffers == 0) {
            return std::nullopt;
        }
        const std::uint32_t latest = count - 1;
        const std::uint32_t queue = descriptors_ - 1;
        const std::uint32_t reach = std::max(queue, buffers) - 1;
        std::uint32_t first = latest >= reach ? latest - reach : 0;
        std::uint32_t skipped = 0;
        if (taken_) {
            if (last_ + 1 >= first) {
                first = last_ + 1;
            } else {
                skipped = first - (last_ + 1);
            }
        }
        if (first + (buffers - 1) > latest) {
            // Buffers the counter has not seen sent yet cannot have been
            // taken: the newest there are is the answer.
            first = latest >= buffers - 1 ? latest - (buffers - 1) : 0;
        }
        taken_ = true;
        last_ = first + (buffers - 1);
        fit(ring.at(first));
        const std::int64_t play = eof_us(first + queue);
        return Taken{.play_us = play, .late = now_us > play, .skipped = skipped};
    }

    // When end-of-frame `index` fires, by the fitted line.
    [[nodiscard]] std::int64_t eof_us(std::uint32_t index) const {
        const std::int64_t n = static_cast<std::int64_t>(index) - static_cast<std::int64_t>(base_index_);
        return (base_q8_ + ((n * period_q16_) >> 8)) >> 8;
    }

   private:
    // A line through the end-of-frame times, in 1/256 us. It moves to each
    // end-of-frame recorded and a sixteenth of the way towards its time, so
    // that one interrupt's lateness moves it little, and a very late one no
    // more than a limit's worth. Its period is the nominal one until the
    // line has run long enough to measure it, and then the slope from where
    // the line began to where it is now.
    void fit(const DmaRecord& record) {
        const std::int64_t time_q8 = record.time_us * 256;
        if (!fitted_) {
            fitted_ = true;
            base_index_ = anchor_index_ = record.index;
            base_q8_ = anchor_q8_ = time_q8;
            period_q16_ = nominal_q16_;
            return;
        }
        const std::int64_t n = static_cast<std::int64_t>(record.index) - static_cast<std::int64_t>(base_index_);
        base_q8_ += (n * period_q16_) >> 8;
        base_index_ = record.index;
        base_q8_ += std::clamp<std::int64_t>(time_q8 - base_q8_, -kMaxStepQ8, kMaxStepQ8) / 16;
        const std::int64_t span = static_cast<std::int64_t>(record.index) - static_cast<std::int64_t>(anchor_index_);
        if (span >= kMinBaseline) {
            const std::int64_t measured = ((base_q8_ - anchor_q8_) << 8) / span;
            // A crystal and a clock divider are within a few hundred parts
            // per million of nominal. Further than 0.1% is not the clock.
            if (std::llabs(measured - nominal_q16_) * 1000 < nominal_q16_) {
                period_q16_ = measured;
            }
        }
    }

    // Buffers before the period is measured: 1,024 of 128 frames is 2.7 s,
    // over which a millisecond's lateness in the first interrupt moves the
    // period by a microsecond.
    static constexpr std::int64_t kMinBaseline = 1024;
    // The most one end-of-frame's time pulls the line by, before the
    // sixteenth: half a millisecond.
    static constexpr std::int64_t kMaxStepQ8 = 500 * 256;

    std::uint32_t descriptors_ = 2;
    std::int64_t nominal_q16_ = 0;
    std::int64_t period_q16_ = 0;
    bool taken_ = false;
    std::uint32_t last_ = 0;
    bool fitted_ = false;
    std::uint32_t base_index_ = 0;
    std::int64_t base_q8_ = 0;
    std::uint32_t anchor_index_ = 0;
    std::int64_t anchor_q8_ = 0;
};

// --- A DAC that is not there ----------------------------------------------------
//
// The capture and null sinks have no peripheral, so nothing paces them and
// nothing says when their samples would have played. This does both as a DMA
// ring of `descriptors` buffers would: its buffers go out one after another
// from the first write, which plays a queue's length later; a write returns
// once the buffer before it has started to play, a queue's length ahead; and
// a write after the ring has run dry plays at the next buffer's start. Under
// QEMU the times are the emulator's, which is what a play-time report there
// measures.
class VirtualDac {
   public:
    struct Write {
        std::int64_t play_us = 0;
        // When the write would have returned: the caller waits until then.
        std::int64_t return_us = 0;
        // The queue had run dry before it: silence played that nobody wrote.
        bool gap = false;
    };

    void open(std::uint32_t descriptors, std::uint32_t frames, std::uint32_t sample_rate) {
        buffer_us_ = (static_cast<std::int64_t>(frames) * 1'000'000) / std::max<std::uint32_t>(sample_rate, 1);
        queue_us_ = static_cast<std::int64_t>(std::max<std::uint32_t>(descriptors, 2) - 1) * buffer_us_;
        next_ = 0;
        started_ = false;
    }

    [[nodiscard]] std::int64_t latency_us() const { return queue_us_; }

    // One buffer arriving at `now_us`.
    [[nodiscard]] Write write(std::int64_t now_us) {
        bool gap = false;
        if (!started_) {
            next_ = now_us + queue_us_;
            started_ = true;
        } else if (next_ < now_us) {
            // The ring ran dry and played zeros: this buffer is the next to
            // start.
            gap = true;
            next_ += ((now_us - next_ + buffer_us_ - 1) / buffer_us_) * buffer_us_;
        }
        const Write out{.play_us = next_, .return_us = std::max(now_us, next_ - queue_us_), .gap = gap};
        next_ += buffer_us_;
        return out;
    }

   private:
    std::int64_t buffer_us_ = 0;
    std::int64_t queue_us_ = 0;
    std::int64_t next_ = 0;
    bool started_ = false;
};

// --- The scheduler -----------------------------------------------------------------

struct PlayoutWrite {
    // When the block's first frame leaves the audio port.
    std::int64_t play_us = 0;
    // The block was written too late to play whole.
    bool late = false;
    // The sink ran dry before the block: silence played that nobody wrote.
    bool gap = false;
};

// Where Playout writes. write() takes exactly Playout::kBlockFrames frames per
// output and blocks until the sink has them; it says when they play, or
// nothing for a sink that cannot say, which Playout then plays as it comes.
class PlayoutSink {
   public:
    PlayoutSink() = default;
    virtual ~PlayoutSink() = default;
    PlayoutSink(const PlayoutSink&) = delete;
    PlayoutSink& operator=(const PlayoutSink&) = delete;

    [[nodiscard]] virtual std::optional<PlayoutWrite> write(std::span<const std::span<const float>> outputs) = 0;
};

struct PlayoutTuning {
    std::uint32_t sample_rate = 48000;
    // No correction while the smoothed error is inside this.
    std::int64_t deadband_us = 250;
    // At most one frame dropped or repeated per this many: 256 frames is
    // 0.39% of the rate, inside row R9's 0.5%.
    std::uint32_t slew_interval = 256;
    // Beyond this the error is fixed at once, by padding or skipping, as at
    // the start of a stream.
    std::int64_t resync_us = 20'000;
    // Blocks of silence written ahead of a stream at most, to find out where
    // the sink is before the first frame is placed.
    std::uint32_t prime_blocks = 16;
};

struct PlayoutStats {
    std::uint64_t blocks = 0;
    std::uint64_t stream_frames = 0;    // decoded frames written
    std::uint64_t silence_frames = 0;   // padding
    std::uint64_t skipped_frames = 0;   // decoded frames never written: late
    std::uint64_t dropped_frames = 0;   // single frames left out to catch up
    std::uint64_t repeated_frames = 0;  // single frames written twice to wait
    std::uint32_t resyncs = 0;          // times a placed stream was placed again
    // Blocks of the stream the sink took too late to play whole, and the
    // breaks in what is heard: a late block of the stream, or the sink
    // running dry after one. Silence running dry into silence, as before a
    // stream, is neither.
    std::uint64_t late_blocks = 0;
    std::uint64_t underruns = 0;
    // The last error measured, and the smoothed one corrections follow: how
    // much later a frame played than it should have, negative when early.
    std::int64_t error_us = 0;
    std::int64_t smoothed_error_us = 0;
    // The largest error measured once the stream had settled (after its
    // first second), either way.
    std::int64_t worst_error_us = 0;
    // The last stream frame written with a known play time: its place in the
    // stream, when it plays, and when it should have.
    std::uint64_t last_frame = 0;
    std::int64_t last_play_us = 0;
    std::int64_t last_target_us = 0;
    bool have_last = false;
};

class Playout {
   public:
    // The block a sink is written in: the player's 256-frame block, which
    // every DMA plan here divides.
    static constexpr std::size_t kBlockFrames = 256;
    static constexpr std::size_t kMaxOutputs = 16;

    using Tuning = PlayoutTuning;
    using Stats = PlayoutStats;

    // `storage` holds kBlockFrames floats per output, for up to `capacity`
    // outputs; the playout writes `capacity` of them until set_outputs() says
    // otherwise.
    Playout(std::span<float> storage, std::size_t capacity, Tuning tuning = {}) : tuning_(tuning) {
        capacity_ = std::min({capacity, kMaxOutputs, storage.size() / kBlockFrames});
        for (std::size_t o = 0; o < capacity_; ++o) {
            staging_[o] = storage.subspan(o * kBlockFrames, kBlockFrames);
            views_[o] = staging_[o];
        }
        outputs_ = capacity_;
        restart();
    }

    [[nodiscard]] std::size_t outputs() const { return outputs_; }

    // The sink was opened for `outputs` outputs: blocks carry that many from
    // here on. What is staged is dropped.
    void set_outputs(std::size_t outputs) {
        outputs_ = std::min(outputs, capacity_);
        drop_staged();
    }
    [[nodiscard]] const Stats& stats() const { return stats_; }
    [[nodiscard]] const Tuning& tuning() const { return tuning_; }

    // A new stream: nothing is placed yet, and the figures start again. What
    // is staged is dropped.
    void restart() {
        realign();
        slew_ = 0;
        since_slew_ = 0;
        measured_ = false;
        settled_after_ = 0;
        stats_ = Stats{};
    }

    // The sink was opened again, so where its blocks play is not known any
    // more: what is staged is dropped, and the next frame is placed afresh.
    // The figures carry on.
    void realign() {
        drop_staged();
        aligned_ = false;
        have_next_ = false;
        fresh_ = false;
        after_stream_ = false;
        pad_ = 0;
        skip_ = 0;
    }

    // When a frame staged now would play at the soonest, once the sink has
    // said where its blocks play; nothing before. A player uses it to drop a
    // burst whose frames could not play in time anyway.
    [[nodiscard]] std::optional<std::int64_t> next_play_us(std::int64_t now_us) const {
        if (!have_next_) {
            return std::nullopt;
        }
        return std::max(next_block_play_ + offset_us(staged_), now_us);
    }

    // `count` decoded frames, one span per output each at least that long,
    // the first of which is frame `first_frame` of the stream and should play
    // at `target_us`; `now_us` is when they are handed over.
    void push(std::span<const std::span<const float>> frames, std::size_t count, std::int64_t target_us,
              std::uint64_t first_frame, std::int64_t now_us, PlayoutSink& sink) {
        std::size_t k = 0;
        while (k < count) {
            if (aligned_ && behind(now_us)) {
                // The sink has played past what was written to it: the
                // writer stalled. What is staged is late, and the stream is
                // placed again, which counts once the stream had been placed.
                drop_staged();
                aligned_ = false;
                stats_.resyncs += measured_ ? 1 : 0;
            }
            if (!aligned_) {
                align(target_us + offset_us(k), now_us, sink);
            }
            if (skip_ > 0) {
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(skip_, count - k));
                k += n;
                skip_ -= n;
                stats_.skipped_frames += n;
                continue;
            }
            if (pad_ > 0) {
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(pad_, kBlockFrames - staged_));
                stage_silence(n);
                pad_ -= n;
                if (staged_ == kBlockFrames) {
                    write_block(sink);
                }
                continue;
            }
            bool repeat = false;
            if (slew_ != 0 && ++since_slew_ >= tuning_.slew_interval) {
                since_slew_ = 0;
                if (slew_ > 0) {
                    // Late: this frame is left out, and the rest play one
                    // frame sooner.
                    ++k;
                    ++stats_.dropped_frames;
                    continue;
                }
                // Early: this frame goes out twice.
                repeat = true;
                ++stats_.repeated_frames;
            }
            if (!block_has_target_) {
                block_has_target_ = true;
                block_target_ = target_us + offset_us(k);
                block_target_at_ = staged_;
                block_frame_ = first_frame + k;
            }
            stage_frame(frames, k);
            if (!repeat) {
                ++k;
            }
            if (staged_ == kBlockFrames) {
                write_block(sink);
            }
        }
    }

    // What is staged, padded to a whole block and written, at `now_us`: the
    // end of a stream. Frames the sink has already played past are dropped
    // instead, and count as skipped. A server may end a stream well after its
    // last chunk has played, and a block written then would play late and be
    // measured as the stream's worst error.
    void flush(std::int64_t now_us, PlayoutSink& sink) {
        if (staged_ == 0) {
            return;
        }
        if (behind(now_us)) {
            drop_staged();
            return;
        }
        stage_silence(kBlockFrames - staged_);
        write_block(sink);
    }

   private:
    // Microseconds for `frames` frames, rounded.
    [[nodiscard]] std::int64_t offset_us(std::size_t frames) const {
        return ((static_cast<std::int64_t>(frames) * 1'000'000) + (tuning_.sample_rate / 2)) / tuning_.sample_rate;
    }
    // Frames for `us` microseconds, rounded; 0 for none.
    [[nodiscard]] std::uint64_t frames_for(std::int64_t us) const {
        if (us <= 0) {
            return 0;
        }
        return static_cast<std::uint64_t>(((us * tuning_.sample_rate) + 500'000) / 1'000'000);
    }

    // The block the sink would take next has started to play already.
    [[nodiscard]] bool behind(std::int64_t now_us) const { return have_next_ && next_block_play_ <= now_us; }

    void stage_frame(std::span<const std::span<const float>> frames, std::size_t k) {
        for (std::size_t o = 0; o < outputs_; ++o) {
            staging_[o][staged_] = o < frames.size() && k < frames[o].size() ? frames[o][k] : 0.0F;
        }
        ++staged_;
        ++staged_stream_;
    }

    void stage_silence(std::size_t n) {
        for (std::size_t o = 0; o < outputs_; ++o) {
            std::fill_n(staging_[o].begin() + static_cast<std::ptrdiff_t>(staged_), n, 0.0F);
        }
        staged_ += n;
    }

    // What is staged goes unwritten: its stream frames count as skipped.
    void drop_staged() {
        stats_.skipped_frames += staged_stream_;
        staged_ = 0;
        staged_stream_ = 0;
        block_has_target_ = false;
    }

    // Where the stream's next frame goes: after silence until its target, or
    // with the frames that are already late left out.
    void align(std::int64_t target_us, std::int64_t now_us, PlayoutSink& sink) {
        aligned_ = true;
        slew_ = 0;
        since_slew_ = 0;
        measured_ = false;
        if (!have_next_ || !fresh_ || behind(now_us)) {
            prime(now_us, sink);
        }
        if (!have_next_) {
            return;  // a sink that says nothing: play as it comes
        }
        const std::int64_t delta = target_us - (next_block_play_ + offset_us(staged_));
        if (delta >= 0) {
            pad_ = frames_for(delta);
        } else {
            skip_ = frames_for(-delta);
        }
    }

    // Blocks of silence until the sink says where a block plays, and that it
    // is not late: after an idle spell or a stall a DMA ring's queue holds
    // buffers already about to play, which a write takes first. Nothing is
    // staged when it begins.
    void prime(std::int64_t now_us, PlayoutSink& sink) {
        for (std::uint32_t b = 0; b < tuning_.prime_blocks && (!have_next_ || !fresh_ || behind(now_us)); ++b) {
            stage_silence(kBlockFrames - staged_);
            write_block(sink);
            if (!sink_says_) {
                break;
            }
        }
    }

    void write_block(PlayoutSink& sink) {
        const std::optional<PlayoutWrite> written =
            sink.write(std::span<const std::span<const float>>(views_.data(), outputs_));
        ++stats_.blocks;
        stats_.stream_frames += staged_stream_;
        stats_.silence_frames += staged_ - staged_stream_;
        sink_says_ = written.has_value();
        if (written) {
            // A break in what is heard: the sink ran dry after a block of the
            // stream, or a block of it went out late. Silence that runs dry
            // into silence is not one.
            const bool broken = written->late || written->gap;
            stats_.underruns += broken && (after_stream_ || block_has_target_) ? 1 : 0;
            stats_.late_blocks += written->late && block_has_target_ ? 1 : 0;
            have_next_ = true;
            fresh_ = !written->late;
            next_block_play_ = written->play_us + offset_us(kBlockFrames);
            if (block_has_target_) {
                const std::int64_t play = written->play_us + offset_us(block_target_at_);
                measure(play - block_target_, play, broken);
            }
        } else {
            have_next_ = false;
        }
        after_stream_ = block_has_target_;
        staged_ = 0;
        staged_stream_ = 0;
        block_has_target_ = false;
    }

    void measure(std::int64_t error, std::int64_t play_us, bool broken) {
        stats_.error_us = error;
        stats_.last_frame = block_frame_;
        stats_.last_play_us = play_us;
        stats_.last_target_us = block_target_;
        stats_.have_last = true;
        if (!measured_) {
            measured_ = true;
            smoothed_ = error;
            settled_after_ = stats_.stream_frames + tuning_.sample_rate;
        } else {
            smoothed_ += (error - smoothed_) / 4;
        }
        stats_.smoothed_error_us = smoothed_;
        if (stats_.stream_frames >= settled_after_ && std::llabs(error) > std::llabs(stats_.worst_error_us)) {
            stats_.worst_error_us = error;
        }
        // Fixed at once, by silence or by leaving late frames out: past the
        // resync limit, or past the deadband where what is heard broke
        // anyway.
        if (std::llabs(error) > (broken ? tuning_.deadband_us : tuning_.resync_us)) {
            ++stats_.resyncs;
            aligned_ = false;
            return;
        }
        if (smoothed_ > tuning_.deadband_us) {
            slew_ = 1;
        } else if (smoothed_ < -tuning_.deadband_us) {
            slew_ = -1;
        } else if (std::llabs(smoothed_) < tuning_.deadband_us / 2) {
            slew_ = 0;
        }
    }

    Tuning tuning_;
    std::size_t capacity_ = 0;
    std::size_t outputs_ = 0;
    std::array<std::span<float>, kMaxOutputs> staging_{};
    std::array<std::span<const float>, kMaxOutputs> views_{};
    std::size_t staged_ = 0;
    std::size_t staged_stream_ = 0;

    bool block_has_target_ = false;
    std::int64_t block_target_ = 0;
    std::size_t block_target_at_ = 0;
    std::uint64_t block_frame_ = 0;

    bool aligned_ = false;
    std::uint64_t pad_ = 0;
    std::uint64_t skip_ = 0;
    int slew_ = 0;
    std::uint32_t since_slew_ = 0;

    bool have_next_ = false;
    bool fresh_ = false;
    bool sink_says_ = false;
    std::int64_t next_block_play_ = 0;

    bool after_stream_ = false;
    bool measured_ = false;
    std::int64_t smoothed_ = 0;
    std::uint64_t settled_after_ = 0;
    Stats stats_;
};

}  // namespace ac3forge
