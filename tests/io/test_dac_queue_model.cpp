// The streaming example's model of its DAC's DMA queue, on the host.
//
// The model (ac3forge/dac_queue_model.hpp) lives in the ESP-IDF component and
// includes nothing from ESP-IDF - the arrangement test_interleave.cpp and
// test_layout.cpp have. The i2s and tdm sinks give it esp_timer_get_time();
// this gives it the clock of the DMA below, which is what the model assumes of
// the real one: a fixed capacity, drained at exactly the byte rate whatever
// the writer does, and a write that returns once the whole block is in the
// queue, as i2s_channel_write with portMAX_DELAY does.
//
// The model cannot run under QEMU, which has no I2S, and on a board it sees
// only what the board happens to do. Three defects were found in it on
// 2026-09-10, each with a harness of this kind: the idle gap between two plays
// counted as one underrun; drain arithmetic in bytes that overflowed after
// about five weeks idle at sixteen slots; and a first fix for that which
// rounded the queue's figure down twice a block until it fell behind the
// DMA's for good. They are the first, fifth and last cases below.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <random>

#include "ac3forge/dac_queue_model.hpp"

namespace {

constexpr std::int64_t kMicro = 1'000'000;

// 48 kHz in 32-bit slots. The player writes one decoded block at a time, 256
// frames or 5.33 ms, into a queue of 960 frames, 20 ms: Kconfig's default
// depth of 4 descriptors of 240. (For stereo the i2s sink reshapes that into 8
// descriptors of 128, 1,024 frames, so that each descriptor divides a block;
// the model works with whatever capacity it is opened with.)
constexpr std::int64_t kSampleRate = 48'000;
constexpr std::int64_t kSlotBytes = 4;
constexpr std::int64_t kBlockFrames = 256;
constexpr std::int64_t kQueueFrames = 960;
constexpr std::int64_t kBlockUs = kBlockFrames * kMicro / kSampleRate;  // 5,333
constexpr std::int64_t kQueueUs = kQueueFrames * kMicro / kSampleRate;  // 20,000
constexpr std::int64_t kStereo = 2;
constexpr std::int64_t kSixteenSlots = 16;

// The decoder, as the model sees it: each block is ready 3 ms after the write
// before it returned. So a queue that was full when a write returned has 17 ms
// left when the next block arrives, and a play that starts on an empty queue
// has the first block less those 3 ms when its second arrives - its least
// headroom, if nothing goes wrong later.
constexpr std::int64_t kWorkUs = 3'000;
constexpr std::int64_t kFullHeadroomUs = kQueueUs - kWorkUs;         // 17,000
constexpr std::int64_t kSecondBlockHeadroomUs = kBlockUs - kWorkUs;  // 2,333

constexpr std::int64_t kBootUs = 2'000'000;  // when a board's first block arrives
constexpr std::uint64_t kBlocks = 300;       // a play: 1.6 s of audio

// The model counts in whole bytes and whole microseconds, rounding each down -
// a byte of stereo is 2.6 us - and the DMA below rounds each wait up to the
// next microsecond. A figure this close to the one worked out by hand is that
// figure.
constexpr std::int64_t kSlackUs = 3;

template <typename T>
[[nodiscard]] std::int64_t off_by(T got, std::int64_t expected) {
    const auto value = static_cast<std::int64_t>(got);
    return value > expected ? value - expected : expected - value;
}

// The DMA, kept exactly: its level is in millionths of a byte, so a drain of
// any whole number of microseconds is a whole number of them.
class SimulatedDma {
   public:
    SimulatedDma(std::int64_t bytes_per_second, std::int64_t capacity_bytes)
        : rate_(bytes_per_second), capacity_(capacity_bytes * kMicro) {}

    // A write of `bytes` that starts at now_us. Returns when it returned: at
    // once if the queue has room for all of it, otherwise as soon as enough
    // has drained.
    std::int64_t write(std::int64_t now_us, std::int64_t bytes) {
        drain_to(now_us);
        const std::int64_t excess = level_ + (bytes * kMicro) - capacity_;
        const std::int64_t done_us = excess > 0 ? now_us + ((excess + rate_ - 1) / rate_) : now_us;
        drain_to(done_us);
        level_ += bytes * kMicro;
        return done_us;
    }

   private:
    void drain_to(std::int64_t now_us) {
        const std::int64_t elapsed_us = now_us - at_us_;
        at_us_ = now_us;
        // Emptied within the interval: tested first, because the drain over
        // forty days at sixteen slots is past 2^63 millionths of a byte.
        if (elapsed_us >= (level_ + rate_ - 1) / rate_) {
            level_ = 0;
            return;
        }
        level_ -= elapsed_us * rate_;
    }

    std::int64_t rate_;      // bytes a second, which is millionths of a byte a microsecond
    std::int64_t capacity_;  // in millionths of a byte, as level_ is
    std::int64_t level_ = 0;
    std::int64_t at_us_ = 0;  // when level_ was
};

// A sink over that DMA, doing with the model what sink/i2s/ and sink/tdm/ do
// around each write: arriving() as the block arrives, the write, and queued()
// once the write has returned. `slots` 32-bit slots at 48 kHz, 960 frames deep.
struct Sink {
    explicit Sink(std::int64_t slots)
        : frame_bytes(slots * kSlotBytes),
          dma(kSampleRate * frame_bytes, kQueueFrames * frame_bytes) {
        model.open(static_cast<std::uint32_t>(kSampleRate * frame_bytes),
                   static_cast<std::size_t>(kQueueFrames * frame_bytes));
    }

    // One block, ready at ready_us. Returns when its write returned.
    std::int64_t write(std::int64_t ready_us) {
        const std::int64_t bytes = kBlockFrames * frame_bytes;
        model.arriving(ready_us);
        back_us = dma.write(ready_us, bytes);
        model.queued(static_cast<std::size_t>(bytes), back_us);
        return back_us;
    }

    // `blocks` blocks, the first ready at first_us and each of the rest
    // kWorkUs after the write before it returned. Returns when the last write
    // returned.
    std::int64_t play(std::int64_t first_us, std::uint64_t blocks) {
        std::int64_t ready_us = first_us;
        for (std::uint64_t i = 0; i < blocks; ++i) {
            ready_us = write(ready_us) + kWorkUs;
        }
        return back_us;
    }

    std::int64_t frame_bytes;
    SimulatedDma dma;
    ac3forge::DacQueueModel model;
    std::int64_t back_us = 0;  // when the last write returned
};

}  // namespace

TEST_CASE("the DAC queue model starts its figures again for each play", "[io][dac]") {
    // The board's case: a play at boot, the player idle for 55.4 s, then a
    // play started from the control surface. The second play's first block
    // meets a queue that has been empty for all of that, and is exempt, as
    // the first block after boot is: nothing was owed to the DAC.
    Sink sink(kStereo);
    sink.model.restart();
    const std::int64_t end_a = sink.play(kBootUs, kBlocks);
    const auto a = sink.model.play();
    CHECK(a.writes == kBlocks);
    CHECK(a.underruns == 0U);
    CHECK(a.dry_us == 0U);
    CHECK(off_by(a.min_headroom_us, kSecondBlockHeadroomUs) <= kSlackUs);

    sink.model.restart();
    sink.play(end_a + 55'400'000, kBlocks);
    const auto b = sink.model.play();
    CHECK(b.writes == kBlocks);
    CHECK(b.underruns == 0U);
    CHECK(b.dry_us == 0U);
    CHECK(b.min_headroom_us == a.min_headroom_us);
    CHECK(sink.model.writes() == 2 * kBlocks);
}

TEST_CASE("the DAC queue model keeps what the last play left queued", "[io][dac]") {
    // Stop, and play again at once: the new play's first block arrives 1 ms
    // after the last play's final write returned, to a queue still 19 ms deep,
    // and its write waits 4.3 ms for room. restart() leaves that residual
    // where it is. A model that emptied its queue there would take the wait
    // from a block of 5.3 ms, think 1 ms was queued, and count the second
    // block, 3 ms later, as an underrun of 2 ms the DAC never had.
    Sink sink(kStereo);
    const std::int64_t end_a = sink.play(kBootUs, kBlocks);
    sink.model.restart();
    sink.play(end_a + 1'000, kBlocks);
    const auto b = sink.model.play();
    CHECK(b.writes == kBlocks);
    CHECK(b.underruns == 0U);
    CHECK(b.dry_us == 0U);
    // Full from its first write on, so never less than a full queue's
    // headroom - where a play that starts on an empty queue has 2.3 ms at its
    // second block.
    CHECK(off_by(b.min_headroom_us, kFullHeadroomUs) <= kSlackUs);
}

TEST_CASE("the DAC queue model counts a stall once for as long as the DMA was dry",
          "[io][dac]") {
    // One block 100 ms late in the middle of a play, with the queue full ahead
    // of it: the DAC played the 17 ms that were left when the block was due,
    // then 83 ms of zeros. One underrun, however many blocks the gap was worth.
    constexpr std::int64_t kStallUs = 100'000;
    Sink sink(kStereo);
    sink.play(kBootUs, 150);
    const std::int64_t stalled = sink.write(sink.back_us + kWorkUs + kStallUs);
    const std::int64_t end_a = sink.play(stalled + kWorkUs, 149);
    const auto a = sink.model.play();
    CHECK(a.writes == kBlocks);
    CHECK(a.underruns == 1U);
    CHECK(off_by(a.dry_us, kStallUs - kFullHeadroomUs) <= kSlackUs);
    CHECK(a.min_headroom_us == 0);

    // And the next play owes it nothing.
    sink.model.restart();
    sink.play(end_a + 5'000'000, kBlocks);
    const auto b = sink.model.play();
    CHECK(b.underruns == 0U);
    CHECK(b.dry_us == 0U);
    CHECK(off_by(b.min_headroom_us, kSecondBlockHeadroomUs) <= kSlackUs);
}

TEST_CASE("the DAC queue model counts a late second block after restart as after open",
          "[io][dac]") {
    // A play's first block is exempt and its second is not. Arriving 80 ms
    // after the first write returned, the second finds that the one block the
    // first put in ran out 74.7 ms earlier - which counts, the same whether
    // the play is the first after open() or follows another after restart().
    constexpr std::int64_t kLateUs = 80'000;
    const auto late_second_block = [](Sink& sink, std::int64_t first_us) {
        const std::int64_t back_us = sink.write(first_us);
        sink.play(back_us + kLateUs, kBlocks - 1);
        return sink.model.play();
    };

    Sink fresh(kStereo);
    const auto after_open = late_second_block(fresh, kBootUs);
    CHECK(after_open.writes == kBlocks);
    CHECK(after_open.underruns == 1U);
    CHECK(off_by(after_open.dry_us, kLateUs - kBlockUs) <= kSlackUs);
    CHECK(after_open.min_headroom_us == 0);

    Sink replayed(kStereo);
    const std::int64_t end_a = replayed.play(kBootUs, kBlocks);
    replayed.model.restart();
    const auto after_restart = late_second_block(replayed, end_a + 5'000'000);
    CHECK(after_restart.writes == after_open.writes);
    CHECK(after_restart.underruns == after_open.underruns);
    CHECK(after_restart.dry_us == after_open.dry_us);
    CHECK(after_restart.min_headroom_us == after_open.min_headroom_us);
}

TEST_CASE("the DAC queue model takes forty days idle on a sixteen-slot bus", "[io][dac]") {
    // Sixteen 32-bit slots at 48 kHz drain 3,072,000 bytes a second, and forty
    // days is 3,456,000,000,000 us: their product is past 2^63. So once the
    // queue has run dry the model works in time, not bytes. Worked in bytes,
    // the drain across an idle gap overflowed after about five weeks, which a
    // UBSan build reports as signed overflow and any other build turns into a
    // figure that means nothing. The second play has to come out exactly as
    // the first did.
    constexpr std::int64_t kFortyDaysUs = std::int64_t{40} * 86'400 * kMicro;
    Sink sink(kSixteenSlots);
    const std::int64_t end_a = sink.play(kBootUs, kBlocks);
    const auto a = sink.model.play();
    CHECK(a.underruns == 0U);
    CHECK(off_by(a.min_headroom_us, kSecondBlockHeadroomUs) <= kSlackUs);

    sink.model.restart();
    sink.play(end_a + kFortyDaysUs, kBlocks);
    const auto b = sink.model.play();
    CHECK(b.writes == a.writes);
    CHECK(b.underruns == a.underruns);
    CHECK(b.dry_us == a.dry_us);
    CHECK(b.min_headroom_us == a.min_headroom_us);
    CHECK(sink.model.writes() == 2 * kBlocks);
}

TEST_CASE("the DAC queue model counts writes since open across plays", "[io][dac]") {
    // writes() is the seam's sink_frames_written(), which counts from
    // sink_open; play().writes is the sink line's sink.writes, which counts
    // from the start of the play.
    Sink sink(kStereo);
    std::int64_t back_us = kBootUs;
    std::uint64_t total = 0;
    for (const std::uint64_t blocks : std::array<std::uint64_t, 3>{100, 250, 7}) {
        sink.model.restart();
        back_us = sink.play(back_us + 1'000'000, blocks);
        total += blocks;
        CHECK(sink.model.play().writes == blocks);
        CHECK(sink.model.writes() == total);
    }

    // A play with nothing written yet has no figures, and no headroom
    // measured: report() prints -1 for that.
    sink.model.restart();
    CHECK(sink.model.play().writes == 0U);
    CHECK(sink.model.play().underruns == 0U);
    CHECK(sink.model.play().min_headroom_us == -1);
    CHECK(sink.model.writes() == total);
}

TEST_CASE("the DAC queue model keeps up with the DMA through a ten-minute play", "[io][dac]") {
    // Ten minutes of blocks with the decoder taking anything from 1 to 5 ms
    // over each, so the queue is full at some arrivals and down to its last
    // block at others, and never dry: a block is 5.3 ms, so the least
    // headroom any arrival can see is 0.3 ms. Each of the model's roundings
    // happens 112,500 times here, and one that always goes the same way adds
    // up. A version that took the queue's figure through microseconds and
    // back at every arrival rounded it down twice a block with nothing to
    // correct it: it fell behind the DMA's by 5.8 KB of this 7.7 KB queue,
    // and after three and a half minutes began counting underruns the DAC
    // never had.
    //
    // The draws are taken modulo rather than through a distribution, whose
    // output the standard leaves to the library, so every platform plays the
    // same ten minutes.
    constexpr auto kTenMinutes = static_cast<std::uint64_t>(600 * kSampleRate / kBlockFrames);
    constexpr std::int64_t kSlowestWorkUs = 5'000;
    std::mt19937 rng(20260910);
    Sink sink(kStereo);
    sink.model.restart();
    std::int64_t ready_us = kBootUs;
    for (std::uint64_t i = 0; i < kTenMinutes; ++i) {
        const std::int64_t work_us = 1'000 + static_cast<std::int64_t>(rng() % 4'001);
        ready_us = sink.write(ready_us) + work_us;
    }
    const auto ten = sink.model.play();
    CHECK(ten.writes == kTenMinutes);
    CHECK(ten.underruns == 0U);
    CHECK(ten.dry_us == 0U);
    CHECK(ten.min_headroom_us >= kBlockUs - kSlowestWorkUs - kSlackUs);
}
