#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/encoder/silent_frame.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "ac3/sendspin/chunks.hpp"
#include "ac3/sendspin/frames.hpp"

// The audio chunks, including planning/hearth-sendspin-extension.md's test
// vectors for _ac3forge_player@v1: a burst chunk from wrap_frame for an AC-3
// frame and one from Eac3BurstPacker for E-AC-3 syncframes of fewer than six
// blocks, checked field by field against the burst the library packs for a
// receiver.

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
