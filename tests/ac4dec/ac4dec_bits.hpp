#pragma once

// A bit writer and a syntax recorder for the AC-4 decoder's syntax tests:
// the writer builds a substream field by field, MSB first as both parts read,
// and the recorder keeps the SyntaxRecords the reader emits so a test can say
// which elements were read, at which width and with which value.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4dec/decoder.hpp"

namespace ac4dec_test {

class BitWriter {
   public:
    BitWriter& put(std::uint64_t value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            bits_.push_back(((value >> static_cast<unsigned>(i)) & 1U) != 0);
        }
        return *this;
    }

    BitWriter& flag(bool value) { return put(value ? 1U : 0U, 1); }

    // variable_bits(n_bits) (Part 1 clause 4.2.2) for `value`: the inverse of
    // BitReader::variable_bits().
    BitWriter& variable_bits(std::uint64_t value, int n_bits) {
        std::vector<std::uint64_t> groups;
        groups.push_back(value & ((std::uint64_t{1} << n_bits) - 1));
        value >>= n_bits;
        while (value > 0) {
            value -= 1;
            groups.push_back(value & ((std::uint64_t{1} << n_bits) - 1));
            value >>= n_bits;
        }
        for (std::size_t i = groups.size(); i-- > 0;) {
            put(groups[i], n_bits);
            flag(i != 0);
        }
        return *this;
    }

    // The codeword of `index` in `codebook` (an ac4::detail::Codebook; a
    // template so that this header needs none of the decoder's own).
    template <typename Codebook>
    BitWriter& code(const Codebook& codebook, int index) {
        for (const auto& entry : codebook.sorted) {
            if (entry.index == index) {
                return put(entry.code, entry.bits);
            }
        }
        FAIL("codebook " << codebook.name << " has no index " << index);
        return *this;
    }

    BitWriter& append(const BitWriter& other) {
        bits_.insert(bits_.end(), other.bits_.begin(), other.bits_.end());
        return *this;
    }

    // Pads with zeros to the next byte boundary.
    BitWriter& align() {
        while (bits_.size() % 8 != 0) {
            bits_.push_back(false);
        }
        return *this;
    }

    [[nodiscard]] std::size_t size() const { return bits_.size(); }

    [[nodiscard]] std::vector<std::byte> bytes() const {
        std::vector<std::byte> out((bits_.size() + 7) / 8, std::byte{0});
        for (std::size_t i = 0; i < bits_.size(); ++i) {
            if (bits_[i]) {
                out[i / 8] |= static_cast<std::byte>(0x80U >> (i % 8));
            }
        }
        return out;
    }

   private:
    std::vector<bool> bits_;
};

// Keeps every record; the element names are string literals that outlive it.
struct Recorder {
    std::vector<ac4::SyntaxRecord> records;

    void operator()(const ac4::SyntaxRecord& record) { records.push_back(record); }

    [[nodiscard]] int count(std::string_view name) const {
        int n = 0;
        for (const auto& record : records) {
            n += record.name == name ? 1 : 0;
        }
        return n;
    }

    // The value of the first record of that name; FAILs when none was read.
    [[nodiscard]] std::uint64_t value(std::string_view name) const {
        for (const auto& record : records) {
            if (record.name == name) {
                return record.value;
            }
        }
        FAIL("no " << std::string{name} << " was read");
        return 0;
    }

    [[nodiscard]] std::uint64_t end_bit() const {
        return records.empty() ? 0 : std::uint64_t{records.back().bit_offset} + records.back().bits;
    }
};

}  // namespace ac4dec_test
