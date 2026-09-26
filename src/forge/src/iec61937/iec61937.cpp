#include "ac3/iec61937/iec61937.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/core/eac3_tables.hpp"

namespace ac3::iec61937 {

namespace {

void put_word_le(std::vector<std::byte>& out, std::uint16_t word) {
    out.push_back(static_cast<std::byte>(word & 0xFF));
    out.push_back(static_cast<std::byte>(word >> 8));
}

// Shared by every packer here: the elementary stream is big-endian within
// each 16-bit word, the IEC 61937 carrier is little-endian PCM16, and the
// burst is zero-padded to its length either way. AC-3 and E-AC-3 syncframes
// are measured in whole 16-bit words (Table 5.18's byte counts and Annex E's
// frmsiz are both word counts), but an AC-4 frame is counted in bytes and can
// end half-way through a word: its last byte then goes in the word's high
// half, and the low half is stuffed with zero (IEC 61937-1 6.1.2 and 6.3.2).
void pack_payload_words(std::vector<std::byte>& burst, std::span<const std::byte> payload,
                        std::size_t burst_bytes) {
    for (std::size_t i = 0; i < payload.size(); i += 2) {
        const auto high = std::to_integer<std::uint16_t>(payload[i]);
        const auto low = i + 1 < payload.size() ? std::to_integer<std::uint16_t>(payload[i + 1])
                                                : std::uint16_t{0};
        put_word_le(burst, static_cast<std::uint16_t>((high << 8) | low));
    }
    burst.resize(burst_bytes, std::byte{0});
}

}  // namespace

std::expected<std::vector<std::byte>, WrapError> wrap_frame(std::span<const std::byte> frame) {
    if (frame.size() < 6 || (frame.size() & 1) != 0 ||
        std::to_integer<std::uint8_t>(frame[0]) != 0x0B ||
        std::to_integer<std::uint8_t>(frame[1]) != 0x77) {
        return std::unexpected(WrapError::kNotAFrame);
    }
    if (frame.size() + 8 > kBurstBytes) {
        return std::unexpected(WrapError::kFrameTooLarge);
    }
    // bsmod: the 3 bits after the 5-bit bsid in byte 5.
    const auto bsmod = std::to_integer<std::uint16_t>(frame[5]) & 0x7;

    std::vector<std::byte> burst;
    burst.reserve(kBurstBytes);
    put_word_le(burst, 0xF872);  // Pa
    put_word_le(burst, 0x4E1F);  // Pb
    put_word_le(burst, static_cast<std::uint16_t>(1 | (bsmod << 8)));  // Pc: type 1 = AC-3
    put_word_le(burst, static_cast<std::uint16_t>(frame.size() * 8));  // Pd: bits
    pack_payload_words(burst, frame, kBurstBytes);
    return burst;
}

struct Eac3BurstPacker::Impl {
    std::vector<std::byte> pending_;
    int blocks_pending_ = 0;
};

Eac3BurstPacker::Eac3BurstPacker() : impl_(std::make_unique<Impl>()) {}
Eac3BurstPacker::~Eac3BurstPacker() = default;
Eac3BurstPacker::Eac3BurstPacker(Eac3BurstPacker&&) noexcept = default;
Eac3BurstPacker& Eac3BurstPacker::operator=(Eac3BurstPacker&&) noexcept = default;

std::expected<std::optional<std::vector<std::byte>>, WrapError> Eac3BurstPacker::push(
    std::span<const std::byte> access_unit) {
    if (access_unit.size() < 6 || (access_unit.size() & 1) != 0 ||
        std::to_integer<std::uint8_t>(access_unit[0]) != 0x0B ||
        std::to_integer<std::uint8_t>(access_unit[1]) != 0x77) {
        return std::unexpected(WrapError::kNotAFrame);
    }
    if (impl_->pending_.size() + access_unit.size() + 8 > kEac3BurstBytes) {
        impl_->pending_.clear();
        impl_->blocks_pending_ = 0;
        return std::unexpected(WrapError::kFrameTooLarge);
    }

    // byte 4: fscod (2 bits) | numblkscod (2 bits) | acmod (3 bits) | lfeon
    // (1 bit). byte 5's top 5 bits are bsid. Same layout this project's own
    // eac3_decoder.cpp parses, read directly here the way spdif_header_eac3
    // does (pkt->data[4], pkt->data[5]) rather than pulling in a decoder.
    const auto byte4 = std::to_integer<std::uint32_t>(access_unit[4]);
    const auto bsid = std::to_integer<std::uint32_t>(access_unit[5]) >> 3;
    const auto fscod = byte4 >> 6;
    const auto numblkscod = (byte4 >> 4) & 0x3;
    // fscod == 3 selects the reduced-sample-rate path (§E2.3.1.3), which does
    // not transmit numblkscod at all — it is implicitly always the six-block
    // code. Mirrors spdif_header_eac3's own bsid > 10 && fscod != 3 guard.
    const int blocks = (bsid > 10 && fscod != 3)
                           ? eac3::blocks_per_syncframe(static_cast<int>(numblkscod))
                           : 6;

    impl_->pending_.insert(impl_->pending_.end(), access_unit.begin(), access_unit.end());
    impl_->blocks_pending_ += blocks;
    if (impl_->blocks_pending_ < 6) {
        return std::nullopt;
    }

    std::vector<std::byte> burst;
    burst.reserve(kEac3BurstBytes);
    put_word_le(burst, 0xF872);  // Pa
    put_word_le(burst, 0x4E1F);  // Pb
    put_word_le(burst, 0x0015);  // Pc: IEC61937_EAC3, no data-type-dependent bits
    put_word_le(burst, static_cast<std::uint16_t>(impl_->pending_.size()));  // Pd: BYTES, not bits
    pack_payload_words(burst, impl_->pending_, kEac3BurstBytes);

    impl_->pending_.clear();
    impl_->blocks_pending_ = 0;
    return std::optional<std::vector<std::byte>>{std::move(burst)};
}

std::expected<std::vector<std::byte>, WrapError> wrap_stream(
    std::span<const std::span<const std::byte>> units, bool eac3) {
    std::vector<std::byte> payload;
    if (eac3) {
        Eac3BurstPacker packer;
        for (const auto& unit : units) {
            const auto burst = packer.push(unit);
            if (!burst.has_value()) {
                return std::unexpected(burst.error());
            }
            if (burst->has_value()) {
                payload.insert(payload.end(), (**burst).begin(), (**burst).end());
            }
        }
        return payload;
    }
    payload.reserve(units.size() * kBurstBytes);
    for (const auto& unit : units) {
        const auto burst = wrap_frame(unit);
        if (!burst.has_value()) {
            return std::unexpected(burst.error());
        }
        payload.insert(payload.end(), burst->begin(), burst->end());
    }
    return payload;
}

// ---------------------------------------------------------------------------
// AC-4 (IEC 61937-14:2017).
//
// The tables below are transcribed from Part 14 and checked against renders
// of its pages; tests/iec61937/test_iec61937_ac4.cpp holds a second
// transcription of their cases, made separately, and checks the two against
// each other and against what the standard's own arithmetic implies.
//
// Readings, with their reasons:
//
// 1. Data-burst 0 of a sequence. Tables 6, 12, 18 and 24 give the periods of
//    five bursts at 29.97, 59.94 and 119.88 fps but not which frame of a
//    stream is burst 0. In AC-3 terms the answer would not matter; here it
//    decides the period each frame gets. ETSI TS 103 190-2 clause 5.11 has the
//    same five-frame cycle for the decoder's sample rate converter, and locks
//    it to the stream: phase phi_t is sequence_counter mod 5, or the last
//    frame's phase plus one when sequence_counter is 0 (the first frame after
//    a splice, TS 103 190-1 4.3.3.2.2), or 0 for a first frame with counter 0.
//    Both tables are the same cycle anchored at the same frame, the one that
//    starts where the fractional frame grid meets the integer sample grid:
//    Part 14's bursts start at the nearest IEC 60958 frame to each frame's
//    exact start from there (1602, 1601, 1602, 1601, 1602 at 29.97 fps), and
//    TS 103 190-2 Table 47's decoded frames at the sample below it (1601, 1602,
//    1601, 1602, 1602). So a frame goes in data-burst phi_t, and a stream
//    packed from any frame gives each frame the same period, as clause 5.11
//    gives each the same decoded length.
//
// 2. Pd's unit for AC-4 and AC-4 LD. Part 14 says bits (5.3.1, 5.3.7) and
//    tabulates the maximum in bits (Tables 9 and 26: 65 408 for a 2 048-frame
//    period, which only fits as bits). IEC 61937-2:2021+AMD1:2026 Table 2
//    lists bytes for both, and bits for no other part of data type 24. The
//    part that defines the data-burst governs (IEC 61937-1 6.1.8.2: data types
//    are allocated in Part 2 and specified in the other parts), so the packer
//    writes bits. The sync frame states its own size, which lets the reader
//    accept a Pd in either unit, and it does.
//
// Also noted, without a choice to make: Part 14 clause 5.3.7 sends the reader
// of AC-4 LD's maximum burst-lengths to Table 9 where it means Table 26, and
// Table 25's 400/401 row to Table 6 where it means Table 24; the values agree.
// Table 25's code 14 (187.5 fps) has no frame_rate_index in ETSI TS 103 190-1
// V1.4.1, whose Table 83 reserves 14 and 15, so no frame reaches that row.
// ---------------------------------------------------------------------------

namespace {

// One row of Part 14's tables for one burst type. Part 14 lists the AC-4 audio
// frame rates in the order TS 103 190-1 Table 83 indexes them (the Pc codes of
// Tables 7, 13 and 19 equal frame_rate_index); each row names the index its
// rate has there, and the tests check the periods against Table 83's rates.
struct Ac4Row {
    int fs_index;                             // 1: 48 kHz, 0: 44.1 kHz (TS 103 190-1 4.3.3.2.5)
    int frame_rate_index;                     // -1: a rate TS 103 190-1 V1.4.1 has no index for
    std::uint8_t code;                        // Pc bits 8 to 11
    std::array<std::uint16_t, 5> periods;     // data-bursts 0 to 4
    std::array<std::uint16_t, 5> max_length;  // Pd, in the type's unit
};

constexpr std::array<std::uint16_t, 5> same(std::uint16_t value) {
    return {value, value, value, value, value};
}

// clang-format off
// AC-4, subdata type 0, at an IEC 60958 frame rate equal to the base sampling
// frequency: Table 5 (period), Table 6 (sequence), Tables 7 and 8 (code) and
// Table 9 (the largest burst-length, in bits).
constexpr std::array<Ac4Row, 15> kAc4Rows{{
    {1,  0,  0, same(2002), same(63936)},                                          // 23,976 fps
    {1,  1,  1, same(2000), same(63872)},                                          // 24
    {1,  2,  2, same(1920), same(61312)},                                          // 25
    {1,  3,  3, {1602, 1601, 1602, 1601, 1602}, {51136, 51104, 51136, 51104, 51136}},  // 29,97
    {1,  4,  4, same(1600), same(51072)},                                          // 30
    {1,  5,  5, same(1001), same(31904)},                                          // 47,952
    {1,  6,  6, same(1000), same(31872)},                                          // 48
    {1,  7,  7, same(960),  same(30592)},                                          // 50
    {1,  8,  8, {801, 801, 800, 801, 801}, {25504, 25504, 25472, 25504, 25504}},   // 59,94
    {1,  9,  9, same(800),  same(25472)},                                          // 60
    {1, 10, 10, same(480),  same(15232)},                                          // 100
    {1, 11, 11, {400, 401, 400, 401, 400}, {12672, 12704, 12672, 12704, 12672}},   // 119,88
    {1, 12, 12, same(400),  same(12672)},                                          // 120
    {1, 13, 13, same(2048), same(65408)},                                          // 23,438
    {0, 13, 13, same(2048), same(65408)},                                          // 21,533 at 44,1 kHz
}};

// AC-4 HBR4, subdata type 1, at four times the base sampling frequency: Table
// 11, Table 12, Tables 13 and 14, and Table 15 (in bytes).
constexpr std::array<Ac4Row, 15> kAc4Hbr4Rows{{
    {1,  0,  0, same(8008), same(32016)},                                          // 23,976 fps
    {1,  1,  1, same(8000), same(31984)},                                          // 24
    {1,  2,  2, same(7680), same(30704)},                                          // 25
    {1,  3,  3, {6408, 6404, 6408, 6404, 6408}, {25616, 25600, 25616, 25600, 25616}},  // 29,97
    {1,  4,  4, same(6400), same(25584)},                                          // 30
    {1,  5,  5, same(4004), same(16000)},                                          // 47,952
    {1,  6,  6, same(4000), same(15984)},                                          // 48
    {1,  7,  7, same(3840), same(15344)},                                          // 50
    {1,  8,  8, {3204, 3204, 3200, 3204, 3204}, {12800, 12800, 12784, 12800, 12800}},  // 59,94
    {1,  9,  9, same(3200), same(12784)},                                          // 60
    {1, 10, 10, same(1920), same(7664)},                                           // 100
    {1, 11, 11, {1600, 1604, 1600, 1604, 1600}, {6384, 6400, 6384, 6400, 6384}},   // 119,88
    {1, 12, 12, same(1600), same(6384)},                                           // 120
    {1, 13, 13, same(8192), same(32752)},                                          // 23,438
    {0, 13, 13, same(8192), same(32752)},                                          // 21,533 at 44,1 kHz
}};

// AC-4 HBR16, subdata type 2, at sixteen times the base sampling frequency:
// Table 17, Table 18, Tables 19 and 20, and Table 21 (in 8-byte units).
constexpr std::array<Ac4Row, 15> kAc4Hbr16Rows{{
    {1,  0,  0, same(32032), same(16014)},                                         // 23,976 fps
    {1,  1,  1, same(32000), same(15998)},                                         // 24
    {1,  2,  2, same(30720), same(15358)},                                         // 25
    {1,  3,  3, {25632, 25616, 25632, 25616, 25632}, {12814, 12806, 12814, 12806, 12814}},  // 29,97
    {1,  4,  4, same(25600), same(12798)},                                         // 30
    {1,  5,  5, same(16016), same(8006)},                                          // 47,952
    {1,  6,  6, same(16000), same(7998)},                                          // 48
    {1,  7,  7, same(15360), same(7678)},                                          // 50
    {1,  8,  8, {12816, 12816, 12800, 12816, 12816}, {6406, 6406, 6398, 6406, 6406}},  // 59,94
    {1,  9,  9, same(12800), same(6398)},                                          // 60
    {1, 10, 10, same(7680),  same(3838)},                                          // 100
    {1, 11, 11, {6400, 6416, 6400, 6416, 6400}, {3198, 3206, 3198, 3206, 3198}},   // 119,88
    {1, 12, 12, same(6400),  same(3198)},                                          // 120
    {1, 13, 13, same(32768), same(16382)},                                         // 23,438
    {0, 13, 13, same(32768), same(16382)},                                         // 21,533 at 44,1 kHz
}};

// AC-4 LD, subdata type 3, at 48 kHz only: Table 23, Table 24, Table 25 and
// Table 26 (in bits).
constexpr std::array<Ac4Row, 4> kAc4LdRows{{
    {1, 10, 10, same(480), same(15232)},                                           // 100 fps
    {1, 11, 11, {400, 401, 400, 401, 400}, {12672, 12704, 12672, 12704, 12672}},   // 119,88
    {1, 12, 12, same(400), same(12672)},                                           // 120
    {1, -1, 14, same(256), same(8064)},                                            // 187,5
}};
// clang-format on

[[nodiscard]] std::span<const Ac4Row> ac4_rows(BurstDataType type) {
    switch (type) {
        case BurstDataType::kAc4:
            return kAc4Rows;
        case BurstDataType::kAc4Hbr4:
            return kAc4Hbr4Rows;
        case BurstDataType::kAc4Hbr16:
            return kAc4Hbr16Rows;
        case BurstDataType::kAc4Ld:
            return kAc4LdRows;
        case BurstDataType::kAc3:
        case BurstDataType::kEac3:
            break;
    }
    return {};
}

// The link's rate over the base sampling frequency (clauses 5.3.1, 5.3.3,
// 5.3.5 and 5.3.7).
[[nodiscard]] std::uint32_t link_multiplier(BurstDataType type) {
    switch (type) {
        case BurstDataType::kAc4Hbr4:
            return 4;
        case BurstDataType::kAc4Hbr16:
            return 16;
        case BurstDataType::kAc3:
        case BurstDataType::kEac3:
        case BurstDataType::kAc4:
        case BurstDataType::kAc4Ld:
            break;
    }
    return 1;
}

[[nodiscard]] Ac4BurstTiming timing_of(BurstDataType type, const Ac4Row& row) {
    const std::uint32_t base_hz = row.fs_index == 0 ? 44100U : 48000U;
    return Ac4BurstTiming{.code = row.code,
                          .link_rate_hz = base_hz * link_multiplier(type),
                          .periods = row.periods,
                          .max_length = row.max_length};
}

// Pd for a payload of `bytes` in `type`'s unit: bits for AC-4 and AC-4 LD,
// bytes for HBR4, 8-byte units for HBR16, a part-filled unit counting whole.
[[nodiscard]] std::size_t ac4_length_code(BurstDataType type, std::size_t bytes) {
    switch (type) {
        case BurstDataType::kAc4Hbr4:
            return bytes;
        case BurstDataType::kAc4Hbr16:
            return (bytes + 7) / 8;
        case BurstDataType::kAc3:
        case BurstDataType::kEac3:
        case BurstDataType::kAc4:
        case BurstDataType::kAc4Ld:
            break;
    }
    return bytes * 8;
}

// The head of an AC-4 sync frame (Part 14 Tables A.1 to A.3): the syncword,
// then a 16-bit size, or 0xFFFF and a 24-bit one.
struct Ac4Head {
    std::size_t header_bytes = 0;
    std::size_t frame_size = 0;
    bool crc = false;

    [[nodiscard]] std::size_t total() const { return header_bytes + frame_size + (crc ? 2U : 0U); }
};

constexpr std::uint16_t kAc4Syncword = 0xAC40;
constexpr std::uint16_t kAc4SyncwordCrc = 0xAC41;
// The most a sync frame's head takes: the syncword, 0xFFFF and 24 bits.
constexpr std::size_t kAc4HeadMaxBytes = 7;

enum class HeadRead : std::uint8_t { kHead, kNotAFrame, kNeedMore };

// Reads a sync frame's head from its first bytes, in stream order.
[[nodiscard]] HeadRead read_ac4_head(std::span<const std::byte> first, Ac4Head& head) {
    const auto byte = [&](std::size_t i) { return std::to_integer<std::size_t>(first[i]); };
    if (first.size() < 4) {
        return HeadRead::kNeedMore;
    }
    const std::size_t syncword = (byte(0) << 8) | byte(1);
    if (syncword != kAc4Syncword && syncword != kAc4SyncwordCrc) {
        return HeadRead::kNotAFrame;
    }
    head.crc = syncword == kAc4SyncwordCrc;
    head.frame_size = (byte(2) << 8) | byte(3);
    head.header_bytes = 4;
    if (head.frame_size == 0xFFFF) {
        if (first.size() < kAc4HeadMaxBytes) {
            return HeadRead::kNeedMore;
        }
        head.frame_size = (byte(4) << 16) | (byte(5) << 8) | byte(6);
        head.header_bytes = kAc4HeadMaxBytes;
    }
    return HeadRead::kHead;
}

// MSB-first bits over a span, for the few fields of a table of contents the
// packer reads; nothing once the span runs out.
class TocBits {
   public:
    explicit TocBits(std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::optional<std::uint32_t> read(int count) {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            if (bit_ >= bytes_.size() * 8) {
                return std::nullopt;
            }
            const auto byte = std::to_integer<std::uint32_t>(bytes_[bit_ / 8]);
            value = (value << 1U) | ((byte >> (7U - (bit_ % 8))) & 1U);
            ++bit_;
        }
        return value;
    }

    // variable_bits(n) (ETSI TS 103 190-1 4.2.2, Table 3). Eight rounds are
    // far more than any value a table of contents holds, and keep the sum well
    // inside 32 bits.
    [[nodiscard]] std::optional<std::uint32_t> variable_bits(int count) {
        std::uint32_t value = 0;
        for (int round = 0; round < 8; ++round) {
            const std::optional<std::uint32_t> part = read(count);
            const std::optional<std::uint32_t> more = part ? read(1) : std::nullopt;
            if (!more) {
                return std::nullopt;
            }
            value += *part;
            if (*more == 0) {
                return value;
            }
            value = (value << static_cast<unsigned>(count)) + (1U << static_cast<unsigned>(count));
        }
        return std::nullopt;
    }

   private:
    std::span<const std::byte> bytes_;
    std::size_t bit_ = 0;
};

}  // namespace

std::optional<Ac4BurstTiming> ac4_burst_timing(BurstDataType type, int fs_index,
                                               int frame_rate_index) {
    if (frame_rate_index < 0) {
        return std::nullopt;
    }
    for (const Ac4Row& row : ac4_rows(type)) {
        if (row.fs_index == fs_index && row.frame_rate_index == frame_rate_index) {
            return timing_of(type, row);
        }
    }
    return std::nullopt;
}

std::optional<Ac4BurstTiming> ac4_burst_timing_for_code(BurstDataType type, int code) {
    // The 48 kHz rows come first, and code 13's 44.1 kHz row gives the same
    // periods and maxima as its 48 kHz one.
    for (const Ac4Row& row : ac4_rows(type)) {
        if (row.code == code) {
            return timing_of(type, row);
        }
    }
    return std::nullopt;
}

std::optional<BurstDataType> ac4_burst_type_for(std::size_t frame_bytes, int fs_index,
                                                int frame_rate_index) {
    for (const BurstDataType type :
         {BurstDataType::kAc4, BurstDataType::kAc4Hbr4, BurstDataType::kAc4Hbr16}) {
        const std::optional<Ac4BurstTiming> timing =
            ac4_burst_timing(type, fs_index, frame_rate_index);
        if (timing && ac4_length_code(type, frame_bytes) <=
                          *std::min_element(timing->max_length.begin(), timing->max_length.end())) {
            return type;
        }
    }
    return std::nullopt;
}

std::optional<Ac4SyncFrame> read_ac4_sync_frame(std::span<const std::byte> bytes) {
    Ac4Head head;
    if (read_ac4_head(bytes.first(std::min(bytes.size(), kAc4HeadMaxBytes)), head) !=
            HeadRead::kHead ||
        head.total() != bytes.size()) {
        return std::nullopt;
    }
    // ac4_toc() as far as frame_rate_index (TS 103 190-1 Table 4; TS 103 190-2
    // 6.2.1.1, the same fields in the same order).
    TocBits toc(bytes.subspan(head.header_bytes, head.frame_size));
    std::optional<std::uint32_t> version = toc.read(2);
    if (version == 3U) {
        const std::optional<std::uint32_t> more = toc.variable_bits(2);
        version = more ? std::optional<std::uint32_t>(*version + *more) : std::nullopt;
    }
    const std::optional<std::uint32_t> sequence_counter = version ? toc.read(10) : std::nullopt;
    std::optional<std::uint32_t> wait = sequence_counter ? toc.read(1) : std::nullopt;
    if (wait == 1U) {
        const std::optional<std::uint32_t> wait_frames = toc.read(3);
        // br_code in TS 103 190-2; reserved in TS 103 190-1. Two bits either way.
        wait = wait_frames == 0U ? std::optional<std::uint32_t>(0U)
               : wait_frames     ? toc.read(2)
                                 : std::nullopt;
    }
    const std::optional<std::uint32_t> fs_index = wait ? toc.read(1) : std::nullopt;
    const std::optional<std::uint32_t> frame_rate_index = fs_index ? toc.read(4) : std::nullopt;
    if (!frame_rate_index) {
        return std::nullopt;
    }
    return Ac4SyncFrame{.bytes = bytes.size(),
                        .crc = head.crc,
                        .sequence_counter = static_cast<int>(*sequence_counter),
                        .fs_index = static_cast<int>(*fs_index),
                        .frame_rate_index = static_cast<int>(*frame_rate_index)};
}

Ac4BurstPacker::Ac4BurstPacker(BurstDataType type) : type_(type) {}

std::expected<std::vector<std::byte>, WrapError> Ac4BurstPacker::push(
    std::span<const std::byte> sync_frame) {
    const std::optional<Ac4SyncFrame> frame =
        is_ac4(type_) ? read_ac4_sync_frame(sync_frame) : std::nullopt;
    if (!frame) {
        return std::unexpected(WrapError::kNotAFrame);
    }
    if (fs_index_ && *fs_index_ != frame->fs_index) {
        return std::unexpected(WrapError::kRateChanged);
    }
    // The frame's place in TS 103 190-2 5.11's five-frame cycle (reading 1
    // above): a frame the packer refuses below is still one of the stream's,
    // and a following frame with sequence_counter 0 continues from it.
    const int phase = frame->sequence_counter != 0 ? frame->sequence_counter % 5
                      : phase_                     ? (*phase_ + 1) % 5
                                                   : 0;
    fs_index_ = frame->fs_index;
    phase_ = phase;

    const std::optional<Ac4BurstTiming> timing =
        ac4_burst_timing(type_, frame->fs_index, frame->frame_rate_index);
    if (!timing) {
        return std::unexpected(WrapError::kUnsupportedRate);
    }
    const auto index = static_cast<std::size_t>(phase);
    const std::size_t length = ac4_length_code(type_, frame->bytes);
    if (length > timing->max_length[index]) {
        return std::unexpected(WrapError::kFrameTooLarge);
    }
    const std::size_t period = timing->periods[index];
    const auto pc = static_cast<std::uint16_t>(static_cast<unsigned>(type_) |
                                               (static_cast<unsigned>(timing->code) << 8U));
    const auto pd = static_cast<std::uint16_t>(length);

    std::vector<std::byte> burst;
    burst.reserve(period * 4);
    put_word_le(burst, 0xF872);  // Pa
    put_word_le(burst, 0x4E1F);  // Pb
    put_word_le(burst, pc);
    put_word_le(burst, pd);
    // The maxima leave the preamble and two IEC 60958 frames of spacing
    // (IEC 61937-1 6.3.4) inside the period, so the frame always fits. The
    // zeros after it are HBR16's padding and the stuffing both.
    pack_payload_words(burst, sync_frame, period * 4);
    last_ = Packed{.pc = pc,
                   .pd = pd,
                   .period = static_cast<std::uint32_t>(period),
                   .sequence_index = phase,
                   .payload_bytes = type_ == BurstDataType::kAc4Hbr16 ? length * 8 : frame->bytes,
                   .link_rate_hz = timing->link_rate_hz};
    return burst;
}

std::expected<std::vector<std::byte>, WrapError> wrap_ac4_stream(
    std::span<const std::span<const std::byte>> frames, BurstDataType type) {
    Ac4BurstPacker packer(type);
    std::vector<std::byte> carrier;
    for (const auto& frame : frames) {
        const auto burst = packer.push(frame);
        if (!burst.has_value()) {
            return std::unexpected(burst.error());
        }
        carrier.insert(carrier.end(), burst->begin(), burst->end());
    }
    return carrier;
}

// ---------------------------------------------------------------------------
// De-framing.
// ---------------------------------------------------------------------------

namespace {

// Pa 0xF872 then Pb 0x4E1F, as they appear in carrier bytes under each word
// order. Distinct four-byte strings, and neither is a substring of the
// other's repetition, so finding one identifies the order outright - there is
// no heuristic here to get wrong.
constexpr std::array<std::byte, 4> kPreambleLe{std::byte{0x72}, std::byte{0xF8}, std::byte{0x1F},
                                               std::byte{0x4E}};
constexpr std::array<std::byte, 4> kPreambleBe{std::byte{0xF8}, std::byte{0x72}, std::byte{0x4E},
                                               std::byte{0x1F}};

constexpr std::size_t kPreambleBytes = 8;  // Pa Pb Pc Pd

// The elementary-stream syncword, 0x0B77, as the first payload word. Every
// AC-3 and E-AC-3 syncframe starts with it, so a "preamble" not followed by
// one is a false match inside payload bytes or stuffing. An AC-4 sync frame
// starts with 0xAC40 or 0xAC41 instead (IEC 61937-14 Table A.3).
constexpr std::uint16_t kSyncword = 0x0B77;

[[nodiscard]] bool starts_frame(BurstDataType type, std::uint16_t first_word) {
    return is_ac4(type) ? first_word == kAc4Syncword || first_word == kAc4SyncwordCrc
                        : first_word == kSyncword;
}

[[nodiscard]] std::uint16_t read_word(std::span<const std::byte> bytes, std::size_t index,
                                      WordOrder order) {
    const auto low = std::to_integer<std::uint16_t>(bytes[index]);
    const auto high = std::to_integer<std::uint16_t>(bytes[index + 1]);
    return order == WordOrder::kLittleEndian ? static_cast<std::uint16_t>(low | (high << 8))
                                             : static_cast<std::uint16_t>((low << 8) | high);
}

// Where a preamble starts at or after `from`, and which order it was in.
// Scans byte-wise rather than word-wise: nothing guarantees a capture began
// on a word boundary, and a carrier read from the middle of a file routinely
// does not.
struct Preamble {
    std::size_t offset;
    WordOrder order;
};

[[nodiscard]] std::optional<Preamble> find_preamble(std::span<const std::byte> carrier,
                                                    std::size_t from,
                                                    std::optional<WordOrder> locked) {
    if (carrier.size() < 4) {
        return std::nullopt;
    }
    for (std::size_t i = from; i + 4 <= carrier.size(); ++i) {
        const auto four = carrier.subspan(i, 4);
        if ((!locked || *locked == WordOrder::kLittleEndian) &&
            std::equal(four.begin(), four.end(), kPreambleLe.begin())) {
            return Preamble{i, WordOrder::kLittleEndian};
        }
        if ((!locked || *locked == WordOrder::kBigEndian) &&
            std::equal(four.begin(), four.end(), kPreambleBe.begin())) {
            return Preamble{i, WordOrder::kBigEndian};
        }
    }
    return std::nullopt;
}

// Pd's unit is data-type-dependent: bits for AC-3, AC-4 and AC-4 LD (and for
// IEC 61937-2's general rule), bytes for E-AC-3 and AC-4 HBR4, 8-byte units
// for AC-4 HBR16. Returns the payload length in elementary bytes. A bit count
// that is not a whole number of bytes rounds up, so the carrier words that
// hold it are still accounted for.
[[nodiscard]] std::size_t payload_bytes_from_pd(std::uint16_t pd,
                                                std::optional<BurstDataType> type) {
    if (type == BurstDataType::kEac3 || type == BurstDataType::kAc4Hbr4) {
        return pd;
    }
    if (type == BurstDataType::kAc4Hbr16) {
        return static_cast<std::size_t>(pd) * 8;
    }
    return (static_cast<std::size_t>(pd) + 7) / 8;
}

// All seven bits of Pc's data type: an AC-3 burst is conventional type 1 with
// subdata type 0, and 1 with any other subdata type is not AC-3.
[[nodiscard]] std::optional<BurstDataType> known_data_type(std::uint16_t pc) {
    // clang-format off
    switch (pc & 0x7F) {
        case 0x01: return BurstDataType::kAc3;
        case 0x15: return BurstDataType::kEac3;
        case 0x18: return BurstDataType::kAc4;
        case 0x38: return BurstDataType::kAc4Hbr4;
        case 0x58: return BurstDataType::kAc4Hbr16;
        case 0x78: return BurstDataType::kAc4Ld;
        default: return std::nullopt;
    }
    // clang-format on
}

// The elementary bytes and the carrier's payload bytes an AC-4 burst holds,
// once the sync frame's head has said how long the frame is.
struct Ac4Payload {
    std::size_t frame_bytes = 0;
    std::size_t payload_bytes = 0;
};

// Whether Pd agrees with a sync frame of `frame_bytes`: in the type's own unit
// (IEC 61937-14), or for AC-4 and AC-4 LD in bytes (IEC 61937-2 Table 2;
// reading 2 above). HBR16's payload runs on to a whole 8-byte unit.
[[nodiscard]] std::optional<Ac4Payload> ac4_payload(BurstDataType type, std::uint16_t pd,
                                                    std::size_t frame_bytes) {
    const std::size_t payload = payload_bytes_from_pd(pd, type);
    switch (type) {
        case BurstDataType::kAc4:
        case BurstDataType::kAc4Ld:
            if (payload == frame_bytes || pd == frame_bytes) {
                return Ac4Payload{.frame_bytes = frame_bytes, .payload_bytes = frame_bytes};
            }
            break;
        case BurstDataType::kAc4Hbr4:
            if (payload == frame_bytes) {
                return Ac4Payload{.frame_bytes = frame_bytes, .payload_bytes = payload};
            }
            break;
        case BurstDataType::kAc4Hbr16:
            if (payload >= frame_bytes && payload - frame_bytes < 8) {
                return Ac4Payload{.frame_bytes = frame_bytes, .payload_bytes = payload};
            }
            break;
        case BurstDataType::kAc3:
        case BurstDataType::kEac3:
            break;
    }
    return std::nullopt;
}

// Carrier bytes one payload occupies: whole 16-bit words, so an odd byte
// count still costs a full word.
[[nodiscard]] std::size_t payload_carrier_bytes(std::size_t payload_bytes) {
    return ((payload_bytes + 1) / 2) * 2;
}

// The payload's `index`-th byte in elementary-stream order, the payload
// starting at `offset`: little-endian words put the stream's first byte second.
[[nodiscard]] std::byte payload_byte(std::span<const std::byte> carrier, std::size_t offset,
                                     std::size_t index, WordOrder order) {
    return carrier[order == WordOrder::kLittleEndian ? offset + (index ^ 1U) : offset + index];
}

// Un-swap payload words into elementary-stream order, appending exactly
// `payload_bytes` of them. The mirror of pack_payload_words above; a
// big-endian carrier already holds them in stream order and only copies.
void unpack_payload_words(std::span<const std::byte> carrier, std::size_t offset,
                          std::size_t payload_bytes, WordOrder order, std::vector<std::byte>& out) {
    const auto first = out.size();
    out.resize(first + payload_bytes);
    for (std::size_t i = 0; i < payload_bytes; ++i) {
        // Little-endian words put the stream's first byte second: word
        // (b0 b1) carries stream bytes (b1 b0).
        const std::size_t source =
            order == WordOrder::kLittleEndian ? offset + (i ^ 1u) : offset + i;
        out[first + i] = carrier[source];
    }
}

}  // namespace

std::string_view describe(UnwrapError error) {
    switch (error) {
        case UnwrapError::kNoSync: return "no IEC 61937 preamble found";
        case UnwrapError::kTruncatedBurst: return "burst payload cut off by end of input";
        case UnwrapError::kPayloadTooLarge: return "burst length exceeds its repetition period";
    }
    return "unknown error";
}

std::string_view data_type_name(BurstDataType type) {
    switch (type) {
        case BurstDataType::kAc3:
            return "AC-3";
        case BurstDataType::kEac3:
            return "E-AC-3";
        case BurstDataType::kAc4:
            return "AC-4";
        case BurstDataType::kAc4Hbr4:
            return "AC-4 HBR4";
        case BurstDataType::kAc4Hbr16:
            return "AC-4 HBR16";
        case BurstDataType::kAc4Ld:
            return "AC-4 LD";
    }
    return "unknown";
}

std::size_t repetition_period(BurstDataType type) {
    switch (type) {
        case BurstDataType::kAc3:
            return kBurstBytes;
        case BurstDataType::kEac3:
            return kEac3BurstBytes;
        case BurstDataType::kAc4:
        case BurstDataType::kAc4Hbr4:
        case BurstDataType::kAc4Hbr16:
        case BurstDataType::kAc4Ld:
            break;
    }
    std::size_t longest = 0;
    for (const Ac4Row& row : ac4_rows(type)) {
        for (const std::uint16_t period : row.periods) {
            longest = std::max<std::size_t>(longest, period);
        }
    }
    // Four bytes to an IEC 60958 frame: two subframes of 16 bits.
    return longest * 4;
}

void BurstReader::compact() {
    if (pos_ == 0) {
        return;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(pos_));
    dropped_ += pos_;
    pos_ = 0;
}

std::expected<void, UnwrapError> BurstReader::push(std::span<const std::byte> carrier,
                                                   std::vector<std::byte>& out) {
    buffer_.insert(buffer_.end(), carrier.begin(), carrier.end());

    for (;;) {
        if (state_ == State::kPayload) {
            const auto available = buffer_.size() - pos_;
            if (available < payload_needed_) {
                break;
            }
            if (emitting_) {
                unpack_payload_words(buffer_, pos_, payload_bytes_, payload_order_, out);
                ++bursts_;
            }
            pos_ += payload_needed_;
            state_ = State::kSyncing;
            continue;
        }

        const auto found = find_preamble(buffer_, pos_, order_);
        if (!found.has_value()) {
            // Keep only what a preamble could still straddle into the next
            // chunk: three bytes, one short of the pattern. std::max, because
            // a burst that ended within those last three bytes has already
            // put pos_ past them and must not be walked back into.
            pos_ = std::max(pos_, buffer_.size() >= 3 ? buffer_.size() - 3 : 0);
            break;
        }
        // Ten bytes decide a candidate: the four preamble words plus the
        // first payload word, which must be the syncframe's own.
        if (found->offset + kPreambleBytes + 2 > buffer_.size()) {
            pos_ = found->offset;
            break;
        }

        const std::span<const std::byte> view{buffer_};
        const auto pc = read_word(view, found->offset + 4, found->order);
        const auto pd = read_word(view, found->offset + 6, found->order);
        const auto type = known_data_type(pc);
        const auto payload_bytes = payload_bytes_from_pd(pd, type);

        if (!type.has_value()) {
            // Another codec's passthrough, or a null/pause burst. Its length
            // is still readable under the general bits rule, so step over the
            // payload rather than rescanning through it - a false preamble
            // inside somebody else's payload would only cost accuracy here.
            // A length that could not fit any burst period is not believed,
            // and the resync simply starts after the preamble instead.
            ++skipped_bursts_;
            payload_needed_ = payload_bytes <= kEac3BurstBytes - kPreambleBytes
                                  ? payload_carrier_bytes(payload_bytes)
                                  : 0;
            payload_bytes_ = 0;
            emitting_ = false;
            pos_ = found->offset + kPreambleBytes;
            state_ = State::kPayload;
            continue;
        }
        if (payload_bytes > repetition_period(*type) - kPreambleBytes) {
            return std::unexpected(UnwrapError::kPayloadTooLarge);
        }
        if (payload_bytes < 2 ||
            !starts_frame(*type, read_word(view, found->offset + kPreambleBytes, found->order))) {
            // The preamble bytes turned up inside payload or stuffing. Resync
            // one byte on rather than trusting a length nothing corroborates.
            ++false_syncs_;
            pos_ = found->offset + 1;
            continue;
        }
        // An AC-4 frame states its own length, which Pd has to agree with, and
        // which is what goes out: HBR16's padding stays behind.
        Ac4Payload lengths{.frame_bytes = payload_bytes, .payload_bytes = payload_bytes};
        if (is_ac4(*type)) {
            std::array<std::byte, kAc4HeadMaxBytes> first{};
            const std::size_t start = found->offset + kPreambleBytes;
            // Whole words only: in a little-endian carrier a word's first
            // stream byte is its second carrier byte.
            const std::size_t have = std::min(first.size(), (buffer_.size() - start) / 2 * 2);
            for (std::size_t i = 0; i < have; ++i) {
                first[i] = payload_byte(view, start, i, found->order);
            }
            Ac4Head head;
            const HeadRead read =
                read_ac4_head(std::span<const std::byte>(first).first(have), head);
            if (read == HeadRead::kNeedMore) {
                pos_ = found->offset;
                break;
            }
            const std::optional<Ac4Payload> agreed =
                read == HeadRead::kHead ? ac4_payload(*type, pd, head.total()) : std::nullopt;
            if (!agreed || agreed->payload_bytes > repetition_period(*type) - kPreambleBytes) {
                ++false_syncs_;
                pos_ = found->offset + 1;
                continue;
            }
            lengths = *agreed;
        }

        data_type_ = data_type_.value_or(*type);
        order_ = found->order;
        last_header_ =
            BurstHeader{.data_type = *type,
                        .data_type_dependent = static_cast<std::uint8_t>((pc >> 8) & 0x1F),
                        .stream_number = static_cast<std::uint8_t>(pc >> 13),
                        .error_flag = (pc & 0x80) != 0,
                        .pd = pd,
                        .payload_bytes = lengths.frame_bytes,
                        .offset = dropped_ + found->offset};
        payload_bytes_ = lengths.frame_bytes;
        payload_needed_ = payload_carrier_bytes(lengths.payload_bytes);
        payload_order_ = found->order;
        emitting_ = true;
        pos_ = found->offset + kPreambleBytes;
        state_ = State::kPayload;
    }

    compact();
    return {};
}

std::expected<void, UnwrapError> BurstReader::finish() const {
    if (state_ == State::kPayload) {
        return std::unexpected(UnwrapError::kTruncatedBurst);
    }
    return {};
}

std::expected<std::vector<std::byte>, UnwrapError> unwrap_stream(
    std::span<const std::byte> carrier) {
    BurstReader reader;
    std::vector<std::byte> out;
    if (const auto pushed = reader.push(carrier, out); !pushed.has_value()) {
        return std::unexpected(pushed.error());
    }
    if (const auto done = reader.finish(); !done.has_value()) {
        return std::unexpected(done.error());
    }
    if (reader.bursts() == 0) {
        return std::unexpected(UnwrapError::kNoSync);
    }
    return out;
}

void carrier_from_capture(std::span<const float> interleaved, std::uint16_t channels,
                          std::vector<std::byte>& out) {
    if (channels == 0) {
        return;
    }
    const auto stride = static_cast<std::size_t>(channels);
    // Only the first two channels: IEC 61937 is a stereo carrier, and a
    // capture that offers more is padding the rest.
    const auto carried = std::min<std::size_t>(stride, 2);
    for (std::size_t base = 0; base + stride <= interleaved.size(); base += stride) {
        for (std::size_t ch = 0; ch < carried; ++ch) {
            // The exact inverse of every backend's int16 -> float step
            // (x / 32768.0f), so a word that came in as PCM16 comes back
            // bit-identical. Clamping matters only for a float source, which
            // by construction is not carrying bursts anyway.
            const auto clamped = std::clamp(interleaved[base + ch], -1.0f, 1.0f);
            const auto word = static_cast<std::int32_t>(std::lround(clamped * 32768.0f));
            const auto sample = static_cast<std::uint16_t>(
                std::clamp(word, std::int32_t{-32768}, std::int32_t{32767}));
            out.push_back(static_cast<std::byte>(sample & 0xFF));
            out.push_back(static_cast<std::byte>(sample >> 8));
        }
    }
}

void PassthroughDetector::push(std::span<const float> interleaved, std::uint16_t channels) {
    if (decided() || channels == 0) {
        return;
    }
    // Take only what is left of the inspection budget, so a caller handing
    // over a second of audio at a time cannot make this hold a second of
    // audio: undecided or not, buffered_ never exceeds kInspectBytes by more
    // than the frame that crossed it.
    const auto stride = static_cast<std::size_t>(channels);
    const auto per_frame = 2 * std::min<std::size_t>(stride, 2);
    const auto budget = (kInspectBytes - inspected_ + per_frame - 1) / per_frame;
    const auto take = std::min(budget, interleaved.size() / stride);
    const auto before = buffered_.size();
    carrier_from_capture(interleaved.first(take * stride), channels, buffered_);
    inspected_ += buffered_.size() - before;

    // Same acceptance test the reader applies: a preamble AND a syncframe
    // behind it. A lone preamble pattern shows up in ordinary loud audio
    // often enough that it cannot be the whole answer.
    const std::span<const std::byte> view{buffered_};
    std::size_t from = 0;
    for (;;) {
        const auto found = find_preamble(view, from, std::nullopt);
        if (!found.has_value()) {
            break;
        }
        if (found->offset + kPreambleBytes + 2 > view.size()) {
            break;
        }
        const auto pc = read_word(view, found->offset + 4, found->order);
        const auto type = known_data_type(pc);
        const auto pd = read_word(view, found->offset + 6, found->order);
        const auto payload_bytes = payload_bytes_from_pd(pd, type);
        if (type.has_value() && payload_bytes >= 2 &&
            payload_bytes <= repetition_period(*type) - kPreambleBytes &&
            starts_frame(*type, read_word(view, found->offset + kPreambleBytes, found->order))) {
            detected_ = type;
            order_ = found->order;
            // Keep the carrier from this burst on: everything before it is
            // whatever the capture was doing beforehand, and no reader wants
            // to resync through it.
            buffered_.erase(buffered_.begin(),
                            buffered_.begin() + static_cast<std::ptrdiff_t>(found->offset));
            return;
        }
        from = found->offset + 1;
    }
    if (inspected_ >= kInspectBytes) {
        buffered_.clear();
    }
}

}  // namespace ac3::iec61937
