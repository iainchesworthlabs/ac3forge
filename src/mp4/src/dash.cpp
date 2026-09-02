#include "mp4/dash.hpp"

#include "mp4/hls.hpp"  // hls_codec_string: DASH's @codecs is the same RFC 6381 string

#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <span>
#include <string>
#include <string_view>

#include "manifest_detail.hpp"
#include "mp4/mp4.hpp"

namespace mp4 {

namespace {

using manifest_detail::segment_infos;

// ISO/IEC 23009-1 §5.3.9.6's SegmentTimeline: a run-length-encoded list of
// <S d="..." r="..."/> entries, one per run of consecutive equal-duration
// segments (r is how many times d repeats AFTER the first, so a run of N
// equal segments is r="N-1" - or the attribute omitted for N=1, since r's
// default is 0). fragment()'s own segments are constant-duration except a
// possibly shorter final one (MediaSegment::duration_samples' own comment
// in mp4.hpp), so this is normally one or two <S> lines, but the encoding
// is fully general - it stays correct for any duration pattern a future
// caller's own MediaSegment list might have.
//
// The FIRST entry always carries @t, the timeline's own start
// (MediaSegment::base_media_decode_time, which is the same number that
// fragment's tfdt holds - ISO/IEC 14496-12 §8.8.12). Writing it rather than
// leaning on @t's "continue from the previous segment, or 0" default is what
// makes a manifest describing a rolling WINDOW of a live stream correct:
// FragmentWriter::window()'s first segment is not the track's first segment
// once the window has rolled, and a player told nothing would place it at
// zero. TS 103 420 §D.2.3's own example MPD writes <S t="0" .../> for a
// timeline that does start at zero, so this is also just what the spec's
// worked example looks like.
[[nodiscard]] std::string build_segment_timeline(std::span<const SegmentInfo> segments) {
    std::string out = "      <SegmentTimeline>\n";
    std::size_t i = 0;
    while (i < segments.size()) {
        const auto duration = segments[i].duration_samples;
        std::size_t run = 1;
        while (i + run < segments.size() && segments[i + run].duration_samples == duration) {
            ++run;
        }
        const std::string start =
            i == 0 ? fmt::format(" t=\"{}\"", segments[i].base_media_decode_time) : std::string{};
        if (run > 1) {
            out += fmt::format("        <S{} d=\"{}\" r=\"{}\"/>\n", start, duration, run - 1);
        } else {
            out += fmt::format("        <S{} d=\"{}\"/>\n", start, duration);
        }
        i += run;
    }
    out += "      </SegmentTimeline>\n";
    return out;
}

// The Representation's channel-configuration and (for object audio) JOC
// descriptors - see DashOptions' own citations for every scheme URI and
// value rule here. Ordered AudioChannelConfiguration then
// SupplementalProperty because ISO/IEC 23009-1's RepresentationBaseType is a
// sequence, not a choice: both must also precede the SegmentTemplate that
// follows them.
[[nodiscard]] std::string build_representation_descriptors(const AudioTrack& track,
                                                           const DashOptions& options) {
    std::string out;
    if (options.dolby_channel_configuration.empty()) {
        out += fmt::format(
            "    <AudioChannelConfiguration "
            "schemeIdUri=\"urn:mpeg:mpegB:cicp:ChannelConfiguration\" value=\"{}\"/>\n",
            track.channels);
    } else {
        out += fmt::format(
            "    <AudioChannelConfiguration "
            "schemeIdUri=\"tag:dolby.com,2014:dash:audio_channel_configuration:2011\" "
            "value=\"{}\"/>\n",
            options.dolby_channel_configuration);
    }
    if (options.joc_complexity_index) {
        out +=
            "    <SupplementalProperty "
            "schemeIdUri=\"tag:dolby.com,2018:dash:EC3_ExtensionType:2018\" value=\"JOC\"/>\n";
        out += fmt::format(
            "    <SupplementalProperty "
            "schemeIdUri=\"tag:dolby.com,2018:dash:EC3_ExtensionComplexityIndex:2018\" "
            "value=\"{}\"/>\n",
            *options.joc_complexity_index);
    }
    return out;
}

}  // namespace

std::string build_dash_adaptation_set(const AudioTrack& track,
                                      std::span<const SegmentInfo> segments,
                                      const DashOptions& options) {
    const auto bandwidth = manifest_detail::estimate_bandwidth_bps(segments, track.sample_rate);
    // @startNumber is the number $Number$ takes for the timeline's FIRST <S>
    // entry (ISO/IEC 23009-1 §5.3.9.5.3), which for a rolling live window is
    // not 1 - it is whichever segment the window now begins at. Empty-segment
    // callers get 1, the attribute's own default.
    const std::uint32_t start_number =
        segments.empty() ? 1 : segments.front().sequence_number;

    // mimeType "audio/mp4" (RFC 4337-family ISOBMFF audio media type) and
    // codecs the same RFC 6381 sample-entry fourcc mp4::hls_codec_string
    // documents (ISO/IEC 14496-15 §5.5's 'ac-3'/'ec-3' registration) - DASH
    // (ISO/IEC 23009-1 §5.3.7.2) uses the identical 'Codecs' parameter HLS
    // does, both deriving it from RFC 6381's general ISOBMFF-file-family
    // registration rather than defining their own. TS 103 420 §D.2.1 says
    // the same for an object-audio track specifically: "The value of the
    // @codecs attribute is ec-3" - the object layer does not change it.
    //
    // A SegmentTimeline (rather than a flat SegmentTemplate @duration),
    // exact per-segment durations rather than a nominal one - a real gap a
    // flat @duration leaves: with a nominal duration and a shorter final
    // segment (the normal case - see mp4.hpp's own comment on
    // MediaSegment::duration_samples), a player computing "how many
    // segments does mediaPresentationDuration/@duration imply" can overshoot
    // by one and request a segment number that does not exist. Confirmed
    // against a real decode: ffmpeg's own dash demuxer did exactly that
    // against an earlier flat-@duration version of this function, logging
    // "Failed to open fragment of playlist" for the one-past-the-end
    // request - functionally harmless there (it still recovered and decoded
    // every real sample), but not something to ship deliberately when the
    // exact, spec-correct alternative is no harder to build from the same
    // segment list.
    return fmt::format(
        "<AdaptationSet mimeType=\"audio/mp4\" segmentAlignment=\"true\">\n"
        "  <Representation id=\"{}\" codecs=\"{}\" bandwidth=\"{}\" audioSamplingRate=\"{}\">\n"
        "{}"
        "    <SegmentTemplate timescale=\"{}\" initialization=\"{}\" media=\"{}\" "
        "startNumber=\"{}\">\n"
        "{}"
        "    </SegmentTemplate>\n"
        "  </Representation>\n"
        "</AdaptationSet>\n",
        options.representation_id, hls_codec_string(track), bandwidth, track.sample_rate,
        build_representation_descriptors(track, options), track.sample_rate,
        options.init_segment_uri, options.segment_uri_template, start_number,
        build_segment_timeline(segments));
}

std::string build_dash_adaptation_set(const AudioTrack& track,
                                      std::span<const MediaSegment> segments,
                                      const DashOptions& options) {
    return build_dash_adaptation_set(track, segment_infos(segments), options);
}

std::string build_dash_mpd(const AudioTrack& track, std::span<const SegmentInfo> segments,
                           std::string_view adaptation_set, const MpdOptions& options) {
    // profiles: isoff-live is the ISO/IEC 23009-1 §8.4 profile a
    // SegmentTemplate-based presentation declares, static or dynamic alike -
    // "live" there names the SegmentTemplate addressing scheme, not MPD@type.
    // minBufferTime PT2S: a REQUIRED attribute (§5.3.1.2) with no better
    // answer available here than a conventional couple of seconds, since this
    // module has no model of the delivery network its caller will use.
    if (options.is_static) {
        std::uint64_t total_samples = 0;
        for (const auto& segment : segments) {
            total_samples += segment.duration_samples;
        }
        const double total_seconds =
            static_cast<double>(total_samples) / static_cast<double>(track.sample_rate);
        return fmt::format(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
            "mediaPresentationDuration=\"PT{:.3f}S\" minBufferTime=\"PT2S\" "
            "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\">\n"
            "  <Period>\n"
            "{}"
            "  </Period>\n"
            "</MPD>\n",
            total_seconds, adaptation_set);
    }

    // Dynamic. No mediaPresentationDuration - the presentation has not ended,
    // so there is no total to state, and stating one would tell a player the
    // stream stops there. @availabilityStartTime is what anchors every
    // segment's availability to wall-clock time, and <Period start="PT0S">
    // ties the timeline's own zero to it. TS 103 420 §D.2.3's example MPD is
    // this same set of attributes.
    const std::string publish =
        options.publish_time.empty()
            ? std::string{}
            : fmt::format(" publishTime=\"{}\"", options.publish_time);
    return fmt::format(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"dynamic\" "
        "availabilityStartTime=\"{}\"{} minimumUpdatePeriod=\"PT{:.3f}S\" "
        "timeShiftBufferDepth=\"PT{:.3f}S\" minBufferTime=\"PT2S\" "
        "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\">\n"
        "  <Period id=\"1\" start=\"PT0S\">\n"
        "{}"
        "  </Period>\n"
        "</MPD>\n",
        options.availability_start_time, publish, options.minimum_update_period_seconds,
        options.time_shift_buffer_depth_seconds, adaptation_set);
}

std::string build_dash_mpd(const AudioTrack& track, std::span<const MediaSegment> segments,
                           std::string_view adaptation_set, const MpdOptions& options) {
    return build_dash_mpd(track, segment_infos(segments), adaptation_set, options);
}

}  // namespace mp4
