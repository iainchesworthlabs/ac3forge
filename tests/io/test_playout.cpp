// The Sendspin player's playout on the host: the DMA ring's clock, the DAC
// that is not there, and the scheduler that places decoded frames in time
// (esp-idf/ac3forge/include/ac3forge/playout.hpp).
//
// The header includes nothing from ESP-IDF, the arrangement
// test_dac_queue_model.cpp has. The I2S channel below is ESP-IDF v6.1's as
// components/esp_driver_i2s/i2s_common.c has it, and it knows what the clock
// is trying to find out: which ring buffer each frame went into, and so when
// the frame played. Every frame a stream writes carries its own number, so a
// run checks every frame's play time against the time it was due.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "ac3forge/playout.hpp"

namespace {

using ac3forge::DmaClock;
using ac3forge::DmaRing;
using ac3forge::Playout;
using ac3forge::PlayoutSink;
using ac3forge::PlayoutWrite;
using ac3forge::VirtualDac;

constexpr std::uint32_t kRate = 48'000;
constexpr std::size_t kBlock = Playout::kBlockFrames;
constexpr std::uint64_t kSecond = kRate;
// A 128-frame buffer at 48 kHz, in microseconds.
constexpr double kBufferUs = 128.0 * 1e6 / kRate;

// When frame `frame` of a stream is due, counted from its first.
[[nodiscard]] std::int64_t frames_us(std::uint64_t frame) {
    return static_cast<std::int64_t>(((frame * 1'000'000) + (kRate / 2)) / kRate);
}

// A frame's number as the samples of two outputs: each float holds an
// integer exactly up to 2^24, and a ten-minute stream has more frames than
// that. 0 on the first output is silence.
constexpr std::uint64_t kLowBits = 22;
[[nodiscard]] std::array<float, 2> encode(std::uint64_t frame) {
    return {static_cast<float>((frame & ((1U << kLowBits) - 1)) + 1),
            static_cast<float>((frame >> kLowBits) + 1)};
}
[[nodiscard]] std::optional<std::uint64_t> decode(float low, float high) {
    if (low == 0.0F) {
        return std::nullopt;
    }
    return ((static_cast<std::uint64_t>(high) - 1) << kLowBits) | (static_cast<std::uint64_t>(low) - 1);
}

// What the frames of one stream did, as they played.
struct Truth {
    // When the stream's first frame was due.
    std::int64_t first_target_us = 0;
    // Errors before this frame are left out of `worst_us` and `off_frames`.
    std::uint64_t settled_from = kSecond;

    std::uint64_t frames = 0;
    std::optional<std::int64_t> first_error_us;
    std::int64_t worst_us = 0;
    // Settled frames that played more than a millisecond from their time.
    std::uint64_t off_frames = 0;
    // A frame further on from the one before than a dropped frame accounts
    // for, or before it: a skip or a resync.
    std::uint64_t jumps = 0;
    std::optional<std::uint64_t> last_frame;

    void played(std::uint64_t frame, std::int64_t play_us) {
        const std::int64_t error = play_us - (first_target_us + frames_us(frame));
        ++frames;
        if (!first_error_us) {
            first_error_us = error;
        }
        if (frame >= settled_from) {
            worst_us = std::max(worst_us, std::llabs(error));
            off_frames += std::llabs(error) > 1'000 ? 1 : 0;
        }
        if (last_frame && (frame > *last_frame + 2 || frame < *last_frame)) {
            ++jumps;
        }
        last_frame = frame;
    }
};

// ESP-IDF v6.1's I2S TX channel: a ring of `descriptors` buffers of `frames`
// frames sent one after another from `enabled_us`; an interrupt when each
// finishes, which puts the buffer on a queue of buffers free to write that
// holds descriptors - 1 and drops its oldest when full, and calls on_sent; and
// a write that takes the oldest buffer and waits for the ring when there is
// none. The ring's clock runs `ppm` fast of the writer's, and each interrupt
// runs up to `jitter_us` after its buffer finished.
class SimulatedI2s final : public PlayoutSink {
   public:
    struct Options {
        std::uint32_t descriptors = 8;
        std::uint32_t frames = 128;
        double ppm = 0.0;
        std::int64_t jitter_us = 0;
        std::int64_t enabled_us = 1'000'000;
        // The writer's time in a write, copying into the buffers.
        std::int64_t copy_us = 40;
    };

    explicit SimulatedI2s(const Options& options) : options_(options), now_(options.enabled_us) {
        clock_.open(options.descriptors, options.frames, kRate);
    }

    [[nodiscard]] std::int64_t now() const { return now_; }
    // The writer does something else for `us`: decodes, or stalls.
    void idle(std::int64_t us) { advance(now_ + us); }

    Truth truth;
    [[nodiscard]] const DmaClock& clock() const { return clock_; }
    [[nodiscard]] std::uint64_t silence_frames() const { return silence_frames_; }
    // When the ring's end-of-frame `index` fired, by the ring's own clock.
    [[nodiscard]] std::int64_t eof_true(std::uint64_t index) const {
        return options_.enabled_us + std::llround(static_cast<double>(index + 1) * period_us());
    }

    [[nodiscard]] std::optional<PlayoutWrite> write(std::span<const std::span<const float>> outputs) override {
        const auto buffers = static_cast<std::uint32_t>(kBlock / options_.frames);
        std::uint64_t first = 0;
        for (std::uint32_t b = 0; b < buffers; ++b) {
            if (queue_.empty()) {
                advance(interrupt_us(next_eof_));
            }
            if (b == 0) {
                first = queue_.front();
            }
            queue_.pop_front();
        }
        advance(now_ + options_.copy_us);
        // The buffer of end-of-frame `first` plays when the ring comes round
        // to it: from end-of-frame first + descriptors - 1.
        const std::int64_t start = eof_true(first + options_.descriptors - 1);
        const double frame_us = period_us() / options_.frames;
        for (std::size_t k = 0; k < kBlock; ++k) {
            const std::optional<std::uint64_t> frame =
                outputs.size() >= 2 ? decode(outputs[0][k], outputs[1][k]) : std::nullopt;
            if (!frame) {
                ++silence_frames_;
                continue;
            }
            truth.played(*frame, start + std::llround(static_cast<double>(k) * frame_us));
        }
        const std::optional<DmaClock::Taken> taken = clock_.took(ring_, now_, buffers);
        if (!taken) {
            return std::nullopt;
        }
        return PlayoutWrite{.play_us = taken->play_us, .late = taken->late, .gap = taken->skipped > 0};
    }

   private:
    [[nodiscard]] double period_us() const {
        return static_cast<double>(options_.frames) * 1e6 / kRate / (1.0 + (options_.ppm * 1e-6));
    }
    // When the interrupt for end-of-frame `index` runs: a fixed spread of
    // lateness, the same on every run.
    [[nodiscard]] std::int64_t interrupt_us(std::uint64_t index) const {
        const std::uint64_t spread = static_cast<std::uint64_t>(options_.jitter_us) + 1;
        return eof_true(index) + static_cast<std::int64_t>((index * 2'654'435'761U) % spread);
    }
    // Every interrupt due by `to` runs.
    void advance(std::int64_t to) {
        while (interrupt_us(next_eof_) <= to) {
            ring_.sent(interrupt_us(next_eof_));
            if (queue_.size() == options_.descriptors - 1) {
                queue_.pop_front();
            }
            queue_.push_back(next_eof_);
            ++next_eof_;
        }
        now_ = std::max(now_, to);
    }

    Options options_;
    std::int64_t now_ = 0;
    DmaRing ring_;
    DmaClock clock_;
    std::deque<std::uint64_t> queue_;
    std::uint64_t next_eof_ = 0;
    std::uint64_t silence_frames_ = 0;
};

// A stream of `frames` frames, decoded a block at a time by a decoder that
// takes `decode_us` a block and pushed to `playout`, the first due at
// `first_target_us`. `between(frame)` runs before the block starting at
// `frame`, for a test to stall the writer.
template <typename Between>
void play(Playout& playout, SimulatedI2s& sink, std::int64_t first_target_us, std::uint64_t frames,
          std::int64_t decode_us, Between between) {
    std::array<std::array<float, kBlock>, 2> samples{};
    std::array<std::span<const float>, 2> views{};
    sink.truth.first_target_us = first_target_us;
    for (std::uint64_t f = 0; f < frames; f += kBlock) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(kBlock, frames - f));
        for (std::size_t k = 0; k < n; ++k) {
            const std::array<float, 2> value = encode(f + k);
            samples[0][k] = value[0];
            samples[1][k] = value[1];
        }
        views[0] = std::span<const float>(samples[0].data(), n);
        views[1] = std::span<const float>(samples[1].data(), n);
        between(f);
        sink.idle(decode_us);
        playout.push(views, n, first_target_us + frames_us(f), f, sink.now(), sink);
    }
    playout.flush(sink);
}

void play(Playout& playout, SimulatedI2s& sink, std::int64_t first_target_us, std::uint64_t frames,
          std::int64_t decode_us) {
    play(playout, sink, first_target_us, frames, decode_us, [](std::uint64_t) {});
}

// A playout for two outputs, with its own staging.
struct TwoOutputs {
    std::vector<float> storage = std::vector<float>(2 * kBlock);
    Playout playout{storage, 2};
};

// A sink that keeps what it was written and says what it is told to: that
// its blocks play one after another from `first_play_us`, and, for the next
// write only, that it was late or ran dry before it. Nothing, with no
// `first_play_us`.
class ScriptedSink final : public PlayoutSink {
   public:
    std::optional<std::int64_t> first_play_us;
    bool late_next = false;
    bool gap_next = false;
    std::vector<std::size_t> output_counts;
    std::vector<float> first_output;

    [[nodiscard]] std::optional<PlayoutWrite> write(std::span<const std::span<const float>> outputs) override {
        const std::uint64_t written = output_counts.size();
        output_counts.push_back(outputs.size());
        first_output.insert(first_output.end(), outputs[0].begin(), outputs[0].end());
        if (!first_play_us) {
            return std::nullopt;
        }
        const PlayoutWrite answer{.play_us = *first_play_us + frames_us(written * kBlock),
                                  .late = late_next,
                                  .gap = gap_next};
        late_next = false;
        gap_next = false;
        return answer;
    }
};

}  // namespace

// --- DmaClock ------------------------------------------------------------------

TEST_CASE("the DMA clock places a fresh channel's first buffer a queue after its own", "[io][playout]") {
    DmaRing ring;
    DmaClock clock;
    clock.open(8, 128, kRate);
    // A fresh channel's queue is empty, so a write of two buffers waits for
    // the first two end-of-frames and takes them.
    ring.sent(1'002'667);
    ring.sent(1'005'333);
    const std::optional<DmaClock::Taken> taken = clock.took(ring, 1'005'400, 2);
    REQUIRE(taken);
    // End-of-frame 0's buffer plays from end-of-frame 7, seven buffers on.
    CHECK(std::llabs(taken->play_us - (1'002'667 + std::llround(7 * kBufferUs))) <= 1);
    CHECK_FALSE(taken->late);
    CHECK(taken->skipped == 0);
    CHECK(clock.latency_us() == 18'666);
}

TEST_CASE("the DMA clock says nothing before the ring has sent a buffer", "[io][playout]") {
    DmaRing ring;
    DmaClock clock;
    clock.open(8, 128, kRate);
    CHECK_FALSE(clock.took(ring, 1'000'000, 2).has_value());
}

TEST_CASE("the DMA clock takes the oldest buffer a full queue holds after an idle spell", "[io][playout]") {
    DmaRing ring;
    DmaClock clock;
    clock.open(8, 128, kRate);
    const auto eof = [](std::uint64_t k) { return 1'000'000 + std::llround(static_cast<double>(k + 1) * kBufferUs); };
    ring.sent(eof(0));
    ring.sent(eof(1));
    REQUIRE(clock.took(ring, eof(1) + 40, 2));
    // Nothing written through end-of-frame 99: the queue holds 93 to 99, and
    // dropped 2 to 92 as they aged out, which played as silence.
    for (std::uint64_t k = 2; k <= 99; ++k) {
        ring.sent(eof(k));
    }
    const std::optional<DmaClock::Taken> taken = clock.took(ring, eof(99) + 40, 2);
    REQUIRE(taken);
    CHECK(taken->skipped == 91);
    // End-of-frame 93's buffer is the next to play, at end-of-frame 100.
    CHECK(std::llabs(taken->play_us - eof(100)) <= 2);
    CHECK_FALSE(taken->late);
}

TEST_CASE("the DMA clock follows a writer that waits for each buffer", "[io][playout]") {
    DmaRing ring;
    DmaClock clock;
    clock.open(8, 128, kRate);
    const auto eof = [](std::uint64_t k) { return 1'000'000 + std::llround(static_cast<double>(k + 1) * kBufferUs); };
    for (std::uint64_t k = 0; k < 200; k += 2) {
        ring.sent(eof(k));
        ring.sent(eof(k + 1));
        const std::optional<DmaClock::Taken> taken = clock.took(ring, eof(k + 1) + 40, 2);
        REQUIRE(taken);
        CHECK(std::llabs(taken->play_us - eof(k + 7)) <= 2);
        CHECK(taken->skipped == 0);
        CHECK_FALSE(taken->late);
    }
}

TEST_CASE("the DMA clock comes back from an end-of-frame during a write to a full queue", "[io][playout]") {
    DmaRing ring;
    DmaClock clock;
    clock.open(8, 128, kRate);
    const auto eof = [](std::uint64_t k) { return 1'000'000 + std::llround(static_cast<double>(k + 1) * kBufferUs); };
    for (std::uint64_t k = 0; k <= 20; ++k) {
        ring.sent(eof(k));
    }
    // The write takes 14 and 15 from the full queue, and end-of-frame 21
    // fires before it returns: the clock cannot tell that from a write that
    // took 15 and 16, and says so, a buffer late. 14's buffer was playing by
    // then anyway.
    ring.sent(eof(21));
    const std::optional<DmaClock::Taken> first = clock.took(ring, eof(21) + 40, 2);
    REQUIRE(first);
    CHECK(std::llabs(first->play_us - eof(22)) <= 2);
    // The next writes take 16 to 21 from the queue without waiting, and the
    // last of them finds the clock's answer past the newest end-of-frame
    // there is, which is where it comes back.
    REQUIRE(clock.took(ring, eof(21) + 80, 2));
    REQUIRE(clock.took(ring, eof(21) + 120, 2));
    const std::optional<DmaClock::Taken> fourth = clock.took(ring, eof(21) + 160, 2);
    REQUIRE(fourth);
    CHECK(std::llabs(fourth->play_us - eof(27)) <= 2);
    ring.sent(eof(22));
    ring.sent(eof(23));
    const std::optional<DmaClock::Taken> fifth = clock.took(ring, eof(23) + 40, 2);
    REQUIRE(fifth);
    CHECK(std::llabs(fifth->play_us - eof(29)) <= 2);
    CHECK(fifth->skipped == 0);
}

TEST_CASE("the DMA clock does not count a write longer than the queue as a skip", "[io][playout]") {
    DmaRing ring;
    DmaClock clock;
    // Two descriptors hold one free buffer, and a block is two.
    clock.open(2, 128, kRate);
    const auto eof = [](std::uint64_t k) { return 1'000'000 + std::llround(static_cast<double>(k + 1) * kBufferUs); };
    for (std::uint64_t k = 0; k < 20; k += 2) {
        ring.sent(eof(k));
        ring.sent(eof(k + 1));
        const std::optional<DmaClock::Taken> taken = clock.took(ring, eof(k + 1) + 40, 2);
        REQUIRE(taken);
        CHECK(taken->skipped == 0);
        CHECK(std::llabs(taken->play_us - eof(k + 1)) <= 2);
    }
}

TEST_CASE("the DMA clock measures a ring running 300 ppm fast", "[io][playout]") {
    SimulatedI2s sink({.ppm = 300.0});
    std::vector<float> storage(2 * kBlock);
    Playout playout(storage, 2);
    play(playout, sink, sink.now() + 250'000, 10 * kSecond, 300);
    // Well past the baseline the period is measured over: the line predicts
    // the ring's end-of-frames to within a few microseconds, where the
    // nominal period would be off by 0.8 us a buffer.
    const DmaClock& clock = sink.clock();
    for (std::uint64_t k = 4000; k < 4100; ++k) {
        CHECK(std::llabs(clock.eof_us(static_cast<std::uint32_t>(k)) - sink.eof_true(k)) <= 5);
    }
}

// --- VirtualDac ------------------------------------------------------------------

TEST_CASE("the virtual DAC paces writes to its queue and says when they play", "[io][playout]") {
    VirtualDac dac;
    dac.open(12, 256, kRate);
    const std::int64_t queue = 11 * 5'333;
    CHECK(dac.latency_us() == queue);
    VirtualDac::Write w = dac.write(0);
    CHECK(w.play_us == queue);
    CHECK(w.return_us == 0);
    CHECK_FALSE(w.gap);
    // Each write plays a buffer after the one before, and returns once the
    // buffer before it has started, a queue's length ahead: a writer that
    // writes as soon as it may runs at the ring's rate from the start.
    std::int64_t now = 0;
    for (int i = 1; i < 40; ++i) {
        w = dac.write(now);
        CHECK(w.play_us == queue + (i * 5'333));
        CHECK(w.return_us == i * 5'333);
        CHECK_FALSE(w.gap);
        now = w.return_us;
    }
}

TEST_CASE("the virtual DAC reports a gap after a writer stalls, and not at its first write", "[io][playout]") {
    VirtualDac dac;
    dac.open(12, 256, kRate);
    VirtualDac::Write w = dac.write(1'000'000);
    CHECK_FALSE(w.gap);
    w = dac.write(1'000'000);
    const std::int64_t after = w.play_us + (2 * 5'333);
    // The next buffer should have started a microsecond ago: the ring played
    // it as zeros, and the one after is the next to start.
    const std::int64_t stall = w.play_us + 5'333 + 1;
    w = dac.write(stall);
    CHECK(w.gap);
    CHECK(w.play_us == after);
    CHECK(w.return_us == stall);
    w = dac.write(stall);
    CHECK_FALSE(w.gap);
    CHECK(w.play_us == after + 5'333);
}

// --- Playout, against a sink that says what it is told -----------------------

TEST_CASE("the playout plays every frame in order to a sink that says nothing", "[io][playout]") {
    std::vector<float> storage(kBlock);
    Playout playout(storage, 1);
    ScriptedSink sink;
    std::vector<float> samples(1000);
    for (std::size_t k = 0; k < samples.size(); ++k) {
        samples[k] = static_cast<float>(k + 1);
    }
    const std::array<std::span<const float>, 1> views{std::span<const float>(samples)};
    playout.push(views, samples.size(), 5'000'000, 0, 0, sink);
    playout.flush(sink);
    // One block of silence, written to find out where the sink is, then the
    // frames as they came and the last block padded.
    REQUIRE(sink.first_output.size() == 5 * kBlock);
    CHECK(std::all_of(sink.first_output.begin(), sink.first_output.begin() + kBlock,
                      [](float x) { return x == 0.0F; }));
    for (std::size_t k = 0; k < samples.size(); ++k) {
        CHECK(sink.first_output[kBlock + k] == samples[k]);
    }
    const Playout::Stats& stats = playout.stats();
    CHECK(stats.stream_frames == 1000);
    CHECK(stats.silence_frames == kBlock + 24);
    CHECK(stats.skipped_frames == 0);
    CHECK_FALSE(stats.have_last);
    CHECK(stats.underruns == 0);
    CHECK_FALSE(playout.next_play_us(0).has_value());
}

TEST_CASE("the playout writes as many outputs as the sink was opened for", "[io][playout]") {
    std::vector<float> storage(4 * kBlock);
    Playout playout(storage, 4);
    ScriptedSink sink;
    playout.set_outputs(2);
    std::vector<float> samples(kBlock, 0.5F);
    const std::array<std::span<const float>, 3> views{std::span<const float>(samples), std::span<const float>(samples),
                                                      std::span<const float>(samples)};
    playout.push(views, kBlock, 0, 0, 0, sink);
    playout.flush(sink);
    REQUIRE_FALSE(sink.output_counts.empty());
    CHECK(std::all_of(sink.output_counts.begin(), sink.output_counts.end(), [](std::size_t n) { return n == 2; }));
    playout.set_outputs(9);
    CHECK(playout.outputs() == 4);
}

TEST_CASE("the playout pads a stream to its time and says when its next frame plays", "[io][playout]") {
    std::vector<float> storage(kBlock);
    Playout playout(storage, 1);
    ScriptedSink sink;
    sink.first_play_us = 1'000'000;
    std::vector<float> samples(kBlock, 0.25F);
    const std::array<std::span<const float>, 1> views{std::span<const float>(samples)};
    CHECK_FALSE(playout.next_play_us(0).has_value());
    // Due 100 ms after the first silence block plays: that block, then 100 ms
    // less its own length of silence, then the frames, the last 192 of which
    // wait in the next block.
    playout.push(views, kBlock, 1'100'000, 0, 0, sink);
    const Playout::Stats& stats = playout.stats();
    CHECK(stats.silence_frames == 4800);
    CHECK(stats.stream_frames == 64);
    CHECK(stats.error_us == 0);
    REQUIRE(stats.have_last);
    CHECK(stats.last_play_us == 1'100'000);
    REQUIRE(playout.next_play_us(0));
    CHECK(*playout.next_play_us(0) == 1'100'000 + frames_us(kBlock));
    // Never sooner than now.
    CHECK(*playout.next_play_us(2'000'000) == 2'000'000);
    CHECK(stats.underruns == 0);
}

TEST_CASE("the playout counts a gap under a stream as an underrun, and not before it", "[io][playout]") {
    std::vector<float> storage(kBlock);
    Playout playout(storage, 1);
    ScriptedSink sink;
    sink.first_play_us = 1'000'000;
    // The sink ran dry before the first block: the idle spell before a
    // stream, which is no underrun.
    sink.gap_next = true;
    std::vector<float> samples(kBlock, 0.25F);
    const std::array<std::span<const float>, 1> views{std::span<const float>(samples)};
    playout.push(views, kBlock, 1'000'000 + frames_us(kBlock), 0, 0, sink);
    playout.push(views, kBlock, 1'000'000 + frames_us(2 * kBlock), kBlock, 0, sink);
    CHECK(playout.stats().underruns == 0);
    // Then it runs dry under the stream, and a block is late: each a break,
    // placed again at once since the error it measures is past the deadband.
    sink.gap_next = true;
    sink.first_play_us = *sink.first_play_us + 1'000;
    playout.push(views, kBlock, 1'000'000 + frames_us(3 * kBlock), 2 * kBlock, 0, sink);
    CHECK(playout.stats().underruns == 1);
    CHECK(playout.stats().resyncs == 1);
    // Placed again, the next block leaves out the millisecond's 48 frames it
    // is late by, and goes out with the block after, on time but late in
    // the writing.
    sink.late_next = true;
    playout.push(views, kBlock, 1'000'000 + frames_us(4 * kBlock), 3 * kBlock, 0, sink);
    CHECK(playout.stats().skipped_frames == 48);
    playout.push(views, kBlock, 1'000'000 + frames_us(5 * kBlock), 4 * kBlock, 0, sink);
    CHECK(playout.stats().error_us == 0);
    CHECK(playout.stats().underruns == 2);
    CHECK(playout.stats().late_blocks == 1);
    CHECK(playout.stats().resyncs == 1);
}

// --- Playout on the simulated channel ------------------------------------------

TEST_CASE("the playout starts a stream on time on a ring that has run idle", "[io][playout]") {
    SimulatedI2s sink({});
    sink.idle(2'000'000);
    TwoOutputs out;
    play(out.playout, sink, sink.now() + 250'000, 20 * kSecond, 300);
    REQUIRE(sink.truth.first_error_us);
    // A frame is 20.8 us.
    CHECK(std::llabs(*sink.truth.first_error_us) <= 21);
    CHECK(sink.truth.frames == 20 * kSecond);
    CHECK(sink.truth.worst_us <= 25);
    CHECK(sink.truth.jumps == 0);
    const Playout::Stats& stats = out.playout.stats();
    CHECK(stats.underruns == 0);
    CHECK(stats.late_blocks == 0);
    CHECK(stats.resyncs == 0);
    CHECK(stats.dropped_frames == 0);
    CHECK(stats.repeated_frames == 0);
    CHECK(std::llabs(stats.worst_error_us) <= 25);
}

TEST_CASE("the playout follows a DAC 300 ppm fast for ten minutes", "[io][playout]") {
    SimulatedI2s sink({.ppm = 300.0});
    TwoOutputs out;
    const std::uint64_t frames = 600 * kSecond;
    play(out.playout, sink, sink.now() + 250'000, frames, 300);
    CHECK(sink.truth.frames == frames + out.playout.stats().repeated_frames);
    CHECK(sink.truth.worst_us <= 400);
    CHECK(sink.truth.off_frames == 0);
    CHECK(sink.truth.jumps == 0);
    const Playout::Stats& stats = out.playout.stats();
    CHECK(stats.underruns == 0);
    CHECK(stats.resyncs == 0);
    CHECK(stats.dropped_frames == 0);
    // 300 ppm of 28.8 million frames is 8,640 frames to wait out.
    CHECK(stats.repeated_frames >= 8'400);
    CHECK(stats.repeated_frames <= 8'900);
    // What the playout measured is what happened.
    CHECK(std::llabs(stats.worst_error_us) <= 400);
}

TEST_CASE("the playout follows a DAC 300 ppm slow", "[io][playout]") {
    SimulatedI2s sink({.descriptors = 12, .frames = 256, .ppm = -300.0});
    TwoOutputs out;
    const std::uint64_t frames = 120 * kSecond;
    play(out.playout, sink, sink.now() + 250'000, frames, 300);
    CHECK(sink.truth.frames == frames - out.playout.stats().dropped_frames);
    CHECK(sink.truth.worst_us <= 400);
    CHECK(sink.truth.jumps == 0);
    const Playout::Stats& stats = out.playout.stats();
    CHECK(stats.underruns == 0);
    CHECK(stats.resyncs == 0);
    CHECK(stats.repeated_frames == 0);
    // 300 ppm of 5.76 million frames is 1,728 frames to catch up.
    CHECK(stats.dropped_frames >= 1'600);
    CHECK(stats.dropped_frames <= 1'850);
}

TEST_CASE("the playout skips the frames of a stream that are already late", "[io][playout]") {
    SimulatedI2s sink({});
    sink.idle(2'000'000);
    TwoOutputs out;
    play(out.playout, sink, sink.now() - 100'000, 5 * kSecond, 300);
    REQUIRE(sink.truth.first_error_us);
    CHECK(std::llabs(*sink.truth.first_error_us) <= 21);
    const Playout::Stats& stats = out.playout.stats();
    // 100 ms late, and the ring's own latency on top.
    CHECK(stats.skipped_frames > 4800);
    CHECK(stats.skipped_frames < 4800 + 1500);
    CHECK(stats.underruns == 0);
    CHECK(sink.truth.worst_us <= 25);
}

TEST_CASE("the playout counts a stall as one underrun and places the stream again", "[io][playout]") {
    SimulatedI2s sink({});
    TwoOutputs out;
    const std::uint64_t stall_at = 30 * kSecond;
    // Errors are counted from a second after the stall.
    sink.truth.settled_from = stall_at + kSecond;
    play(out.playout, sink, sink.now() + 250'000, 60 * kSecond, 300, [&](std::uint64_t frame) {
        if (frame == (stall_at / kBlock) * kBlock) {
            sink.idle(150'000);
        }
    });
    const Playout::Stats& stats = out.playout.stats();
    CHECK(stats.underruns == 1);
    CHECK(stats.resyncs == 1);
    CHECK(sink.truth.worst_us <= 25);
    CHECK(sink.truth.off_frames == 0);
    // The frames due during the stall were never played: one jump.
    CHECK(sink.truth.jumps == 1);
    CHECK(stats.skipped_frames > 6000);
}

TEST_CASE("the playout places frames through late interrupts", "[io][playout]") {
    // Each interrupt runs up to 200 us after its buffer finished. The clock
    // times the interrupts, so it runs about 100 us late of the ring, and
    // the frames play about that early.
    SimulatedI2s sink({.ppm = 120.0, .jitter_us = 200});
    TwoOutputs out;
    play(out.playout, sink, sink.now() + 250'000, 120 * kSecond, 300);
    CHECK(sink.truth.worst_us <= 600);
    CHECK(sink.truth.off_frames == 0);
    CHECK(sink.truth.jumps == 0);
    const Playout::Stats& stats = out.playout.stats();
    CHECK(stats.underruns == 0);
    CHECK(stats.resyncs == 0);
}

TEST_CASE("the playout starts each stream afresh on a ring that runs on", "[io][playout]") {
    SimulatedI2s sink({});
    TwoOutputs out;
    play(out.playout, sink, sink.now() + 250'000, 5 * kSecond, 300);
    sink.idle(3'000'000);
    out.playout.restart();
    sink.truth = Truth{};
    play(out.playout, sink, sink.now() + 250'000, 5 * kSecond, 300);
    REQUIRE(sink.truth.first_error_us);
    CHECK(std::llabs(*sink.truth.first_error_us) <= 21);
    CHECK(sink.truth.worst_us <= 25);
    CHECK(out.playout.stats().underruns == 0);
    CHECK(out.playout.stats().stream_frames == 5 * kSecond);
}
