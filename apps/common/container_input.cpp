#include "container_input.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

#include "ac3/io/elementary.hpp"
#include "matroska/reader.hpp"
#include "mp4/reader.hpp"
#include "mpegts/reader.hpp"

namespace ac3::apps {

namespace {

// EBML's own magic: the four bytes of the EBML header id every Matroska and
// WebM file opens with - the same kEbmlHeader constant
// src/matroska/src/ebml_detail.hpp holds, written out big-endian.
constexpr std::array<std::byte, 4> kEbmlMagic{std::byte{0x1A}, std::byte{0x45}, std::byte{0xDF},
                                              std::byte{0xA3}};

// ISOBMFF has no magic at offset 0 - it opens with a box, whose first four
// bytes are a LENGTH. The type is what identifies it, four bytes in, and
// 'ftyp' is what a well-formed file leads with (ISO/IEC 14496-12 4.3 says it
// "should be placed as early as possible"). 'styp' is a bare CMAF media
// segment, and a plain 'moov'/'mdat'/'moof' opener occurs in files written by
// tools that skipped ftyp - all of them are what a reader is handed in
// practice.
constexpr std::array<std::string_view, 5> kIsobmffLeadingTypes{"ftyp", "styp", "moov", "moof",
                                                               "mdat"};

[[nodiscard]] bool has_isobmff_box_at_start(std::span<const std::byte> head) {
    if (head.size() < 8) {
        return false;
    }
    const std::string_view type{reinterpret_cast<const char*>(head.data()) + 4, 4};
    return std::ranges::find(kIsobmffLeadingTypes, type) != kIsobmffLeadingTypes.end();
}

// A transport stream has no header at all - it is a bare repeating grid of
// 188-byte packets, each starting with 0x47, and a capture may begin
// anywhere in it. So the test is the grid itself: a sync byte that recurs at
// one of the three strides in the wild (188, M2TS's 192, or 204 with parity)
// several times over. A lone 0x47 proves nothing; five in a row exactly a
// stride apart is not a coincidence.
//
// Checked LAST, after the two formats that do have magic: an MP4 or Matroska
// file can easily contain a 0x47 pattern by chance somewhere in its audio,
// and the grid test is the loosest of the three.
constexpr std::array<std::size_t, 3> kTsStrides{188, 192, 204};
constexpr int kTsSyncRuns = 5;

[[nodiscard]] bool has_ts_packet_grid(std::span<const std::byte> head) {
    for (std::size_t at = 0; at < head.size(); ++at) {
        if (std::to_integer<std::uint8_t>(head[at]) != 0x47) {
            continue;
        }
        for (const auto stride : kTsStrides) {
            int seen = 1;
            for (int i = 1; i < kTsSyncRuns; ++i) {
                const std::size_t next = at + (stride * static_cast<std::size_t>(i));
                if (next >= head.size() || std::to_integer<std::uint8_t>(head[next]) != 0x47) {
                    break;
                }
                ++seen;
            }
            if (seen >= kTsSyncRuns) {
                return true;
            }
        }
    }
    return false;
}

// How much of the head of a file sniff_container needs to look at. Every
// magic/grid check above resolves within a few hundred bytes at most; this is
// generous headroom rather than a measured minimum, and small enough that
// sniffing a multi-gigabyte rip costs nothing.
constexpr std::size_t kContainerSniffBytes = 64 * 1024;

// One demuxed track/programme's frames, concatenated into a single owned
// buffer - ac3::split_frames/split_access_units need one contiguous stream,
// but a container's frames are views scattered across the source file (or,
// for mpegts, across its own reassembly buffer), never contiguous with each
// other.
[[nodiscard]] std::vector<std::byte> concat_frames(
    std::span<const std::span<const std::byte>> frames) {
    std::size_t total = 0;
    for (const auto& frame : frames) {
        total += frame.size();
    }
    std::vector<std::byte> out;
    out.reserve(total);
    for (const auto& frame : frames) {
        out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}

}  // namespace

ContainerKind sniff_container(std::span<const std::byte> head) {
    const auto sniffed = head.first(std::min(head.size(), kContainerSniffBytes));
    if (sniffed.size() >= kEbmlMagic.size() &&
        std::equal(kEbmlMagic.begin(), kEbmlMagic.end(), sniffed.begin())) {
        return ContainerKind::kMatroska;
    }
    if (has_isobmff_box_at_start(sniffed)) {
        return ContainerKind::kMp4;
    }
    // A raw AC-3/E-AC-3 elementary stream is checked for BEFORE the packet
    // grid below, not after: this is what actually reads a well-formed
    // syncframe rather than one coincidental byte, and it settles a real
    // collision the grid alone cannot - AC-3 at 48 kbps/48 kHz codes
    // exactly 192-byte frames, one of the grid's own three strides, and a
    // steady or otherwise low-entropy signal encodes near-identical frames,
    // so "0x47 recurs every 192 bytes" is something a perfectly ordinary
    // elementary stream can produce on its own, not just an MPEG-TS capture.
    // ac3::io::read_frame_header validates the sync word and the whole of
    // bsi, which no accidental byte pattern satisfies by chance the way a
    // single recurring byte can.
    if (ac3::io::read_frame_header(sniffed).has_value()) {
        return ContainerKind::kUnknown;
    }
    if (has_ts_packet_grid(sniffed)) {
        return ContainerKind::kMpegTs;
    }
    return ContainerKind::kUnknown;
}

ElementaryStreamResult elementary_stream_from_bytes(std::span<const std::byte> file) {
    switch (sniff_container(file)) {
        case ContainerKind::kUnknown:
            return {.bytes = std::vector<std::byte>(file.begin(), file.end()), .error = {}};
        case ContainerKind::kMatroska: {
            const auto demuxed = matroska::demux(file);
            if (!demuxed) {
                return {.bytes = {},
                       .error = std::string{"Matroska/WebM file this build cannot demux ("} +
                                std::string{matroska::describe(demuxed.error())} + ")"};
            }
            return {.bytes = concat_frames(demuxed->frames), .error = {}};
        }
        case ContainerKind::kMp4: {
            const auto demuxed = mp4::demux(file);
            if (!demuxed) {
                return {.bytes = {},
                       .error = std::string{"MP4 file this build cannot demux ("} +
                                std::string{mp4::describe(demuxed.error())} + ")"};
            }
            return {.bytes = concat_frames(demuxed->samples), .error = {}};
        }
        case ContainerKind::kMpegTs: {
            const auto demuxed = mpegts::demux(file);
            if (!demuxed) {
                return {.bytes = {},
                       .error = std::string{"Transport Stream this build cannot demux ("} +
                                std::string{mpegts::describe(demuxed.error())} + ")"};
            }
            return {.bytes = concat_frames(demuxed->payloads), .error = {}};
        }
    }
    return {.bytes = {}, .error = "unrecognised container"};
}

}  // namespace ac3::apps
