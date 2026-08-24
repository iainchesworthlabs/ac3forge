#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mp4/export.hpp"
#include "mp4/mp4.hpp"

// HLS signaling (RFC 8216, "HTTP Live Streaming") for the CMAF segments
// mp4::fragment() produces.
//
// Deliberately as codec-blind as the rest of mp4:: allows: WHICH codec
// string and CHANNELS value to use is either derived straight from
// AudioTrack (already opaque-by-design - see mp4.hpp) or supplied by the
// caller through HlsOptions::channels_attribute. This module only knows HLS
// *syntax* - #EXTM3U/#EXT-X-STREAM-INF/#EXT-X-MEDIA/#EXT-X-MAP/#EXTINF - not
// AC-3/E-AC-3 semantics, the same boundary mp4::mux() itself draws around
// 'dec3'/'dac3' (see AudioTrack::codec_config).

namespace mp4 {

struct HlsOptions {
    std::string init_segment_uri{"init.mp4"};
    // "{}" is replaced with each fragment's 1-based sequence number
    // (MediaSegment::sequence_number). A pattern with no "{}" is used
    // verbatim for every segment - the caller's choice/mistake, not this
    // module's to second-guess.
    std::string segment_uri_pattern{"segment{}.m4s"};
    // #EXT-X-MEDIA's CHANNELS attribute (Apple's HLS Authoring Specification
    // for Apple Devices, "Audio" requirements
    // https://developer.apple.com/documentation/http-live-streaming/hls-authoring-specification-for-apple-devices):
    // ordinarily just the decimal channel count (e.g. "6"), but Dolby
    // Digital Plus with Atmos objects needs "<N>/JOC" instead, where N is
    // the decodable object count (this project's own
    // ac3::io::ScannedStream::oba_complexity_index, TS 103 420
    // §8.3.2's complexity_index_type_a) - reiterated, with a worked example
    // ("CHANNELS="12/JOC""), by Dolby's own Online Delivery Kit
    // documentation
    // (https://ott.dolby.com/OnDelKits/DDP/Dolby_Digital_Plus_Online_Delivery_Kit_v1.5/Documentation/Content_Creation/SDM/help_files/topics/hls_c_hls_signal_atmos_ddp.html)
    // and shown verbatim in a real manifest
    // (CODECS="avc1.64001f,ec-3" / CHANNELS="12/JOC") by AWS MediaLive's own
    // HLS+Atmos documentation. Empty means "just the track's channel count",
    // correct for plain (non-Atmos) AC-3/E-AC-3 - mp4:: itself has no
    // opinion on JOC, since that is TS 103 420 object-layer syntax this
    // module never reads; the caller (which already read
    // oba_complexity_index off the bitstream to build the dec3 box, see
    // ac3::io::build_codec_config_box) is the one that knows.
    std::string channels_attribute{};
    // #EXT-X-VERSION. 7 is the first version whose #EXT-X-MAP may appear in
    // a plain (non-I-frame-only) Media Playlist, which every fMP4 media
    // playlist needs (RFC 8216 §7's compatibility table).
    std::uint32_t version = 7;
    // Whether this is the whole, final asset (RFC 8216 §4.3.3.5's VOD
    // #EXT-X-PLAYLIST-TYPE plus a closing #EXT-X-ENDLIST) rather than a live,
    // still-growing playlist. True is right for mp4::fragment()'s batch
    // output, where every fragment is already known. False is the live shape
    // (RFC 8216 §6.2.2): both tags are omitted, and #EXT-X-MEDIA-SEQUENCE -
    // written from the FIRST listed segment's own sequence number either way -
    // is what tells a player that segments have rolled off the front of the
    // playlist since it last reloaded. Rewrite the playlist from
    // mp4::FragmentWriter::window() each time a segment closes, then rewrite it
    // once more with vod = true when the session ends.
    bool vod = true;
};

// The RFC 6381 'Codecs' parameter value for one AudioTrack: just the
// ISOBMFF sample entry's own four-character code (mp4::kCodecAc3/kCodecEac3,
// ISO/IEC 14496-15 §5.5's registration), since neither AC-3 nor E-AC-3
// registers any of the dot-separated profile/level fields RFC 6381 §3 makes
// room for (unlike e.g. "avc1.640028") - confirmed against every real HLS
// manifest example this module's own documentation cites, which always show
// a bare CODECS="...,ec-3"/"...,ac-3". A named function rather than just
// using track.codec_id directly at each call site exists so there is one
// place this claim, and its citation, live.
[[nodiscard]] MP4_EXPORT std::string_view hls_codec_string(const AudioTrack& track);

// One audio rendition of a master playlist's #EXT-X-MEDIA group (RFC 8216
// §4.3.4.1): its own Media Playlist, its own CHANNELS value, its own segments
// for the bandwidth arithmetic.
//
// More than one exists for a reason Apple's HLS Authoring Specification for
// Apple Devices spells out for Dolby Atmos: alongside the CHANNELS="<N>/JOC"
// rendition an asset should carry an equivalent 5.1 bitstream with
// CHANNELS="6" IN THE SAME GROUP, so a client that cannot render the object
// layer selects the plain bed rather than the asset failing to play. The two
// renditions are the same programme at the same duration, which is what makes
// them interchangeable inside one group - see ac3::io::strip_objects
// (ac3/io/object_strip.hpp), which produces exactly that companion without
// re-encoding anything.
struct HlsRendition {
    AudioTrack track{};
    // A manifest only ever reads a segment's bookkeeping, never its bytes
    // (mp4.hpp's SegmentInfo) - the same reason build_hls_master_playlist's
    // single-rendition form and build_hls_media_playlist both settled on it.
    // mp4::segment_info()/segment_infos() convert from a caller's own
    // FragmentedOutput::media_segments or FragmentWriter::window().
    std::span<const SegmentInfo> segments{};
    // Relative to the master playlist.
    std::string media_playlist_uri{};
    // #EXT-X-MEDIA's NAME, a human-readable label; REQUIRED by RFC 8216
    // §4.3.4.1 and shown in a player's audio-track picker.
    std::string name{"Audio"};
    // CHANNELS, per HlsOptions::channels_attribute's own note. Empty means
    // the track's plain channel count.
    std::string channels_attribute{};
    // DEFAULT=YES. Exactly one rendition in a group should carry it; the
    // #EXT-X-STREAM-INF URI points at that one's Media Playlist.
    bool is_default = false;
};

// A master playlist (RFC 8216 §4.3.4) over one or more audio renditions: an
// #EXT-X-MEDIA line each, all sharing one GROUP-ID, plus a single
// #EXT-X-STREAM-INF variant referencing that group - the self-referencing
// pattern real audio-only HLS content uses (there is no separate video Media
// Playlist for the variant to point at; the audio rendition IS the variant),
// so its URI is the default rendition's own playlist.
//
// BANDWIDTH (a REQUIRED EXT-X-STREAM-INF attribute) is the largest of the
// renditions' average bits/second: a client plays exactly one of them, so the
// variant needs the bandwidth of whichever it might pick, and an average
// across an asset is the only honest answer a batch fragmenter has (see
// manifest_detail::estimate_bandwidth_bps). CODECS comes from the default
// rendition's track.
[[nodiscard]] MP4_EXPORT std::string build_hls_master_playlist(
    std::span<const HlsRendition> renditions, const HlsOptions& options = {});

// The single-rendition form: one #EXT-X-MEDIA named "Audio", DEFAULT=YES,
// with HlsOptions::channels_attribute as its CHANNELS - the one-element case
// of the renditions form above, which it delegates to.
[[nodiscard]] MP4_EXPORT std::string build_hls_master_playlist(
    const AudioTrack& track, std::span<const SegmentInfo> segments,
    std::string_view media_playlist_uri, const HlsOptions& options = {});

// Convenience overload for a caller holding mp4::fragment()'s batch output
// (mp4::segment_infos() is what it forwards through). The SegmentInfo form
// above is the one a live caller wants: mp4::FragmentWriter::window() hands
// back exactly that, without keeping the windowed segments' bytes alive
// purely to name them.
[[nodiscard]] MP4_EXPORT std::string build_hls_master_playlist(
    const AudioTrack& track, std::span<const MediaSegment> segments,
    std::string_view media_playlist_uri, const HlsOptions& options = {});

// The media playlist itself (RFC 8216 §4.3.3): #EXTM3U, #EXT-X-MAP pointing
// at the initialization segment, then #EXTINF/segment-URI pairs for every
// fragment in order.
[[nodiscard]] MP4_EXPORT std::string build_hls_media_playlist(
    const AudioTrack& track, std::span<const SegmentInfo> segments,
    const HlsOptions& options = {});

[[nodiscard]] MP4_EXPORT std::string build_hls_media_playlist(
    const AudioTrack& track, std::span<const MediaSegment> segments,
    const HlsOptions& options = {});

}  // namespace mp4
