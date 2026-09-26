#include "ac3/sendspin/chunks.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "ac3/sendspin/frames.hpp"

namespace ac3::sendspin {

namespace {

[[nodiscard]] std::uint64_t read_be(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 0;
    for (const std::uint8_t byte : bytes) {
        value = (value << 8U) | byte;
    }
    return value;
}

void write_be(std::span<std::uint8_t> out, std::uint64_t value) {
    for (std::size_t i = out.size(); i > 0; --i) {
        out[i - 1] = static_cast<std::uint8_t>(value & 0xFFU);
        value >>= 8U;
    }
}

[[nodiscard]] AudioChunk read_header(std::span<const std::uint8_t> message) {
    return AudioChunk{
        .timestamp_us = static_cast<std::int64_t>(read_be(message.subspan(1, 8))),
        .send_ahead_us = static_cast<std::uint32_t>(read_be(message.subspan(9, 4))),
        .data = {},
    };
}

void write_header(std::span<std::uint8_t> out, std::uint8_t id, std::int64_t timestamp_us,
                  std::uint32_t send_ahead_us) {
    out[0] = id;
    write_be(out.subspan(1, 8), static_cast<std::uint64_t>(timestamp_us));
    write_be(out.subspan(9, 4), send_ahead_us);
}

}  // namespace

std::string_view describe(ChunkError error) {
    switch (error) {
        case ChunkError::kNone:
            return "no error";
        case ChunkError::kTooShort:
            return "a chunk shorter than its header";
        case ChunkError::kWrongId:
            return "a chunk with a different message ID";
        case ChunkError::kReservedBits:
            return "an AC-3 or E-AC-3 burst with Pc bits 5 or 6 set";
        case ChunkError::kUnknownDataType:
            return "a burst whose data type is none of AC-3, E-AC-3 and AC-4";
        case ChunkError::kLengthMismatch:
            return "a burst whose Pd, or AC-4 frame size, disagrees with its payload";
        case ChunkError::kPayloadTooLarge:
            return "a burst payload larger than its data type allows";
        case ChunkError::kNoSyncword:
            return "a burst payload that does not start with a syncframe";
    }
    return "unknown error";
}

std::expected<AudioChunk, ChunkError> parse_player_chunk(std::span<const std::uint8_t> message,
                                                        Dialect dialect) {
    const std::size_t header = audio_chunk_header_bytes(dialect);
    if (message.size() < header) {
        return std::unexpected(ChunkError::kTooShort);
    }
    if (message.front() != message_id::kPlayerAudio) {
        return std::unexpected(ChunkError::kWrongId);
    }
    if (dialect == Dialect::kSpecification) {
        AudioChunk chunk = read_header(message);
        chunk.data = message.subspan(header);
        return chunk;
    }
    return AudioChunk{
        .timestamp_us = static_cast<std::int64_t>(read_be(message.subspan(1, 8))),
        .send_ahead_us = 0,
        .data = message.subspan(header),
    };
}

bool write_player_chunk_header(std::span<std::uint8_t> out, std::int64_t timestamp_us,
                               std::uint32_t send_ahead_us, Dialect dialect) {
    if (out.size() < audio_chunk_header_bytes(dialect)) {
        return false;
    }
    if (dialect == Dialect::kSpecification) {
        write_header(out, message_id::kPlayerAudio, timestamp_us, send_ahead_us);
    } else {
        out[0] = message_id::kPlayerAudio;
        write_be(out.subspan(1, 8), static_cast<std::uint64_t>(timestamp_us));
    }
    return true;
}

std::expected<BurstChunk, ChunkError> parse_burst_chunk(std::span<const std::uint8_t> message) {
    if (message.empty() || message.front() != message_id::kAc3forgeBurst) {
        return std::unexpected(message.empty() ? ChunkError::kTooShort : ChunkError::kWrongId);
    }
    if (message.size() <= kBurstChunkHeaderBytes) {
        return std::unexpected(ChunkError::kTooShort);
    }
    BurstChunk burst;
    burst.chunk = read_header(message);
    burst.pc = static_cast<std::uint16_t>(read_be(message.subspan(13, 2)));
    burst.pd = static_cast<std::uint16_t>(read_be(message.subspan(15, 2)));
    burst.chunk.data = message.subspan(kBurstChunkHeaderBytes);

    // Bits 5 and 6 are AC-4's subdata type (IEC 61937-14 Table 2) and zero for
    // AC-3 and E-AC-3.
    const std::uint16_t conventional = burst.pc & 0x1FU;
    const bool ac4 = conventional == static_cast<std::uint16_t>(BurstDataType::kAc4);
    if (!ac4 && (burst.pc & 0x60U) != 0) {
        return std::unexpected(ChunkError::kReservedBits);
    }
    if (!ac4 && conventional != static_cast<std::uint16_t>(BurstDataType::kAc3) &&
        conventional != static_cast<std::uint16_t>(BurstDataType::kEac3)) {
        return std::unexpected(ChunkError::kUnknownDataType);
    }
    const BurstDataType type = burst.data_type();
    const std::span<const std::uint8_t> payload = burst.chunk.data;
    if (payload.size() > max_burst_payload(type)) {
        return std::unexpected(ChunkError::kPayloadTooLarge);
    }
    if (burst.pd != burst_length_code(type, payload.size()) ||
        (type == BurstDataType::kAc4Hbr16 && payload.size() % 8 != 0)) {
        return std::unexpected(ChunkError::kLengthMismatch);
    }
    if (!ac4) {
        if (payload.size() < 2 || payload[0] != 0x0B || payload[1] != 0x77) {
            return std::unexpected(ChunkError::kNoSyncword);
        }
        return burst;
    }
    // One AC-4 sync frame (IEC 61937-14 Annex A): 0xAC40, or 0xAC41 with a CRC
    // word after the frame, then a 16-bit frame_size, or 0xFFFF and a 24-bit
    // one. It fills the payload, which HBR16 pads to a whole 8-byte unit.
    if (payload.size() < 4 || payload[0] != 0xAC || (payload[1] != 0x40 && payload[1] != 0x41)) {
        return std::unexpected(ChunkError::kNoSyncword);
    }
    std::size_t head = 4;
    std::size_t size = (static_cast<std::size_t>(payload[2]) << 8U) | payload[3];
    if (size == 0xFFFF) {
        if (payload.size() < 7) {
            return std::unexpected(ChunkError::kLengthMismatch);
        }
        head = 7;
        size = (static_cast<std::size_t>(payload[4]) << 16U) |
               (static_cast<std::size_t>(payload[5]) << 8U) | payload[6];
    }
    const std::size_t frame = head + size + (payload[1] == 0x41 ? 2U : 0U);
    const bool fits = type == BurstDataType::kAc4Hbr16
                          ? frame <= payload.size() && payload.size() - frame < 8
                          : frame == payload.size();
    if (!fits) {
        return std::unexpected(ChunkError::kLengthMismatch);
    }
    return burst;
}

bool write_burst_chunk_header(std::span<std::uint8_t> out, std::int64_t timestamp_us,
                              std::uint32_t send_ahead_us, std::uint16_t pc, std::uint16_t pd) {
    if (out.size() < kBurstChunkHeaderBytes) {
        return false;
    }
    write_header(out, message_id::kAc3forgeBurst, timestamp_us, send_ahead_us);
    write_be(out.subspan(13, 2), pc);
    write_be(out.subspan(15, 2), pd);
    return true;
}

}  // namespace ac3::sendspin
