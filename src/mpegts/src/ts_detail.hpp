#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// The transport-stream constants and the PSI section CRC shared between
// mpegts.cpp (the writer: mux() and Writer) and reader.cpp (the reader:
// demux() and Reader). Internal to src/mpegts/src/ on purpose - plumbing
// between translation units of the same library, not public API; see
// src/mp4/src/isobmff_detail.hpp and src/matroska/src/ebml_detail.hpp for
// the same pattern in the sibling container modules.
//
// One list, not two. A reader that transcribed its own copy of the packet
// size, the PID numbering or the CRC polynomial could drift from the
// writer's, and the round-trip test would be the only thing that noticed -
// and only for the cases it happens to cover.

namespace mpegts::detail {

inline constexpr std::size_t kTsPacketSize = 188;
inline constexpr std::uint8_t kSyncByte = 0x47;
inline constexpr std::uint16_t kPatPid = 0x0000;
// ISO/IEC 13818-1 Table 2-3: the null packet, which carries no data and is
// what a constant-rate multiplexer pads with. Skipped on sight.
inline constexpr std::uint16_t kNullPid = 0x1FFF;

// ISO/IEC 13818-1 Table 2-34's stream_type values this module deals in.
//
// 0x06 is "PES packets containing private data" - not an audio stream_type
// at all. DVB uses it for AC-3/E-AC-3 together with one of the descriptors
// below, which is what mux() writes (see mpegts.hpp's header comment on why
// this project's WRITER implements the DVB profile rather than ATSC's).
inline constexpr std::uint8_t kStreamTypePrivateData = 0x06;
// ATSC A/52 Annex A registers its own two stream_types instead. The writer
// never emits them; a reader that could not read them would be unable to
// open an ATSC broadcast capture or most North American disc rips, so both
// profiles are recognised on the way in even though only one is written.
inline constexpr std::uint8_t kStreamTypeAtscAc3 = 0x81;
inline constexpr std::uint8_t kStreamTypeAtscEac3 = 0x87;

// ISO/IEC 13818-1 Table 2-19: private_stream_1, the PES stream_id a
// stream_type 0x06 payload is carried under.
inline constexpr std::uint8_t kPesStreamIdPrivateStream1 = 0xBD;

// ETSI EN 300 468 Annex D.2 Table D.1 and Annex D.4 Table D.3.
inline constexpr std::uint8_t kTagAc3Descriptor = 0x6A;
inline constexpr std::uint8_t kTagEnhancedAc3Descriptor = 0x7A;
// ISO/IEC 13818-1 §2.6.8's registration_descriptor. Read-side only: a
// stream carrying format_identifier 'AC-3' or 'EAC3' here is naming its
// codec the third way the wild actually does it, alongside the ATSC
// stream_types and the DVB descriptors above.
inline constexpr std::uint8_t kTagRegistrationDescriptor = 0x05;

// ISO/IEC 13818-1 Annex B: the CRC_32 every PSI section ends with is the
// non-reflected CRC-32/MPEG-2 variant - generator polynomial 0x04C11DB7,
// initial value all-ones, no output XOR, most-significant-bit-first. This is
// NOT the reflected CRC-32 (poly 0xEDB88320, e.g. zlib/PNG's) that "CRC-32"
// alone often means; transcribing that instead is a real, easy-to-make
// mistake, so it is self-checked below against the standard "123456789" test
// vector rather than merely trusted.
//
// The writer uses it to stamp each section; the reader uses it to decide
// whether a section is worth believing at all, which matters far more on the
// way in - a broadcast capture with a bit error would otherwise be parsed as
// a confidently wrong programme map.
[[nodiscard]] constexpr std::uint32_t crc32_mpeg2(std::span<const std::byte> data) {
    std::uint32_t crc = 0xFFFF'FFFFU;
    for (const auto b : data) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b)) << 24U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000'0000U) != 0 ? (crc << 1U) ^ 0x04C1'1DB7U : (crc << 1U);
        }
    }
    return crc;
}

inline constexpr std::array<std::byte, 9> kCrc32CheckVector = {
    std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'},
    std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
static_assert(crc32_mpeg2(kCrc32CheckVector) == 0x0376'E6E7U,
              "CRC-32/MPEG-2 self-check against the standard \"123456789\" test vector failed - "
              "a wrong polynomial or bit order here corrupts every PAT/PMT section silently, "
              "since nothing but a real demuxer's CRC check would ever notice.");

}  // namespace mpegts::detail
