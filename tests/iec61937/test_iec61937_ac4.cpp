#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/iec61937/iec61937.hpp"
#include "ac4/ac4.hpp"

// AC-4 over IEC 61937 (IEC 61937-14:2017), phase D11 of planning/ac4.md.
//
// The first two cases hold a second transcription of Part 14's tables, typed
// from the printed pages separately from the one in iec61937.cpp and in the
// shape the pages print them, and check the library's against it and against
// what the standard's own arithmetic implies. The rest pack streams at every
// frame rate each burst type has, read them back, and check that every frame
// comes back unchanged with the repetition period, sequence and Pc fields the
// tables give: constructed frames, whose table of contents is written here,
// the Dolby Encoding Engine's streams at 23.44, 24, 25 and 29.97 fps, and
// every DEE leg of a directory, the committed ones unless AC4DEC_STREAM_DIR
// names another.

namespace {

namespace iec = ac3::iec61937;
using iec::BurstDataType;

std::uint8_t u8(std::span<const std::byte> bytes, std::size_t index) {
    return std::to_integer<std::uint8_t>(bytes[index]);
}

std::uint16_t le16(std::span<const std::byte> bytes, std::size_t index) {
    return static_cast<std::uint16_t>(u8(bytes, index) | (u8(bytes, index + 1) << 8));
}

// --- The tables as Part 14 prints them --------------------------------------

// One row of a burst type's tables: the frame rate as the table prints it, the
// frame_rate_index ETSI TS 103 190-1 Tables 83 and 84 give that rate (-1 for
// none), the code (Tables 7, 8, 13, 14, 19, 20, 25), the period, printed as
// "low / high" at the three 1000/1001 rates (Tables 5, 11, 17, 23), and the
// maximum burst-length printed beside each of the two (Tables 9, 15, 21, 26).
struct Printed {
    std::string_view fps;
    int fs_index;
    int frame_rate_index;
    int code;
    std::uint32_t period_low;
    std::uint32_t period_high;
    std::uint32_t max_low;
    std::uint32_t max_high;
};

// clang-format off
// Tables 5, 7, 8 and 9: AC-4.
constexpr std::array<Printed, 15> kPrintedAc4{{
    {"23,976", 1, 0, 0, 2002, 2002, 63936, 63936},
    {"24", 1, 1, 1, 2000, 2000, 63872, 63872},
    {"25", 1, 2, 2, 1920, 1920, 61312, 61312},
    {"29,97", 1, 3, 3, 1601, 1602, 51104, 51136},
    {"30", 1, 4, 4, 1600, 1600, 51072, 51072},
    {"47,952", 1, 5, 5, 1001, 1001, 31904, 31904},
    {"48", 1, 6, 6, 1000, 1000, 31872, 31872},
    {"50", 1, 7, 7, 960, 960, 30592, 30592},
    {"59,94", 1, 8, 8, 800, 801, 25472, 25504},
    {"60", 1, 9, 9, 800, 800, 25472, 25472},
    {"100", 1, 10, 10, 480, 480, 15232, 15232},
    {"119,88", 1, 11, 11, 400, 401, 12672, 12704},
    {"120", 1, 12, 12, 400, 400, 12672, 12672},
    {"23,438", 1, 13, 13, 2048, 2048, 65408, 65408},
    {"21,533", 0, 13, 13, 2048, 2048, 65408, 65408},
}};

// Tables 11, 13, 14 and 15: AC-4 HBR4.
constexpr std::array<Printed, 15> kPrintedHbr4{{
    {"23,976", 1, 0, 0, 8008, 8008, 32016, 32016},
    {"24", 1, 1, 1, 8000, 8000, 31984, 31984},
    {"25", 1, 2, 2, 7680, 7680, 30704, 30704},
    {"29,97", 1, 3, 3, 6404, 6408, 25600, 25616},
    {"30", 1, 4, 4, 6400, 6400, 25584, 25584},
    {"47,952", 1, 5, 5, 4004, 4004, 16000, 16000},
    {"48", 1, 6, 6, 4000, 4000, 15984, 15984},
    {"50", 1, 7, 7, 3840, 3840, 15344, 15344},
    {"59,94", 1, 8, 8, 3200, 3204, 12784, 12800},
    {"60", 1, 9, 9, 3200, 3200, 12784, 12784},
    {"100", 1, 10, 10, 1920, 1920, 7664, 7664},
    {"119,88", 1, 11, 11, 1600, 1604, 6384, 6400},
    {"120", 1, 12, 12, 1600, 1600, 6384, 6384},
    {"23,438", 1, 13, 13, 8192, 8192, 32752, 32752},
    {"21,533", 0, 13, 13, 8192, 8192, 32752, 32752},
}};

// Tables 17, 19, 20 and 21: AC-4 HBR16.
constexpr std::array<Printed, 15> kPrintedHbr16{{
    {"23,976", 1, 0, 0, 32032, 32032, 16014, 16014},
    {"24", 1, 1, 1, 32000, 32000, 15998, 15998},
    {"25", 1, 2, 2, 30720, 30720, 15358, 15358},
    {"29,97", 1, 3, 3, 25616, 25632, 12806, 12814},
    {"30", 1, 4, 4, 25600, 25600, 12798, 12798},
    {"47,952", 1, 5, 5, 16016, 16016, 8006, 8006},
    {"48", 1, 6, 6, 16000, 16000, 7998, 7998},
    {"50", 1, 7, 7, 15360, 15360, 7678, 7678},
    {"59,94", 1, 8, 8, 12800, 12816, 6398, 6406},
    {"60", 1, 9, 9, 12800, 12800, 6398, 6398},
    {"100", 1, 10, 10, 7680, 7680, 3838, 3838},
    {"119,88", 1, 11, 11, 6400, 6416, 3198, 3206},
    {"120", 1, 12, 12, 6400, 6400, 3198, 3198},
    {"23,438", 1, 13, 13, 32768, 32768, 16382, 16382},
    {"21,533", 0, 13, 13, 32768, 32768, 16382, 16382},
}};

// Tables 23, 25 and 26: AC-4 LD. 187,5 fps has no frame_rate_index in
// TS 103 190-1 V1.4.1, whose Table 83 reserves 14 and 15.
constexpr std::array<Printed, 4> kPrintedLd{{
    {"100", 1, 10, 10, 480, 480, 15232, 15232},
    {"119,88", 1, 11, 11, 400, 401, 12672, 12704},
    {"120", 1, 12, 12, 400, 400, 12672, 12672},
    {"187,5", 1, -1, 14, 256, 256, 8064, 8064},
}};

// Tables 6, 12, 18 and 24: data-bursts 0 to 4 at 29,97, 59,94 and 119,88 fps.
struct PrintedSequence {
    int frame_rate_index;
    std::array<std::uint32_t, 5> periods;
};
constexpr std::array<PrintedSequence, 3> kTable6{{
    {3, {1602, 1601, 1602, 1601, 1602}},
    {8, {801, 801, 800, 801, 801}},
    {11, {400, 401, 400, 401, 400}},
}};
constexpr std::array<PrintedSequence, 3> kTable12{{
    {3, {6408, 6404, 6408, 6404, 6408}},
    {8, {3204, 3204, 3200, 3204, 3204}},
    {11, {1600, 1604, 1600, 1604, 1600}},
}};
constexpr std::array<PrintedSequence, 3> kTable18{{
    {3, {25632, 25616, 25632, 25616, 25632}},
    {8, {12816, 12816, 12800, 12816, 12816}},
    {11, {6400, 6416, 6400, 6416, 6400}},
}};
constexpr std::array<PrintedSequence, 1> kTable24{{
    {11, {400, 401, 400, 401, 400}},
}};
// clang-format on

struct PrintedType {
    BurstDataType type;
    std::span<const Printed> rows;
    std::span<const PrintedSequence> sequences;
    // Part 14 5.3.1, 5.3.3, 5.3.5 and 5.3.7: the IEC 60958 frame rate over the
    // base sampling frequency.
    std::uint32_t multiplier;
};

const std::array<PrintedType, 4> kPrintedTypes{{
    {BurstDataType::kAc4, kPrintedAc4, kTable6, 1},
    {BurstDataType::kAc4Hbr4, kPrintedHbr4, kTable12, 4},
    {BurstDataType::kAc4Hbr16, kPrintedHbr16, kTable18, 16},
    {BurstDataType::kAc4Ld, kPrintedLd, kTable24, 1},
}};

// The five periods a printed row stands for: Table 6's sequence at a
// 1000/1001 rate, the one printed period five times otherwise.
std::array<std::uint32_t, 5> printed_periods(const PrintedType& table, const Printed& row) {
    for (const PrintedSequence& sequence : table.sequences) {
        if (sequence.frame_rate_index == row.frame_rate_index) {
            return sequence.periods;
        }
    }
    return {row.period_low, row.period_low, row.period_low, row.period_low, row.period_low};
}

// The maximum printed beside a period.
std::uint32_t printed_max(const Printed& row, std::uint32_t period) {
    return period == row.period_low ? row.max_low : row.max_high;
}

// ETSI TS 103 190-1 Tables 83 (48 kHz) and 84 (44,1 kHz): each
// frame_rate_index's frame rate as a ratio, and the rates Part 14 adds.
struct Rate {
    std::uint64_t numerator;
    std::uint64_t denominator;
};

Rate table83_rate(int fs_index, int frame_rate_index) {
    constexpr std::array<Rate, 13> kTable83{{{24000, 1001},
                                             {24, 1},
                                             {25, 1},
                                             {30000, 1001},
                                             {30, 1},
                                             {48000, 1001},
                                             {48, 1},
                                             {50, 1},
                                             {60000, 1001},
                                             {60, 1},
                                             {100, 1},
                                             {120000, 1001},
                                             {120, 1}}};
    if (frame_rate_index == 13) {
        // (23,44) at 48 kHz and 11 025/512 at 44,1 kHz: a 2 048-sample frame.
        return fs_index == 1 ? Rate{48000, 2048} : Rate{44100, 2048};
    }
    if (frame_rate_index < 0) {
        return Rate{375, 2};  // Part 14's 187,5 fps, AC-4 LD only
    }
    return kTable83[static_cast<std::size_t>(frame_rate_index)];
}

// --- Frames ------------------------------------------------------------------

// MSB-first bits, for the table of contents of a constructed frame.
class BitWriter {
   public:
    void put(std::uint32_t value, int bits) {
        for (int bit = bits - 1; bit >= 0; --bit) {
            if (fill_ == 0) {
                bytes_.push_back(std::byte{0});
            }
            if (((value >> bit) & 1U) != 0) {
                bytes_.back() |= static_cast<std::byte>(0x80U >> fill_);
            }
            fill_ = (fill_ + 1) % 8;
        }
    }
    [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }

   private:
    std::vector<std::byte> bytes_;
    int fill_ = 0;
};

struct FrameSpec {
    int fs_index = 1;
    int frame_rate_index = 13;
    int sequence_counter = 1;
    // raw_ac4_frame's size in bytes.
    std::size_t raw_bytes = 64;
    bool crc = false;
    // TS 103 190-1's wait_frames, with br_code when it is not 0.
    std::optional<int> wait_frames{};
    // bitstream_version 3 escapes to variable_bits(2).
    int bitstream_version = 2;
};

// An AC-4 sync frame (Part 14 Annex A) around a raw frame whose table of
// contents starts as `spec` says; the rest of the raw frame is filler, which
// nothing here reads, and the CRC word is not a real CRC, which nothing here
// checks either.
std::vector<std::byte> sync_frame(const FrameSpec& spec) {
    BitWriter toc;
    if (spec.bitstream_version >= 3) {
        toc.put(3, 2);
        // variable_bits(2) for bitstream_version - 3: one round for 0 to 3.
        toc.put(static_cast<std::uint32_t>(spec.bitstream_version - 3), 2);
        toc.put(0, 1);
    } else {
        toc.put(static_cast<std::uint32_t>(spec.bitstream_version), 2);
    }
    toc.put(static_cast<std::uint32_t>(spec.sequence_counter), 10);
    toc.put(spec.wait_frames ? 1U : 0U, 1);
    if (spec.wait_frames) {
        toc.put(static_cast<std::uint32_t>(*spec.wait_frames), 3);
        if (*spec.wait_frames > 0) {
            toc.put(2, 2);
        }
    }
    toc.put(static_cast<std::uint32_t>(spec.fs_index), 1);
    toc.put(static_cast<std::uint32_t>(spec.frame_rate_index), 4);
    toc.put(1, 1);  // b_iframe_global
    std::vector<std::byte> raw = toc.bytes();
    REQUIRE(raw.size() <= spec.raw_bytes);
    for (std::size_t i = raw.size(); i < spec.raw_bytes; ++i) {
        raw.push_back(static_cast<std::byte>((i * 37U + 11U) & 0xFFU));
    }

    std::vector<std::byte> frame;
    const auto put16 = [&](std::uint32_t value) {
        frame.push_back(static_cast<std::byte>((value >> 8) & 0xFFU));
        frame.push_back(static_cast<std::byte>(value & 0xFFU));
    };
    put16(spec.crc ? 0xAC41U : 0xAC40U);
    if (spec.raw_bytes < 0xFFFF) {
        put16(static_cast<std::uint32_t>(spec.raw_bytes));
    } else {
        put16(0xFFFF);
        frame.push_back(static_cast<std::byte>((spec.raw_bytes >> 16) & 0xFFU));
        put16(static_cast<std::uint32_t>(spec.raw_bytes & 0xFFFFU));
    }
    frame.insert(frame.end(), raw.begin(), raw.end());
    if (spec.crc) {
        put16(0x5A5A);
    }
    return frame;
}

// The Pd a sync frame of `bytes` takes in `type`'s unit.
std::size_t length_code(BurstDataType type, std::size_t bytes) {
    switch (type) {
        case BurstDataType::kAc4Hbr4:
            return bytes;
        case BurstDataType::kAc4Hbr16:
            return (bytes + 7) / 8;
        default:
            return bytes * 8;
    }
}

// The whole sync frames of an .ac4 file, back to back as ac4::scan finds them.
std::vector<std::vector<std::byte>> frames_of_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    std::vector<std::byte> data(chars.size());
    std::transform(chars.begin(), chars.end(), data.begin(),
                   [](char c) { return static_cast<std::byte>(c); });
    const ac4::ScanResult scanned = ac4::scan(data);
    REQUIRE_FALSE(scanned.frames.empty());
    REQUIRE_FALSE(scanned.stopped_at.has_value());
    std::vector<std::vector<std::byte>> frames;
    for (std::size_t i = 0; i < scanned.frames.size(); ++i) {
        const std::size_t begin = scanned.frames[i].offset;
        const std::size_t end =
            i + 1 < scanned.frames.size() ? scanned.frames[i + 1].offset : data.size();
        frames.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(begin),
                            data.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return frames;
}

std::vector<std::span<const std::byte>> views_of(const std::vector<std::vector<std::byte>>& owned) {
    return {owned.begin(), owned.end()};
}

// A frame's place in ETSI TS 103 190-2 5.11's five-frame cycle, for a frame
// whose sequence_counter is not 0.
int phase_of(int sequence_counter) {
    return sequence_counter % 5;
}

// Packs `frames` with `type`, checking every burst against the printed tables
// and Part 1's layout, then reads the carrier back a burst at a time and
// checks that every frame comes back unchanged at the offset its period puts
// it. `expected_periods` holds each burst's period as the caller works it out.
void pack_and_read_back(BurstDataType type, const std::vector<std::vector<std::byte>>& frames,
                        const std::vector<std::uint32_t>& expected_periods, int code) {
    REQUIRE(expected_periods.size() == frames.size());
    iec::Ac4BurstPacker packer(type);
    iec::BurstReader reader;
    std::uint64_t offset = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        CAPTURE(i, frames[i].size());
        const auto burst = packer.push(frames[i]);
        REQUIRE(burst.has_value());
        REQUIRE(packer.last().has_value());
        const auto& packed = *packer.last();
        CHECK(packed.period == expected_periods[i]);
        CHECK(burst->size() == static_cast<std::size_t>(expected_periods[i]) * 4);
        // Pa, Pb, then Pc: the data type in bits 0 to 6, the code in 8 to 11,
        // the error flag, bit 12 and the bitstream number all clear.
        CHECK(le16(*burst, 0) == 0xF872);
        CHECK(le16(*burst, 2) == 0x4E1F);
        const std::uint16_t pc = le16(*burst, 4);
        CHECK((pc & 0x7FU) == static_cast<unsigned>(type));
        CHECK(((pc >> 8U) & 0x0FU) == static_cast<unsigned>(code));
        CHECK((pc & 0xF080U) == 0U);
        CHECK(pc == packed.pc);
        CHECK(le16(*burst, 6) == length_code(type, frames[i].size()));
        CHECK(le16(*burst, 6) == packed.pd);
        // The frame big-endian in little-endian words, an odd last byte in the
        // high half of its word, and nothing but zeros after it.
        const std::vector<std::byte>& frame = frames[i];
        const std::size_t n = frame.size();
        bool payload_matches = true;
        for (std::size_t b = 0; b < n; ++b) {
            payload_matches = payload_matches && (*burst)[8 + (b ^ 1U)] == frame[b];
        }
        CHECK(payload_matches);
        if (n % 2 == 1) {
            // The low half of the last word, which Part 1 6.3.2 stuffs.
            CHECK((*burst)[8 + n - 1] == std::byte{0});
        }
        CHECK(std::all_of(burst->begin() + static_cast<std::ptrdiff_t>(8 + n + (n % 2)),
                          burst->end(), [](std::byte b) { return b == std::byte{0}; }));

        std::vector<std::byte> out;
        REQUIRE(reader.push(*burst, out).has_value());
        CHECK(out == frame);
        REQUIRE(reader.last_header().has_value());
        const iec::BurstHeader& header = *reader.last_header();
        CHECK(header.data_type == type);
        CHECK((header.data_type_dependent & 0x0F) == code);
        CHECK(header.pd == packed.pd);
        CHECK(header.payload_bytes == frame.size());
        // Measured as a receiver measures it: Pa to Pa.
        CHECK(header.offset == offset);
        offset += burst->size();
    }
    REQUIRE(reader.finish().has_value());
    CHECK(reader.bursts() == frames.size());
    CHECK(reader.false_syncs() == 0);
}

}  // namespace

// --- The tables ----------------------------------------------------------------

TEST_CASE("IEC 61937-14: every row of every AC-4 burst type as the standard prints it",
          "[iec61937][ac4]") {
    for (const PrintedType& table : kPrintedTypes) {
        CAPTURE(iec::data_type_name(table.type));
        for (const Printed& row : table.rows) {
            CAPTURE(row.fps, row.code);
            const std::array<std::uint32_t, 5> periods = printed_periods(table, row);
            const std::uint32_t base = row.fs_index == 1 ? 48000U : 44100U;

            const std::optional<iec::Ac4BurstTiming> by_code =
                iec::ac4_burst_timing_for_code(table.type, row.code);
            REQUIRE(by_code.has_value());
            std::optional<iec::Ac4BurstTiming> timing = by_code;
            if (row.frame_rate_index >= 0) {
                timing = iec::ac4_burst_timing(table.type, row.fs_index, row.frame_rate_index);
                REQUIRE(timing.has_value());
            }
            CHECK(timing->code == row.code);
            CHECK(timing->link_rate_hz == base * table.multiplier);
            for (std::size_t burst = 0; burst < 5; ++burst) {
                CHECK(timing->periods[burst] == periods[burst]);
                CHECK(timing->max_length[burst] == printed_max(row, periods[burst]));
                // The code names the same periods at either base rate.
                CHECK(by_code->periods[burst] == periods[burst]);
            }
        }
    }
}

TEST_CASE("IEC 61937-14: codes outside the tables and rates a type has no row for give nothing",
          "[iec61937][ac4]") {
    // Tables 7 and 13 reserve 14 and 15; Table 25 reserves 0 to 9, 13 and 15.
    for (const BurstDataType type :
         {BurstDataType::kAc4, BurstDataType::kAc4Hbr4, BurstDataType::kAc4Hbr16}) {
        CHECK_FALSE(iec::ac4_burst_timing_for_code(type, 14).has_value());
        CHECK_FALSE(iec::ac4_burst_timing_for_code(type, 15).has_value());
        // Table 84: frame_rate_index 13 is the only 44,1 kHz rate.
        for (int index = 0; index < 13; ++index) {
            CHECK_FALSE(iec::ac4_burst_timing(type, 0, index).has_value());
        }
        CHECK_FALSE(iec::ac4_burst_timing(type, 1, 14).has_value());
    }
    for (const int code : {0, 5, 9, 13, 15}) {
        CHECK_FALSE(iec::ac4_burst_timing_for_code(BurstDataType::kAc4Ld, code).has_value());
    }
    // AC-4 LD at 48 kHz only, and at 100, 119,88 and 120 fps.
    CHECK_FALSE(iec::ac4_burst_timing(BurstDataType::kAc4Ld, 1, 2).has_value());
    CHECK_FALSE(iec::ac4_burst_timing(BurstDataType::kAc4Ld, 0, 13).has_value());
    CHECK(iec::ac4_burst_timing(BurstDataType::kAc4Ld, 1, 11).has_value());
    // AC-3 and E-AC-3 are not AC-4 types.
    CHECK_FALSE(iec::ac4_burst_timing(BurstDataType::kAc3, 1, 13).has_value());
    CHECK_FALSE(iec::ac4_burst_timing_for_code(BurstDataType::kEac3, 13).has_value());
}

TEST_CASE("IEC 61937-14: the tables agree with the arithmetic the standard implies",
          "[iec61937][ac4]") {
    for (const PrintedType& table : kPrintedTypes) {
        CAPTURE(iec::data_type_name(table.type));
        for (const Printed& row : table.rows) {
            CAPTURE(row.fps);
            const std::array<std::uint32_t, 5> periods = printed_periods(table, row);
            const std::uint64_t base = row.fs_index == 1 ? 48000 : 44100;
            const Rate rate = table83_rate(row.fs_index, row.frame_rate_index);
            // Five bursts span five frames exactly: 5 x base x multiplier /
            // frame rate IEC 60958 frames, which is where TS 103 190-1's frame
            // rate for the row's frame_rate_index meets Part 14's periods.
            std::uint64_t sum = 0;
            for (const std::uint32_t period : periods) {
                sum += period;
            }
            CHECK(sum * rate.numerator == 5 * base * table.multiplier * rate.denominator);
            // At the base rate, data-burst k starts at the IEC 60958 frame
            // nearest the k-th frame's exact start, counted from burst 0.
            const double exact =
                static_cast<double>(base * rate.denominator) / static_cast<double>(rate.numerator);
            std::uint64_t start = 0;
            for (std::size_t k = 0; k < 5; ++k) {
                start += periods[k] / table.multiplier;
                CHECK(periods[k] % table.multiplier == 0);
                CHECK(start ==
                      static_cast<std::uint64_t>(std::llround(exact * static_cast<double>(k + 1))));
            }
            // The maxima leave the four preamble words and the two IEC 60958
            // frames of Part 1's burst spacing, four bytes to a frame.
            for (const std::uint32_t period : periods) {
                const std::uint64_t bytes = (static_cast<std::uint64_t>(period) - 2) * 4 - 8;
                const std::uint64_t in_unit = table.type == BurstDataType::kAc4Hbr4    ? bytes
                                              : table.type == BurstDataType::kAc4Hbr16 ? bytes / 8
                                                                                       : bytes * 8;
                CHECK(printed_max(row, period) == in_unit);
            }
            // An observation, which nothing relies on: Part 14 lists the rates
            // in Table 83's order, so the code is the frame_rate_index.
            if (row.frame_rate_index >= 0) {
                CHECK(row.code == row.frame_rate_index);
            }
        }
    }
}

TEST_CASE("IEC 61937-14: the longest period of each type bounds a burst", "[iec61937][ac4]") {
    CHECK(iec::repetition_period(BurstDataType::kAc4) == 2048 * 4);
    CHECK(iec::repetition_period(BurstDataType::kAc4Hbr4) == 8192 * 4);
    CHECK(iec::repetition_period(BurstDataType::kAc4Hbr16) == 32768 * 4);
    CHECK(iec::repetition_period(BurstDataType::kAc4Ld) == 480 * 4);
    CHECK(iec::data_type_name(BurstDataType::kAc4) == "AC-4");
    CHECK(iec::data_type_name(BurstDataType::kAc4Hbr16) == "AC-4 HBR16");
    CHECK(iec::is_ac4(BurstDataType::kAc4Ld));
    CHECK_FALSE(iec::is_ac4(BurstDataType::kEac3));
}

// --- Sync frames ---------------------------------------------------------------

TEST_CASE("read_ac4_sync_frame: the head and the table of contents' first fields",
          "[iec61937][ac4]") {
    const std::vector<std::byte> plain =
        sync_frame({.fs_index = 1, .frame_rate_index = 8, .sequence_counter = 1019});
    const auto read = iec::read_ac4_sync_frame(plain);
    REQUIRE(read.has_value());
    CHECK(read->bytes == plain.size());
    CHECK_FALSE(read->crc);
    CHECK(read->sequence_counter == 1019);
    CHECK(read->fs_index == 1);
    CHECK(read->frame_rate_index == 8);

    // wait_frames with br_code, a CRC word, bitstream_version's escape, and a
    // frame_size past 16 bits.
    const std::vector<std::byte> escaped = sync_frame({.fs_index = 0,
                                                       .frame_rate_index = 13,
                                                       .sequence_counter = 7,
                                                       .raw_bytes = 70000,
                                                       .crc = true,
                                                       .wait_frames = 3,
                                                       .bitstream_version = 5});
    const auto big = iec::read_ac4_sync_frame(escaped);
    REQUIRE(big.has_value());
    CHECK(big->bytes == 2 + 5 + 70000 + 2);
    CHECK(big->crc);
    CHECK(big->sequence_counter == 7);
    CHECK(big->fs_index == 0);
    CHECK(big->frame_rate_index == 13);
    const auto waited =
        iec::read_ac4_sync_frame(sync_frame({.frame_rate_index = 4, .wait_frames = 0}));
    REQUIRE(waited.has_value());
    CHECK(waited->frame_rate_index == 4);

    // Not one whole frame: a byte short, a byte over, another syncword.
    CHECK_FALSE(iec::read_ac4_sync_frame(std::span(plain).first(plain.size() - 1)).has_value());
    std::vector<std::byte> longer = plain;
    longer.push_back(std::byte{0});
    CHECK_FALSE(iec::read_ac4_sync_frame(longer).has_value());
    std::vector<std::byte> other = plain;
    other[1] = std::byte{0x42};
    CHECK_FALSE(iec::read_ac4_sync_frame(other).has_value());
    // A raw frame too short to reach frame_rate_index.
    const std::vector<std::byte> tiny{std::byte{0xAC}, std::byte{0x40}, std::byte{0x00},
                                      std::byte{0x01}, std::byte{0x80}};
    CHECK_FALSE(iec::read_ac4_sync_frame(tiny).has_value());
}

// --- Packing and reading back ------------------------------------------------------

TEST_CASE("Ac4BurstPacker: every frame rate of every type packs and reads back unchanged",
          "[iec61937][ac4]") {
    for (const PrintedType& table : kPrintedTypes) {
        CAPTURE(iec::data_type_name(table.type));
        for (const Printed& row : table.rows) {
            if (row.frame_rate_index < 0) {
                continue;  // no frame can say 187,5 fps
            }
            CAPTURE(row.fps, row.fs_index);
            const std::array<std::uint32_t, 5> periods = printed_periods(table, row);
            // Twelve frames from sequence_counter 3, of sizes that run from
            // small to the largest the smallest maximum allows, odd and even,
            // so every burst of a sequence gets both kinds.
            const std::uint32_t tightest = std::min(row.max_low, row.max_high);
            const std::size_t largest = table.type == BurstDataType::kAc4Hbr4 ? tightest
                                        : table.type == BurstDataType::kAc4Hbr16
                                            ? static_cast<std::size_t>(tightest) * 8
                                            : tightest / 8;
            std::vector<std::vector<std::byte>> frames;
            std::vector<std::uint32_t> expected;
            for (int i = 0; i < 12; ++i) {
                const int counter = 3 + i;
                // frame_size escapes to 24 bits at 0xFFFF bytes and more.
                const std::size_t header = largest - 4 < 0xFFFF ? 4 : 7;
                const std::size_t raw =
                    i == 11 ? largest - header : 40 + static_cast<std::size_t>(i) * 13;
                frames.push_back(sync_frame({.fs_index = row.fs_index,
                                             .frame_rate_index = row.frame_rate_index,
                                             .sequence_counter = counter,
                                             .raw_bytes = raw}));
                expected.push_back(periods[static_cast<std::size_t>(phase_of(counter))]);
            }
            REQUIRE(frames.back().size() == largest);
            pack_and_read_back(table.type, frames, expected, row.code);
        }
    }
}

TEST_CASE("Ac4BurstPacker: a sequence follows the stream's own counter wherever packing starts",
          "[iec61937][ac4]") {
    // 29,97 fps: Table 6's five periods, by sequence_counter mod 5.
    const auto frame = [](int counter) {
        return sync_frame({.frame_rate_index = 3, .sequence_counter = counter, .raw_bytes = 100});
    };
    const std::array<std::uint32_t, 5> table6{1602, 1601, 1602, 1601, 1602};

    // From frame 0 and from frame 3 of the same stream: the same period for
    // each frame.
    for (const int first : {0, 1, 3, 4}) {
        CAPTURE(first);
        iec::Ac4BurstPacker packer;
        for (int counter = first; counter < first + 10; ++counter) {
            REQUIRE(packer.push(frame(counter)).has_value());
            CHECK(packer.last()->sequence_index == counter % 5);
            CHECK(packer.last()->period == table6[static_cast<std::size_t>(counter % 5)]);
        }
    }

    // The counter wraps from 1 020 to 1, and 1 020 is a multiple of five, so
    // the cycle runs on through the wrap.
    {
        iec::Ac4BurstPacker packer;
        std::vector<int> phases;
        for (const int counter : {1018, 1019, 1020, 1, 2}) {
            REQUIRE(packer.push(frame(counter)).has_value());
            phases.push_back(packer.last()->sequence_index);
        }
        CHECK(phases == std::vector<int>{3, 4, 0, 1, 2});
    }

    // A splice: the first frame after it carries counter 0 (TS 103 190-1
    // 4.3.3.2.2), and continues the phase before it (TS 103 190-2 5.11).
    {
        iec::Ac4BurstPacker packer;
        std::vector<int> phases;
        for (const int counter : {6, 7, 0, 501, 502}) {
            REQUIRE(packer.push(frame(counter)).has_value());
            phases.push_back(packer.last()->sequence_index);
        }
        CHECK(phases == std::vector<int>{1, 2, 3, 1, 2});
    }

    // A first frame with counter 0 is data-burst 0.
    iec::Ac4BurstPacker packer;
    REQUIRE(packer.push(frame(0)).has_value());
    CHECK(packer.last()->sequence_index == 0);
    CHECK(packer.last()->period == 1602);
}

TEST_CASE("Ac4BurstPacker: a frame over its burst's maximum is refused and the stream goes on",
          "[iec61937][ac4]") {
    // 29,97 fps: data-burst 0 (1 602 frames) takes 51 136 bits, data-burst 1
    // (1 601) only 51 104. A frame of 6 392 bytes is 51 136 bits.
    const auto frame = [](int counter, std::size_t bytes) {
        return sync_frame(
            {.frame_rate_index = 3, .sequence_counter = counter, .raw_bytes = bytes - 4});
    };
    iec::Ac4BurstPacker packer;
    CHECK(packer.push(frame(5, 6392)).has_value());
    CHECK(packer.push(frame(6, 6392)).error() == iec::WrapError::kFrameTooLarge);
    CHECK(packer.push(frame(6, 6388)).has_value());
    CHECK(packer.push(frame(7, 6393)).error() == iec::WrapError::kFrameTooLarge);

    // HBR4 carries in bytes what AC-4 cannot in bits.
    iec::Ac4BurstPacker hbr4(BurstDataType::kAc4Hbr4);
    const auto large = hbr4.push(frame(5, 20000));
    REQUIRE(large.has_value());
    CHECK(large->size() == 6408U * 4);
    CHECK(le16(*large, 6) == 20000);
}

TEST_CASE("Ac4BurstPacker: what it refuses", "[iec61937][ac4]") {
    iec::Ac4BurstPacker packer;
    CHECK(packer.push(std::vector<std::byte>{std::byte{0x0B}, std::byte{0x77}}).error() ==
          iec::WrapError::kNotAFrame);
    std::vector<std::byte> truncated = sync_frame({});
    truncated.pop_back();
    CHECK(packer.push(truncated).error() == iec::WrapError::kNotAFrame);

    // A base sampling frequency the stream did not start with.
    REQUIRE(packer.push(sync_frame({.fs_index = 1, .frame_rate_index = 13})).has_value());
    CHECK(packer.push(sync_frame({.fs_index = 0, .frame_rate_index = 13})).error() ==
          iec::WrapError::kRateChanged);
    // A change of frame rate at 48 kHz is only a change of period.
    const auto faster = packer.push(sync_frame({.fs_index = 1, .frame_rate_index = 10}));
    REQUIRE(faster.has_value());
    CHECK(faster->size() == 480U * 4);

    // AC-4 LD at 25 fps, and every type at 44,1 kHz outside index 13.
    iec::Ac4BurstPacker ld(BurstDataType::kAc4Ld);
    CHECK(ld.push(sync_frame({.frame_rate_index = 2})).error() == iec::WrapError::kUnsupportedRate);
    iec::Ac4BurstPacker at_44k;
    CHECK(at_44k.push(sync_frame({.fs_index = 0, .frame_rate_index = 3})).error() ==
          iec::WrapError::kUnsupportedRate);
    // frame_rate_index 14 and 15 are reserved (TS 103 190-1 Table 83).
    iec::Ac4BurstPacker reserved;
    CHECK(reserved.push(sync_frame({.fs_index = 1, .frame_rate_index = 14})).error() ==
          iec::WrapError::kUnsupportedRate);

    // A packer of a type that is not AC-4.
    iec::Ac4BurstPacker wrong(BurstDataType::kEac3);
    CHECK(wrong.push(sync_frame({})).error() == iec::WrapError::kNotAFrame);
}

TEST_CASE("Ac4BurstPacker: HBR16 counts in 8-byte units and pads the last one with zeros",
          "[iec61937][ac4]") {
    // A frame of 70 009 bytes, past frame_size's 16 bits: 8 752 units, the
    // last holding one byte of the frame and seven zeros.
    std::vector<std::vector<std::byte>> frames;
    frames.push_back(sync_frame(
        {.frame_rate_index = 13, .sequence_counter = 1, .raw_bytes = 70000, .crc = true}));
    frames.push_back(sync_frame({.frame_rate_index = 13, .sequence_counter = 2, .raw_bytes = 13}));
    REQUIRE(frames[0].size() == 70009);

    iec::Ac4BurstPacker packer(BurstDataType::kAc4Hbr16);
    const auto burst = packer.push(frames[0]);
    REQUIRE(burst.has_value());
    CHECK(burst->size() == 32768U * 4);
    CHECK(le16(*burst, 6) == 8752);
    CHECK(packer.last()->payload_bytes == 8752 * 8);
    CHECK(packer.last()->link_rate_hz == 768000);

    const std::vector<std::uint32_t> periods{32768, 32768};
    pack_and_read_back(BurstDataType::kAc4Hbr16, frames, periods, 13);
}

TEST_CASE("wrap_ac4_stream: a stream's bursts back to back read back whole", "[iec61937][ac4]") {
    std::vector<std::vector<std::byte>> frames;
    for (int counter = 1; counter <= 7; ++counter) {
        frames.push_back(sync_frame({.frame_rate_index = 8,
                                     .sequence_counter = counter,
                                     .raw_bytes = 200 + static_cast<std::size_t>(counter)}));
    }
    const auto carrier = iec::wrap_ac4_stream(views_of(frames));
    REQUIRE(carrier.has_value());
    // 59,94 fps from counter 1: data-bursts 1, 2, 3, 4, 0, 1, 2 of Table 6.
    CHECK(carrier->size() == (801U + 800 + 801 + 801 + 801 + 801 + 800) * 4);

    const auto back = iec::unwrap_stream(*carrier);
    REQUIRE(back.has_value());
    std::vector<std::byte> joined;
    for (const auto& frame : frames) {
        joined.insert(joined.end(), frame.begin(), frame.end());
    }
    CHECK(*back == joined);
    CHECK(iec::wrap_ac4_stream(views_of(frames), BurstDataType::kAc4Ld).error() ==
          iec::WrapError::kUnsupportedRate);
}

// --- Choosing a burst type -------------------------------------------------------

TEST_CASE("ac4_burst_type_for: the smallest type whose every burst holds the frame",
          "[iec61937][ac4]") {
    // 29,97 fps: AC-4's tightest burst takes 51 104 bits, 6 388 bytes.
    CHECK(iec::ac4_burst_type_for(6388, 1, 3) == BurstDataType::kAc4);
    CHECK(iec::ac4_burst_type_for(6389, 1, 3) == BurstDataType::kAc4Hbr4);
    CHECK(iec::ac4_burst_type_for(25600, 1, 3) == BurstDataType::kAc4Hbr4);
    CHECK(iec::ac4_burst_type_for(25601, 1, 3) == BurstDataType::kAc4Hbr16);
    CHECK(iec::ac4_burst_type_for(12806 * 8, 1, 3) == BurstDataType::kAc4Hbr16);
    CHECK_FALSE(iec::ac4_burst_type_for(12806 * 8 + 1, 1, 3).has_value());
    CHECK_FALSE(iec::ac4_burst_type_for(100, 0, 3).has_value());
}

// --- Reading ----------------------------------------------------------------------

TEST_CASE("BurstReader: a Pd in bytes as IEC 61937-2 Table 2 has it reads the same frame",
          "[iec61937][ac4]") {
    const std::vector<std::byte> frame = sync_frame({.frame_rate_index = 13, .raw_bytes = 301});
    iec::Ac4BurstPacker packer;
    auto burst = packer.push(frame);
    REQUIRE(burst.has_value());
    // Rewrite Pd from bits to bytes.
    (*burst)[6] = static_cast<std::byte>(frame.size() & 0xFF);
    (*burst)[7] = static_cast<std::byte>(frame.size() >> 8);
    const auto back = iec::unwrap_stream(*burst);
    REQUIRE(back.has_value());
    CHECK(*back == frame);

    // A Pd that agrees with the frame in neither unit is not believed.
    (*burst)[6] = std::byte{0x10};
    (*burst)[7] = std::byte{0x00};
    iec::BurstReader reader;
    std::vector<std::byte> out;
    REQUIRE(reader.push(*burst, out).has_value());
    CHECK(out.empty());
    CHECK(reader.bursts() == 0);
    CHECK(reader.false_syncs() >= 1);
}

TEST_CASE("BurstReader: AC-4 in a big-endian carrier fed a byte at a time", "[iec61937][ac4]") {
    std::vector<std::vector<std::byte>> frames;
    for (int counter = 1; counter <= 3; ++counter) {
        frames.push_back(sync_frame({.frame_rate_index = 11,
                                     .sequence_counter = counter,
                                     .raw_bytes = 51 + static_cast<std::size_t>(counter)}));
    }
    const auto little = iec::wrap_ac4_stream(views_of(frames), BurstDataType::kAc4Ld);
    REQUIRE(little.has_value());
    std::vector<std::byte> big(little->size());
    for (std::size_t i = 0; i + 1 < little->size(); i += 2) {
        big[i] = (*little)[i + 1];
        big[i + 1] = (*little)[i];
    }
    iec::BurstReader reader;
    std::vector<std::byte> out;
    for (const std::byte b : big) {
        REQUIRE(reader.push(std::span(&b, 1), out).has_value());
    }
    REQUIRE(reader.finish().has_value());
    CHECK(reader.word_order() == iec::WordOrder::kBigEndian);
    CHECK(reader.data_type() == BurstDataType::kAc4Ld);
    CHECK(reader.bursts() == 3);
    std::vector<std::byte> joined;
    for (const auto& frame : frames) {
        joined.insert(joined.end(), frame.begin(), frame.end());
    }
    CHECK(out == joined);
}

TEST_CASE("BurstReader: an AC-4 Pd past its type's longest period is refused", "[iec61937][ac4]") {
    // Pc = AC-4, Pd = 65 535 bits: 8 192 bytes, past the 8 184 a 2 048-frame
    // burst holds after its preamble.
    const std::vector<std::byte> hostile{std::byte{0x72}, std::byte{0xF8}, std::byte{0x1F},
                                         std::byte{0x4E}, std::byte{0x18}, std::byte{0x0D},
                                         std::byte{0xFF}, std::byte{0xFF}, std::byte{0x40},
                                         std::byte{0xAC}, std::byte{0x00}, std::byte{0x10}};
    CHECK(iec::unwrap_stream(hostile).error() == iec::UnwrapError::kPayloadTooLarge);
}

TEST_CASE("BurstReader: data type 1 with a subdata type is not AC-3", "[iec61937][ac4]") {
    // IEC 61937-2 Table 2: data type 1 is AC-3 only with subdata type 0.
    std::vector<std::byte> carrier{
        std::byte{0x72}, std::byte{0xF8}, std::byte{0x1F}, std::byte{0x4E}, std::byte{0x21},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x77}, std::byte{0x0B}};
    carrier.resize(512, std::byte{0});
    iec::BurstReader reader;
    std::vector<std::byte> out;
    REQUIRE(reader.push(carrier, out).has_value());
    CHECK(reader.bursts() == 0);
    CHECK(reader.skipped_bursts() == 1);
}

TEST_CASE("PassthroughDetector: recognises an AC-4 carrier arriving as capture floats",
          "[iec61937][ac4][capture]") {
    std::vector<std::vector<std::byte>> frames;
    for (int counter = 1; counter <= 3; ++counter) {
        frames.push_back(
            sync_frame({.frame_rate_index = 13, .sequence_counter = counter, .raw_bytes = 500}));
    }
    const auto carrier = iec::wrap_ac4_stream(views_of(frames));
    REQUIRE(carrier.has_value());
    std::vector<float> floats;
    for (std::size_t i = 0; i + 1 < carrier->size(); i += 2) {
        const auto word = static_cast<std::uint16_t>(u8(*carrier, i) | (u8(*carrier, i + 1) << 8));
        floats.push_back(static_cast<float>(static_cast<std::int16_t>(word)) / 32768.0F);
    }
    iec::PassthroughDetector detector;
    detector.push(floats, 2);
    CHECK(detector.detected() == BurstDataType::kAc4);
}

// --- The Dolby Encoding Engine's streams --------------------------------------------

TEST_CASE("Ac4BurstPacker: DEE's streams at four frame rates pack and read back unchanged",
          "[iec61937][ac4]") {
    struct Stream {
        std::string_view leg;
        int frame_rate_index;
    };
    // 23,44 fps (index 13, 2 048 samples), 24, 25 and 29,97 fps.
    for (const Stream& stream :
         {Stream{"ac4-stereo-64", 13}, Stream{"ac4-ims-film-96-24", 1},
          Stream{"ac4-ims-music-128-25", 2}, Stream{"ac4-ims-music-64-2997", 3}}) {
        CAPTURE(stream.leg);
        const std::vector<std::vector<std::byte>> frames = frames_of_file(
            std::filesystem::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / stream.leg / "dee.ac4");
        const std::optional<iec::Ac4BurstTiming> timing =
            iec::ac4_burst_timing(BurstDataType::kAc4, 1, stream.frame_rate_index);
        REQUIRE(timing.has_value());
        std::vector<std::uint32_t> expected;
        for (const auto& frame : frames) {
            const auto read = iec::read_ac4_sync_frame(frame);
            REQUIRE(read.has_value());
            REQUIRE(read->frame_rate_index == stream.frame_rate_index);
            // DEE counts from 0 or 1 and runs on without a splice, so the
            // phase is the counter's.
            expected.push_back(
                timing->periods[static_cast<std::size_t>(phase_of(read->sequence_counter))]);
        }
        pack_and_read_back(BurstDataType::kAc4, frames, expected, timing->code);

        // And the carrier as a whole reads back to the file, which the
        // inspector then walks as it walks the file.
        const auto carrier = iec::wrap_ac4_stream(views_of(frames));
        REQUIRE(carrier.has_value());
        const auto back = iec::unwrap_stream(*carrier);
        REQUIRE(back.has_value());
        const ac4::ScanResult scanned = ac4::scan(*back);
        CHECK(scanned.frames.size() == frames.size());
        CHECK_FALSE(scanned.stopped_at.has_value());
    }
}

TEST_CASE("Ac4BurstPacker: every DEE leg of a directory packs and reads back unchanged",
          "[iec61937][ac4]") {
    // The committed legs; AC4DEC_STREAM_DIR points it at another directory of DEE legs, such as
    // the whole gold set, as it does the decoder's syntax test. Each stream goes in the smallest
    // burst type its largest frame fits, at the rate its first frame states.
    const char* const elsewhere = std::getenv("AC4DEC_STREAM_DIR");
    const std::filesystem::path root =
        elsewhere != nullptr ? std::filesystem::path{elsewhere}
                             : std::filesystem::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR};
    std::vector<std::filesystem::path> legs;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root)) {
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "dee.ac4")) {
            legs.push_back(entry.path());
        }
    }
    std::sort(legs.begin(), legs.end());
    REQUIRE_FALSE(legs.empty());
    for (const std::filesystem::path& leg : legs) {
        CAPTURE(leg.filename().string());
        const std::vector<std::vector<std::byte>> frames = frames_of_file(leg / "dee.ac4");
        const std::optional<iec::Ac4SyncFrame> first = iec::read_ac4_sync_frame(frames.front());
        REQUIRE(first.has_value());
        std::size_t largest = 0;
        for (const std::vector<std::byte>& frame : frames) {
            largest = std::max(largest, frame.size());
        }
        const std::optional<BurstDataType> type =
            iec::ac4_burst_type_for(largest, first->fs_index, first->frame_rate_index);
        REQUIRE(type.has_value());
        const std::optional<iec::Ac4BurstTiming> timing =
            iec::ac4_burst_timing(*type, first->fs_index, first->frame_rate_index);
        REQUIRE(timing.has_value());
        // TS 103 190-2 5.11's phase: sequence_counter's, or after a splice (a counter of 0) the
        // last frame's plus one, and 0 for a first frame with a counter of 0.
        std::vector<std::uint32_t> expected;
        std::optional<int> phase;
        for (const std::vector<std::byte>& frame : frames) {
            const std::optional<iec::Ac4SyncFrame> read = iec::read_ac4_sync_frame(frame);
            REQUIRE(read.has_value());
            REQUIRE(read->frame_rate_index == first->frame_rate_index);
            phase = read->sequence_counter != 0 ? phase_of(read->sequence_counter)
                    : phase                     ? (*phase + 1) % 5
                                                : 0;
            expected.push_back(timing->periods[static_cast<std::size_t>(*phase)]);
        }
        pack_and_read_back(*type, frames, expected, timing->code);
    }
}
