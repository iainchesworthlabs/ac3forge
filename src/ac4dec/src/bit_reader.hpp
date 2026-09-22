#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "ac4dec/decoder.hpp"

// The bit reader every syntax function reads through. MSB first, as both
// parts read (Part 1 clause 3.4). Reading past the end does not throw and
// does not read out of bounds: it returns zeros and sets a sticky flag, which
// each syntax function checks at the points where carrying on would loop on
// a count read from nothing.
//
// Every read that corresponds to a syntax element names that element, and
// when a SyntaxSink is attached the reader emits one SyntaxRecord for it (see
// SyntaxRecord in ac4dec/decoder.hpp for what counts as one element). Reads
// that are not syntax elements - byte_align, fill bits, skipped bytes - go
// through skip() and align(), which emit nothing.

namespace ac4::detail {

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
        std::uint32_t value = 0;
        for (int i = 0; i < bits; ++i) {
            value = (value << 1U) | bit_at(pos_ + static_cast<std::size_t>(i));
        }
        return value;
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
};

}  // namespace ac4::detail
