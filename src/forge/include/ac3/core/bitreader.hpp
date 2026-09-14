#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ac3 {

// MSB-first bit reader, the mirror of BitWriter (A/52 §5.1). Reading past
// the end sets a sticky overflow flag and yields zeros — callers check
// overflowed() once at a suitable boundary instead of guarding every read.
//
// Reads come out of a 64-bit cache refilled a byte at a time, rather than
// one bit per loop iteration as this used to do. A decode reads every
// mantissa, exponent group and GAQ codeword through here — some fifty
// thousand fields a frame for a 5.1 stream — and the bit-at-a-time form
// cost about eight cycles per BIT of each: on an ESP32-S3 that was most of
// the mantissa stage of a decode that had nothing else left in it
// (docs/platforms/esp32.md). The values, the position and the overflow
// behaviour are exactly what they were; only the number of instructions
// between them changed.
class BitReader {
public:
    explicit BitReader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] std::uint32_t read(int bits) {
        assert(bits >= 0 && bits <= 32);
        if (bits == 0) {
            return 0;
        }
        if (cached_bits_ < bits) {
            refill(bits);
        }
        // The cache holds cached_bits_ valid bits in its low bits, the
        // oldest highest; the field is its top `bits` of them. refill()
        // guarantees cached_bits_ is in [bits, 64] here, so shift is in
        // [0, 63] - spelled out for the analyzer, which otherwise explores
        // an infeasible cached_bits_ >= 64 + bits == 0 path and flags the
        // shift below as unbounded.
        const int shift = cached_bits_ - bits;
        assert(shift >= 0 && shift < 64);
        const auto value = static_cast<std::uint32_t>((cache_ >> shift) & mask(bits));
        cached_bits_ = shift;
        cache_ &= (std::uint64_t{1} << shift) - 1;
        position_ += static_cast<std::size_t>(bits);
        return value;
    }

    [[nodiscard]] std::uint32_t read_bit() { return read(1); }

    void skip(std::size_t bits) {
        position_ += bits;
        if (position_ > data_.size() * 8) {
            overflowed_ = true;
        }
        // The cache no longer describes the position; drop it and let the
        // next read rebuild it from the byte the position now lands in.
        cache_ = 0;
        cached_bits_ = 0;
        next_byte_ = position_ >> 3;
        pending_skip_ = static_cast<int>(position_ & 7);
    }

    [[nodiscard]] std::size_t bit_position() const { return position_; }
    [[nodiscard]] bool overflowed() const { return overflowed_; }

private:
    [[nodiscard]] static constexpr std::uint64_t mask(int bits) {
        return bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
    }

    // Make at least `need` bits available. Whole bytes are appended while
    // there is room for them and data to take them from; when the data runs
    // out the cache is padded with zero bits instead, and the read that
    // needed them is the one that has overflowed - the same zeros, and the
    // same flag, that the bit-at-a-time reader produced past the end.
    void refill(int need) {
        while (cached_bits_ + 8 <= 64 && next_byte_ < data_.size()) {
            cache_ = (cache_ << 8) | std::to_integer<std::uint64_t>(data_[next_byte_]);
            cached_bits_ += 8;
            ++next_byte_;
            if (pending_skip_ != 0) {
                // A skip() left the position mid-byte: the bits of this byte
                // before it were consumed already.
                cached_bits_ -= pending_skip_;
                cache_ &= mask(cached_bits_);
                pending_skip_ = 0;
            }
        }
        if (cached_bits_ < need) {
            const int pad = need - cached_bits_;
            cache_ <<= pad;
            cached_bits_ += pad;
            overflowed_ = true;
        }
    }

    std::span<const std::byte> data_;
    std::size_t position_ = 0;
    std::size_t next_byte_ = 0;  // the first byte not yet in the cache
    std::uint64_t cache_ = 0;
    int cached_bits_ = 0;
    int pending_skip_ = 0;  // bits of the next cached byte already consumed by skip()
    bool overflowed_ = false;
};

}  // namespace ac3
