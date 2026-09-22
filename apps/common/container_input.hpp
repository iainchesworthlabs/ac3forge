#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Turning a container file into the elementary stream ac3::forge actually
// decodes (container readers (mkv/mp4/ts)) - shared by ac3cli (decode/qc/levels/play/monitor)
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
// ac3::forge to decode bare elementary streams and wants nothing else. This
// file depends on both instead, which is fine at this layer - apps/common
// already does for RecordingSink (ac3/io/wav.hpp, ac3/iec61937/iec61937.hpp) -
// since disambiguating a container from a bare elementary stream is exactly
// where knowing both sides earns its keep (see ContainerKind's own comment).

namespace mp4 {
struct ReadTrack;
}  // namespace mp4

namespace ac3::apps {

// Which container a file actually is, decided by its first bytes rather than
// its name - a rip is as likely to be called "title00.mkv" when it is not
// one as it is to have no extension at all. `head` needs only the first few
// KiB; a whole file works too but is wasted effort. kUnknown covers a bare
// elementary stream (or a WAV, for the callers that also accept one), which
// is most of what either caller actually sees.
//
// The MPEG-TS check is the loosest of the three - it has no magic, only a
// recurring sync byte - and a bare AC-3/E-AC-3 stream can satisfy it by
// accident: at some common bitrate/rate pairs (48 kbit/s at 48 kHz codes
// exactly 192-byte frames, one of the three grid strides) a low-entropy
// signal encodes near-identical bytes every frame, which looks exactly like
// a packet grid to a check that only counts recurrence (tools/ci/
// fuzz_encoder_space.py's REGRESSION_SEEDS, seed 3600083275727211684, found
// this for real). sniff_container's implementation checks for a genuine,
// syntactically-valid AC-3/E-AC-3 frame header first, which no accidental
// byte pattern satisfies the way a single recurring byte can - see its own
// comment.
enum class ContainerKind : std::uint8_t { kUnknown, kMatroska, kMp4, kMpegTs };

[[nodiscard]] ContainerKind sniff_container(std::span<const std::byte> head);

// A token for each kind: "matroska", "mp4" and "mpegts", and empty for
// kUnknown.
[[nodiscard]] std::string_view container_token(ContainerKind kind);

// An MP4 track's codec configuration box, read into its syntax values (ETSI
// TS 102 366 Annex F): dac3's or dec3's fields, or, for dac4, the name and
// size alone, since its fields are AC-4's (TS 103 190-2 Annex E.5).
struct CodecBox {
    std::string type{};  // "dac3", "dec3" or "dac4"
    int fscod = 0;
    int bsid = 0;
    int bsmod = 0;
    int acmod = 0;
    bool lfeon = false;
    int bit_rate_code = 0;   // dac3
    int data_rate_kbps = 0;  // dec3
    // dec3: the independent substreams (num_ind_sub + 1), and the first
    // one's dependents and their chan_loc.
    int independent_substreams = 0;
    int num_dep_sub = 0;
    int chan_loc = 0;
    bool asvc = false;
    // dec3's Atmos extension (TS 103 420 §8.3.2.2), when the box carries it.
    std::optional<int> complexity_index = std::nullopt;
    std::size_t bytes = 0;
};

// What a container declared about the track it gave up, for whoever shows the
// file. A bare elementary stream has kind kUnknown and nothing else set.
struct ContainerFacts {
    ContainerKind kind = ContainerKind::kUnknown;
    // The codec as the container names it: "ec-3" (MP4), "A_EAC3"
    // (Matroska). MPEG-TS names a stream by stream_type instead.
    std::string codec_id{};
    // MP4's track_ID, Matroska's TrackNumber or MPEG-TS's elementary PID.
    std::uint64_t track = 0;
    // ISO 639-2, as stored ("und" when the file names none); empty for
    // MPEG-TS, whose language descriptor is not read.
    std::string language{};
    // What the track holds: MP4 samples, Matroska frames or MPEG-TS PES
    // payloads.
    std::uint64_t samples = 0;
    // The rate and channel count the track declares; 0 where it declares
    // none (MPEG-TS).
    std::uint32_t sample_rate = 0;
    int channels = 0;
    // MP4: mdhd's and mvhd's timescales, the edit list's length, and the
    // codec configuration box.
    std::uint32_t timescale = 0;
    std::uint32_t movie_timescale = 0;
    std::size_t edits = 0;
    std::optional<CodecBox> codec_box = std::nullopt;
    // MPEG-TS: the programme, its PMT's PID, the stream_type, how the PMT
    // named the codec ("atsc_stream_type", "dvb_descriptor",
    // "registration_descriptor" or "dvb_extension_descriptor") and the
    // packet size.
    std::uint16_t program_number = 0;
    std::uint16_t pmt_pid = 0;
    std::uint8_t stream_type = 0;
    std::string signalling{};
    std::size_t packet_size = 0;
    // MPEG-TS: the PMT's own AC-3/E-AC-3 audio descriptor, decoded (see
    // mpegts::parse_service_descriptor) rather than pulled in as
    // mpegts::ServiceInfo directly - plain values here, matching CodecBox
    // above, so this header stays free of every container library's own
    // types. service_present false (the default, every other service_*
    // field also left at its own default) means the PMT had no such
    // descriptor at all - registration-descriptor and AC-4 signalling, or a
    // malformed one - not that it said "nothing". mainid/asvc/full_service
    // stay std::nullopt for the same reason mpegts::ServiceInfo's own fields
    // do: nothing here is guessed.
    bool service_present = false;
    int service_bsmod = 0;
    bool service_bsmod_present = false;
    std::optional<bool> service_full_service = std::nullopt;
    int service_bsid = 0;
    std::optional<int> service_mainid = std::nullopt;
    int service_priority = 3;
    std::optional<int> service_asvc = std::nullopt;
    bool service_mix_metadata = false;
};

// The part of a decoded stream its container says to play, in samples at the
// stream's rate: skip `start`, then play `length`, or to the end when that is
// unset. What an encoder's priming and a last frame's padding look like from
// the outside - the default, all of it, is what a container that says nothing
// means.
struct StreamTrim {
    std::uint64_t start = 0;
    std::optional<std::uint64_t> length = std::nullopt;
};

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
    // From an MP4 track's edit list, when it has the shape an audio encoder
    // writes: any empty edits (a delay before the track, which is not audio),
    // then one edit at normal speed. Decoding does not apply it; a player
    // does. Only the player does so far - ac3cli and the GUI still decode
    // every sample.
    StreamTrim trim{};
    // Set, with `trim` left at its default, when the file has an edit list
    // of another shape: more than one edit with media in it, or one played
    // at other than normal speed. A sentence for whoever shows the file.
    std::string trim_note{};
    // What the container said about the track, when `file` is one.
    ContainerFacts container{};
};

[[nodiscard]] ElementaryStreamResult elementary_stream_from_bytes(std::span<const std::byte> file);

// The trim an MP4 track's edit list describes, as elementary_stream_from_bytes
// reads it: whole, with `note` empty, for a track with no edit list or only
// empty edits; the one edit with media in it, counted in samples at the
// track's rate, when it plays at normal speed; and whole, with `note` saying
// why, for any other shape.
[[nodiscard]] StreamTrim trim_from_edit_list(const mp4::ReadTrack& track, std::string& note);

}  // namespace ac3::apps
