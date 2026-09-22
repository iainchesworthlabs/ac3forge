#include "container_input.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "ac3/io/elementary.hpp"
#include "mp4/mp4.hpp"
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

// `value` counted in 1/`from` seconds, as a count in 1/`to` seconds, to the
// nearest. Whole seconds first and the remainder after, so no step
// overflows for any value a file can hold; a result past 64 bits saturates.
[[nodiscard]] std::uint64_t rescale(std::uint64_t value, std::uint32_t to, std::uint32_t from) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t whole = value / from;
    const std::uint64_t part = ((value % from) * to + (from / 2)) / from;
    if (whole > (kMax - part) / to) {
        return kMax;
    }
    return (whole * to) + part;
}

[[nodiscard]] ContainerFacts facts_of(const mp4::Demuxed& demuxed) {
    const mp4::ReadTrack& track = demuxed.track;
    ContainerFacts facts;
    facts.kind = ContainerKind::kMp4;
    facts.codec_id = track.codec_id;
    facts.track = track.track_id;
    facts.language = track.language;
    facts.samples = demuxed.samples.size();
    facts.sample_rate = track.sample_rate;
    facts.channels = track.channels;
    facts.timescale = track.timescale;
    facts.movie_timescale = track.movie_timescale;
    facts.edits = track.edits.size();
    const mp4::CodecConfig& config = track.codec_config;
    if (!config.payload.empty()) {
        CodecBox box;
        box.bytes = config.payload.size();
        if (config.ac4) {
            box.type = "dac4";
        } else {
            box.type = config.eac3 ? "dec3" : "dac3";
            box.fscod = config.fscod;
            box.bsid = config.bsid;
            box.bsmod = config.bsmod;
            box.acmod = config.acmod;
            box.lfeon = config.lfeon;
            box.bit_rate_code = config.bit_rate_code;
            box.data_rate_kbps = config.data_rate_kbps;
            box.independent_substreams = config.eac3 ? config.num_ind_sub + 1 : 0;
            box.num_dep_sub = config.num_dep_sub;
            box.chan_loc = config.chan_loc;
            box.asvc = config.asvc;
            box.complexity_index = config.oba_complexity_index;
        }
        facts.codec_box = std::move(box);
    }
    return facts;
}

[[nodiscard]] ContainerFacts facts_of(const matroska::Demuxed& demuxed) {
    ContainerFacts facts;
    facts.kind = ContainerKind::kMatroska;
    facts.codec_id = demuxed.track.codec_id;
    facts.track = demuxed.track.track_number;
    facts.language = demuxed.track.language;
    facts.samples = demuxed.frames.size();
    facts.sample_rate = demuxed.track.sample_rate;
    facts.channels = demuxed.track.channels;
    return facts;
}

[[nodiscard]] std::string_view signalling_token(mpegts::CodecSignalling signalling) {
    switch (signalling) {
        case mpegts::CodecSignalling::kAtscStreamType: return "atsc_stream_type";
        case mpegts::CodecSignalling::kDvbDescriptor: return "dvb_descriptor";
        case mpegts::CodecSignalling::kRegistrationDescriptor: return "registration_descriptor";
        case mpegts::CodecSignalling::kDvbExtensionDescriptor: return "dvb_extension_descriptor";
    }
    return "";
}

[[nodiscard]] ContainerFacts facts_of(const mpegts::Demuxed& demuxed) {
    const mpegts::ReadStream& stream = demuxed.stream;
    ContainerFacts facts;
    facts.kind = ContainerKind::kMpegTs;
    facts.track = stream.elementary_pid;
    facts.samples = demuxed.payloads.size();
    facts.program_number = stream.program_number;
    facts.pmt_pid = stream.pmt_pid;
    facts.stream_type = stream.stream_type;
    facts.signalling = std::string{signalling_token(stream.signalling)};
    facts.packet_size = stream.packet_size;
    if (stream.service.has_value()) {
        const mpegts::ServiceInfo& service = *stream.service;
        facts.service_present = true;
        facts.service_bsmod = service.bsmod;
        facts.service_bsmod_present = service.bsmod_present;
        facts.service_full_service = service.full_service;
        facts.service_bsid = service.bsid;
        facts.service_mainid = service.mainid;
        facts.service_priority = service.priority;
        facts.service_asvc = service.asvc.has_value() ? std::optional<int>{*service.asvc}
                                                       : std::nullopt;
        facts.service_mix_metadata = service.mix_metadata;
    }
    return facts;
}

}  // namespace

std::string_view container_token(ContainerKind kind) {
    switch (kind) {
        case ContainerKind::kMatroska: return "matroska";
        case ContainerKind::kMp4: return "mp4";
        case ContainerKind::kMpegTs: return "mpegts";
        case ContainerKind::kUnknown: break;
    }
    return "";
}

StreamTrim trim_from_edit_list(const mp4::ReadTrack& track, std::string& note) {
    StreamTrim trim;
    note.clear();
    const mp4::EditListEntry* media = nullptr;
    std::size_t with_media = 0;
    for (const mp4::EditListEntry& edit : track.edits) {
        if (edit.media_time < 0) {
            continue;  // an empty edit
        }
        ++with_media;
        if (media == nullptr) {
            media = &edit;
        }
    }
    if (media == nullptr) {
        return trim;
    }
    constexpr std::int32_t kNormalSpeed = 0x00010000;
    constexpr std::string_view kUntrimmed =
        ", so it is not applied and every sample of the track plays.";
    if (with_media > 1) {
        note = "The file's edit list has " + std::to_string(with_media) +
               " edits with audio in them" + std::string{kUntrimmed};
        return trim;
    }
    if (media->media_rate != kNormalSpeed) {
        note = "The file's edit list plays the audio at another speed" + std::string{kUntrimmed};
        return trim;
    }
    // The rate the stream's samples are counted at: the sample entry's field,
    // which holds every rate AC-3 and E-AC-3 code, and mdhd's timescale
    // otherwise, which is normally the same number.
    const std::uint32_t rate = track.sample_rate != 0 ? track.sample_rate : track.timescale;
    if (rate == 0 || track.timescale == 0) {
        note = "The file's edit list has no timescale to be read in" + std::string{kUntrimmed};
        return trim;
    }
    trim.start = rescale(static_cast<std::uint64_t>(media->media_time), rate, track.timescale);
    // A zero duration runs to the end of the media, as a fragmented file
    // writes it; without a movie timescale there is nothing to count one in.
    if (media->segment_duration != 0 && track.movie_timescale != 0) {
        trim.length = rescale(media->segment_duration, rate, track.movie_timescale);
    }
    return trim;
}

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
            return {.bytes = concat_frames(demuxed->frames),
                    .error = {},
                    .container = facts_of(*demuxed)};
        }
        case ContainerKind::kMp4: {
            const auto demuxed = mp4::demux(file);
            if (!demuxed) {
                return {.bytes = {},
                       .error = std::string{"MP4 file this build cannot demux ("} +
                                std::string{mp4::describe(demuxed.error())} + ")"};
            }
            if (demuxed->track.codec_id == mp4::kCodecAc4) {
                // An 'ac-4' sample is the raw_ac4_frame ALONE (TS 103 190-2
                // Annex E.4) - no sync word, no frame_size, no CRC - so a
                // plain concatenation is not an elementary stream anything
                // can re-sync on. Re-wrap each sample in Annex G.3.1's
                // ac4_syncframe: 0xAC40 (the no-CRC sync word), the 16-bit
                // frame_size, and the escape (0xFFFF + 24-bit) for a frame
                // the short field cannot hold.
                std::vector<std::byte> out;
                for (const auto sample : demuxed->samples) {
                    const auto put = [&out](std::uint32_t v, int bytes) {
                        for (int b = bytes - 1; b >= 0; --b) {
                            out.push_back(static_cast<std::byte>((v >> (8 * b)) & 0xFFu));
                        }
                    };
                    put(0xAC40u, 2);
                    if (sample.size() >= 0xFFFF) {
                        put(0xFFFFu, 2);
                        put(static_cast<std::uint32_t>(sample.size()), 3);
                    } else {
                        put(static_cast<std::uint32_t>(sample.size()), 2);
                    }
                    out.insert(out.end(), sample.begin(), sample.end());
                }
                return {.bytes = std::move(out), .error = {}, .container = facts_of(*demuxed)};
            }
            std::string note;
            const StreamTrim trim = trim_from_edit_list(demuxed->track, note);
            return {.bytes = concat_frames(demuxed->samples),
                    .error = {},
                    .trim = trim,
                    .trim_note = std::move(note),
                    .container = facts_of(*demuxed)};
        }
        case ContainerKind::kMpegTs: {
            const auto demuxed = mpegts::demux(file);
            if (!demuxed) {
                return {.bytes = {},
                       .error = std::string{"Transport Stream this build cannot demux ("} +
                                std::string{mpegts::describe(demuxed.error())} + ")"};
            }
            return {.bytes = concat_frames(demuxed->payloads),
                    .error = {},
                    .container = facts_of(*demuxed)};
        }
    }
    return {.bytes = {}, .error = "unrecognised container"};
}

}  // namespace ac3::apps
