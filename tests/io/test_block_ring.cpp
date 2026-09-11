// The ring between the player's decode task and its output task, tested on the
// host.
//
// ac3forge/block_ring.hpp is the index arithmetic and the storage layout of a
// single-producer, single-consumer ring; the waiting is FreeRTOS's, in the
// component's player.cpp, and is not here. What can be wrong here is an
// off-by-one between slots, a slot overwritten before it is released, or a
// span that overlaps its neighbour - all silent in audio until the wrong block
// plays.

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "ac3forge/block_ring.hpp"

using ac3forge::BlockRing;

TEST_CASE("an empty ring has nothing to read and every slot to write", "[io][block_ring]") {
    std::vector<float> storage(BlockRing::storage_floats(4, 2, 8));
    BlockRing ring;
    ring.reset(storage, 4, 2, 8);
    REQUIRE(ring.capacity() == 4);
    REQUIRE(ring.empty());
    REQUIRE_FALSE(ring.full());
    REQUIRE(ring.queued() == 0);
    REQUIRE(ring.write_slot() == 0);
}

TEST_CASE("slots are published and released in order, and wrap", "[io][block_ring]") {
    std::vector<float> storage(BlockRing::storage_floats(3, 1, 4));
    BlockRing ring;
    ring.reset(storage, 3, 1, 4);
    // Three times round, so every slot is reused twice.
    for (std::size_t n = 0; n < 9; ++n) {
        REQUIRE(ring.write_slot() == n % 3);
        ring.span(ring.write_slot(), 0)[0] = static_cast<float>(n);
        ring.publish();
        REQUIRE_FALSE(ring.empty());
        REQUIRE(ring.queued() == 1);
        REQUIRE(ring.read_slot() == n % 3);
        REQUIRE(ring.span(ring.read_slot(), 0)[0] == static_cast<float>(n));
        ring.release();
        REQUIRE(ring.empty());
    }
}

TEST_CASE("a full ring takes nothing more until a slot is released", "[io][block_ring]") {
    std::vector<float> storage(BlockRing::storage_floats(4, 1, 1));
    BlockRing ring;
    ring.reset(storage, 4, 1, 1);
    for (std::size_t n = 0; n < 4; ++n) {
        REQUIRE_FALSE(ring.full());
        ring.publish();
    }
    REQUIRE(ring.full());
    REQUIRE(ring.queued() == 4);
    // The consumer's release frees exactly one slot, and it is the oldest -
    // the one the producer writes next.
    REQUIRE(ring.read_slot() == 0);
    ring.release();
    REQUIRE_FALSE(ring.full());
    REQUIRE(ring.queued() == 3);
    REQUIRE(ring.write_slot() == 0);
    ring.publish();
    REQUIRE(ring.full());
}

TEST_CASE("each slot's spans are its own and do not overlap", "[io][block_ring]") {
    constexpr std::size_t kBlocks = 3;
    constexpr std::size_t kSpans = 4;
    constexpr std::size_t kSamples = 5;
    std::vector<float> storage(BlockRing::storage_floats(kBlocks, kSpans, kSamples), -1.0F);
    BlockRing ring;
    ring.reset(storage, kBlocks, kSpans, kSamples);
    // Every sample of every span marked with its own (slot, span, sample), so
    // a span that ran into its neighbour would overwrite a mark.
    for (std::size_t slot = 0; slot < kBlocks; ++slot) {
        for (std::size_t s = 0; s < kSpans; ++s) {
            const auto span = ring.span(slot, s);
            REQUIRE(span.size() == kSamples);
            for (std::size_t k = 0; k < kSamples; ++k) {
                span[k] = static_cast<float>((slot * 100) + (s * 10) + k);
            }
        }
    }
    for (std::size_t slot = 0; slot < kBlocks; ++slot) {
        for (std::size_t s = 0; s < kSpans; ++s) {
            const auto span = ring.span(slot, s);
            for (std::size_t k = 0; k < kSamples; ++k) {
                REQUIRE(span[k] == static_cast<float>((slot * 100) + (s * 10) + k));
            }
        }
    }
    // And together they are exactly the storage, with nothing left over.
    for (const float value : storage) {
        REQUIRE(value >= 0.0F);
    }
}

TEST_CASE("reset empties a ring that held blocks", "[io][block_ring]") {
    std::vector<float> storage(BlockRing::storage_floats(2, 1, 1));
    BlockRing ring;
    ring.reset(storage, 2, 1, 1);
    ring.publish();
    ring.publish();
    REQUIRE(ring.full());
    ring.reset(storage, 2, 1, 1);
    REQUIRE(ring.empty());
    REQUIRE(ring.queued() == 0);
    REQUIRE(ring.write_slot() == 0);
    REQUIRE(ring.read_slot() == 0);
}

TEST_CASE("a producer thread and a consumer thread see every block once, in order",
          "[io][block_ring]") {
    // What the two tasks do on a board, with threads: the producer fills a
    // slot and publishes it, the consumer takes it and checks it is the next
    // block, not a stale or a skipped one. The spinning stands in for the
    // player's semaphores.
    constexpr std::size_t kBlocks = 4;
    constexpr std::size_t kSpans = 2;
    constexpr std::size_t kSamples = 16;
    constexpr std::uint32_t kTotal = 20000;
    std::vector<float> storage(BlockRing::storage_floats(kBlocks, kSpans, kSamples));
    BlockRing ring;
    ring.reset(storage, kBlocks, kSpans, kSamples);

    std::atomic<bool> mismatch{false};
    std::thread consumer([&] {
        for (std::uint32_t expected = 0; expected < kTotal; ++expected) {
            while (ring.empty()) {
                std::this_thread::yield();
            }
            const std::size_t slot = ring.read_slot();
            for (std::size_t s = 0; s < kSpans; ++s) {
                const auto span = ring.span(slot, s);
                // Exact in float: kTotal * 2 + 1 is far below 2^24.
                const auto want = static_cast<float>((expected * 2) + s);
                if (span.front() != want || span.back() != want) {
                    mismatch.store(true);
                }
            }
            ring.release();
        }
    });
    for (std::uint32_t n = 0; n < kTotal; ++n) {
        while (ring.full()) {
            std::this_thread::yield();
        }
        const std::size_t slot = ring.write_slot();
        for (std::size_t s = 0; s < kSpans; ++s) {
            const auto span = ring.span(slot, s);
            const auto value = static_cast<float>((n * 2) + s);
            for (float& sample : span) {
                sample = value;
            }
        }
        ring.publish();
    }
    consumer.join();
    REQUIRE_FALSE(mismatch.load());
    REQUIRE(ring.empty());
}
