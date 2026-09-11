#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

// The blocks between the decode task and the output task (player.hpp).
//
// The decode task hands over each 256-sample block the decoder delivers, and
// the output task renders it onto the layout and writes it to the sink on the
// other core. Between them is this ring: a fixed number of slots, each a fixed
// number of planar spans of a fixed number of samples, carved out of storage
// the caller allocates wherever it likes - PSRAM on a board that has it, since
// sixteen blocks of sixteen channels are 256 KB.
//
// One producer and one consumer, which is all the player has, so the indices
// need no lock: the producer alone advances `published`, the consumer alone
// advances `released`, and each reads the other's with acquire ordering after
// the other stored it with release ordering - so a slot's samples, written
// before its publish, are visible to the consumer that sees the publish, and a
// slot released by the consumer is not rewritten until the producer sees the
// release. Both counters run on past the slot count and wrap at 2^32; their
// difference is what is queued, which unsigned arithmetic gets right across the
// wrap for any ring of up to 2^31 slots.
//
// What this does not do is wait. A producer that finds the ring full, or a
// consumer that finds it empty, has to block on something the RTOS provides,
// and that is the player's business (player.cpp) - which keeps this free of
// ESP-IDF, so tests/io/test_block_ring.cpp can check the arithmetic on the
// host.

namespace ac3forge {

class BlockRing {
   public:
    // How many floats of storage `blocks` slots of `spans` spans of `samples`
    // samples need.
    [[nodiscard]] static constexpr std::size_t storage_floats(std::size_t blocks, std::size_t spans,
                                                              std::size_t samples) {
        return blocks * spans * samples;
    }

    BlockRing() = default;
    BlockRing(const BlockRing&) = delete;
    BlockRing& operator=(const BlockRing&) = delete;

    // Lays the ring out over `storage`, which must hold at least
    // storage_floats(blocks, spans, samples) floats and outlive the ring's use,
    // and empties it. Not safe while either side is using the ring.
    void reset(std::span<float> storage, std::size_t blocks, std::size_t spans,
               std::size_t samples) {
        storage_ = storage;
        blocks_ = blocks;
        spans_ = spans;
        samples_ = samples;
        published_.store(0, std::memory_order_relaxed);
        released_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t capacity() const { return blocks_; }
    [[nodiscard]] std::size_t spans_per_slot() const { return spans_; }
    [[nodiscard]] std::size_t samples_per_span() const { return samples_; }

    // Published and not yet released. Either side may ask; the answer is
    // exact for the side asking and a lower or upper bound for the other.
    [[nodiscard]] std::size_t queued() const {
        return static_cast<std::uint32_t>(published_.load(std::memory_order_acquire) -
                                          released_.load(std::memory_order_acquire));
    }

    // --- the producer ----------------------------------------------------
    // No slot free to fill.
    [[nodiscard]] bool full() const {
        return static_cast<std::uint32_t>(published_.load(std::memory_order_relaxed) -
                                          released_.load(std::memory_order_acquire)) >= blocks_;
    }
    // The slot to fill next. Only meaningful while !full().
    [[nodiscard]] std::size_t write_slot() const {
        return published_.load(std::memory_order_relaxed) % blocks_;
    }
    // The slot at write_slot() is filled: hand it to the consumer.
    void publish() {
        published_.store(published_.load(std::memory_order_relaxed) + 1,
                         std::memory_order_release);
    }

    // --- the consumer ----------------------------------------------------
    // Nothing published to take.
    [[nodiscard]] bool empty() const {
        return published_.load(std::memory_order_acquire) ==
               released_.load(std::memory_order_relaxed);
    }
    // The slot to take next. Only meaningful while !empty().
    [[nodiscard]] std::size_t read_slot() const {
        return released_.load(std::memory_order_relaxed) % blocks_;
    }
    // The slot at read_slot() is done with: hand it back to the producer.
    void release() {
        released_.store(released_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    // Span `span` of slot `slot`: samples_per_span() floats.
    [[nodiscard]] std::span<float> span(std::size_t slot, std::size_t span) const {
        return storage_.subspan(((slot * spans_) + span) * samples_, samples_);
    }

   private:
    std::span<float> storage_{};
    std::size_t blocks_ = 0;
    std::size_t spans_ = 0;
    std::size_t samples_ = 0;
    std::atomic<std::uint32_t> published_{0};  // written by the producer only
    std::atomic<std::uint32_t> released_{0};   // written by the consumer only
};

}  // namespace ac3forge
