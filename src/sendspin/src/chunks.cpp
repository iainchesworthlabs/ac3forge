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
            return "a burst with Pc bits 5 or 6 set";
        case ChunkError::kUnknownDataType:
            return "a burst whose data type is neither AC-3 nor E-AC-3";
        case ChunkError::kLengthMismatch:
            return "a burst whose Pd disagrees with its payload";
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

    if ((burst.pc & 0x60U) != 0) {
        return std::unexpected(ChunkError::kReservedBits);
    }
    const std::uint16_t type_code = burst.pc & 0x1FU;
    if (type_code != static_cast<std::uint16_t>(BurstDataType::kAc3) &&
        type_code != static_cast<std::uint16_t>(BurstDataType::kEac3)) {
        return std::unexpected(ChunkError::kUnknownDataType);
    }
    const BurstDataType type = burst.data_type();
    const std::span<const std::uint8_t> payload = burst.chunk.data;
    if (payload.size() > max_burst_payload(type)) {
        return std::unexpected(ChunkError::kPayloadTooLarge);
    }
    if (burst.pd != burst_length_code(type, payload.size())) {
        return std::unexpected(ChunkError::kLengthMismatch);
    }
    if (payload.size() < 2 || payload[0] != 0x0B || payload[1] != 0x77) {
        return std::unexpected(ChunkError::kNoSyncword);
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
