#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include "ac3/sendspin/dialect.hpp"

// The two timestamped audio chunks a Sendspin player receives.
//
// player@v1 (roles/player/v1.md), ID 4, integers big-endian:
//
//     [4][int64 timestamp µs][uint32 send_ahead µs][one encoded frame]
//
// aiosendspin 9.1.1 has no send_ahead, so its header is nine bytes
// (planning/hearth-sendspin-extension.md, C31):
//
//     [4][int64 timestamp µs][one encoded frame]
//
// _ac3forge_player@v1 (planning/hearth-sendspin-extension.md, Burst chunks),
// ID 192: the same header, then one IEC 61937 burst without its sync words,
// byte-swapping or zero stuffing:
//
//     [192][int64 timestamp µs][uint32 send_ahead µs][uint16 Pc][uint16 Pd][payload]
//
// Pc is the burst-info word as ac3::iec61937 writes it: the data type in bits 0
// to 6 (1 AC-3, 21 E-AC-3, and 24 AC-4 with its subdata type in bits 5 and 6), the
// error flag in bit 7, the data-type-dependent bits 8 to 12 and the data stream
// number in 13 to 15. Pd is the payload length in bits for AC-3, AC-4 and AC-4 LD,
// in bytes for E-AC-3 and AC-4 HBR4, and in 8-byte units for AC-4 HBR16, whose
// payload is zero-padded to a whole unit (IEC 61937-14 5.3.5).
//
// The parsers check what one chunk can say about itself. Whether a chunk's data
// type matches the running stream, and whether its timestamp is late, are the
// session's to judge.

namespace ac3::sendspin {

inline constexpr std::size_t kAudioChunkHeaderBytes = 13;
inline constexpr std::size_t kBurstChunkHeaderBytes = 17;

// player@v1's header in `dialect`.
[[nodiscard]] constexpr std::size_t audio_chunk_header_bytes(Dialect dialect) {
    return dialect == Dialect::kSpecification ? kAudioChunkHeaderBytes : 9;
}

struct AudioChunk {
    std::int64_t timestamp_us = 0;
    // 0 from a chunk in aiosendspin 9.1.1's form, which carries none: 0 is a
    // saturated value, which a player never samples.
    std::uint32_t send_ahead_us = 0;
    // The encoded frame (player@v1) or the burst payload (_ac3forge_player@v1).
    std::span<const std::uint8_t> data;
};

// The send_ahead a server writes for `microseconds`: saturated to the uint32
// range, as player@v1 requires, never wrapped.
[[nodiscard]] constexpr std::uint32_t saturate_send_ahead(std::int64_t microseconds) {
    if (microseconds <= 0) {
        return 0;
    }
    if (microseconds >= std::int64_t{0xFFFFFFFF}) {
        return 0xFFFFFFFFU;
    }
    return static_cast<std::uint32_t>(microseconds);
}

// Whether a received send_ahead is saturated and so says nothing usable about
// delay. A player neither schedules by send_ahead nor samples a saturated one.
[[nodiscard]] constexpr bool is_saturated_send_ahead(std::uint32_t send_ahead_us) {
    return send_ahead_us == 0 || send_ahead_us == 0xFFFFFFFFU;
}

enum class ChunkError : std::uint8_t {
    kNone,
    kTooShort,         // shorter than its header, or a burst with no payload
    kWrongId,          // not the message ID the parser reads
    kReservedBits,     // Pc bits 5 and 6 set on an AC-3 or E-AC-3 burst
    kUnknownDataType,  // a Pc data type other than AC-3, E-AC-3 or AC-4
    kLengthMismatch,   // Pd, or an AC-4 frame's own size, disagrees with the payload
    kPayloadTooLarge,  // more payload than the data type's burst can carry
    kNoSyncword,       // a payload that does not start with 0x0B77, or 0xAC40/0xAC41 for AC-4
};

[[nodiscard]] std::string_view describe(ChunkError error);

// player@v1's chunk, ID 4, in `dialect`'s form. The frame may be empty; the codec
// judges it.
[[nodiscard]] std::expected<AudioChunk, ChunkError> parse_player_chunk(
    std::span<const std::uint8_t> message, Dialect dialect);

// Writes ID 4's header in `dialect`'s form into `out`, and returns false when `out`
// is shorter than audio_chunk_header_bytes(dialect). aiosendspin 9.1.1's form
// leaves `send_ahead_us` out.
bool write_player_chunk_header(std::span<std::uint8_t> out, std::int64_t timestamp_us,
                               std::uint32_t send_ahead_us, Dialect dialect);

// Pc bits 0 to 6 (IEC 61937-2 Table 2), as ac3::iec61937::BurstDataType names them.
enum class BurstDataType : std::uint8_t {
    kAc3 = 1,
    kEac3 = 21,
    kAc4 = 24,       // IEC 61937-14: data type 24, subdata type 0
    kAc4Hbr4 = 56,   // 24, subdata type 1
    kAc4Hbr16 = 88,  // 24, subdata type 2
    kAc4Ld = 120,    // 24, subdata type 3
};

[[nodiscard]] constexpr bool is_ac4(BurstDataType type) {
    return (static_cast<unsigned>(type) & 0x1FU) == 24U;
}

// Payload bytes a burst of each type can carry: its longest repetition period less
// the eight-byte preamble. 6,144 or 24,576 carrier bytes for AC-3 and E-AC-3; for
// AC-4, whose period follows its frame rate, 8,192, 32,768, 131,072 and 1,920 for
// the four types (IEC 61937-14 Tables 5, 11, 17 and 23).
[[nodiscard]] constexpr std::size_t max_burst_payload(BurstDataType type) {
    switch (type) {
        case BurstDataType::kAc3:
            return 6144 - 8;
        case BurstDataType::kEac3:
            return 24576 - 8;
        case BurstDataType::kAc4:
            return 8192 - 8;
        case BurstDataType::kAc4Hbr4:
            return 32768 - 8;
        case BurstDataType::kAc4Hbr16:
            return 131072 - 8;
        case BurstDataType::kAc4Ld:
            return 1920 - 8;
    }
    return 0;
}

struct BurstChunk {
    AudioChunk chunk;
    std::uint16_t pc = 0;
    std::uint16_t pd = 0;
    [[nodiscard]] BurstDataType data_type() const { return static_cast<BurstDataType>(pc & 0x7FU); }
};

// _ac3forge_player@v1's chunk, ID 192.
[[nodiscard]] std::expected<BurstChunk, ChunkError> parse_burst_chunk(
    std::span<const std::uint8_t> message);

// The Pd a payload of `payload_bytes` bytes carries for `type`. An AC-4 HBR16
// payload is a whole number of 8-byte units already, its last one zero-padded.
[[nodiscard]] constexpr std::uint16_t burst_length_code(BurstDataType type,
                                                        std::size_t payload_bytes) {
    switch (type) {
        case BurstDataType::kEac3:
        case BurstDataType::kAc4Hbr4:
            return static_cast<std::uint16_t>(payload_bytes);
        case BurstDataType::kAc4Hbr16:
            return static_cast<std::uint16_t>((payload_bytes + 7) / 8);
        case BurstDataType::kAc3:
        case BurstDataType::kAc4:
        case BurstDataType::kAc4Ld:
            break;
    }
    return static_cast<std::uint16_t>(payload_bytes * 8);
}

// Writes ID 192's 17-byte header into `out`; returns false when `out` is shorter.
bool write_burst_chunk_header(std::span<std::uint8_t> out, std::int64_t timestamp_us,
                              std::uint32_t send_ahead_us, std::uint16_t pc, std::uint16_t pd);

}  // namespace ac3::sendspin
