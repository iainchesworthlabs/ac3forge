#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "isobmff_detail.hpp"
#include "mp4/mp4.hpp"

// Fragmented MP4 (ISO/IEC 14496-12 §8.8) / CMAF (ISO/IEC 23000-19). See
// mp4.hpp's own comment on fragment() for the overall shape; this file is
// the box layout itself.

namespace mp4 {

namespace {

using detail::Bytes;
using detail::put_box;
using detail::put_bytes;
using detail::put_fourcc;
using detail::put_fullbox;
using detail::put_u32;
using detail::put_u64;

// A fragmented file's ftyp/styp brands (ISO/IEC 14496-12 §4.3/§8.16.2 - styp
// "has the same format as" ftyp). 'iso5'/'iso6' are the ISO BMFF edition
// brands that introduced movie-fragment support ('mvex'/'moof'); real
// fragmenting muxers commonly declare exactly this pair (verified against a
// real ffmpeg -movflags +frag_keyframe... output while writing this file).
// 'cmfc' is CMAF's own base structural brand (ISO/IEC 23000-19) - included
// because every constraint it names (one chunk per fragment, tfdt present,
// default-base-is-moof) is what this module actually writes, not aspirational.
constexpr std::array<std::string_view, 2> kFragmentedCompatibleBrands{"iso6", "cmfc"};

// ETSI TS 103 420 §E.5's object-based-audio CMAF profile brand, appended to
// the pair above when FragmentOptions::object_audio_brand says this track
// carries TS 103 420's object layer: "The FileTypeBox compatibility brand
// shall be ceao and should be used to indicate media tracks that conform to
// this media profile", which DASH-IF IOP Part 8 v5.0.0 §5.3.3 repeats for
// DASH delivery. Added rather than substituted - a JOC track is still an
// 'iso6'/'cmfc' fragmented CMAF track, and §E.2 says as much by requiring
// conformance to ISO/IEC 23000-19 on top of the profile, not instead of it.
constexpr std::string_view kObjectAudioBrand = "ceao";

std::vector<std::string_view> compatible_brands(const FragmentOptions& options) {
    std::vector<std::string_view> brands(kFragmentedCompatibleBrands.begin(),
                                         kFragmentedCompatibleBrands.end());
    if (options.object_audio_brand) {
        brands.push_back(kObjectAudioBrand);
    }
    return brands;
}

Bytes build_init_ftyp(const FragmentOptions& options) {
    return detail::build_brand_box("ftyp", "iso5", 0, compatible_brands(options));
}

Bytes build_media_styp(const FragmentOptions& options) {
    return detail::build_brand_box("styp", "iso5", 0, compatible_brands(options));
}

// ISO/IEC 14496-12 §8.8.3's Track Extends Box: the per-fragment defaults a
// trun can omit and still be well-formed. default_sample_size is
// deliberately 0 ("no default") - AC-3/E-AC-3 frame sizes vary (CBR framing
// still rounds per-frame, VBR always varies), so every trun below supplies
// an explicit per-sample size instead (sample-size-present, see build_trun).
//
// default_sample_flags packs ISO/IEC 14496-12 §8.8.3.1/§8.6.4.3's sample
// flags field: reserved(4) | is_leading(2) | sample_depends_on(2) |
// sample_is_depended_on(2) | sample_has_redundancy(2) |
// sample_padding_value(3) | sample_is_non_sync_sample(1) |
// sample_degradation_priority(16). sample_depends_on = 2 ("this sample does
// not depend on others") and sample_is_non_sync_sample = 0 (it IS a
// sync/random-access sample) are true of every AC-3/E-AC-3 access unit
// ac3::io::scan groups into the one opaque frame mp4:: ever sees (the
// independent substream plus any dependents - see mp4.hpp's own header
// comment) - so every sample is both independently decodable and a valid
// fragment/segment start point.
Bytes build_trex(std::uint32_t default_sample_duration) {
    Bytes body;
    put_u32(body, 1);  // track_ID
    put_u32(body, 1);  // default_sample_description_index
    put_u32(body, default_sample_duration);
    put_u32(body, 0);           // default_sample_size: none, see above
    put_u32(body, 0x02000000);  // default_sample_flags, see above
    Bytes out;
    put_fullbox(out, "trex", 0, 0, body);
    return out;
}

Bytes build_mvex(std::uint32_t default_sample_duration) {
    Bytes out;
    put_box(out, "mvex", build_trex(default_sample_duration));
    return out;
}

// The initialization segment: ftyp + moov, where moov's one trak has an
// EMPTY sample table (this trak describes zero samples - every sample lives
// in a later moof/trun instead, ISO/IEC 14496-12 §8.8.3) and mvex/trex
// supply the per-fragment defaults. Otherwise identical in shape to
// mp4.cpp's build_moov: same mvhd/tkhd/mdhd/hdlr/smhd/dinf/stsd, from the
// same shared builders.
Bytes build_init_segment(const AudioTrack& track, const FragmentOptions& options,
                         std::uint64_t total_samples) {
    Bytes stbl_body;
    put_bytes(stbl_body, detail::build_stsd(track));
    put_bytes(stbl_body, detail::build_stts(0, 0));
    put_bytes(stbl_body, detail::build_stsc(0));
    put_bytes(stbl_body, detail::build_stsz({}));
    put_bytes(stbl_body, detail::build_stco({}));
    Bytes stbl;
    put_box(stbl, "stbl", stbl_body);

    Bytes minf_body;
    put_bytes(minf_body, detail::build_smhd());
    put_bytes(minf_body, detail::build_dinf());
    put_bytes(minf_body, stbl);
    Bytes minf;
    put_box(minf, "minf", minf_body);

    Bytes mdia_body;
    put_bytes(mdia_body, detail::build_mdhd(track.sample_rate, total_samples, track.language));
    put_bytes(mdia_body, detail::build_hdlr(options.writing_app));
    put_bytes(mdia_body, minf);
    Bytes mdia;
    put_box(mdia, "mdia", mdia_body);

    Bytes trak_body;
    put_bytes(trak_body, detail::build_tkhd(total_samples));
    put_bytes(trak_body, mdia);
    Bytes trak;
    put_box(trak, "trak", trak_body);

    // A batch API knows the whole track's duration up front (every frame is
    // already in hand - see mp4.hpp's own comment on fragment() being
    // batch), so mvhd/tkhd/mdhd above carry the REAL total, the same
    // convention mux() uses. FragmentWriter passes 0 here instead - the live
    // session that cannot know its own total - which is the one and only
    // difference between the two init segments (see FragmentWriter's own
    // comment in mp4.hpp).
    Bytes moov_body;
    put_bytes(moov_body, detail::build_mvhd(track.sample_rate, total_samples));
    put_bytes(moov_body, trak);
    put_bytes(moov_body, build_mvex(track.samples_per_frame));
    Bytes moov;
    put_box(moov, "moov", moov_body);

    Bytes out;
    put_bytes(out, build_init_ftyp(options));
    put_bytes(out, moov);
    return out;
}

// ISO/IEC 14496-12 §8.8.7's Track Fragment Header Box. flags 0x020000 is
// default-base-is-moof: trun's data_offset below is measured from THIS
// moof's own start, rather than the deprecated "first byte of the enclosing
// file" default - which for a media segment served on its own (no preceding
// bytes at all) cannot mean anything sensible anyway. CMAF (ISO/IEC
// 23000-19) requires default-base-is-moof for exactly this reason. No other
// optional field is set: sample-description-index/duration/size/flags all
// fall back to trex's defaults (build_trex above).
Bytes build_tfhd() {
    Bytes body;
    put_u32(body, 1);  // track_ID
    Bytes out;
    put_fullbox(out, "tfhd", 0, 0x020000, body);
    return out;
}

// ISO/IEC 14496-12 §8.8.12's Track Fragment Base Media Decode Time Box:
// version 1 for a 64-bit baseMediaDecodeTime, so a track can run indefinitely
// without wrapping. Lets a player/packager seek to any fragment and know its
// absolute position in the track's own timescale without having decoded
// every prior fragment first - the entire reason tfdt exists, and why CMAF
// makes it mandatory in every fragment.
Bytes build_tfdt(std::uint64_t base_media_decode_time) {
    Bytes body;
    put_u64(body, base_media_decode_time);
    Bytes out;
    put_fullbox(out, "tfdt", 1, 0, body);
    return out;
}

// ISO/IEC 14496-12 §8.8.8's Track Run Box. flags 0x000201 is
// data-offset-present (0x000001) | sample-size-present (0x000200): every
// sample's duration and flags come from trex's defaults (build_trex above -
// true for every access unit alike), but sizes do not (frames vary), so
// sizes alone are listed explicitly per sample.
Bytes build_trun(std::span<const std::span<const std::byte>> frames, std::int32_t data_offset) {
    Bytes body;
    put_u32(body, static_cast<std::uint32_t>(frames.size()));
    // data_offset is a SIGNED field (§8.8.8.1); the cast below preserves its
    // bit pattern exactly the way put_u32 already treats every other field
    // here as "32 bits to write", not "an unsigned quantity".
    put_u32(body, static_cast<std::uint32_t>(data_offset));
    for (const auto& frame : frames) {
        put_u32(body, static_cast<std::uint32_t>(frame.size()));
    }
    Bytes out;
    put_fullbox(out, "trun", 0, 0x000201, body);
    return out;
}

// One fragment's moof: mfhd + traf(tfhd + tfdt + trun). Returns kFileTooLarge
// if this fragment's own data_offset would not fit trun's signed 32-bit
// field - astronomically unlikely for an audio fragment, but a fragmenter
// silently truncating a bad value would be worse than refusing.
std::expected<Bytes, MuxError> build_moof(std::uint32_t sequence_number,
                                          std::uint64_t base_media_decode_time,
                                          std::span<const std::span<const std::byte>> frames) {
    Bytes mfhd_body;
    put_u32(mfhd_body, sequence_number);
    Bytes mfhd;
    put_fullbox(mfhd, "mfhd", 0, 0, mfhd_body);

    const Bytes tfhd = build_tfhd();
    const Bytes tfdt = build_tfdt(base_media_decode_time);

    // trun's data_offset counts from the START OF THIS MOOF (default-base-is
    // -moof, set in tfhd above) to the first sample byte in the mdat that
    // follows - moof's own total size plus mdat's 8-byte header. moof's size
    // does not depend on data_offset's VALUE (it is a fixed 4-byte field
    // either way), so this is the same two-pass shape mux() uses for stco:
    // build once with a placeholder purely to measure, then again for real,
    // rather than patching already-serialized bytes in place.
    const Bytes trun_measured = build_trun(frames, 0);
    Bytes traf_body_measured;
    put_bytes(traf_body_measured, tfhd);
    put_bytes(traf_body_measured, tfdt);
    put_bytes(traf_body_measured, trun_measured);
    Bytes traf_measured;
    put_box(traf_measured, "traf", traf_body_measured);
    Bytes moof_body_measured;
    put_bytes(moof_body_measured, mfhd);
    put_bytes(moof_body_measured, traf_measured);
    Bytes moof_measured;
    put_box(moof_measured, "moof", moof_body_measured);

    constexpr std::int64_t kMdatHeaderBytes = 8;
    const std::int64_t data_offset =
        static_cast<std::int64_t>(moof_measured.size()) + kMdatHeaderBytes;
    if (data_offset > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
        return std::unexpected(MuxError::kFileTooLarge);
    }

    const Bytes trun = build_trun(frames, static_cast<std::int32_t>(data_offset));
    Bytes traf_body;
    put_bytes(traf_body, tfhd);
    put_bytes(traf_body, tfdt);
    put_bytes(traf_body, trun);
    Bytes traf;
    put_box(traf, "traf", traf_body);
    Bytes moof_body;
    put_bytes(moof_body, mfhd);
    put_bytes(moof_body, traf);
    Bytes moof;
    put_box(moof, "moof", moof_body);
    // Same box structure as moof_measured above, data_offset VALUE aside -
    // see the comment on the two-pass build above.
    assert(moof.size() == moof_measured.size());
    return moof;
}

// One complete media segment: styp + moof + mdat, plus the bookkeeping
// mp4/hls.hpp and mp4/dash.hpp need about it. Shared by fragment() and
// FragmentWriter - which is exactly why the two produce byte-identical media
// segments for the same inputs (mp4.hpp's own contract for the writer): the
// only per-fragment state either caller carries is `sequence_number` and
// `base_media_decode_time`, and both arrive here as arguments.
std::expected<MediaSegment, MuxError> build_media_segment(
    const AudioTrack& track, const FragmentOptions& options, std::uint32_t sequence_number,
    std::uint64_t base_media_decode_time, std::span<const std::span<const std::byte>> frames) {
    auto moof = build_moof(sequence_number, base_media_decode_time, frames);
    if (!moof) {
        return std::unexpected(moof.error());
    }

    std::uint64_t mdat_body_bytes = 0;
    for (const auto& frame : frames) {
        mdat_body_bytes += frame.size();
    }
    constexpr std::uint64_t kMdatHeaderBytes = 8;
    if (kMdatHeaderBytes + mdat_body_bytes > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(MuxError::kFileTooLarge);
    }

    const Bytes styp = build_media_styp(options);

    Bytes segment_bytes;
    segment_bytes.reserve(styp.size() + moof->size() +
                          static_cast<std::size_t>(kMdatHeaderBytes + mdat_body_bytes));
    put_bytes(segment_bytes, styp);
    put_bytes(segment_bytes, *moof);
    put_u32(segment_bytes, static_cast<std::uint32_t>(kMdatHeaderBytes + mdat_body_bytes));
    put_fourcc(segment_bytes, "mdat");
    for (const auto& frame : frames) {
        put_bytes(segment_bytes, frame);
    }

    return MediaSegment{
        .bytes = std::move(segment_bytes),
        .sequence_number = sequence_number,
        .sample_count = static_cast<std::uint32_t>(frames.size()),
        .duration_samples = static_cast<std::uint64_t>(frames.size()) * track.samples_per_frame,
        .base_media_decode_time = base_media_decode_time,
    };
}

// Everything fragment() and FragmentWriter::create() both refuse, in one
// place so the writer cannot drift into accepting a track the batch form
// would have rejected (mp4.hpp promises they validate alike).
std::optional<MuxError> validate(const AudioTrack& track, const FragmentOptions& options) {
    if (track.channels <= 0 || track.sample_rate == 0 ||
        track.sample_rate > std::numeric_limits<std::uint16_t>::max() ||
        track.samples_per_frame == 0 || track.codec_config.empty() ||
        (track.codec_id != kCodecAc3 && track.codec_id != kCodecEac3)) {
        return MuxError::kInvalidTrack;
    }
    if (options.frames_per_fragment == 0) {
        return MuxError::kInvalidOptions;
    }
    return std::nullopt;
}

}  // namespace

SegmentInfo segment_info(const MediaSegment& segment) {
    return SegmentInfo{
        .sequence_number = segment.sequence_number,
        .sample_count = segment.sample_count,
        .duration_samples = segment.duration_samples,
        .base_media_decode_time = segment.base_media_decode_time,
        .byte_size = segment.bytes.size(),
    };
}

std::expected<FragmentedOutput, MuxError> fragment(
    const AudioTrack& track, std::span<const std::span<const std::byte>> frames,
    const FragmentOptions& options) {
    if (frames.empty()) {
        return std::unexpected(MuxError::kNoFrames);
    }
    if (const auto invalid = validate(track, options)) {
        return std::unexpected(*invalid);
    }

    const std::uint64_t total_samples =
        static_cast<std::uint64_t>(frames.size()) * track.samples_per_frame;
    if (total_samples > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(MuxError::kFileTooLarge);
    }

    FragmentedOutput out;
    out.init_segment = build_init_segment(track, options, total_samples);

    const auto step = static_cast<std::size_t>(options.frames_per_fragment);
    std::uint64_t samples_emitted = 0;
    std::uint32_t sequence_number = 1;
    for (std::size_t start = 0; start < frames.size(); start += step) {
        const std::size_t count = std::min(step, frames.size() - start);
        auto segment = build_media_segment(track, options, sequence_number, samples_emitted,
                                           frames.subspan(start, count));
        if (!segment) {
            return std::unexpected(segment.error());
        }
        samples_emitted += segment->duration_samples;
        ++sequence_number;
        out.media_segments.push_back(std::move(*segment));
    }

    return out;
}

std::expected<FragmentedOutput, MuxError> fragment(
    const AudioTrack& track, std::span<const std::vector<std::byte>> frames,
    const FragmentOptions& options) {
    const std::vector<std::span<const std::byte>> views(frames.begin(), frames.end());
    return fragment(track, views, options);
}

FragmentWriter::FragmentWriter(AudioTrack track, FragmentOptions options,
                               std::vector<std::byte> init_segment)
    : track_(std::move(track)),
      options_(std::move(options)),
      init_segment_(std::move(init_segment)) {}

std::expected<FragmentWriter, MuxError> FragmentWriter::create(const AudioTrack& track,
                                                               const FragmentOptions& options) {
    if (const auto invalid = validate(track, options)) {
        return std::unexpected(*invalid);
    }
    // 0 = "duration unknown", the live session's honest answer where
    // fragment() writes the real total - see build_init_segment's own comment
    // and mp4.hpp's on this class.
    return FragmentWriter{track, options, build_init_segment(track, options, 0)};
}

std::expected<MediaSegment, MuxError> FragmentWriter::close_fragment() {
    const std::vector<std::span<const std::byte>> views(pending_.begin(), pending_.end());
    auto segment = build_media_segment(track_, options_, sequence_number_, decode_time_, views);
    if (!segment) {
        return std::unexpected(segment.error());
    }
    pending_.clear();
    decode_time_ += segment->duration_samples;
    ++sequence_number_;

    window_.push_back(segment_info(*segment));
    if (options_.playlist_window_segments != 0 &&
        window_.size() > options_.playlist_window_segments) {
        window_.erase(window_.begin(),
                      window_.begin() + static_cast<std::ptrdiff_t>(
                                            window_.size() - options_.playlist_window_segments));
    }
    return segment;
}

std::expected<std::optional<MediaSegment>, MuxError> FragmentWriter::push(
    std::span<const std::byte> frame) {
    // Copied, not viewed: a fragment's frames are held until the fragment
    // closes, and a live caller reuses its encode buffer on the very next
    // call. Bounded at options_.frames_per_fragment frames whatever the
    // session's length, which is the whole point of this class over
    // fragment().
    pending_.emplace_back(frame.begin(), frame.end());
    ++frames_written_;
    if (pending_.size() < options_.frames_per_fragment) {
        return std::optional<MediaSegment>{};
    }
    auto segment = close_fragment();
    if (!segment) {
        return std::unexpected(segment.error());
    }
    return std::optional<MediaSegment>{std::move(*segment)};
}

std::expected<std::optional<MediaSegment>, MuxError> FragmentWriter::finalize() {
    if (pending_.empty()) {
        return std::optional<MediaSegment>{};
    }
    auto segment = close_fragment();
    if (!segment) {
        return std::unexpected(segment.error());
    }
    return std::optional<MediaSegment>{std::move(*segment)};
}

}  // namespace mp4
