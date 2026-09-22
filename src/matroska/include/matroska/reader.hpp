#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "matroska/export.hpp"

// The read side of matroska::mux()/matroska::Writer: pulling one audio
// track's frames back out of a Matroska file.
//
// This is a container reader and nothing more, in exactly the sense the
// writer beside it is a container writer and nothing more (see
// matroska/matroska.hpp): it walks EBML, finds the track the caller asked
// for, and hands each frame back as opaque bytes. It has NO dependency on
// ac3::forge and no knowledge of AC-3 - what comes out is whatever the muxer
// put in. The one place it names a codec is track auto-selection
// (kCodecAc3/kCodecEac3, the same two CodecID strings the writer already
// declares); a caller that wants some other track says so by number.
//
// Two shapes, mirroring the write side exactly:
//
//   demux()  - batch, and ZERO-COPY: the frames it returns are spans into
//              the caller's own buffer, the way ac3::io::scan already hands
//              back access units. For a caller that has the file resident
//              anyway.
//   Reader   - incremental, for a file too big to hold: push() chunks in,
//              frames come back through a callback. Peak memory is one
//              frame plus one chunk, never the file - the same bound
//              matroska::Writer offers on the way out.
//
// Both run the same EBML walker (src/matroska/src/reader.cpp); the only
// difference is where the bytes come from and where the frames point.
//
// UNTRUSTED INPUT. A container arrives from a disc rip, a broadcast capture
// or an HTTP download - not from this project's own writer. Every length in
// an EBML file is self-declared, so a malformed or hostile one can claim an
// element is gigabytes long, nest masters until a recursive walker runs out
// of stack, or lace a block into hundreds of frames whose declared sizes
// overrun it. ReadOptions below bounds the first two, split_block() rejects
// the third, and fuzz/fuzz_matroska_demux.cpp drives the whole walker with
// arbitrary bytes.

namespace matroska {

namespace detail {
// Reader's parse state, defined in src/matroska/src/reader.cpp. A
// namespace-scope type rather than a nested one so the walker's own
// free functions there can name it - a private nested struct would be
// unreachable from them.
struct ReaderState;
}  // namespace detail

enum class DemuxError : std::uint8_t {
    kNotMatroska,    // no EBML header where one has to be
    kTruncated,      // the input ends before a track was ever described
    kMalformed,      // a vint, element or block layout that cannot be parsed
    kNoAudioTrack,   // Tracks held no track this reader could use
    kLimitExceeded,  // an element size or nesting depth beyond ReadOptions
};

[[nodiscard]] MATROSKA_EXPORT std::string_view describe(DemuxError error);

// What the container itself declares about the track - the read-side twin of
// AudioTrack, minus the fields Matroska has nowhere to put
// (samples_per_frame is the muxer's own input, recoverable only from the
// bitstream).
struct ReadTrack {
    std::uint64_t track_number = 0;
    std::string codec_id;
    // Matroska defines both of these as absent-with-a-default (8000 Hz, one
    // channel), and that default is what a file omitting them MEANS - not
    // "unknown", so it is what this reports.
    std::uint32_t sample_rate = 8000;
    int channels = 1;
    std::string language{"und"};
};

// Bounds on what the reader will do for one file - defences against a
// hostile container rather than tuning knobs (see the header comment). The
// defaults sit far above anything a real AC-3/E-AC-3 file reaches (the
// largest legal access unit is under 64 KiB) and far below anything that
// would exhaust a machine.
struct ReadOptions {
    // The track to extract. 0 auto-selects: the first audio TrackEntry whose
    // CodecID is kCodecEac3 or kCodecAc3. Any other value names a
    // TrackNumber explicitly and accepts whatever CodecID it carries, which
    // is how a caller reads a track this module has no opinion about.
    std::uint64_t track_number = 0;
    // The largest single element the reader will hold. Elements bigger than
    // this that it does not need (an attachment, a cue index) are skipped
    // without ever being buffered; one it does need is kLimitExceeded.
    std::uint64_t max_element_bytes = 16U << 20;  // 16 MiB
    // Frames one laced block may carry. Lacing packs several frames into one
    // block and the count field's own ceiling is 256; a file claiming that
    // many is not rejected for it, but nothing beyond it is entertained.
    std::uint32_t max_frames_per_block = 256;
    // TrackEntry elements read before the reader stops believing the file.
    std::uint32_t max_tracks = 128;
    // How deep master elements may nest. The walker is iterative, so this
    // bounds a vector rather than the call stack, but an unbounded one is
    // still a memory claim an input should not get to make. Matroska's own
    // deepest path here is Segment > Cluster > BlockGroup.
    std::uint32_t max_depth = 16;
};

struct Demuxed {
    ReadTrack track;
    // Views into the buffer passed to demux(), which must outlive this.
    std::vector<std::span<const std::byte>> frames;
};

// Reads a complete Matroska file held in one buffer. Frames come back as
// views into `file` - no audio is copied.
//
// A file whose Segment uses EBML's "unknown size" (what matroska::Writer
// emits for a live recording) reads back exactly like a sized one, and so
// does one truncated mid-cluster: every whole frame before the cut is
// returned rather than an error, because a truncated capture is the normal
// way a live recording ends. A cut BEFORE the track is described has nothing
// to return and is kTruncated.
[[nodiscard]] MATROSKA_EXPORT std::expected<Demuxed, DemuxError> demux(
    std::span<const std::byte> file, const ReadOptions& options = {});

// Incrementally reads frames out of a Matroska file as its bytes arrive -
// matroska::Writer's mirror image, and the shape `ac3cli demux` uses so a
// multi-gigabyte rip never lands in memory.
//
// Frames are delivered to a callback rather than returned, so nothing
// accumulates: the span handed to it is valid for the duration of that call
// only (it points into the reader's own buffer, which the next push()
// reuses). A caller that wants to keep a frame copies it there.
//
// Constructed directly rather than through a validating create() like
// Writer's, because unlike a track there is nothing here to reject: every
// ReadOptions value, including zero, describes a coherent (if strict)
// limit, and a file that trips one reports kLimitExceeded when it does.
//
// Move-only: the parse state lives behind a pointer (a partially-parsed
// container is not a value worth copying).
class MATROSKA_EXPORT Reader {
   public:
    using FrameFn = std::function<void(std::span<const std::byte>)>;

    explicit Reader(const ReadOptions& options = {});
    Reader(Reader&&) noexcept;
    Reader& operator=(Reader&&) noexcept;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader();

    // Feeds the next chunk of the file. Chunk boundaries are arbitrary - an
    // element may span any number of them.
    [[nodiscard]] std::expected<void, DemuxError> push(std::span<const std::byte> chunk,
                                                       const FrameFn& on_frame);

    // Call once, when the input ends: the verdict on the file as a whole.
    // Reports kTruncated only if it stopped before a track was ever
    // described; a cut mid-cluster is not an error (see demux() above).
    //
    // Takes no callback and emits no frames, unlike the finish() of a
    // container whose last packet can be terminated by end-of-input alone:
    // every Matroska element declares its own length, so push() has already
    // parsed everything that was ever going to parse, and whatever is still
    // buffered is the head of an element whose rest never arrived.
    [[nodiscard]] std::expected<void, DemuxError> finish();

    // What the container declared, once Tracks has been read - track_found()
    // says whether it has been.
    [[nodiscard]] const ReadTrack& track() const;
    [[nodiscard]] bool track_found() const;
    [[nodiscard]] std::size_t frames_read() const;

   private:
    std::unique_ptr<detail::ReaderState> state_;
};

}  // namespace matroska
