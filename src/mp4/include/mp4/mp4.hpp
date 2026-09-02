#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mp4/export.hpp"

// A minimal MP4 (ISOBMFF) muxer, per ISO/IEC 14496-12 (the ISO Base Media
// File Format).
//
// This is a container writer and nothing more, in exactly the sense
// matroska::matroska is: it knows how to nest ISOBMFF boxes and lay out one
// audio track's samples, and it takes each frame - and the codec's own
// configuration box payload - as opaque bytes. It has NO dependency on
// ac3::forge and no knowledge of AC-3: a caller muxing E-AC-3 hands over
// whole access units plus a ready-made 'dec3' payload (see
// ac3::io::build_codec_config_box, ac3/io/dec3.hpp); a caller muxing
// something else hands over whatever its own frames and sample-entry config
// box are. Keeping the codec-specific box payload opaque here is exactly why
// matroska::matroska stays codec-blind too, applied to the one place MP4
// needs codec-specific bytes that Matroska's plain CodecID string does not:
// see mp4::AudioTrack::codec_config below.
//
// Deliberately small. Enough to produce a file a player will open with the
// right channel layout, duration and codec signalling:
//   - one audio track, ftyp/moov/mdat only,
//   - one sample per chunk (no interleaving/multi-track concerns to solve),
//   - stts/stsz/stco built straight off the frame sizes handed in.
// No edit lists, no multiple tracks. Those matter for large-file seeking and
// multi-track muxing, not for playing back what this project produces.
//
// fragment() below (ROADMAP.md's A2) is the fMP4/CMAF follow-up mux() itself
// used to defer: an initialization segment plus one or more media segments,
// built from the same opaque AudioTrack/frame shape - see its own comment
// further down. FragmentWriter beside it (IO4) is the incremental form of the
// same thing, for a session whose length is not known up front. mp4/hls.hpp
// and mp4/dash.hpp build the HLS media playlist and DASH MPD that point at
// what either produces.

namespace mp4 {

// ISOBMFF sample entry codes (ISO/IEC 14496-15 §5.5 registers 'ac-3'; ETSI
// TS 102 366 Annex F itself is what ties each to its dac3/dec3 box). These
// are also, unmodified, RFC 6381's own 'Codecs' parameter value for either
// codec (see mp4/hls.hpp's hls_codec_string) - neither registers any of the
// dot-separated profile/level fields RFC 6381 §3 makes room for, so the bare
// sample-entry fourcc IS the codec string.
inline constexpr std::string_view kCodecAc3 = "ac-3";
inline constexpr std::string_view kCodecEac3 = "ec-3";
// TS 103 190-2 Annex E.4's AC4SampleEntry ('ac-4', configuration box
// 'dac4'). Unlike the two above, the bare fourcc is NOT the whole RFC 6381
// codec string - Annex E.13 appends bitstream/presentation/mdcompat fields -
// so an AC-4 AudioTrack that feeds HLS/DASH should also set
// AudioTrack::rfc6381 (ac4::rfc6381_codec_string produces it).
inline constexpr std::string_view kCodecAc4 = "ac-4";

enum class MuxError : std::uint8_t {
    kNoFrames,
    kInvalidTrack,    // zero/negative channels or sample rate, unrecognised codec id, or no
                      // codec_config payload
    kFileTooLarge,    // mdat would need a 64-bit chunk offset (co64), unsupported in this cut
    kInvalidOptions,  // e.g. FragmentOptions::frames_per_fragment == 0
};

[[nodiscard]] MP4_EXPORT std::string_view describe(MuxError error);

struct AudioTrack {
    // Selects the sample entry box ('ac-3' or 'ec-3') and, through it, which
    // configuration box wraps `codec_config` ('dac3' or 'dec3' respectively -
    // see ETSI TS 102 366 Annex F). Any other value is kInvalidTrack: this
    // module only knows how to describe an AC-3/E-AC-3 sample entry, the same
    // way matroska::AudioTrack::codec_id is free-form but this one is not -
    // an MP4 sample entry's box layout genuinely depends on which codec it
    // is, unlike Matroska's CodecID string.
    std::string codec_id{kCodecEac3};
    std::uint32_t sample_rate = 48000;
    int channels = 2;
    // Samples one frame represents, used to build stts and to compute the
    // track's duration. An E-AC-3/AC-3 access unit is 1536.
    std::uint32_t samples_per_frame = 1536;
    // The sample entry's one child configuration box, PAYLOAD ONLY (this
    // module writes the box's own size+FourCC header, choosing 'dac3' or
    // 'dec3' from codec_id above). Opaque to this module by design - see
    // ac3::io::build_codec_config_box (ac3/io/dec3.hpp) for how AC-3/E-AC-3
    // callers produce it, and examples/mux_mp4.cpp for the full round trip.
    std::vector<std::byte> codec_config;
    std::string language{"und"};
    // RFC 6381 codec string override for hls_codec_string()/the DASH
    // manifest, for the one codec here whose string is not its fourcc
    // (kCodecAc4 - see its comment). Empty means "the codec_id IS the
    // string", which stays true for AC-3/E-AC-3. Kept here rather than
    // parsed out of codec_config so this module stays codec-blind.
    std::string rfc6381{};
};

struct MuxOptions {
    std::string writing_app{"ac3forge"};
};

// Mux frames into a complete .mp4, returned as bytes. No file I/O here, so
// this stays testable without touching a disk - matroska::mux()'s own reason
// applies unchanged. Frames arrive as views (matroska::mux's own reasoning
// there too); the vector-list overload below forwards for owned lists.
[[nodiscard]] MP4_EXPORT std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::span<const std::byte>> frames,
    const MuxOptions& options = {});

[[nodiscard]] MP4_EXPORT std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::vector<std::byte>> frames,
    const MuxOptions& options = {});

// --- Fragmented MP4 / CMAF --------------------------------------------------
//
// fragment() lays out the same track and frames as mux(), but as a
// fragmented movie (ISO/IEC 14496-12 §8.8's moof/mfhd/traf/tfhd/tfdt/trun)
// split into CMAF-shaped pieces (ISO/IEC 23000-19): an initialization
// segment (ftyp+moov, whose one trak carries mvex/trex instead of a
// populated sample table - a fragmented track's own stbl describes zero
// samples, ISO/IEC 14496-12 §8.8.3) that a player/packager loads once, and
// one or more media segments (styp+moof+mdat, one per fragment) that carry
// the actual samples and are what an HLS/DASH segment URI ends up pointing
// at - see mp4/hls.hpp and mp4/dash.hpp.
//
// A batch API, the same shape mux() already is: every frame is known up
// front (mirrors how matroska::mux() stayed batch-only when
// matroska::Writer was added later for a true live/incremental caller - see
// that header's own comment). fragment() therefore fills in real
// durations/timestamps throughout, including the track's total duration in
// mvhd/tkhd/mdhd; FragmentWriter below is the live/incremental form, and the
// unknown total duration is the one place the two differ (see its own
// comment).

struct FragmentOptions {
    std::string writing_app{"ac3forge"};
    // How many frames (access units) each fragment/media segment carries. A
    // fragment boundary is also wherever a player or CDN can start an
    // independent HTTP request, so this is really "how long is one HLS/DASH
    // segment" - 48 frames of 1536 samples at 48 kHz is 1.536 s, inside the
    // 1-10 s range the CMAF/DASH-IF interoperability guidelines assume most
    // packagers and CDNs are tuned for. Every AC-3/E-AC-3 access unit this
    // project produces is independently decodable (see
    // AudioTrack::samples_per_frame's own comment, and how ac3::io::scan
    // already groups a whole access unit - independent substream plus any
    // dependents - into the one opaque frame mp4:: ever sees), so any
    // grouping is valid; this only trades segment count for
    // segment-switch/start-up latency.
    std::uint32_t frames_per_fragment = 48;
    // Adds 'ceao' to the ftyp/styp compatible-brands list. ETSI TS 103 420
    // §E.5 ("Core object-based audio media profile"): "The FileTypeBox
    // compatibility brand shall be ceao and should be used to indicate media
    // tracks that conform to this media profile" - the object-based-audio
    // CMAF profile §E defines for a backward-compatible object-audio E-AC-3
    // track, which DASH-IF IOP Part 8 v5.0.0 §5.3.3 then repeats as
    // "Additionally, a compatibility brand of 'ceao' should be used".
    // Caller-supplied, exactly like HlsOptions::channels_attribute and
    // DashOptions::joc_complexity_index (mp4/hls.hpp, mp4/dash.hpp): whether a
    // stream carries TS 103 420's object layer is bitstream syntax this module
    // never reads - the caller that scanned oba_complexity_index off it to
    // build the dec3 box is the one that knows.
    bool object_audio_brand = false;
    // FragmentWriter only - fragment() returns every segment it built, so it
    // has nothing to window. How many of the most recent segments' SegmentInfo
    // FragmentWriter::window() keeps, for a rolling live HLS playlist and DASH
    // SegmentTimeline. 0 - the default - keeps every segment, which is what a
    // session published whole afterwards wants; a real live origin that
    // deletes segments behind itself sets this to its own time-shift buffer
    // depth in segments. RFC 8216 §6.2.2 wants a live Media Playlist to hold
    // at least three target durations of media, so a window below 3 is
    // accepted here but is not something a player will enjoy.
    std::uint32_t playlist_window_segments = 0;
};

// One media segment: styp + moof + mdat, ready to write out as-is (e.g.
// "segment3.m4s"). The bookkeeping fields alongside `bytes` are exactly what
// mp4/hls.hpp and mp4/dash.hpp need to build a playlist/MPD without
// re-parsing the segment's own boxes back out.
struct MediaSegment {
    std::vector<std::byte> bytes;
    std::uint32_t sequence_number = 0;   // this fragment's mfhd sequence_number (1-based)
    std::uint32_t sample_count = 0;      // frames carried in this fragment
    std::uint64_t duration_samples = 0;  // sample_count * AudioTrack::samples_per_frame
    // This fragment's own tfdt baseMediaDecodeTime: where it starts on the
    // track's timeline, in AudioTrack::sample_rate units. Zero for the first
    // segment, and the running sum of every earlier segment's duration_samples
    // after that. A DASH SegmentTimeline's first <S t="..."> needs it whenever
    // the manifest describes a WINDOW of segments rather than the whole track
    // from zero - which is exactly the live case (see mp4/dash.hpp).
    std::uint64_t base_media_decode_time = 0;
};

// The bookkeeping half of a MediaSegment - everything a playlist or an MPD
// needs to know about a segment, without the segment's bytes. A live writer
// keeps a rolling window of these to rebuild its manifests whenever a new
// segment closes (see FragmentWriter::window()); keeping whole MediaSegments
// there instead would mean holding the entire time-shift buffer's audio in
// memory purely to be able to name it.
struct SegmentInfo {
    std::uint32_t sequence_number = 0;
    std::uint32_t sample_count = 0;
    std::uint64_t duration_samples = 0;
    std::uint64_t base_media_decode_time = 0;
    // MediaSegment::bytes.size() - what the HLS BANDWIDTH and DASH @bandwidth
    // averages are computed from (see src/mp4/src/manifest_detail.hpp).
    std::uint64_t byte_size = 0;
};

// The SegmentInfo describing one MediaSegment, for a caller holding
// fragment()'s batch output that wants the manifest builders' SegmentInfo
// overloads (mp4/hls.hpp, mp4/dash.hpp).
[[nodiscard]] MP4_EXPORT SegmentInfo segment_info(const MediaSegment& segment);

struct FragmentedOutput {
    std::vector<std::byte> init_segment;       // ftyp + moov (mvex/trex, zero samples)
    std::vector<MediaSegment> media_segments;  // one per fragment, in sequence_number order
};

// Fragments frames into an initialization segment plus media segments. No
// file I/O, same as mux(); the caller decides file names (or byte-range
// offsets, for a single concatenated CMAF track file) for init_segment and
// each media_segments[i].
[[nodiscard]] MP4_EXPORT std::expected<FragmentedOutput, MuxError> fragment(
    const AudioTrack& track, std::span<const std::span<const std::byte>> frames,
    const FragmentOptions& options = {});

[[nodiscard]] MP4_EXPORT std::expected<FragmentedOutput, MuxError> fragment(
    const AudioTrack& track, std::span<const std::vector<std::byte>> frames,
    const FragmentOptions& options = {});

// matroska::Writer's and mpegts::Writer's sibling (ROADMAP.md's IO4), for a
// session whose length is not known up front - a live capture, where
// fragment() above cannot help: it needs every frame before it can group them
// into fragments at all. A fragmented movie is the one container shape this
// needs no invention for, which is the whole reason ISO/IEC 14496-12 §8.8
// exists: every fragment carries its own moof, so nothing in a fragment
// depends on a later one.
//
// Contract, the same one mpegts::Writer's own tests assert: for the same
// track, options and frames, the media segments this class hands back are
// BYTE FOR BYTE the media segments fragment() would have built - mfhd
// sequence numbers, tfdt decode times, trun sample sizes and mdat payload
// alike. The only state fragment()'s own loop carries across fragments is the
// running decode time and the sequence number, and both live on this object
// instead.
//
// The initialization segment differs in exactly one respect, and by design:
// mvhd/tkhd/mdhd carry duration 0, where fragment() - which has every frame in
// hand - writes the real total. A live session does not know its total
// duration, and ISO/IEC 14496-12 §8.8.2 says as much by providing mehd for
// the fragmented movie that DOES know one; a track whose overall duration is
// not in the movie header is measured by walking its fragments instead. That
// is the identical concession matroska::Writer makes with EBML's unknown-size
// Segment and its omitted Duration, for the identical reason. Everything else
// in the init segment - stsd and its opaque dec3/dac3 payload, mvex/trex, the
// empty sample table - is byte-identical to fragment()'s.
//
// No file I/O, matching fragment() and both sibling writers: init_segment(),
// push() and finalize() hand back bytes for the caller to write. Memory stays
// bounded at one fragment's frames plus the playlist window
// (FragmentOptions::playlist_window_segments) however long the session runs.
class MP4_EXPORT FragmentWriter {
   public:
    // Validates the track and options exactly the way fragment() does. On
    // success init_segment() already holds the initialization segment.
    [[nodiscard]] static std::expected<FragmentWriter, MuxError> create(
        const AudioTrack& track, const FragmentOptions& options = {});

    // Write this exactly once, before any bytes push() or finalize() return -
    // it is the "init.mp4" an HLS #EXT-X-MAP or a DASH SegmentTemplate
    // @initialization points at.
    [[nodiscard]] const std::vector<std::byte>& init_segment() const { return init_segment_; }

    // Buffers one frame into the writer's current (in-progress) fragment.
    // Returns the media segment that just CLOSED to make room for it - so a
    // segment comes back on every options.frames_per_fragment-th call and
    // nullopt otherwise. Write whatever comes back, in order, as it comes back.
    [[nodiscard]] std::expected<std::optional<MediaSegment>, MuxError> push(
        std::span<const std::byte> frame);

    // Flushes the trailing partial fragment - call exactly once, when the
    // session ends. nullopt when the last push() happened to land on a
    // fragment boundary, or when nothing was ever pushed. Nothing else needs
    // closing: a fragmented movie has no trailer, and the init segment's
    // durations were never written as real numbers to begin with.
    [[nodiscard]] std::expected<std::optional<MediaSegment>, MuxError> finalize();

    [[nodiscard]] std::size_t frames_written() const { return frames_written_; }

    // Every segment emitted so far, or the most recent
    // options.playlist_window_segments of them - the exact list to hand
    // mp4/hls.hpp's and mp4/dash.hpp's SegmentInfo overloads once a segment
    // closes, so a rolling playlist's EXT-X-MEDIA-SEQUENCE and an MPD's
    // SegmentTimeline advance with the window. Invalidated by the next
    // push()/finalize().
    [[nodiscard]] std::span<const SegmentInfo> window() const { return window_; }

   private:
    FragmentWriter(AudioTrack track, FragmentOptions options, std::vector<std::byte> init_segment);

    [[nodiscard]] std::expected<MediaSegment, MuxError> close_fragment();

    AudioTrack track_;
    FragmentOptions options_;
    std::vector<std::byte> init_segment_;
    std::vector<std::vector<std::byte>> pending_;
    std::uint64_t decode_time_ = 0;
    std::uint32_t sequence_number_ = 1;
    std::size_t frames_written_ = 0;
    std::vector<SegmentInfo> window_;
};

}  // namespace mp4
