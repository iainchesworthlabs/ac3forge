#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include "iclforge/ac4dec/decoder.hpp"

// The bit reader every syntax function reads through. MSB first, as both
// parts read (Part 1 clause 3.4). Reading past the end does not throw and
// does not read out of bounds: it returns zeros and sets a sticky flag, which
// each syntax function checks at the points where carrying on would loop on
// a count read from nothing.
//
// The bits are read through a cache of 8 bytes, as a 64-bit word: a peek of up
// to 32 bits is a shift of it, and the word is loaded again only when the
// position has moved 4 bytes on. The bytes past the end of the data are zeros in
// it, which is what a read past the end returns.
//
// Every read that corresponds to a syntax element names that element, and
// when a SyntaxSink is attached the reader emits one SyntaxRecord for it (see
// SyntaxRecord in ac4/syntax.hpp for what counts as one element). Reads
// that are not syntax elements - byte_align, fill bits, skipped bytes - go
// through skip() and align(), which emit nothing.

namespace iclforge::ac4::detail {

class BitReader {
   public:
    BitReader(std::span<const std::byte> data, int substream, SyntaxSink sink) noexcept
        : data_(data), substream_(substream), sink_(sink) {}

    // A fixed-width syntax element of up to 32 bits.
    std::uint32_t read(int bits, std::string_view name) noexcept {
        const std::size_t start = pos_;
        const std::uint32_t value = peek_raw(bits);
        advance(bits);
        emit(start, bits, value, name);
        return value;
    }

    bool read_flag(std::string_view name) noexcept { return read(1, name) != 0; }

    // Part 1 clause 4.2.2's variable_bits(n_bits), recorded as one element.
    // The text sets no limit on how many groups follow one another, so none
    // is set here: the loop ends at a clear b_read_more or at the end of the
    // substream. A value too large for 64 bits is recorded, and returned,
    // modulo 2^64. The caller gets all 64 bits: the sizes built from this are
    // compared against the substream rather than looped over, so handing back
    // the low 32 would let a value of 2^32 or more pass a check its true size
    // fails.
    std::uint64_t variable_bits(int n_bits, std::string_view name) noexcept {
        const std::size_t start = pos_;
        std::uint64_t value = 0;
        while (true) {
            value += peek_raw(n_bits);
            advance(n_bits);
            const bool more = peek_raw(1) != 0;
            advance(1);
            if (!more || overflow_) {
                break;
            }
            value <<= n_bits;
            value += std::uint64_t{1} << n_bits;
        }
        emit_element(start, pos_, value, name);
        return value;
    }

    // A field whose width the stream sets and whose bits the syntax does not
    // interpret (add_data, extensions_bits, drc2_bits): one record of the
    // whole width, valued at its last 64 bits. A record's width is 16 bits,
    // so a run longer than 65535 bits is recorded as consecutive 65535-bit
    // records, the last shorter.
    void read_run(std::uint64_t bits, std::string_view name) noexcept {
        while (bits > 0) {
            const std::uint64_t width = bits < kMaxRecordBits ? bits : kMaxRecordBits;
            const std::size_t start = pos_;
            std::uint64_t value = 0;
            for (std::uint64_t i = 0; i < width; ++i) {
                value = (value << 1U) | bit_at(pos_ + static_cast<std::size_t>(i));
            }
            advance_bits(static_cast<std::size_t>(width));
            emit(start, static_cast<int>(width), value, name);
            bits -= width;
        }
    }

    // Unrecorded bits: alignment, fill, reserved payload skipped over.
    void skip(std::size_t bits) noexcept { advance_bits(bits); }

    void align() noexcept { advance_bits((8 - (pos_ % 8)) % 8); }

    // The peeks and the recorded-read helpers below exist for the Huffman and
    // escape decoders, which decide a width before they know it.
    [[nodiscard]] std::uint32_t peek_raw(int bits) const noexcept {
        if (bits <= 0) {
            return 0;
        }
        if (bits > 32) {
            return peek_slow(bits);
        }
        return static_cast<std::uint32_t>(window() >> static_cast<unsigned>(64 - bits));
    }

    void consume(int bits) noexcept { advance(bits); }

    // A zero-width element (a sign-bit group with no nonzero lines) is not a
    // record: it occupies no bits, and the Python transcription skips it too.
    // Nor is an element that runs past the end of the substream: the reader
    // hands back zeros for it and the syntax function fails at its next
    // check, so the trace ends with the last element the substream holds.
    void emit(std::size_t start, int bits, std::uint64_t value, std::string_view name) const {
        if (sink_ && bits > 0 && start + static_cast<std::size_t>(bits) <= size_bits()) {
            sink_(SyntaxRecord{substream_, static_cast<std::uint32_t>(start),
                               static_cast<std::uint16_t>(bits), value, name});
        }
    }

    // One element, whatever its width. A record's width is 16 bits, so an
    // element wider than 65535 bits is recorded as consecutive 65535-bit
    // records, the last shorter, each valued at its own last 64 bits - the
    // shape read_run() uses, which the Python transcription follows too. Only
    // variable_bits() can reach that width, and a value of 65536 bits of
    // continuation groups carries nothing worth recording as a number.
    void emit_element(std::size_t start, std::size_t end, std::uint64_t value,
                      std::string_view name) const {
        const std::size_t total = end - start;
        if (total <= kMaxRecordBits) {
            emit(start, static_cast<int>(total), value, name);
            return;
        }
        for (std::size_t at = start; at < end;) {
            const std::size_t width = std::min<std::size_t>(kMaxRecordBits, end - at);
            std::uint64_t chunk = 0;
            for (std::size_t i = 0; i < width; ++i) {
                chunk = (chunk << 1U) | bit_at(at + i);
            }
            emit(at, static_cast<int>(width), chunk, name);
            at += width;
        }
    }

    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] std::size_t size_bits() const noexcept { return data_.size() * 8U; }
    [[nodiscard]] std::size_t remaining_bits() const noexcept {
        return pos_ >= size_bits() ? 0 : size_bits() - pos_;
    }
    [[nodiscard]] bool overflow() const noexcept { return overflow_; }
    [[nodiscard]] int substream() const noexcept { return substream_; }
    [[nodiscard]] SyntaxSink sink() const noexcept { return sink_; }

    // Moves to an absolute bit position inside the data, as when metadata()
    // is reached through audio_size rather than by reading audio_data().
    void seek(std::size_t bit_position) noexcept {
        if (bit_position > size_bits()) {
            overflow_ = true;
            pos_ = size_bits();
        } else {
            pos_ = bit_position;
        }
    }

   private:
    static constexpr std::uint64_t kMaxRecordBits = 65535;

    // The 64 bits from the current position on, the first in the top bit, zeros
    // past the end of the data. The cache holds the 8 bytes from cache_byte_ on; the
    // position is inside its first 4 bytes, so at least 33 of the bits are real.
    [[nodiscard]] std::uint64_t window() const noexcept {
        const std::size_t byte = pos_ >> 3U;
        if (byte < cache_byte_ || byte - cache_byte_ > 3U) {
            load(byte);
        }
        return cache_ << (pos_ - cache_byte_ * 8U);
    }

    void load(std::size_t byte) const noexcept {
        cache_byte_ = byte;
        std::uint64_t word = 0;
        if (byte + 8U <= data_.size()) {
            std::memcpy(&word, data_.data() + byte, sizeof word);
            if constexpr (std::endian::native == std::endian::little) {
                word = std::byteswap(word);
            }
        } else {
            for (std::size_t i = 0; i < 8U; ++i) {
                word = (word << 8U) |
                       (byte + i < data_.size() ? static_cast<std::uint64_t>(data_[byte + i]) : 0U);
            }
        }
        cache_ = word;
    }

    // A peek wider than the word holds, bit by bit: the low 32 bits of the value.
    [[nodiscard]] std::uint32_t peek_slow(int bits) const noexcept {
        std::uint32_t value = 0;
        for (int i = 0; i < bits; ++i) {
            value = (value << 1U) | bit_at(pos_ + static_cast<std::size_t>(i));
        }
        return value;
    }

    [[nodiscard]] std::uint32_t bit_at(std::size_t bit) const noexcept {
        if (bit >= size_bits()) {
            return 0;
        }
        const auto byte = static_cast<std::uint32_t>(data_[bit / 8U]);
        return (byte >> (7U - static_cast<unsigned>(bit % 8U))) & 1U;
    }

    void advance(int bits) noexcept { advance_bits(static_cast<std::size_t>(bits)); }

    void advance_bits(std::size_t bits) noexcept {
        pos_ += bits;
        if (pos_ > size_bits()) {
            overflow_ = true;
        }
    }

    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    int substream_ = 0;
    SyntaxSink sink_{};
    bool overflow_ = false;
    // The cache of window(): the 8 bytes from cache_byte_ on, as a big-endian word.
    // The data does not change, so it needs no invalidating; it is mutable because a
    // peek fills it.
    mutable std::uint64_t cache_ = 0;
    mutable std::size_t cache_byte_ = std::numeric_limits<std::size_t>::max();
};

}  // namespace iclforge::ac4::detail
