#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mp4/export.hpp"

// The read side of mp4::mux()/mp4::fragment(): pulling one audio track's
// samples back out of an MP4, plain or fragmented.
//
// A container reader and nothing more, in the sense mp4/mp4.hpp's writer is a
// container writer and nothing more: it walks ISOBMFF boxes, finds the
// 'ac-3'/'ec-3' track, and hands each sample back as opaque bytes. It links
// nothing from ac3::forge. The one place MP4 forces a codec-shaped decision on
// this module is the same place the writer already had one - the sample
// entry's dac3/dec3 configuration box - and CodecConfig below is where that
// lands: the read twin of ac3::io::build_codec_config_box (ac3/io/dec3.hpp),
// parsed here because the box is an ISOBMFF structure the walk is already
// standing on, and reported as plain numbers for a caller that knows what
// they mean.
//
// Two shapes, mirroring the write side, over one box walker:
//
//   demux()  - batch, and ZERO-COPY: samples come back as spans into the
//              caller's buffer. Reads a file whose moov sits either side of
//              mdat, since it can reach any offset.
//   Reader   - incremental, for a file too big to hold: push() chunks in,
//              samples come back through a callback. Needs moov BEFORE mdat
//              (the "faststart" layout mux() and fragment() both write); a
//              moov-last file is kMoovAfterMdat, because locating a sample
//              means seeking backwards and a stream cannot.
//
// UNTRUSTED INPUT. Every box length, chunk offset and sample size in an MP4
// is self-declared, and the sample table is an INDEX - a hostile stsc can
// point a chunk past the end of the file, an stsz can claim four billion
// samples, and boxes can nest until a recursive walker's stack gives out.
// ReadOptions bounds each of those, the walk is iterative, and every sample
// range is checked against the data actually present rather than trusted.
// fuzz/fuzz_mp4_demux.cpp drives the walker with arbitrary bytes.

namespace mp4 {

namespace detail {
// Reader's parse state, defined in src/mp4/src/reader.cpp - a namespace-scope
// type rather than a private nested one so the walker's own free functions
// there can name it.
struct ReaderState;
}  // namespace detail

enum class DemuxError : std::uint8_t {
    kNotIsobmff,       // no box structure worth calling ISOBMFF
    kTruncated,        // the input ends before the track or its samples
    kMalformed,        // a box, sample table or fragment layout that cannot be parsed
    kNoAudioTrack,     // no 'ac-3'/'ec-3' track (see ReadOptions::track_id)
    kLimitExceeded,    // a box size, sample count or nesting depth beyond ReadOptions
    kMoovAfterMdat,    // Reader only: the sample table follows the data it indexes
};

[[nodiscard]] MP4_EXPORT std::string_view describe(DemuxError error);

// The parsed dac3/dec3 sample-entry configuration box - the read twin of
// ac3::io::build_codec_config_box, whose own comments carry the field
// derivations and the primary sources (ETSI TS 102 366 Annex F §F.4/§F.6,
// TS 103 420 §8.3.1/§8.3.2.2 for the Atmos extension).
//
// Reported as the raw syntax values, not as ac3:: enums: this module has no
// dependency on the codec library and no business deciding what fscod 0
// means. `payload` keeps the bytes verbatim so a caller remuxing into
// another container can hand them straight back without this struct having
// to round-trip losslessly.
struct CodecConfig {
    bool eac3 = false;  // dec3 (true) or dac3 (false)
    int fscod = 0;
    int bsid = 0;
    int bsmod = 0;
    int acmod = 0;
    bool lfeon = false;
    // dac3 only: Table 5.18's index into the fixed bit-rate table.
    int bit_rate_code = 0;
    // dec3 only.
    int data_rate_kbps = 0;
    int num_ind_sub = 0;  // as stored: one LESS than the substream count
    int num_dep_sub = 0;
    int chan_loc = 0;
    bool asvc = false;
    // TS 103 420's flag_ec3_extension_type_a / complexity_index_type_a - the
    // Atmos/JOC marker, and the one FFmpeg's E-AC-3 remux path is known to
    // drop. std::nullopt when the flag was clear or the box too short to
    // carry it (it is a trailing extension, and plenty of real dec3 boxes
    // stop before it).
    std::optional<int> oba_complexity_index;
    std::vector<std::byte> payload;
};

// What the container declares about the track - the read-side twin of
// AudioTrack.
struct ReadTrack {
    std::uint32_t track_id = 0;
    std::string codec_id;  // kCodecAc3 or kCodecEac3
    // The sample entry's own 16.16 samplerate field, integer part.
    std::uint32_t sample_rate = 0;
    int channels = 0;
    // mdhd's timescale, which for an audio track is normally the sample rate
    // but is not required to be - reported separately rather than conflated.
    std::uint32_t timescale = 0;
    std::string language{"und"};
    CodecConfig codec_config;
};

// Bounds on what the reader will do for one file - defences against a
// hostile container, not tuning knobs.
struct ReadOptions {
    // The track to extract. 0 auto-selects the first 'ac-3'/'ec-3' track;
    // any other value names a track_ID explicitly.
    std::uint32_t track_id = 0;
    // The largest single box the reader will hold. A box it does not need is
    // skipped without ever being buffered, however large.
    std::uint64_t max_box_bytes = 64U << 20;  // 64 MiB
    // Samples one track may declare. An MP4's sample table is an O(duration)
    // index by construction - that is what a sample table IS - so this
    // bounds the index, not the audio: 4 million access units is about 35
    // hours at 1536 samples and 48 kHz.
    std::uint32_t max_samples = 4'000'000;
    // Entries stsc and stco/co64 may declare, bounded separately because a
    // hostile file can inflate them independently of the sample count.
    std::uint32_t max_chunks = 4'000'000;
    // How deep boxes may nest. The walk is iterative, so this bounds a
    // vector rather than the call stack. ISOBMFF's own deepest path here is
    // moov > trak > mdia > minf > stbl > stsd > sample entry.
    std::uint32_t max_depth = 16;
};

struct Demuxed {
    ReadTrack track;
    // Views into the buffer passed to demux(), which must outlive this. In
    // decode order, which for audio is also presentation order.
    std::vector<std::span<const std::byte>> samples;
};

// Reads a complete MP4 held in one buffer. Samples come back as views into
// `file` - no audio is copied.
//
// Reads both layouts the writer produces and both a real muxer does: a plain
// moov/mdat file (sample table walked through stsc/stsz/stco or co64) and a
// fragmented one (an init segment's mvex/trex defaults plus every
// moof/traf/tfhd/trun that follows). moov may sit either side of mdat.
[[nodiscard]] MP4_EXPORT std::expected<Demuxed, DemuxError> demux(
    std::span<const std::byte> file, const ReadOptions& options = {});

// Incrementally reads samples out of an MP4 as its bytes arrive, for a file
// too big to hold. Samples are delivered to a callback; the span handed to
// it is valid for that call only.
//
// Requires moov before mdat - the layout mux() and fragment() both write,
// and the one a "faststart"/web-optimised file has. A moov-last file reports
// kMoovAfterMdat from finish() rather than pretending: the sample table is
// the only thing that says where a sample begins, and a stream cannot go
// back for it. Use demux() for those.
//
// Move-only: the parse state lives behind a pointer.
class MP4_EXPORT Reader {
   public:
    using SampleFn = std::function<void(std::span<const std::byte>)>;

    explicit Reader(const ReadOptions& options = {});
    Reader(Reader&&) noexcept;
    Reader& operator=(Reader&&) noexcept;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader();

    // Feeds the next chunk of the file. Chunk boundaries are arbitrary.
    [[nodiscard]] std::expected<void, DemuxError> push(std::span<const std::byte> chunk,
                                                       const SampleFn& on_sample);

    // Call once, when the input ends: the verdict on the file as a whole.
    // Takes no callback - every ISOBMFF box declares its own length, so
    // push() has already emitted everything that was ever going to be
    // emitted.
    [[nodiscard]] std::expected<void, DemuxError> finish();

    [[nodiscard]] const ReadTrack& track() const;
    [[nodiscard]] bool track_found() const;
    [[nodiscard]] std::size_t samples_read() const;

   private:
    std::unique_ptr<detail::ReaderState> state_;
};

}  // namespace mp4
