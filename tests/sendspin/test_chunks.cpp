#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/eac3_frame.hpp"
#include "ac3/encoder/silent_frame.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/frames.hpp"

// The audio chunks, including planning/hearth-sendspin-extension.md's test
// vectors for _ac3forge_player@v1: a burst chunk from wrap_frame for
// tests/golden's AC-3 5.1 fixture and one from Eac3BurstPacker for the
// encoder's E-AC-3 syncframes of two blocks, checked field by field against the
// burst the library packs for a receiver, beside the same checks on a silent
// frame and on hand-made one-block syncframes.

namespace {

using ac3::sendspin::BurstDataType;
using ac3::sendspin::ChunkError;
using ac3::sendspin::Dialect;

std::uint16_t le16(std::span<const std::byte> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[at]) |
                                      (std::to_integer<unsigned>(bytes[at + 1]) << 8U));
}

std::vector<std::uint8_t> burst_chunk(std::int64_t timestamp, std::uint32_t send_ahead,
                                      std::uint16_t pc, std::uint16_t pd,
                                      std::span<const std::byte> payload) {
    std::vector<std::uint8_t> message(ac3::sendspin::kBurstChunkHeaderBytes);
    REQUIRE(ac3::sendspin::write_burst_chunk_header(message, timestamp, send_ahead, pc, pd));
    for (const std::byte b : payload) {
        message.push_back(std::to_integer<std::uint8_t>(b));
    }
    return message;
}

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out;
    out.reserve(bytes.size());
    for (const char c : bytes) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

// The message's bytes from `first` for `count`, as numbers.
std::vector<unsigned> bytes_of(const std::vector<std::uint8_t>& message, std::size_t first, std::size_t count) {
    return {message.begin() + static_cast<std::ptrdiff_t>(first),
            message.begin() + static_cast<std::ptrdiff_t>(first + count)};
}

// The checks every burst chunk vector gets against the burst `burst` the library packed for a
// receiver: the header's fields at their offsets, big-endian, and a payload that is the burst's
// own with its 16-bit words swapped back and its stuffing left out.
void check_against_burst(const std::vector<std::uint8_t>& message, std::span<const std::byte> burst,
                         std::span<const std::byte> payload) {
    const std::uint16_t pc = le16(burst, 4);
    const std::uint16_t pd = le16(burst, 6);
    REQUIRE(message.size() == ac3::sendspin::kBurstChunkHeaderBytes + payload.size());
    CHECK(message[0] == 192);
    CHECK(bytes_of(message, 13, 2) == std::vector<unsigned>{static_cast<unsigned>(pc >> 8U), pc & 0xFFU});
    CHECK(bytes_of(message, 15, 2) == std::vector<unsigned>{static_cast<unsigned>(pd >> 8U), pd & 0xFFU});
    REQUIRE(payload.size() % 2 == 0);
    std::size_t different = 0;
    for (std::size_t i = 0; i < payload.size(); ++i) {
        const auto byte = std::to_integer<std::uint8_t>(payload[i]);
        different += message[ac3::sendspin::kBurstChunkHeaderBytes + i] == byte ? 0U : 1U;
        different += std::to_integer<std::uint8_t>(burst[8 + (i ^ 1U)]) == byte ? 0U : 1U;
    }
    CHECK(different == 0);
    std::size_t stuffing = 0;
    for (std::size_t i = 8 + payload.size(); i < burst.size(); ++i) {
        stuffing += burst[i] == std::byte{0} ? 0U : 1U;
    }
    CHECK(stuffing == 0);
}

std::vector<std::uint8_t> ac3_payload_chunk(std::uint16_t pc, std::uint16_t pd,
                                            std::size_t payload_bytes) {
    std::vector<std::byte> payload(payload_bytes, std::byte{0x55});
    if (payload_bytes >= 2) {
        payload[0] = std::byte{0x0B};
        payload[1] = std::byte{0x77};
    }
    return burst_chunk(0, 0, pc, pd, payload);
}

}  // namespace

TEST_CASE("chunks: player@v1 chunk header, big-endian", "[sendspin][chunks]") {
    std::vector<std::uint8_t> message(ac3::sendspin::kAudioChunkHeaderBytes);
    REQUIRE(ac3::sendspin::write_player_chunk_header(message, 0x0102030405060708, 0x0A0B0C0D,
                                                     Dialect::kSpecification));
    CHECK(message == std::vector<std::uint8_t>{4, 1, 2, 3, 4, 5, 6, 7, 8, 0x0A, 0x0B, 0x0C, 0x0D});
    message.push_back(0xEE);

    const auto chunk = ac3::sendspin::parse_player_chunk(message, Dialect::kSpecification);
    REQUIRE(chunk.has_value());
    CHECK(chunk->timestamp_us == 0x0102030405060708);
    CHECK(chunk->send_ahead_us == 0x0A0B0C0D);
    REQUIRE(chunk->data.size() == 1);
    CHECK(chunk->data[0] == 0xEE);
}

TEST_CASE("chunks: aiosendspin 9.1.1's player@v1 header has no send_ahead", "[sendspin][chunks]") {
    // aiosendspin 9.1.1 packs the header as struct ">Bq" (models/__init__.py).
    CHECK(ac3::sendspin::audio_chunk_header_bytes(Dialect::kAiosendspin911) == 9);
    std::vector<std::uint8_t> message(9);
    REQUIRE(ac3::sendspin::write_player_chunk_header(message, 0x0102030405060708, 0x0A0B0C0D,
                                                     Dialect::kAiosendspin911));
    CHECK(message == std::vector<std::uint8_t>{4, 1, 2, 3, 4, 5, 6, 7, 8});
    message.push_back(0xEE);

    const auto chunk = ac3::sendspin::parse_player_chunk(message, Dialect::kAiosendspin911);
    REQUIRE(chunk.has_value());
    CHECK(chunk->timestamp_us == 0x0102030405060708);
    CHECK(chunk->send_ahead_us == 0);
    CHECK(ac3::sendspin::is_saturated_send_ahead(chunk->send_ahead_us));
    REQUIRE(chunk->data.size() == 1);
    CHECK(chunk->data[0] == 0xEE);

    // Read in the specification's form, the frame's first four bytes become send_ahead:
    // the reason a connection's dialect is settled before its first chunk.
    std::vector<std::uint8_t> longer(message);
    longer.insert(longer.end(), {0xF1, 0xF2, 0xF3, 0xF4});
    const auto misread = ac3::sendspin::parse_player_chunk(longer, Dialect::kSpecification);
    REQUIRE(misread.has_value());
    CHECK(misread->send_ahead_us == 0xEEF1F2F3U);
    REQUIRE(misread->data.size() == 1);
    CHECK(misread->data[0] == 0xF4);
}

TEST_CASE("chunks: timestamps are signed", "[sendspin][chunks]") {
    for (const Dialect dialect : {Dialect::kSpecification, Dialect::kAiosendspin911}) {
        std::vector<std::uint8_t> message(ac3::sendspin::audio_chunk_header_bytes(dialect));
        REQUIRE(ac3::sendspin::write_player_chunk_header(message, -1, 0, dialect));
        const auto chunk = ac3::sendspin::parse_player_chunk(message, dialect);
        REQUIRE(chunk.has_value());
        CHECK(chunk->timestamp_us == -1);
        CHECK(chunk->data.empty());
    }
}

TEST_CASE("chunks: player@v1 chunk errors", "[sendspin][chunks]") {
    CHECK(ac3::sendspin::parse_player_chunk(std::vector<std::uint8_t>(12, 4), Dialect::kSpecification)
              .error() == ChunkError::kTooShort);
    CHECK(ac3::sendspin::parse_player_chunk(std::vector<std::uint8_t>(8, 4), Dialect::kAiosendspin911)
              .error() == ChunkError::kTooShort);
    CHECK(ac3::sendspin::parse_player_chunk(std::vector<std::uint8_t>(9, 4), Dialect::kAiosendspin911)
              .has_value());
    std::vector<std::uint8_t> other(13, 0);
    other[0] = 5;
    CHECK(ac3::sendspin::parse_player_chunk(other, Dialect::kSpecification).error() ==
          ChunkError::kWrongId);
    CHECK(ac3::sendspin::parse_player_chunk(other, Dialect::kAiosendspin911).error() ==
          ChunkError::kWrongId);
    std::array<std::uint8_t, 12> small{};
    CHECK_FALSE(ac3::sendspin::write_player_chunk_header(small, 0, 0, Dialect::kSpecification));
    CHECK(ac3::sendspin::write_player_chunk_header(small, 0, 0, Dialect::kAiosendspin911));
    std::array<std::uint8_t, 8> smaller{};
    CHECK_FALSE(ac3::sendspin::write_player_chunk_header(smaller, 0, 0, Dialect::kAiosendspin911));
}

TEST_CASE("chunks: send_ahead saturates at both ends", "[sendspin][chunks]") {
    using ac3::sendspin::is_saturated_send_ahead;
    using ac3::sendspin::saturate_send_ahead;
    CHECK(saturate_send_ahead(-5) == 0);
    CHECK(saturate_send_ahead(0) == 0);
    CHECK(saturate_send_ahead(1) == 1);
    CHECK(saturate_send_ahead(4294967294) == 4294967294U);
    CHECK(saturate_send_ahead(4294967295) == 4294967295U);
    CHECK(saturate_send_ahead(5000000000) == 4294967295U);
    CHECK(is_saturated_send_ahead(0));
    CHECK(is_saturated_send_ahead(4294967295U));
    CHECK_FALSE(is_saturated_send_ahead(150000));
}

TEST_CASE("chunks: an AC-3 burst chunk carries wrap_frame's Pc, Pd and frame",
          "[sendspin][chunks]") {
    const auto frame = ac3::build_silent_stereo_frame(
        {.sample_rate = ac3::SampleRate::k48000, .bitrate_kbps = 192, .dialnorm = 31, .pad441 = false});
    REQUIRE(frame.has_value());
    const auto burst = ac3::iec61937::wrap_frame(*frame);
    REQUIRE(burst.has_value());
    const std::uint16_t pc = le16(*burst, 4);
    const std::uint16_t pd = le16(*burst, 6);

    const std::vector<std::uint8_t> message = burst_chunk(1000000, 180000, pc, pd, *frame);
    CHECK(message.size() == ac3::sendspin::kBurstChunkHeaderBytes + frame->size());
    CHECK(message[0] == ac3::sendspin::message_id::kAc3forgeBurst);

    const auto chunk = ac3::sendspin::parse_burst_chunk(message);
    REQUIRE(chunk.has_value());
    CHECK(chunk->chunk.timestamp_us == 1000000);
    CHECK(chunk->chunk.send_ahead_us == 180000);
    CHECK(chunk->pc == pc);
    CHECK(chunk->pd == pd);
    CHECK(chunk->data_type() == BurstDataType::kAc3);
    CHECK(chunk->pd == ac3::sendspin::burst_length_code(BurstDataType::kAc3, frame->size()));
    REQUIRE(chunk->chunk.data.size() == frame->size());
    for (std::size_t i = 0; i < frame->size(); ++i) {
        CHECK(chunk->chunk.data[i] == std::to_integer<std::uint8_t>((*frame)[i]));
    }
}

TEST_CASE("chunks: an E-AC-3 burst chunk carries Eac3BurstPacker's six blocks",
          "[sendspin][chunks]") {
    // Six numblkscod-0 syncframes, one block each, built as the IEC 61937 tests
    // build them: the packer reads only the header.
    const auto one_block = [](std::size_t payload_words, std::byte fill) {
        std::vector<std::byte> frame(6 + payload_words * 2, fill);
        frame[0] = std::byte{0x0B};
        frame[1] = std::byte{0x77};
        frame[2] = std::byte{0x00};
        frame[3] = std::byte{0x00};
        frame[4] = static_cast<std::byte>(2 << 1);
        frame[5] = static_cast<std::byte>((16 << 3) | 0x7);
        return frame;
    };
    ac3::iec61937::Eac3BurstPacker packer;
    std::vector<std::byte> payload;
    std::vector<std::byte> burst;
    for (int i = 0; i < 6; ++i) {
        const auto unit = one_block(8 + static_cast<std::size_t>(i), static_cast<std::byte>(0xA0 + i));
        payload.insert(payload.end(), unit.begin(), unit.end());
        const auto pushed = packer.push(unit);
        REQUIRE(pushed.has_value());
        if (*pushed) {
            burst = **pushed;
        }
    }
    REQUIRE(burst.size() == ac3::iec61937::kEac3BurstBytes);
    const std::uint16_t pc = le16(burst, 4);
    const std::uint16_t pd = le16(burst, 6);
    CHECK(pc == 0x15);
    CHECK(pd == payload.size());

    const auto chunk = ac3::sendspin::parse_burst_chunk(burst_chunk(-32000, 0, pc, pd, payload));
    REQUIRE(chunk.has_value());
    CHECK(chunk->chunk.timestamp_us == -32000);
    CHECK(chunk->data_type() == BurstDataType::kEac3);
    CHECK(chunk->pd == ac3::sendspin::burst_length_code(BurstDataType::kEac3, payload.size()));
    CHECK(chunk->chunk.data.size() == payload.size());
}

TEST_CASE("chunks: a burst chunk for the first syncframe of tests/golden's AC-3 5.1 fixture",
          "[sendspin][chunks]") {
    const std::vector<std::byte> stream = read_file(AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR "/ac3-51-448/ffmpeg.ac3");
    const auto frames = ac3::split_frames(stream);
    REQUIRE(frames.has_value());
    REQUIRE_FALSE(frames->empty());
    const std::span<const std::byte> frame = frames->front();
    REQUIRE(frame.size() == 1792);
    const auto burst = ac3::iec61937::wrap_frame(frame);
    REQUIRE(burst.has_value());
    REQUIRE(burst->size() == ac3::iec61937::kBurstBytes);
    const std::uint16_t pc = le16(*burst, 4);
    const std::uint16_t pd = le16(*burst, 6);
    // Data type 1 and the frame's bsmod in bits 8 to 10; the frame's length in bits.
    CHECK((pc & 0x1FU) == 1U);
    CHECK(((pc >> 8U) & 0x7U) == (std::to_integer<unsigned>(frame[5]) & 0x7U));
    CHECK(pd == 1792 * 8);

    const std::vector<std::uint8_t> message = burst_chunk(0x0102030405060708, 0x0A0B0C0D, pc, pd, frame);
    CHECK(bytes_of(message, 1, 8) == std::vector<unsigned>{1, 2, 3, 4, 5, 6, 7, 8});
    CHECK(bytes_of(message, 9, 4) == std::vector<unsigned>{0x0A, 0x0B, 0x0C, 0x0D});
    check_against_burst(message, *burst, frame);
    const auto chunk = ac3::sendspin::parse_burst_chunk(message);
    REQUIRE(chunk.has_value());
    CHECK(chunk->data_type() == BurstDataType::kAc3);
}

TEST_CASE("chunks: a burst chunk for the encoder's E-AC-3 syncframes of two blocks", "[sendspin][chunks]") {
    ac3::eac3::FrameConfig config{.bitrate_kbps = 192, .numblkscod = 1};
    const auto frame = ac3::eac3::build_silent_frame(config);
    REQUIRE(frame.has_value());
    ac3::iec61937::Eac3BurstPacker packer;
    std::vector<std::byte> payload;
    std::optional<std::vector<std::byte>> burst;
    int pushed = 0;
    while (!burst && pushed < 6) {
        payload.insert(payload.end(), frame->begin(), frame->end());
        auto packed = packer.push(*frame);
        REQUIRE(packed.has_value());
        burst = std::move(*packed);
        ++pushed;
    }
    // Two blocks each, so three syncframes make the burst's six.
    CHECK(pushed == 3);
    REQUIRE(burst.has_value());
    REQUIRE(burst->size() == ac3::iec61937::kEac3BurstBytes);
    const std::uint16_t pc = le16(*burst, 4);
    const std::uint16_t pd = le16(*burst, 6);
    CHECK(pc == 21);
    CHECK(pd == payload.size());

    const std::vector<std::uint8_t> message = burst_chunk(-1, 0xFFFFFFFFU, pc, pd, payload);
    CHECK(bytes_of(message, 1, 12) == std::vector<unsigned>(12, 0xFF));
    check_against_burst(message, *burst, payload);
    const auto chunk = ac3::sendspin::parse_burst_chunk(message);
    REQUIRE(chunk.has_value());
    CHECK(chunk->data_type() == BurstDataType::kEac3);
    CHECK(chunk->chunk.timestamp_us == -1);
}

TEST_CASE("chunks: burst chunk errors", "[sendspin][chunks]") {
    using ac3::sendspin::parse_burst_chunk;
    const std::uint16_t ac3_pc = 0x0001;
    const std::uint16_t eac3_pc = 0x0015;

    CHECK(parse_burst_chunk(std::vector<std::uint8_t>{}).error() == ChunkError::kTooShort);
    CHECK(parse_burst_chunk(std::vector<std::uint8_t>(40, 4)).error() == ChunkError::kWrongId);
    CHECK(parse_burst_chunk(ac3_payload_chunk(ac3_pc, 0, 0)).error() == ChunkError::kTooShort);

    CHECK(parse_burst_chunk(ac3_payload_chunk(ac3_pc, 64, 8)).has_value());
    // bsmod, the error flag and a stream number are allowed; bits 5 and 6 are not.
    CHECK(parse_burst_chunk(ac3_payload_chunk(0xE781, 64, 8)).has_value());
    CHECK(parse_burst_chunk(ac3_payload_chunk(0x0021, 64, 8)).error() == ChunkError::kReservedBits);
    CHECK(parse_burst_chunk(ac3_payload_chunk(0x0041, 64, 8)).error() == ChunkError::kReservedBits);
    CHECK(parse_burst_chunk(ac3_payload_chunk(0x0007, 64, 8)).error() ==
          ChunkError::kUnknownDataType);

    // AC-3's Pd counts bits, E-AC-3's bytes.
    CHECK(parse_burst_chunk(ac3_payload_chunk(ac3_pc, 8, 8)).error() == ChunkError::kLengthMismatch);
    CHECK(parse_burst_chunk(ac3_payload_chunk(eac3_pc, 64, 8)).error() ==
          ChunkError::kLengthMismatch);
    CHECK(parse_burst_chunk(ac3_payload_chunk(eac3_pc, 8, 8)).has_value());

    CHECK(parse_burst_chunk(ac3_payload_chunk(ac3_pc, 16, 2)).has_value());
    CHECK(parse_burst_chunk(ac3_payload_chunk(eac3_pc, 1, 1)).error() == ChunkError::kNoSyncword);
    std::vector<std::uint8_t> no_sync = ac3_payload_chunk(ac3_pc, 64, 8);
    no_sync[ac3::sendspin::kBurstChunkHeaderBytes] = 0x77;
    CHECK(parse_burst_chunk(no_sync).error() == ChunkError::kNoSyncword);

    const std::size_t ac3_max = ac3::sendspin::max_burst_payload(BurstDataType::kAc3);
    CHECK(ac3_max == 6136);
    CHECK(parse_burst_chunk(ac3_payload_chunk(ac3_pc, static_cast<std::uint16_t>(ac3_max * 8),
                                              ac3_max))
              .has_value());
    CHECK(parse_burst_chunk(ac3_payload_chunk(ac3_pc, 0, ac3_max + 1)).error() ==
          ChunkError::kPayloadTooLarge);
    const std::size_t eac3_max = ac3::sendspin::max_burst_payload(BurstDataType::kEac3);
    CHECK(eac3_max == 24568);
    CHECK(parse_burst_chunk(ac3_payload_chunk(eac3_pc, static_cast<std::uint16_t>(eac3_max),
                                              eac3_max))
              .has_value());
    // The largest chunk fits one frame, so a burst is never fragmented.
    CHECK(ac3::sendspin::kBurstChunkHeaderBytes + eac3_max <= ac3::sendspin::kMaxFramePlaintext);

    std::array<std::uint8_t, 16> small{};
    CHECK_FALSE(ac3::sendspin::write_burst_chunk_header(small, 0, 0, 1, 0));
}

namespace {

// An AC-4 sync frame (IEC 61937-14 Annex A) whose raw frame starts with a table of contents that
// says bitstream_version 2, sequence_counter 1, no wait_frames, 48 kHz, frame_rate_index 13 and an
// I-frame (0x80 0x17 0x60), followed by `raw` - 3 bytes of filler, which nothing here reads.
std::vector<std::byte> ac4_sync_frame(std::size_t raw, bool crc = false) {
    std::vector<std::byte> frame{std::byte{0xAC},
                                 crc ? std::byte{0x41} : std::byte{0x40},
                                 static_cast<std::byte>(raw >> 8U),
                                 static_cast<std::byte>(raw & 0xFFU),
                                 std::byte{0x80},
                                 std::byte{0x17},
                                 std::byte{0x60}};
    for (std::size_t i = 3; i < raw; ++i) {
        frame.push_back(static_cast<std::byte>((i * 29U + 7U) & 0xFFU));
    }
    if (crc) {
        frame.push_back(std::byte{0x12});
        frame.push_back(std::byte{0x34});
    }
    return frame;
}

}  // namespace

TEST_CASE("chunks: an AC-4 burst chunk carries Ac4BurstPacker's Pc, Pd and sync frame",
          "[sendspin][chunks][ac4]") {
    // Even and odd lengths, with and without the CRC word.
    using Case = std::pair<std::size_t, bool>;
    for (const auto& [raw, crc] : {Case{600, false}, Case{601, false}, Case{321, true}}) {
        CAPTURE(raw, crc);
        const std::vector<std::byte> frame = ac4_sync_frame(raw, crc);
        ac3::iec61937::Ac4BurstPacker packer;
        const auto burst = packer.push(frame);
        REQUIRE(burst.has_value());
        const std::uint16_t pc = le16(*burst, 4);
        const std::uint16_t pd = le16(*burst, 6);
        // Data type 24, subdata type 0, the code of 2 048 IEC 60958 frames (IEC 61937-14 Table
        // 7), and the frame's length in bits.
        CHECK(pc == 0x0D18);
        CHECK(pd == frame.size() * 8);

        const std::vector<std::uint8_t> message = burst_chunk(96000, 250000, pc, pd, frame);
        if (frame.size() % 2 == 0) {
            check_against_burst(message, *burst, frame);
        }
        const auto chunk = ac3::sendspin::parse_burst_chunk(message);
        REQUIRE(chunk.has_value());
        CHECK(chunk->data_type() == BurstDataType::kAc4);
        CHECK(ac3::sendspin::is_ac4(chunk->data_type()));
        CHECK(chunk->pd == ac3::sendspin::burst_length_code(BurstDataType::kAc4, frame.size()));
        REQUIRE(chunk->chunk.data.size() == frame.size());
        CHECK(chunk->chunk.data.back() == std::to_integer<std::uint8_t>(frame.back()));
    }
}

TEST_CASE("chunks: AC-4 HBR4 counts in bytes and HBR16 in 8-byte units with the last one padded",
          "[sendspin][chunks][ac4]") {
    using ac3::sendspin::parse_burst_chunk;
    const std::vector<std::byte> frame = ac4_sync_frame(9001);
    REQUIRE(frame.size() == 9005);

    ac3::iec61937::Ac4BurstPacker hbr4(ac3::iec61937::BurstDataType::kAc4Hbr4);
    REQUIRE(hbr4.push(frame).has_value());
    CHECK(hbr4.last()->pc == 0x0D38);
    CHECK(hbr4.last()->pd == 9005);
    const auto four = parse_burst_chunk(burst_chunk(0, 0, hbr4.last()->pc, hbr4.last()->pd, frame));
    REQUIRE(four.has_value());
    CHECK(four->data_type() == BurstDataType::kAc4Hbr4);

    // HBR16's payload is the frame and three zeros: 1 126 units of 8 bytes.
    ac3::iec61937::Ac4BurstPacker hbr16(ac3::iec61937::BurstDataType::kAc4Hbr16);
    REQUIRE(hbr16.push(frame).has_value());
    CHECK(hbr16.last()->pc == 0x0D58);
    CHECK(hbr16.last()->pd == 1126);
    CHECK(hbr16.last()->payload_bytes == 9008);
    std::vector<std::byte> padded = frame;
    padded.resize(9008, std::byte{0});
    const auto sixteen =
        parse_burst_chunk(burst_chunk(0, 0, hbr16.last()->pc, hbr16.last()->pd, padded));
    REQUIRE(sixteen.has_value());
    CHECK(sixteen->data_type() == BurstDataType::kAc4Hbr16);
    // Not a whole number of units, and a whole unit of padding too many.
    CHECK(parse_burst_chunk(burst_chunk(0, 0, 0x0D58, 1126, frame)).error() ==
          ChunkError::kLengthMismatch);
    std::vector<std::byte> over = padded;
    over.resize(9016, std::byte{0});
    CHECK(parse_burst_chunk(burst_chunk(0, 0, 0x0D58, 1127, over)).error() ==
          ChunkError::kLengthMismatch);
}

TEST_CASE("chunks: AC-4 burst chunk errors", "[sendspin][chunks][ac4]") {
    using ac3::sendspin::parse_burst_chunk;
    const std::vector<std::byte> frame = ac4_sync_frame(200);
    const auto bits = static_cast<std::uint16_t>(frame.size() * 8);
    CHECK(parse_burst_chunk(burst_chunk(0, 0, 0x0D18, bits, frame)).has_value());
    // Bits 5 and 6 are AC-4's subdata type, so AC-4 LD (3) is no reserved-bit error; its bursts
    // take at most 1 912 bytes.
    CHECK(parse_burst_chunk(burst_chunk(0, 0, 0x0A78, bits, frame)).has_value());
    const std::vector<std::byte> long_ld = ac4_sync_frame(1909);
    CHECK(parse_burst_chunk(
              burst_chunk(0, 0, 0x0A78, static_cast<std::uint16_t>(long_ld.size() * 8), long_ld))
              .error() == ChunkError::kPayloadTooLarge);
    CHECK(ac3::sendspin::max_burst_payload(BurstDataType::kAc4Ld) == 1912);
    CHECK(ac3::sendspin::max_burst_payload(BurstDataType::kAc4) == 8184);
    // Pd in bytes, which the role does not use for AC-4.
    CHECK(parse_burst_chunk(
              burst_chunk(0, 0, 0x0D18, static_cast<std::uint16_t>(frame.size()), frame))
              .error() == ChunkError::kLengthMismatch);
    // Another syncword, and a frame_size that disagrees with the payload.
    std::vector<std::byte> other = frame;
    other[1] = std::byte{0x42};
    CHECK(parse_burst_chunk(burst_chunk(0, 0, 0x0D18, bits, other)).error() ==
          ChunkError::kNoSyncword);
    std::vector<std::byte> shorter = frame;
    shorter[3] = static_cast<std::byte>(std::to_integer<unsigned>(shorter[3]) - 1U);
    CHECK(parse_burst_chunk(burst_chunk(0, 0, 0x0D18, bits, shorter)).error() ==
          ChunkError::kLengthMismatch);
    // An AC-3 syncframe in an AC-4 burst.
    std::vector<std::byte> ac3_sync = frame;
    ac3_sync[0] = std::byte{0x0B};
    ac3_sync[1] = std::byte{0x77};
    CHECK(parse_burst_chunk(burst_chunk(0, 0, 0x0D18, bits, ac3_sync)).error() ==
          ChunkError::kNoSyncword);
}
