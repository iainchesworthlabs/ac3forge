#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "mp4/export.hpp"
#include "mp4/mp4.hpp"

// DASH signaling (ISO/IEC 23009-1, "Dynamic Adaptive Streaming over HTTP")
// for the same CMAF segments mp4::hls.hpp's helpers describe - that sharing
// is the entire point of CMAF (ISO/IEC 23000-19): one segment format, two
// manifest flavors. See mp4/hls.hpp's own header comment for the same
// codec-blindness this module keeps.

namespace mp4 {

struct DashOptions {
    std::string init_segment_uri{"init.mp4"};
    // ISO/IEC 23009-1 §5.3.9.4.3's SegmentTemplate substitution token for a
    // segment's 1-based number (MediaSegment::sequence_number) - "$Number$",
    // literally, not a "{}" placeholder like HlsOptions (DASH's own
    // template syntax, left as the DASH-native player/CDN sees it rather
    // than pre-expanded, since a real DASH SegmentTemplate is meant to stay
    // a template).
    std::string segment_uri_template{"segment$Number$.m4s"};
    std::string representation_id{"audio"};
    // TS 103 420 §8.3.2's complexity_index_type_a for a stream carrying that
    // spec's backward-compatible object audio (Dolby Atmos in Dolby Digital
    // Plus) - this project's own ac3::io::ScannedStream::oba_complexity_index.
    // Set, it adds the two SupplementalProperty descriptors DASH-IF IOP Part
    // 8 v5.0.0 §5.3.2 names for E-AC-3 with JOC, "as specified in ETSI TS
    // 103 420 clause D.2":
    //   - tag:dolby.com,2018:dash:EC3_ExtensionType:2018 (§D.2.2.1), whose
    //     value "shall be the three character string JOC", and
    //   - tag:dolby.com,2018:dash:EC3_ExtensionComplexityIndex:2018
    //     (§D.2.2.2), whose value "shall be decimal representation of the
    //     eight-bit element complexity_index_type_a in the EC3SpecificBox".
    // Unset (the default) is correct for plain AC-3/E-AC-3, and is what a
    // caller with no object layer to declare leaves alone. Caller-supplied
    // for the same reason HlsOptions::channels_attribute is: TS 103 420's
    // object layer is bitstream syntax mp4:: never reads - the caller that
    // scanned it off the stream to build the dec3 box already has the value.
    std::optional<int> joc_complexity_index = std::nullopt;
    // The AudioChannelConfiguration descriptor's @value, on the Dolby scheme
    // DASH-IF IOP Part 8 v5.0.0 §5.3.2 offers for E-AC-3 -
    // "tag:dolby.com,2014:dash:audio_channel_configuration:2011 as defined in
    // TS 102 366 clause I.1.2.1": four hexadecimal digits of the 16-bit
    // channel-assignment field, left channel in the most significant bit
    // (ATSC A/52-2018 Table E2.5 is the same 16 locations, bit 0 = Left
    // through bit 15 = LFE, "stored in the most significant bit"), so
    // L/C/R/Ls/Rs/LFE is "F801".
    //
    // Empty (the default) emits the OTHER scheme that same clause allows -
    // "urn:mpeg:mpegB:cicp:ChannelConfiguration as defined by
    // ChannelConfiguration in ISO/IEC 23091-3" - with AudioTrack::channels as
    // its value, which is exactly what TS 103 420's own example MPD (§D.2.3)
    // writes for a 5.1 JOC representation, carrying the Dolby form as a
    // commented-out alternative. Caller-supplied for the third time and the
    // same reason: an acmod/chanmap-to-channel-map mapping is AC-3 semantics,
    // not container syntax.
    std::string dolby_channel_configuration{};
};

// Which kind of Media Presentation Description build_dash_mpd() wraps an
// AdaptationSet in (ISO/IEC 23009-1 §5.3.1.2's MPD@type).
struct MpdOptions {
    // "static" - the whole asset exists, its total duration is known, and a
    // player may seek anywhere in it. The right shape for mp4::fragment()'s
    // batch output. False switches to "dynamic": segments appear over time,
    // there is no mediaPresentationDuration, and the timing fields below take
    // over. That is the live shape, and TS 103 420 §D.2.3's own example MPD
    // for an object-audio E-AC-3 presentation is one of these.
    bool is_static = true;
    // Dynamic only, and REQUIRED there (ISO/IEC 23009-1 §5.3.1.2): the
    // wall-clock time, as an ISO 8601 UTC string, that segment number
    // startNumber's playback would begin at - every segment's availability
    // is this plus its own position on the timeline. Supplied by the caller
    // rather than read off a clock here, because mp4:: has no clock: no file
    // I/O, no time, nothing but bytes in and bytes out, which is also what
    // keeps this testable without either.
    std::string availability_start_time{};
    // Dynamic only, optional: when this MPD instance itself was generated
    // (@publishTime). Empty omits the attribute.
    std::string publish_time{};
    // Dynamic only: how often a player should come back for a new MPD
    // (@minimumUpdatePeriod) and how far back the origin keeps segments
    // reachable (@timeShiftBufferDepth). The second should match whatever
    // FragmentOptions::playlist_window_segments amounts to in seconds - the
    // manifest and the origin disagreeing about that is how a live player
    // ends up requesting a segment that has already been deleted.
    double minimum_update_period_seconds = 2.0;
    double time_shift_buffer_depth_seconds = 60.0;
};

// One <AdaptationSet>...</AdaptationSet> XML snippet - codecs, bandwidth,
// audioSamplingRate, an AudioChannelConfiguration, any JOC signaling
// DashOptions asks for, and a SegmentTemplate pointing at the CMAF segments
// fragment() or FragmentWriter produced - ready to nest inside a caller's own
// <Period>, or to hand straight to build_dash_mpd() below.
// Single-representation audio only, per this module's scope (see mp4.hpp's
// own header comment on mp4:: staying single-track): no ABR ladder, no
// multi-period MPD - a caller assembling a richer manifest supplies those
// (whichever other Representations/Periods it has).
//
// Per-segment durations are exact, via a SegmentTemplate/SegmentTimeline
// (ISO/IEC 23009-1 §5.3.9.6) built from each segment's own duration_samples,
// rather than one nominal `duration` attribute assumed constant -
// fragment()'s own segments are constant-duration except (as usual) a
// possibly shorter final one, and a flat nominal duration is exactly what let
// a real player (ffmpeg's dash demuxer, while writing this module) compute
// one too many segments from mediaPresentationDuration and request a segment
// number past the end - harmless there, since it still recovered every real
// sample, but not a gap worth keeping when the exact alternative costs
// nothing extra to build. @startNumber and the timeline's first <S t="...">
// both come from the segments handed in rather than being assumed to start at
// the beginning of the track, which is what lets a rolling live WINDOW (see
// mp4::FragmentWriter::window()) describe itself correctly.
[[nodiscard]] MP4_EXPORT std::string build_dash_adaptation_set(
    const AudioTrack& track, std::span<const SegmentInfo> segments,
    const DashOptions& options = {});

// Convenience overload for a caller holding mp4::fragment()'s batch output.
[[nodiscard]] MP4_EXPORT std::string build_dash_adaptation_set(
    const AudioTrack& track, std::span<const MediaSegment> segments,
    const DashOptions& options = {});

// A complete single-Period MPD document wrapping one build_dash_adaptation_set
// snippet - the file a packager or origin serves as "manifest.mpd". Static by
// default (mediaPresentationDuration summed from the segments handed in);
// MpdOptions::is_static = false makes it the live/dynamic form instead.
[[nodiscard]] MP4_EXPORT std::string build_dash_mpd(const AudioTrack& track,
                                                    std::span<const SegmentInfo> segments,
                                                    std::string_view adaptation_set,
                                                    const MpdOptions& options = {});

[[nodiscard]] MP4_EXPORT std::string build_dash_mpd(const AudioTrack& track,
                                                    std::span<const MediaSegment> segments,
                                                    std::string_view adaptation_set,
                                                    const MpdOptions& options = {});

}  // namespace mp4
