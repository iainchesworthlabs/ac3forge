#include "matroska/reader.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ebml_detail.hpp"
#include "matroska/matroska.hpp"

namespace matroska {

namespace {

using namespace detail;

// --- EBML primitives, the read side of matroska.cpp's put_* ----------------
//
// Each reports "not enough bytes yet" separately from "these bytes are
// wrong", because the incremental Reader has to tell the two apart: the
// first means wait for the next chunk, the second is an error no amount of
// further input fixes.

enum class Need : std::uint8_t { kOk, kMore, kBad };

struct Cursor {
    std::span<const std::byte> data;
    std::size_t pos = 0;

    [[nodiscard]] std::size_t left() const { return data.size() - pos; }
    // Callers check left() first; every use below is inside such a check.
    [[nodiscard]] std::uint8_t at(std::size_t i) const {
        return std::to_integer<std::uint8_t>(data[pos + i]);
    }
};

// An id keeps its own length marker (matroska.cpp's put_id writes it that
// way), so it is read whole and compared against ebml_detail.hpp's constants
// verbatim. A leading 0x00 has no marker bit within the four bytes
// EBMLMaxIDLength allows, so it is not an id at all.
Need read_id(Cursor& c, std::uint32_t& out) {
    if (c.left() < 1) {
        return Need::kMore;
    }
    const std::uint8_t first = c.at(0);
    if (first == 0) {
        return Need::kBad;
    }
    int width = 1;
    for (int i = 0; i < 4; ++i) {
        if ((first & (0x80U >> i)) != 0) {
            width = i + 1;
            break;
        }
    }
    if (c.left() < static_cast<std::size_t>(width)) {
        return Need::kMore;
    }
    std::uint32_t value = first;
    for (int i = 1; i < width; ++i) {
        value = (value << 8) | c.at(static_cast<std::size_t>(i));
    }
    c.pos += static_cast<std::size_t>(width);
    out = value;
    return Need::kOk;
}

// A size drops its marker bit and keeps only the value beneath it - and at
// EVERY width an all-ones value is EBML's reserved "unknown size" rather
// than a length (ebml_detail.hpp's unknown_size_at). The writer only ever
// emits the 8-byte form; a file from anywhere else may use any width, so
// each is checked against its own pattern rather than against the widest.
Need read_size(Cursor& c, std::uint64_t& out, bool& unknown) {
    if (c.left() < 1) {
        return Need::kMore;
    }
    const std::uint8_t first = c.at(0);
    if (first == 0) {
        // No marker bit in the first byte means a width past the 8 bytes
        // EBMLMaxSizeLength allows.
        return Need::kBad;
    }
    int width = 1;
    for (int i = 0; i < 8; ++i) {
        if ((first & (0x80U >> i)) != 0) {
            width = i + 1;
            break;
        }
    }
    if (c.left() < static_cast<std::size_t>(width)) {
        return Need::kMore;
    }
    std::uint64_t value = first & (0xFFU >> width);
    for (int i = 1; i < width; ++i) {
        value = (value << 8) | c.at(static_cast<std::size_t>(i));
    }
    c.pos += static_cast<std::size_t>(width);
    unknown = value == unknown_size_at(width);
    out = value;
    return Need::kOk;
}

// A block's track number is a plain vint: the same encoding as a size, with
// no reserved pattern to honour.
Need read_vint(Cursor& c, std::uint64_t& out) {
    bool unknown = false;
    return read_size(c, out, unknown);
}

// EBML's "uint": big-endian, in however many bytes the element declared.
[[nodiscard]] bool read_uint(std::span<const std::byte> body, std::uint64_t& out) {
    if (body.size() > 8) {
        return false;
    }
    std::uint64_t value = 0;
    for (const auto b : body) {
        value = (value << 8) | std::to_integer<std::uint8_t>(b);
    }
    out = value;
    return true;
}

// EBML's "float": 4 or 8 bytes, IEEE 754, big-endian. matroska.cpp's
// put_float_element writes the 8-byte form; real muxers write both.
[[nodiscard]] bool read_float(std::span<const std::byte> body, double& out) {
    if (body.size() == 4) {
        std::uint32_t bits = 0;
        for (const auto b : body) {
            bits = (bits << 8) | std::to_integer<std::uint8_t>(b);
        }
        out = static_cast<double>(std::bit_cast<float>(bits));
        return true;
    }
    if (body.size() == 8) {
        std::uint64_t bits = 0;
        for (const auto b : body) {
            bits = (bits << 8) | std::to_integer<std::uint8_t>(b);
        }
        out = std::bit_cast<double>(bits);
        return true;
    }
    return false;
}

// EBML strings are not NUL-terminated, but a writer may pad one with NULs to
// a reserved length, so trailing NULs are stripped rather than kept as part
// of the value.
[[nodiscard]] std::string read_string(std::span<const std::byte> body) {
    std::string out;
    out.reserve(body.size());
    for (const auto b : body) {
        out.push_back(static_cast<char>(std::to_integer<std::uint8_t>(b)));
    }
    while (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

// --- what the walker does with each element --------------------------------

// Master elements the walker descends INTO rather than buffering whole. This
// is what keeps peak memory at one frame: a Cluster holds a second of audio
// and a Segment holds the file, so neither is ever resident.
[[nodiscard]] bool is_descended(std::uint32_t id) {
    return id == kSegment || id == kCluster || id == kBlockGroup || id == kTracks ||
           id == kTrackEntry || id == kAudio;
}

// Leaf elements the walker needs the bytes of. Everything else is skipped by
// its declared length without being held - which is what makes a 300 MB
// attachment or a cue index free to read past.
[[nodiscard]] bool is_wanted_leaf(std::uint32_t id) {
    return id == kTrackNumber || id == kTrackType || id == kCodecId || id == kLanguage ||
           id == kSamplingFrequency || id == kChannels || id == kSimpleBlock || id == kBlock;
}

// A Segment-level id, i.e. one that cannot appear inside a Cluster. This is
// how a Cluster written with EBML's "unknown size" ends: it has no length of
// its own to run out, so it runs until something that is not one of its own
// children turns up. Another Cluster is the common case (one per second of
// audio); the rest are what a full file puts after the last one.
[[nodiscard]] bool ends_unknown_cluster(std::uint32_t id) {
    return id == kCluster || id == kTracks || id == kInfo || id == kCues || id == kSeekHead ||
           id == kAttachments || id == kChapters || id == kTags || id == kSegment ||
           id == kEbmlHeader;
}

// --- block payload ---------------------------------------------------------
//
// SimpleBlock and Block share one payload layout: a track-number vint, a
// signed 16-bit timestamp relative to the cluster, a flags byte, then the
// frame data. matroska.cpp writes exactly one unlaced frame per block; a
// file from anywhere else may lace several together, and all three lacing
// forms are in real use, so all three are read here.

enum class Lacing : std::uint8_t { kNone, kXiph, kFixed, kEbml };

// Flags bits 0x06 pick the lacing: 00 none, 01 Xiph, 11 EBML, 10 fixed-size.
[[nodiscard]] Lacing lacing_of(std::uint8_t flags) {
    switch ((flags >> 1) & 0x03U) {
        case 0:
            return Lacing::kNone;
        case 1:
            return Lacing::kXiph;
        case 3:
            return Lacing::kEbml;
        default:
            return Lacing::kFixed;
    }
}

// A signed vint, as EBML lacing uses for each size DELTA after the first:
// the same encoding as an unsigned one, biased down by half its width's
// range so the value can go negative.
[[nodiscard]] bool read_svint(Cursor& c, std::int64_t& out) {
    const std::size_t before = c.pos;
    std::uint64_t raw = 0;
    if (read_vint(c, raw) != Need::kOk) {
        return false;
    }
    const auto width = static_cast<int>(c.pos - before);
    out = static_cast<std::int64_t>(raw) - ((std::int64_t{1} << ((7 * width) - 1)) - 1);
    return true;
}

// Splits one block payload into its frames, appending each as a view into
// `payload`. False for a layout that does not add up - a lace whose declared
// sizes overrun the block is the classic malformed-container case, and the
// one an unchecked reader turns into an out-of-bounds read.
[[nodiscard]] bool split_block(std::span<const std::byte> payload, std::uint64_t want_track,
                               const ReadOptions& options,
                               std::vector<std::span<const std::byte>>& frames_out) {
    Cursor c{payload, 0};
    std::uint64_t track = 0;
    if (read_vint(c, track) != Need::kOk) {
        return false;
    }
    if (c.left() < 3) {  // timestamp(2) + flags(1)
        return false;
    }
    const std::uint8_t flags = c.at(2);
    c.pos += 3;
    if (track != want_track) {
        return true;  // another track's block: parsed, nothing to emit
    }

    const auto body = payload.subspan(c.pos);
    const Lacing lacing = lacing_of(flags);
    if (lacing == Lacing::kNone) {
        frames_out.push_back(body);
        return true;
    }

    Cursor lc{body, 0};
    if (lc.left() < 1) {
        return false;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(lc.at(0)) + 1;
    lc.pos += 1;
    if (count > options.max_frames_per_block) {
        return false;
    }

    // Sizes for the first count-1 frames; the last takes whatever is left,
    // which is what makes an overrun detectable rather than silently
    // truncating. Fixed-size lacing is the exception - it declares no sizes
    // at all and divides the remainder.
    std::vector<std::uint64_t> sizes;
    sizes.reserve(count);
    if (lacing == Lacing::kFixed) {
        const std::uint64_t rest = lc.left();
        if (rest % count != 0) {
            return false;
        }
        sizes.assign(count, rest / count);
    } else if (lacing == Lacing::kXiph) {
        // Each size is a run of 0xFF bytes plus a final byte under 0xFF.
        for (std::uint32_t i = 0; i + 1 < count; ++i) {
            std::uint64_t size = 0;
            for (;;) {
                if (lc.left() < 1) {
                    return false;
                }
                const std::uint8_t b = lc.at(0);
                lc.pos += 1;
                size += b;
                if (b != 0xFF) {
                    break;
                }
            }
            sizes.push_back(size);
        }
    } else if (count >= 2) {
        // EBML lacing: the first size outright, then a signed delta per
        // further size. A single-frame EBML lace declares no size at all -
        // there is no "first" when the only frame is also the last - so this
        // whole branch is skipped for count == 1.
        std::uint64_t first = 0;
        if (read_vint(lc, first) != Need::kOk) {
            return false;
        }
        sizes.push_back(first);
        auto previous = static_cast<std::int64_t>(first);
        for (std::uint32_t i = 1; i + 1 < count; ++i) {
            std::int64_t delta = 0;
            if (!read_svint(lc, delta)) {
                return false;
            }
            previous += delta;
            if (previous < 0) {
                return false;
            }
            sizes.push_back(static_cast<std::uint64_t>(previous));
        }
    }

    std::size_t offset = lc.pos;
    const std::size_t end = body.size();
    for (std::uint32_t i = 0; i < count; ++i) {
        // The last frame of a Xiph/EBML lace takes the remainder; every
        // other frame, and every frame of a fixed-size lace, has a size.
        const std::uint64_t size = (lacing == Lacing::kFixed || i + 1 < count)
                                       ? sizes[i]
                                       : static_cast<std::uint64_t>(end - offset);
        if (size > end - offset) {
            return false;
        }
        frames_out.push_back(body.subspan(offset, static_cast<std::size_t>(size)));
        offset += static_cast<std::size_t>(size);
    }
    // Fixed-size lacing divides the remainder exactly and the other two give
    // the last frame whatever is left, so anything over means the declared
    // sizes disagreed with the block - malformed, not merely padded.
    return offset == end;
}

// --- track selection -------------------------------------------------------

// Auto-selection accepts the two CodecID strings the writer declares. This
// is the only codec-aware line in the module, and a caller naming a
// TrackNumber explicitly bypasses it entirely (see ReadOptions).
[[nodiscard]] bool is_selectable_codec(std::string_view codec_id) {
    return codec_id == kCodecEac3 || codec_id == kCodecAc3;
}

}  // namespace

std::string_view describe(DemuxError error) {
    switch (error) {
        case DemuxError::kNotMatroska:
            return "not a Matroska file: no EBML header where one has to be";
        case DemuxError::kTruncated:
            return "input ends before any track was described";
        case DemuxError::kMalformed:
            return "malformed element, vint or block layout";
        case DemuxError::kNoAudioTrack:
            return "no A_AC3/A_EAC3 audio track (or the requested track number is absent)";
        case DemuxError::kLimitExceeded:
            return "element size or nesting depth beyond the reader's limits";
    }
    return "unknown error";
}

// Everything the walk carries between calls. demux() keeps one on the stack
// and runs the walk once; Reader owns one across push()es. One struct, so
// both drive the identical code - the walker itself has no idea which caller
// it is serving.
namespace detail {

struct ReaderState {
    ReadOptions options;

    // One open master element the walker has descended into.
    struct Open {
        std::uint32_t id = 0;
        std::uint64_t remaining = 0;  // meaningless when unknown_size
        bool unknown_size = false;
    };
    std::vector<Open> open;

    // The TrackEntry being read. Only committed when the entry closes: its
    // CodecID may arrive after its Channels, so nothing can be decided until
    // the entry is whole.
    ReadTrack pending;
    bool pending_is_audio = false;
    bool pending_has_number = false;
    bool in_track_entry = false;
    std::uint32_t track_entries_seen = 0;

    ReadTrack track;
    bool track_found = false;
    // Set once a Tracks element has closed: no later one may retarget a
    // selection already made.
    bool tracks_seen = false;
    bool ebml_header_seen = false;

    // Bytes of an element being skipped wholesale that have not arrived yet.
    std::uint64_t skipping = 0;
    std::size_t frames_read = 0;

    // Reused across blocks so a per-frame vector is not reallocated per
    // block; only ever holds one block's frames.
    std::vector<std::span<const std::byte>> block_frames;

    // Incremental input only: bytes pushed but not yet parsed. demux()
    // leaves this empty and walks the caller's buffer directly, which is
    // what makes it zero-copy.
    std::vector<std::byte> buffer;
};

}  // namespace detail

namespace {

using detail::ReaderState;

// Subtracts `n` from every open element's remaining count. An unknown-size
// element never counts down - it has no length to spend.
void consume_open(ReaderState& s, std::uint64_t n) {
    for (auto& open : s.open) {
        if (open.unknown_size) {
            continue;
        }
        open.remaining = n >= open.remaining ? 0 : open.remaining - n;
    }
}

void close_finished(ReaderState& s) {
    while (!s.open.empty()) {
        auto& top = s.open.back();
        if (top.unknown_size || top.remaining != 0) {
            return;
        }
        if (top.id == kTrackEntry) {
            if (s.in_track_entry && !s.track_found && s.pending_has_number && s.pending_is_audio) {
                const bool wanted = s.options.track_number != 0
                                        ? s.pending.track_number == s.options.track_number
                                        : is_selectable_codec(s.pending.codec_id);
                if (wanted) {
                    s.track = s.pending;
                    s.track_found = true;
                }
            }
            s.in_track_entry = false;
        } else if (top.id == kTracks) {
            s.tracks_seen = true;
        }
        s.open.pop_back();
    }
}

// Parses as much of `data` as it can, returning how many bytes it consumed;
// whatever is left is the head of an element whose bytes have not all
// arrived. Frames are handed to `on_frame` as views into `data`.
std::expected<std::size_t, DemuxError> walk(ReaderState& s, std::span<const std::byte> data,
                                            const Reader::FrameFn& on_frame) {
    Cursor c{data, 0};
    std::size_t committed = 0;  // bytes the caller may drop

    for (;;) {
        // A skip in progress swallows whatever of it is here.
        if (s.skipping > 0) {
            const auto available = static_cast<std::uint64_t>(c.left());
            const std::uint64_t take = available < s.skipping ? available : s.skipping;
            c.pos += static_cast<std::size_t>(take);
            s.skipping -= take;
            consume_open(s, take);
            close_finished(s);
            committed = c.pos;
            if (s.skipping > 0) {
                break;  // need more input
            }
            continue;
        }

        const std::size_t element_start = c.pos;
        std::uint32_t id = 0;
        const Need id_need = read_id(c, id);
        if (id_need == Need::kMore) {
            break;
        }
        if (id_need == Need::kBad) {
            return std::unexpected(DemuxError::kMalformed);
        }

        std::uint64_t size = 0;
        bool unknown_size = false;
        const Need size_need = read_size(c, size, unknown_size);
        if (size_need == Need::kMore) {
            c.pos = element_start;
            break;
        }
        if (size_need == Need::kBad) {
            return std::unexpected(DemuxError::kMalformed);
        }

        // An unknown-size Cluster ends where a sibling begins, since it has
        // no length to run out. Close it before this id is treated as one of
        // its children. Done after the size parses so a chunk boundary
        // inside the header cannot close it and then fail to make progress.
        while (!s.open.empty() && s.open.back().unknown_size &&
               s.open.back().id == kCluster && ends_unknown_cluster(id)) {
            s.open.pop_back();
        }

        const auto header_bytes = static_cast<std::uint64_t>(c.pos - element_start);

        if (id == kEbmlHeader) {
            s.ebml_header_seen = true;
        } else if (!s.ebml_header_seen) {
            // Nothing before the EBML header is Matroska. Saying so here
            // rather than scanning ahead for a sync pattern is deliberate:
            // this reader identifies a file, it does not recover one.
            return std::unexpected(DemuxError::kNotMatroska);
        }

        // Only Segment and Cluster are ever written open-ended in practice,
        // and a reader that accepted the form anywhere could not tell where
        // any element ended.
        if (unknown_size && id != kSegment && id != kCluster) {
            return std::unexpected(DemuxError::kMalformed);
        }

        if (is_descended(id)) {
            if (s.open.size() >= s.options.max_depth) {
                return std::unexpected(DemuxError::kLimitExceeded);
            }
            // A second Tracks element must not retarget a selection already
            // made; skip it whole instead.
            if (id == kTracks && s.tracks_seen) {
                consume_open(s, header_bytes);
                s.skipping = size;
                close_finished(s);
                committed = c.pos;
                continue;
            }
            if (id == kTrackEntry) {
                ++s.track_entries_seen;
                if (s.track_entries_seen > s.options.max_tracks) {
                    return std::unexpected(DemuxError::kLimitExceeded);
                }
                s.pending = ReadTrack{};
                s.pending_is_audio = false;
                s.pending_has_number = false;
                s.in_track_entry = true;
            }
            consume_open(s, header_bytes);
            s.open.push_back(ReaderState::Open{id, size, unknown_size});
            close_finished(s);
            committed = c.pos;
            continue;
        }

        if (!is_wanted_leaf(id)) {
            consume_open(s, header_bytes);
            s.skipping = size;
            close_finished(s);
            committed = c.pos;
            continue;
        }

        // A leaf worth reading has to be here in full.
        if (size > s.options.max_element_bytes) {
            return std::unexpected(DemuxError::kLimitExceeded);
        }
        if (static_cast<std::uint64_t>(c.left()) < size) {
            c.pos = element_start;
            break;  // need more input
        }
        const auto body = data.subspan(c.pos, static_cast<std::size_t>(size));
        c.pos += static_cast<std::size_t>(size);

        switch (id) {
            case kTrackNumber: {
                std::uint64_t value = 0;
                if (!read_uint(body, value)) {
                    return std::unexpected(DemuxError::kMalformed);
                }
                s.pending.track_number = value;
                s.pending_has_number = true;
                break;
            }
            case kTrackType: {
                std::uint64_t value = 0;
                if (!read_uint(body, value)) {
                    return std::unexpected(DemuxError::kMalformed);
                }
                s.pending_is_audio = value == kTrackTypeAudio;
                break;
            }
            case kCodecId:
                s.pending.codec_id = read_string(body);
                break;
            case kLanguage:
                s.pending.language = read_string(body);
                break;
            case kChannels: {
                std::uint64_t value = 0;
                if (!read_uint(body, value)) {
                    return std::unexpected(DemuxError::kMalformed);
                }
                // Past what any layout uses is a corrupt field, not a track
                // worth describing.
                if (value == 0 || value > 255) {
                    return std::unexpected(DemuxError::kMalformed);
                }
                s.pending.channels = static_cast<int>(value);
                break;
            }
            case kSamplingFrequency: {
                double value = 0.0;
                if (!read_float(body, value)) {
                    return std::unexpected(DemuxError::kMalformed);
                }
                // Rejects NaN too - the comparison is false for it, which is
                // why this is written as !(value > 0.0) rather than <= 0.0.
                if (!(value > 0.0) || value > 4'000'000.0) {
                    return std::unexpected(DemuxError::kMalformed);
                }
                s.pending.sample_rate = static_cast<std::uint32_t>(value);
                break;
            }
            case kSimpleBlock:
            case kBlock: {
                // Blocks before Tracks (or for another track) are skipped:
                // there is nothing to match them against yet.
                if (s.track_found) {
                    s.block_frames.clear();
                    if (!split_block(body, s.track.track_number, s.options, s.block_frames)) {
                        return std::unexpected(DemuxError::kMalformed);
                    }
                    for (const auto& frame : s.block_frames) {
                        on_frame(frame);
                        ++s.frames_read;
                    }
                }
                break;
            }
            default:
                break;
        }

        consume_open(s, header_bytes + size);
        close_finished(s);
        committed = c.pos;
    }

    return committed;
}

// The verdict both entry points reach once no more bytes are coming. A cut
// mid-cluster is not an error - see reader.hpp's demux() comment.
std::expected<void, DemuxError> finish_verdict(const ReaderState& s) {
    if (!s.ebml_header_seen) {
        return std::unexpected(DemuxError::kNotMatroska);
    }
    if (!s.track_found) {
        return std::unexpected(s.tracks_seen ? DemuxError::kNoAudioTrack : DemuxError::kTruncated);
    }
    return {};
}

}  // namespace

std::expected<Demuxed, DemuxError> demux(std::span<const std::byte> file,
                                         const ReadOptions& options) {
    detail::ReaderState s;
    s.options = options;

    Demuxed out;
    // Frames are views into `file`, so collecting them costs one pointer
    // pair each and copies no audio - the zero-copy promise in the header.
    const auto collect = [&out](std::span<const std::byte> frame) { out.frames.push_back(frame); };

    const auto consumed = walk(s, file, collect);
    if (!consumed) {
        return std::unexpected(consumed.error());
    }
    const auto verdict = finish_verdict(s);
    if (!verdict) {
        return std::unexpected(verdict.error());
    }
    out.track = s.track;
    return out;
}

Reader::Reader(const ReadOptions& options)
    : state_(std::make_unique<detail::ReaderState>()) {
    state_->options = options;
}

Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;
Reader::~Reader() = default;

const ReadTrack& Reader::track() const { return state_->track; }
bool Reader::track_found() const { return state_->track_found; }
std::size_t Reader::frames_read() const { return state_->frames_read; }

std::expected<void, DemuxError> Reader::push(std::span<const std::byte> chunk,
                                             const FrameFn& on_frame) {
    auto& s = *state_;
    s.buffer.insert(s.buffer.end(), chunk.begin(), chunk.end());
    const auto consumed = walk(s, s.buffer, on_frame);
    if (!consumed) {
        return std::unexpected(consumed.error());
    }
    // Drop what the walker finished with. What is left is the head of an
    // element whose bytes have not all arrived - bounded by
    // max_element_bytes, since anything larger was refused rather than
    // buffered.
    s.buffer.erase(s.buffer.begin(), s.buffer.begin() + static_cast<std::ptrdiff_t>(*consumed));
    return {};
}

std::expected<void, DemuxError> Reader::finish() { return finish_verdict(*state_); }

}  // namespace matroska
