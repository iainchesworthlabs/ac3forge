#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Turning a container file into the elementary stream ac3::forge actually
// decodes (roadmap IO2) - shared by ac3cli (decode/qc/levels/play/monitor)
// and ac3gui (the QC/Inspect pickers), compiled straight into both the same
// way RecordingSink/Fmp4FolderWriter beside this file are: apps/common has no
// library target of its own (see recording_sink.hpp's own comment), and this
// is smaller than either.
//
// Lives here rather than in ac3::forge itself: matroska::matroska/mp4::mp4/
// mpegts::mpegts each say plainly they have no dependency on ac3::forge (see
// e.g. matroska/reader.hpp's own header comment) - the containers are
// deliberately independent of the codec, and giving the codec library a
// dependency back on them would invert that for every third party that links
// ac3::forge to decode bare elementary streams and wants nothing else.

namespace ac3::apps {

// Which container a file actually is, decided by its first bytes rather than
// its name - a rip is as likely to be called "title00.mkv" when it is not
// one as it is to have no extension at all. `head` needs only the first few
// KiB; a whole file works too but is wasted effort. kUnknown covers a bare
// elementary stream (or a WAV, for the callers that also accept one), which
// is most of what either caller actually sees.
enum class ContainerKind : std::uint8_t { kUnknown, kMatroska, kMp4, kMpegTs };

[[nodiscard]] ContainerKind sniff_container(std::span<const std::byte> head);

// `file`'s elementary stream: `file` itself, unchanged, if it does not sniff
// as one of the three containers this build reads, or the first AC-3/E-AC-3
// track demuxed out of one - the same three readers `ac3cli demux` already
// streams through, run here in their batch/zero-copy form since every caller
// already holds the whole file in memory.
struct ElementaryStreamResult {
    std::vector<std::byte> bytes;
    // Empty on success (including the "not a container at all" case, where
    // `bytes` is just `file` copied back). Set when `file` sniffed as a
    // container this build recognises but could not demux - a caller reports
    // this itself rather than getting a generic empty-result failure, since
    // "malformed MP4" and "no AC-3 track" want different messages than "file
    // not found" does.
    std::string error;
};

[[nodiscard]] ElementaryStreamResult elementary_stream_from_bytes(std::span<const std::byte> file);

}  // namespace ac3::apps
