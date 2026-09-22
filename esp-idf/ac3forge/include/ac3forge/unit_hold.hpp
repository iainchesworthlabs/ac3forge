#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <span>

#include "ac3forge/block_ring.hpp"

// A play's first unit, held back until the second has decoded
// (PlayerConfig::hold_first_unit in player.hpp).
//
// A play starts with nothing in the sink's queue but its first unit's blocks,
// one frame of audio, and the units after it decode more slowly than the rest
// while the caches warm. On an ESP32-S3 playing 7.1.4 over WiFi that ran the
// DAC dry in a play's first ten frames and never after
// (planning/esp32-714-realtime.md, "The decisions on the board"). Held until
// the second unit arrives, the first reaches the sink together with it, so the
// DAC starts with two frames queued.
//
// The decoder's views of a block last only for the call that delivers it, so
// what is held is a copy, in a BlockRing laid over storage the caller gives -
// PSRAM where the part has it, since a unit of twelve channels is 72 KB. One
// task fills the ring and empties it here, which the ring allows. The caller
// offers each block as it comes; when offer() declines one, the caller plays
// what release() hands back, in the order it was offered, and then the
// declined block.
//
// Free of ESP-IDF, so tests/io/test_unit_hold.cpp checks it on the host.

namespace ac3forge {

class UnitHold {
   public:
    // The blocks in a unit: six, in an AC-3 syncframe and in an E-AC-3 access
    // unit alike.
    static constexpr std::size_t kBlocks = 6;
    // The most spans a held block can have: its channels and objects together.
    static constexpr std::size_t kMaxSpans = 32;

    // How many floats of storage a unit of `spans` spans of `samples` samples
    // needs.
    [[nodiscard]] static constexpr std::size_t storage_floats(std::size_t spans,
                                                              std::size_t samples) {
        return BlockRing::storage_floats(kBlocks, spans, samples);
    }

    UnitHold() = default;
    UnitHold(const UnitHold&) = delete;
    UnitHold& operator=(const UnitHold&) = delete;

    // Ready to hold blocks of up to `channels` channels and `objects` objects,
    // each span up to `samples` long, in `storage`, which outlives the hold.
    // False, and the hold not armed, when that is more than kMaxSpans spans or
    // more than `storage` has room for.
    bool arm(std::span<float> storage, std::size_t channels, std::size_t objects,
             std::size_t samples) {
        armed_ = false;
        if (channels + objects > kMaxSpans ||
            storage.size() < storage_floats(channels + objects, samples)) {
            return false;
        }
        channels_ = channels;
        objects_ = objects;
        ring_.reset(storage, kBlocks, channels + objects, samples);
        armed_ = true;
        return true;
    }

    [[nodiscard]] bool armed() const { return armed_; }
    [[nodiscard]] std::size_t held() const { return ring_.queued(); }

    // A block from the decoder, `index` its place in its unit. True when it is
    // held. False when it is not, and the caller is to play release()'s blocks
    // and then this one: the hold is not armed, the block starts the next unit
    // (index 0 with blocks held), or it does not fit - a seventh block, more
    // channels or objects than arm() allowed, or spans of unequal or too great
    // a length.
    bool offer(int index, std::span<const std::span<const float>> channels,
               std::span<const std::span<const float>> objects) {
        if (!armed_ || (index == 0 && !ring_.empty()) || ring_.full() ||
            channels.size() > channels_ || objects.size() > objects_) {
            return false;
        }
        const std::size_t samples = !channels.empty()  ? channels.front().size()
                                    : !objects.empty() ? objects.front().size()
                                                       : 0;
        if (samples > ring_.samples_per_span()) {
            return false;
        }
        for (const auto span : channels) {
            if (span.size() != samples) {
                return false;
            }
        }
        for (const auto span : objects) {
            if (span.size() != samples) {
                return false;
            }
        }
        const std::size_t slot = ring_.write_slot();
        for (std::size_t c = 0; c < channels.size(); ++c) {
            copy(channels[c], ring_.span(slot, c));
        }
        for (std::size_t o = 0; o < objects.size(); ++o) {
            copy(objects[o], ring_.span(slot, channels_ + o));
        }
        blocks_[slot] = Block{.index = index,
                              .channels = channels.size(),
                              .objects = objects.size(),
                              .samples = samples};
        ring_.publish();
        return true;
    }

    // Every held block to play(position, index, channels, objects), in the
    // order it was offered, `position` counting from 0; then the hold is
    // disarmed. The views last until play returns.
    template <typename Play>
    void release(Play&& play) {
        std::size_t position = 0;
        while (!ring_.empty()) {
            const std::size_t slot = ring_.read_slot();
            const Block& block = blocks_[slot];
            for (std::size_t c = 0; c < block.channels; ++c) {
                views_[c] = ring_.span(slot, c).first(block.samples);
            }
            for (std::size_t o = 0; o < block.objects; ++o) {
                views_[block.channels + o] = ring_.span(slot, channels_ + o).first(block.samples);
            }
            const std::span<const std::span<const float>> spans(views_.data(),
                                                                block.channels + block.objects);
            play(position, block.index, spans.first(block.channels), spans.subspan(block.channels));
            ++position;
            ring_.release();
        }
        armed_ = false;
    }

    // Drops whatever is held, unplayed, and disarms the hold.
    void clear() {
        while (!ring_.empty()) {
            ring_.release();
        }
        armed_ = false;
    }

   private:
    struct Block {
        int index = 0;
        std::size_t channels = 0;
        std::size_t objects = 0;
        std::size_t samples = 0;
    };

    // memcpy rather than std::copy, which lowers to the S3 mask ROM's memmove
    // at about twelve cycles a byte (eac3_decoder.cpp's write_slot says the
    // same).
    static void copy(std::span<const float> from, std::span<float> to) {
        if (!from.empty()) {
            std::memcpy(to.data(), from.data(), from.size() * sizeof(float));
        }
    }

    BlockRing ring_;
    std::array<Block, kBlocks> blocks_{};
    std::array<std::span<const float>, kMaxSpans> views_{};
    std::size_t channels_ = 0;
    std::size_t objects_ = 0;
    bool armed_ = false;
};

}  // namespace ac3forge
