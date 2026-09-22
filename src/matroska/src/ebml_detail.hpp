#pragma once

#include <cstdint>

// The EBML element ids and reserved values shared between matroska.cpp (the
// writer: mux() and Writer) and reader.cpp (the reader: demux() and Reader).
// Internal to src/matroska/src/ on purpose - this is plumbing between
// translation units of the same library, not public API; see
// src/mp4/src/isobmff_detail.hpp for the identical pattern in the sibling
// container module.
//
// One list, not two: a reader that transcribed its own copy of these numbers
// could drift from the writer's and the round-trip test would be the only
// thing that noticed - and only for the elements that test happens to cover.
//
// Ids are stored exactly as they appear on the wire: an id already carries
// its own length marker in the leading bits, so unlike a size it is never
// re-encoded.

namespace matroska::detail {

inline constexpr std::uint32_t kEbmlHeader = 0x1A45DFA3;
inline constexpr std::uint32_t kEbmlVersion = 0x4286;
inline constexpr std::uint32_t kEbmlReadVersion = 0x42F7;
inline constexpr std::uint32_t kEbmlMaxIdLength = 0x42F2;
inline constexpr std::uint32_t kEbmlMaxSizeLength = 0x42F3;
inline constexpr std::uint32_t kDocType = 0x4282;
inline constexpr std::uint32_t kDocTypeVersion = 0x4287;
inline constexpr std::uint32_t kDocTypeReadVersion = 0x4285;

inline constexpr std::uint32_t kSegment = 0x18538067;
inline constexpr std::uint32_t kInfo = 0x1549A966;
inline constexpr std::uint32_t kTimestampScale = 0x2AD7B1;
inline constexpr std::uint32_t kDuration = 0x4489;
inline constexpr std::uint32_t kMuxingApp = 0x4D80;
inline constexpr std::uint32_t kWritingApp = 0x5741;

inline constexpr std::uint32_t kTracks = 0x1654AE6B;
inline constexpr std::uint32_t kTrackEntry = 0xAE;
inline constexpr std::uint32_t kTrackNumber = 0xD7;
inline constexpr std::uint32_t kTrackUid = 0x73C5;
inline constexpr std::uint32_t kTrackType = 0x83;
inline constexpr std::uint32_t kFlagLacing = 0x9C;
inline constexpr std::uint32_t kCodecId = 0x86;
inline constexpr std::uint32_t kLanguage = 0x22B59C;
inline constexpr std::uint32_t kAudio = 0xE1;
inline constexpr std::uint32_t kSamplingFrequency = 0xB5;
inline constexpr std::uint32_t kChannels = 0x9F;

inline constexpr std::uint32_t kCluster = 0x1F43B675;
inline constexpr std::uint32_t kClusterTimestamp = 0xE7;
inline constexpr std::uint32_t kSimpleBlock = 0xA3;

// Read-side only: a Block carries the same payload layout as a SimpleBlock
// (track-number vint, signed 16-bit relative timestamp, flags, frame data),
// wrapped in a BlockGroup that adds the duration/reference elements a
// SimpleBlock encodes in its own flags byte instead. The writer never emits
// this shape - one SimpleBlock per frame is strictly smaller - but plenty of
// real muxers do, so the reader has to know both.
inline constexpr std::uint32_t kBlockGroup = 0xA0;
inline constexpr std::uint32_t kBlock = 0xA1;

// Read-side only: the remaining Segment-level children, needed not to parse
// them but to recognise them - they are what ends a Cluster written with the
// "unknown size" pattern below, since such a Cluster has no length of its own
// to run out. Also the elements a reader skips wholesale.
inline constexpr std::uint32_t kSeekHead = 0x114D9B74;
inline constexpr std::uint32_t kCues = 0x1C53BB6B;
inline constexpr std::uint32_t kAttachments = 0x1941A469;
inline constexpr std::uint32_t kChapters = 0x1043A770;
inline constexpr std::uint32_t kTags = 0x1254C367;

inline constexpr std::uint8_t kTrackTypeAudio = 0x02;
inline constexpr std::uint64_t kTimestampScaleNs = 1'000'000;  // one tick == 1 ms

// EBML's reserved "unknown size": a size vint whose value bits are ALL set.
// The writer emits it at the widest width it ever writes (8 bytes) for a
// streamed Segment; the reader has to recognise it at EVERY width, since the
// reserved pattern is per-width and a different muxer may pick a narrower
// one. kUnknownSize is the decoded value the reader compares against after
// stripping the marker - see reader.cpp's read_size - and simultaneously the
// exact 8-byte-width pattern the writer reserves so put_vint's automatic
// width-stepping can never emit it by accident for a real size.
inline constexpr std::uint64_t kUnknownSize = (std::uint64_t{1} << 56) - 1;

// The decoded all-ones value at a given vint width, i.e. that width's own
// "unknown size" pattern. width is 1-8.
[[nodiscard]] inline constexpr std::uint64_t unknown_size_at(int width) {
    return (std::uint64_t{1} << (7 * width)) - 1;
}

}  // namespace matroska::detail
