#include "bit_writer.hpp"

#include <array>

namespace ac4::detail {
namespace {

// Part 1 clause 4.2.2: a decoder reads a group, and while the continuation
// bit that follows is set, shifts the value left by n_bits and adds 1 << n_bits
// before reading the next group. So the last group is the value's low n_bits,
// and each group before it is what is left once that one is removed, less
// one. At most 64 bits of value, so at most 64 groups.
struct Groups {
    std::array<std::uint64_t, 64> values{};
    std::size_t count = 0;  // values[count - 1] is written first
};

[[nodiscard]] Groups split(unsigned n_bits, std::uint64_t value) noexcept {
    Groups groups;
    const std::uint64_t mask = (std::uint64_t{1} << n_bits) - 1;
    groups.values[groups.count++] = value & mask;
    std::uint64_t rest = value >> n_bits;
    while (rest > 0 && groups.count < groups.values.size()) {
        rest -= 1;
        groups.values[groups.count++] = rest & mask;
        rest >>= n_bits;
    }
    return groups;
}

}  // namespace

unsigned variable_bits_width(unsigned n_bits, std::uint64_t value) noexcept {
    return static_cast<unsigned>(split(n_bits, value).count) * (n_bits + 1);
}

BitWriter::BitWriter(int substream, SyntaxSink sink) noexcept : substream_(substream), sink_(sink) {}

BitWriter BitWriter::buffered() {
    BitWriter writer;
    writer.buffering_ = true;
    return writer;
}

void BitWriter::clear() noexcept {
    bytes_.clear();
    kept_.clear();
    bits_ = 0;
}

void BitWriter::emit(const SyntaxRecord& record) {
    if (buffering_) {
        kept_.push_back(record);
    } else if (sink_) {
        sink_(record);
    }
}

void BitWriter::append(const BitWriter& other) {
    const auto start = bits_;
    for (std::size_t i = 0; i < other.bits_; ++i) {
        const auto byte = std::to_integer<unsigned>(other.bytes_[i / 8]);
        put(1, (byte >> (7 - (i % 8))) & 1U);
    }
    for (SyntaxRecord record : other.kept_) {
        record.substream = substream_;
        record.bit_offset += static_cast<std::uint32_t>(start);
        emit(record);
    }
}

void BitWriter::put(unsigned bits, std::uint64_t value) {
    for (unsigned i = bits; i > 0; --i) {
        const auto bit = static_cast<unsigned>((value >> (i - 1)) & 1U);
        if (bits_ % 8 == 0) {
            bytes_.push_back(std::byte{0});
        }
        if (bit != 0) {
            const auto shift = static_cast<unsigned>(7 - (bits_ % 8));
            bytes_.back() |= static_cast<std::byte>(1U << shift);
        }
        ++bits_;
    }
}

void BitWriter::write(unsigned bits, std::uint64_t value, std::string_view name) {
    // A zero-width element, such as the sign bits of a codeword whose lines
    // are all zero, is not recorded (docs/verification.md).
    if (bits == 0) {
        return;
    }
    const auto offset = bits_;
    put(bits, value);
    emit(SyntaxRecord{substream_, static_cast<std::uint32_t>(offset), static_cast<std::uint16_t>(bits), value, name});
}

void BitWriter::write_variable_bits(unsigned n_bits, std::uint64_t value, std::string_view name) {
    const auto offset = bits_;
    const Groups groups = split(n_bits, value);
    for (std::size_t i = groups.count; i > 0; --i) {
        put(n_bits, groups.values[i - 1]);
        put(1, i > 1 ? 1U : 0U);
    }
    emit(SyntaxRecord{substream_, static_cast<std::uint32_t>(offset), static_cast<std::uint16_t>(bits_ - offset),
                      value, name});
}

void BitWriter::write_codeword(std::span<const HuffCode> codebook, std::size_t index, std::string_view name) {
    const auto offset = bits_;
    const HuffCode& code = codebook[index];
    put(code.bits, code.code);
    emit(SyntaxRecord{substream_, static_cast<std::uint32_t>(offset), code.bits, index, name});
}

void BitWriter::write_unrecorded(unsigned bits, std::uint64_t value) {
    put(bits, value);
}

void BitWriter::write_as(unsigned bits, std::uint64_t raw, std::uint64_t value, std::string_view name) {
    const auto offset = bits_;
    put(bits, raw);
    emit(SyntaxRecord{substream_, static_cast<std::uint32_t>(offset), static_cast<std::uint16_t>(bits), value, name});
}

void BitWriter::align() {
    while (bits_ % 8 != 0) {
        put(1, 0);
    }
}

}  // namespace ac4::detail
