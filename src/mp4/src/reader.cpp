#include "mp4/reader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "isobmff_detail.hpp"
#include "mp4/mp4.hpp"

namespace mp4 {

namespace {

using detail::BoxHeader;
using detail::BoxRead;
using detail::fourcc;
using detail::get_u16;
using detail::get_u32;
using detail::get_u64;
using detail::get_u8;
using detail::read_box_header;

// Every box type this walk recognises. Anything else is skipped by its own
// declared length.
constexpr std::uint32_t kFtyp = fourcc("ftyp");
constexpr std::uint32_t kStyp = fourcc("styp");
constexpr std::uint32_t kMoov = fourcc("moov");
constexpr std::uint32_t kTrak = fourcc("trak");
constexpr std::uint32_t kTkhd = fourcc("tkhd");
constexpr std::uint32_t kMdia = fourcc("mdia");
constexpr std::uint32_t kMdhd = fourcc("mdhd");
constexpr std::uint32_t kMinf = fourcc("minf");
constexpr std::uint32_t kStbl = fourcc("stbl");
constexpr std::uint32_t kStsd = fourcc("stsd");
constexpr std::uint32_t kStsc = fourcc("stsc");
constexpr std::uint32_t kStsz = fourcc("stsz");
constexpr std::uint32_t kStz2 = fourcc("stz2");
constexpr std::uint32_t kStco = fourcc("stco");
constexpr std::uint32_t kCo64 = fourcc("co64");
constexpr std::uint32_t kMvex = fourcc("mvex");
constexpr std::uint32_t kTrex = fourcc("trex");
constexpr std::uint32_t kMoof = fourcc("moof");
constexpr std::uint32_t kTraf = fourcc("traf");
constexpr std::uint32_t kTfhd = fourcc("tfhd");
constexpr std::uint32_t kTrun = fourcc("trun");
constexpr std::uint32_t kMdat = fourcc("mdat");
constexpr std::uint32_t kAc3Entry = fourcc("ac-3");
constexpr std::uint32_t kEc3Entry = fourcc("ec-3");
constexpr std::uint32_t kDac3 = fourcc("dac3");
constexpr std::uint32_t kDec3 = fourcc("dec3");

// Boxes that are pure containers: the walk descends into them rather than
// buffering them whole, which is what keeps a moov of any size from being
// held in one piece.
[[nodiscard]] bool is_container(std::uint32_t type) {
    return type == kMoov || type == kTrak || type == kMdia || type == kMinf || type == kStbl ||
           type == kMvex || type == kMoof || type == kTraf;
}

// Leaves the walk needs the bytes of. stsd is a FullBox with children rather
// than a plain container, so it is read whole and its one sample entry
// parsed in place - it is a few dozen bytes, not a table.
[[nodiscard]] bool is_wanted_leaf(std::uint32_t type) {
    return type == kTkhd || type == kMdhd || type == kStsd || type == kStsc || type == kStsz ||
           type == kStz2 || type == kStco || type == kCo64 || type == kTrex || type == kTfhd ||
           type == kTrun;
}

// --- the dac3/dec3 configuration box ---------------------------------------
//
// The read twin of ac3::io::build_codec_config_box (ac3/io/dec3.hpp), field
// for field: ETSI TS 102 366 Annex F §F.4 (AC3SpecificBox) and §F.6
// (EC3SpecificBox), plus TS 103 420 §8.3.1/§8.3.2.2's Atmos extension. A
// tiny MSB-first bit reader rather than ac3::BitReader, because this module
// links nothing from ac3::forge - the same boundary that keeps
// AudioTrack::codec_config opaque to the writer.
class BitCursor {
   public:
    explicit BitCursor(std::span<const std::byte> data) : data_(data) {}

    // Zero-extends past the end rather than failing: every field below is
    // optional-by-truncation in some real file, and a short box means "this
    // muxer stopped here", not "corrupt". The caller checks `left()` where
    // the difference matters - see the Atmos extension below.
    [[nodiscard]] std::uint32_t get(int bits) {
        std::uint32_t value = 0;
        for (int i = 0; i < bits; ++i) {
            const std::size_t index = pos_ >> 3U;
            const std::uint32_t bit =
                index < data_.size()
                    ? (static_cast<std::uint32_t>(get_u8(data_, index)) >> (7U - (pos_ & 7U))) & 1U
                    : 0U;
            value = (value << 1U) | bit;
            ++pos_;
        }
        return value;
    }

    [[nodiscard]] std::size_t left() const {
        const std::size_t total = data_.size() * 8;
        return pos_ >= total ? 0 : total - pos_;
    }

   private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
};

CodecConfig parse_codec_config(std::uint32_t box_type, std::span<const std::byte> payload) {
    CodecConfig out;
    out.eac3 = box_type == kDec3;
    out.payload.assign(payload.begin(), payload.end());
    BitCursor bits{payload};

    if (!out.eac3) {
        // §F.4: fscod(2) bsid(5) bsmod(3) acmod(3) lfeon(1) bit_rate_code(5)
        // reserved(5).
        out.fscod = static_cast<int>(bits.get(2));
        out.bsid = static_cast<int>(bits.get(5));
        out.bsmod = static_cast<int>(bits.get(3));
        out.acmod = static_cast<int>(bits.get(3));
        out.lfeon = bits.get(1) != 0;
        out.bit_rate_code = static_cast<int>(bits.get(5));
        return out;
    }

    // §F.6: data_rate(13) num_ind_sub(3), then one independent-substream
    // record. Only the first is read: mp4::AudioTrack describes exactly one
    // track, and ac3::io::scan groups an access unit as one independent
    // substream plus its dependents, so a second record has nowhere to go
    // in ReadTrack - and num_ind_sub is reported verbatim so a caller can
    // see that the file claimed more.
    out.data_rate_kbps = static_cast<int>(bits.get(13));
    out.num_ind_sub = static_cast<int>(bits.get(3));
    out.fscod = static_cast<int>(bits.get(2));
    out.bsid = static_cast<int>(bits.get(5));
    (void)bits.get(1);  // reserved
    out.asvc = bits.get(1) != 0;
    out.bsmod = static_cast<int>(bits.get(3));
    out.acmod = static_cast<int>(bits.get(3));
    out.lfeon = bits.get(1) != 0;
    (void)bits.get(3);  // reserved
    out.num_dep_sub = static_cast<int>(bits.get(4));
    if (out.num_dep_sub > 0) {
        out.chan_loc = static_cast<int>(bits.get(9));
    } else {
        (void)bits.get(1);  // reserved
    }

    // The Atmos extension is a TRAILING addition: a box written before TS
    // 103 420 simply ends here, which is not an error and not "no Atmos" -
    // it is "this box has nothing to say". Only a box that actually carries
    // the byte gets read, hence the explicit length check rather than
    // letting BitCursor zero-extend and reporting a confident false.
    if (bits.left() >= 8) {
        (void)bits.get(7);  // reserved
        const bool flag_type_a = bits.get(1) != 0;
        if (flag_type_a && bits.left() >= 8) {
            out.oba_complexity_index = static_cast<int>(bits.get(8));
        }
    }
    return out;
}

// --- the sample table -------------------------------------------------------

// One sample's byte range in the file. This is what both the plain
// (stsc/stsz/stco) and the fragmented (tfhd/trun) paths produce, and the
// only thing the emitter downstream understands.
struct SampleRef {
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
};

// A run of chunks that all carry the same number of samples - ISO/IEC
// 14496-12 §8.7.4's stsc entry, which is a run-length encoding: an entry
// applies from its own first_chunk until the next entry's.
struct ChunkRun {
    std::uint32_t first_chunk = 0;  // 1-based, as stored
    std::uint32_t samples_per_chunk = 0;
};

// Everything one trak contributes, gathered as its boxes arrive and only
// acted on when the trak closes: stsd may follow stco, and nothing can be
// decided until the whole table is in.
struct PendingTrack {
    std::uint32_t track_id = 0;
    std::uint32_t timescale = 0;
    std::string language{"und"};
    bool has_entry = false;
    std::uint32_t entry_type = 0;
    std::uint32_t sample_rate = 0;
    int channels = 0;
    CodecConfig codec_config;
    std::vector<std::uint32_t> sizes;
    std::vector<ChunkRun> chunk_runs;
    std::vector<std::uint64_t> chunk_offsets;
};

// ISO/IEC 14496-12 §8.4.2.2: three 5-bit (letter - 0x60) codes. The read
// twin of isobmff_detail.hpp's pack_language.
[[nodiscard]] std::string unpack_language(std::uint16_t packed) {
    std::string out;
    for (int shift : {10, 5, 0}) {
        const auto letter = static_cast<unsigned>((packed >> shift) & 0x1FU);
        // 0 is what pack_language writes for a character it could not map;
        // anything outside a-z is not a language code either.
        if (letter == 0 || letter > 26) {
            return "und";
        }
        out.push_back(static_cast<char>('a' + letter - 1));
    }
    return out;
}

// Turns stsc's run-length chunk map, stsz's sizes and stco/co64's offsets
// into one flat list of byte ranges. This is where a hostile sample table
// gets to lie: an stsc run naming a chunk that does not exist, a
// samples_per_chunk of four billion, sizes that sum past any plausible
// file. Every one of those is a false return rather than an allocation or
// an out-of-range read - the ranges themselves are bounds-checked later,
// against the bytes actually present.
[[nodiscard]] bool build_sample_refs(const PendingTrack& track, const ReadOptions& options,
                                     std::vector<SampleRef>& out) {
    if (track.chunk_offsets.empty() || track.sizes.empty()) {
        // A track with a sample entry but no samples is legal (a fragmented
        // file's init segment is exactly that) and not an error; it simply
        // contributes nothing here.
        return true;
    }
    if (track.chunk_runs.empty()) {
        return false;  // sizes and offsets but no map from one to the other
    }
    // §8.7.4: first_chunk is 1-based and strictly increasing.
    if (track.chunk_runs.front().first_chunk != 1) {
        return false;
    }
    for (std::size_t i = 1; i < track.chunk_runs.size(); ++i) {
        if (track.chunk_runs[i].first_chunk <= track.chunk_runs[i - 1].first_chunk) {
            return false;
        }
    }

    out.reserve(track.sizes.size());
    std::size_t sample = 0;
    std::size_t run = 0;
    for (std::size_t chunk = 0; chunk < track.chunk_offsets.size(); ++chunk) {
        // Advance to the run covering this chunk (chunk numbers are 1-based
        // in the file, 0-based here).
        while (run + 1 < track.chunk_runs.size() &&
               track.chunk_runs[run + 1].first_chunk <= chunk + 1) {
            ++run;
        }
        const std::uint32_t per_chunk = track.chunk_runs[run].samples_per_chunk;
        if (per_chunk > options.max_samples) {
            return false;
        }
        std::uint64_t offset = track.chunk_offsets[chunk];
        for (std::uint32_t i = 0; i < per_chunk; ++i) {
            if (sample >= track.sizes.size()) {
                // The chunk map claims more samples than stsz describes.
                // Stopping here rather than failing matches what a player
                // does with a table whose tail is over-long, and the sizes
                // that DID exist are all real.
                return true;
            }
            const std::uint32_t size = track.sizes[sample];
            out.push_back(SampleRef{offset, size});
            offset += size;
            ++sample;
        }
    }
    return true;
}

}  // namespace

std::string_view describe(DemuxError error) {
    switch (error) {
        case DemuxError::kNotIsobmff:
            return "not an ISO base media file: no box structure where one has to be";
        case DemuxError::kTruncated:
            return "input ends before the track or its samples";
        case DemuxError::kMalformed:
            return "malformed box, sample table or fragment layout";
        case DemuxError::kNoAudioTrack:
            return "no 'ac-3'/'ec-3' track (or the requested track id is absent)";
        case DemuxError::kLimitExceeded:
            return "box size, sample count or nesting depth beyond the reader's limits";
        case DemuxError::kMoovAfterMdat:
            return "the sample table (moov) follows the media data it indexes: this layout "
                   "cannot be read as a stream, use demux() on the whole file";
    }
    return "unknown error";
}

namespace detail {

// Everything the walk carries between calls. demux() keeps one on the stack
// and walks once; Reader owns one across push()es - so both drive the
// identical parser, which has no idea which caller it is serving.
struct ReaderState {
    ReadOptions options;

    struct Open {
        std::uint32_t type = 0;
        std::uint64_t end = 0;  // absolute file offset one past this box
        bool to_eof = false;
    };
    std::vector<Open> open;

    // Absolute offset of the next byte the box parser wants. Absolute, not
    // buffer-relative, because a sample table indexes the FILE.
    std::uint64_t parse_pos = 0;
    // Absolute offset of the first byte still held (window_pos for demux is
    // always 0, since it holds everything).
    std::uint64_t window_pos = 0;

    PendingTrack pending;
    bool in_trak = false;
    std::uint32_t traks_seen = 0;

    ReadTrack track;
    bool track_found = false;
    bool saw_box = false;
    bool saw_mdat = false;
    // A moov that arrived after mdat: fatal for the streaming Reader, fine
    // for demux(), so it is recorded rather than rejected here.
    bool moov_after_mdat = false;

    // Fragmented playback: mvex/trex's per-track defaults, and the traf
    // being read.
    std::uint32_t trex_track_id = 0;
    std::uint32_t trex_default_sample_size = 0;
    bool have_trex = false;
    std::uint64_t current_moof_start = 0;
    std::uint32_t traf_track_id = 0;
    std::uint64_t traf_base_offset = 0;
    std::uint32_t traf_default_sample_size = 0;
    bool traf_is_selected = false;

    // Samples found but not yet handed to the caller, in ascending offset
    // order. An MP4's sample table is an O(duration) index by construction;
    // ReadOptions::max_samples bounds it. The AUDIO never accumulates - only
    // these 12-byte records do, and only until their bytes arrive.
    std::vector<SampleRef> samples;
    std::size_t next_sample = 0;
    std::size_t samples_read = 0;

    // Incremental input only.
    std::vector<std::byte> buffer;
};

}  // namespace detail

namespace {

using detail::ReaderState;

// Returns an error rather than void because closing a trak is where the
// sample table is finally assembled, and a table that does not add up is a
// malformed file - discarding that verdict would leave a hostile stsc
// silently producing no samples instead of an explanation.
std::expected<void, DemuxError> close_finished(ReaderState& s, const ReadOptions& options) {
    while (!s.open.empty()) {
        const auto& top = s.open.back();
        if (top.to_eof || s.parse_pos < top.end) {
            return {};
        }
        if (top.type == kTrak) {
            if (s.in_trak && !s.track_found && s.pending.has_entry) {
                const bool wanted = options.track_id != 0
                                        ? s.pending.track_id == options.track_id
                                        : (s.pending.entry_type == kAc3Entry ||
                                           s.pending.entry_type == kEc3Entry);
                if (wanted) {
                    s.track = ReadTrack{
                        .track_id = s.pending.track_id,
                        .codec_id = std::string{s.pending.entry_type == kAc3Entry ? kCodecAc3
                                                                                  : kCodecEac3},
                        .sample_rate = s.pending.sample_rate,
                        .channels = s.pending.channels,
                        .timescale = s.pending.timescale,
                        .language = s.pending.language,
                        .codec_config = s.pending.codec_config,
                    };
                    s.track_found = true;
                    // The plain path's samples exist the moment the table is
                    // whole; the fragmented path's arrive per moof later.
                    if (!build_sample_refs(s.pending, options, s.samples)) {
                        return std::unexpected(DemuxError::kMalformed);
                    }
                }
            }
            s.in_trak = false;
        }
        s.open.pop_back();
    }
    return {};
}

// --- leaf parsers -----------------------------------------------------------
// Each takes the box BODY (everything after the size+type header) and
// returns false only for a layout that cannot be parsed at all; a body too
// short for a field it does not need is not an error.

[[nodiscard]] bool parse_tkhd(std::span<const std::byte> body, PendingTrack& track) {
    if (body.size() < 4) {
        return false;
    }
    const std::uint8_t version = get_u8(body, 0);
    // version 0 packs creation/modification as 32-bit, version 1 as 64-bit;
    // track_ID sits after both (§8.3.2.2).
    const std::size_t at = version == 1 ? 20 : 12;
    if (body.size() < at + 4) {
        return false;
    }
    track.track_id = get_u32(body, at);
    return true;
}

[[nodiscard]] bool parse_mdhd(std::span<const std::byte> body, PendingTrack& track) {
    if (body.size() < 4) {
        return false;
    }
    const std::uint8_t version = get_u8(body, 0);
    const std::size_t timescale_at = version == 1 ? 20 : 12;
    const std::size_t language_at = version == 1 ? 32 : 20;
    if (body.size() < timescale_at + 4) {
        return false;
    }
    track.timescale = get_u32(body, timescale_at);
    if (body.size() >= language_at + 2) {
        track.language = unpack_language(get_u16(body, language_at));
    }
    return true;
}

// stsd is a FullBox whose body is an entry count followed by sample entries.
// Only the first 'ac-3'/'ec-3' one matters here; anything else means this
// trak is not ours, which is not an error.
[[nodiscard]] bool parse_stsd(std::span<const std::byte> body, PendingTrack& track) {
    if (body.size() < 8) {
        return false;
    }
    const std::uint32_t entry_count = get_u32(body, 4);
    std::size_t at = 8;
    for (std::uint32_t i = 0; i < entry_count && at + 8 <= body.size(); ++i) {
        BoxHeader entry{};
        if (read_box_header(body, at, entry) != BoxRead::kOk || entry.to_eof) {
            return false;
        }
        if (entry.size > body.size() - at) {
            return false;
        }
        if (entry.type == kAc3Entry || entry.type == kEc3Entry) {
            // §12.2.3's AudioSampleEntry, on top of §8.5.2's SampleEntry:
            // reserved(6) data_reference_index(2) reserved(8)
            // channelcount(2) samplesize(2) pre_defined(2) reserved(2)
            // samplerate(4, 16.16) = 28 bytes before the child boxes.
            constexpr std::size_t kAudioSampleEntryBytes = 28;
            const std::size_t fields = at + entry.header_bytes;
            if (fields + kAudioSampleEntryBytes > body.size()) {
                return false;
            }
            track.entry_type = entry.type;
            track.channels = static_cast<int>(get_u16(body, fields + 16));
            track.sample_rate = get_u32(body, fields + 24) >> 16U;
            track.has_entry = true;

            // The one child that matters: dac3 or dec3.
            std::size_t child = fields + kAudioSampleEntryBytes;
            const std::size_t entry_end = at + static_cast<std::size_t>(entry.size);
            while (child + 8 <= entry_end) {
                BoxHeader config{};
                if (read_box_header(body, child, config) != BoxRead::kOk || config.to_eof) {
                    return false;
                }
                if (config.size > entry_end - child) {
                    return false;
                }
                if (config.type == kDac3 || config.type == kDec3) {
                    track.codec_config = parse_codec_config(
                        config.type, body.subspan(child + config.header_bytes,
                                                  static_cast<std::size_t>(config.size) -
                                                      config.header_bytes));
                    break;
                }
                child += static_cast<std::size_t>(config.size);
            }
            return true;
        }
        at += static_cast<std::size_t>(entry.size);
    }
    return true;
}

[[nodiscard]] bool parse_stsc(std::span<const std::byte> body, const ReadOptions& options,
                              PendingTrack& track) {
    if (body.size() < 8) {
        return false;
    }
    const std::uint32_t count = get_u32(body, 4);
    if (count > options.max_chunks) {
        return false;
    }
    if (static_cast<std::uint64_t>(count) * 12 + 8 > body.size()) {
        return false;
    }
    track.chunk_runs.clear();
    track.chunk_runs.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = 8 + (static_cast<std::size_t>(i) * 12);
        track.chunk_runs.push_back(
            ChunkRun{.first_chunk = get_u32(body, at), .samples_per_chunk = get_u32(body, at + 4)});
    }
    return true;
}

[[nodiscard]] bool parse_stsz(std::span<const std::byte> body, const ReadOptions& options,
                              PendingTrack& track) {
    if (body.size() < 12) {
        return false;
    }
    const std::uint32_t uniform_size = get_u32(body, 4);
    const std::uint32_t count = get_u32(body, 8);
    if (count > options.max_samples) {
        return false;
    }
    if (uniform_size != 0) {
        // Every sample the same size - legal, though no AC-3/E-AC-3 muxer
        // writes it (frame sizes vary even in CBR).
        track.sizes.assign(count, uniform_size);
        return true;
    }
    if (static_cast<std::uint64_t>(count) * 4 + 12 > body.size()) {
        return false;
    }
    track.sizes.clear();
    track.sizes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        track.sizes.push_back(get_u32(body, 12 + (static_cast<std::size_t>(i) * 4)));
    }
    return true;
}

// §8.7.3.3's compact sample size box: a 4/8/16-bit field per sample instead
// of stsz's flat 32. Not written by this project, but written by real
// muxers, and a file using it is otherwise unreadable.
[[nodiscard]] bool parse_stz2(std::span<const std::byte> body, const ReadOptions& options,
                              PendingTrack& track) {
    if (body.size() < 12) {
        return false;
    }
    const std::uint32_t field_size = get_u8(body, 7);
    const std::uint32_t count = get_u32(body, 8);
    if (count > options.max_samples) {
        return false;
    }
    if (field_size != 4 && field_size != 8 && field_size != 16) {
        return false;
    }
    const std::uint64_t bits = static_cast<std::uint64_t>(count) * field_size;
    if ((bits + 7) / 8 + 12 > body.size()) {
        return false;
    }
    track.sizes.clear();
    track.sizes.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (field_size == 16) {
            track.sizes.push_back(get_u16(body, 12 + (static_cast<std::size_t>(i) * 2)));
        } else if (field_size == 8) {
            track.sizes.push_back(get_u8(body, 12 + i));
        } else {
            const std::uint8_t pair = get_u8(body, 12 + (i / 2));
            track.sizes.push_back((i % 2 == 0) ? (pair >> 4U) : (pair & 0x0FU));
        }
    }
    return true;
}

[[nodiscard]] bool parse_chunk_offsets(std::uint32_t type, std::span<const std::byte> body,
                                       const ReadOptions& options, PendingTrack& track) {
    if (body.size() < 8) {
        return false;
    }
    const std::uint32_t count = get_u32(body, 4);
    if (count > options.max_chunks) {
        return false;
    }
    const std::size_t width = type == kCo64 ? 8 : 4;
    if (static_cast<std::uint64_t>(count) * width + 8 > body.size()) {
        return false;
    }
    track.chunk_offsets.clear();
    track.chunk_offsets.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = 8 + (static_cast<std::size_t>(i) * width);
        track.chunk_offsets.push_back(width == 8 ? get_u64(body, at) : get_u32(body, at));
    }
    return true;
}

// §8.8.7's tfhd. Its flags say which optional fields are present, and
// default-base-is-moof (0x020000) says data offsets run from this moof's
// own start rather than the file's - which is what fragment() writes and
// what CMAF requires.
[[nodiscard]] bool parse_tfhd(std::span<const std::byte> body, ReaderState& s) {
    if (body.size() < 8) {
        return false;
    }
    const std::uint32_t flags = get_u32(body, 0) & 0x00FF'FFFFU;
    s.traf_track_id = get_u32(body, 4);
    std::size_t at = 8;
    s.traf_base_offset = (flags & 0x020000U) != 0 ? s.current_moof_start : 0;
    if ((flags & 0x000001U) != 0) {  // base-data-offset-present
        if (body.size() < at + 8) {
            return false;
        }
        s.traf_base_offset = get_u64(body, at);
        at += 8;
    }
    if ((flags & 0x000002U) != 0) {  // sample-description-index-present
        at += 4;
    }
    if ((flags & 0x000008U) != 0) {  // default-sample-duration-present
        at += 4;
    }
    s.traf_default_sample_size = s.have_trex && s.trex_track_id == s.traf_track_id
                                     ? s.trex_default_sample_size
                                     : 0;
    if ((flags & 0x000010U) != 0) {  // default-sample-size-present
        if (body.size() < at + 4) {
            return false;
        }
        s.traf_default_sample_size = get_u32(body, at);
    }
    s.traf_is_selected = s.track_found && s.traf_track_id == s.track.track_id;
    return true;
}

// §8.8.8's trun.
[[nodiscard]] bool parse_trun(std::span<const std::byte> body, ReaderState& s) {
    if (body.size() < 8) {
        return false;
    }
    const std::uint32_t flags = get_u32(body, 0) & 0x00FF'FFFFU;
    const std::uint32_t count = get_u32(body, 4);
    if (count > s.options.max_samples) {
        return false;
    }
    std::size_t at = 8;
    std::int64_t data_offset = 0;
    if ((flags & 0x000001U) != 0) {  // data-offset-present
        if (body.size() < at + 4) {
            return false;
        }
        data_offset = static_cast<std::int32_t>(get_u32(body, at));
        at += 4;
    }
    if ((flags & 0x000004U) != 0) {  // first-sample-flags-present
        at += 4;
    }
    // Per-sample fields, in the fixed order §8.8.8.2 gives them.
    std::size_t stride = 0;
    std::size_t size_at = 0;
    if ((flags & 0x000100U) != 0) {  // sample-duration-present
        stride += 4;
    }
    const bool has_size = (flags & 0x000200U) != 0;
    if (has_size) {
        size_at = stride;
        stride += 4;
    }
    if ((flags & 0x000400U) != 0) {  // sample-flags-present
        stride += 4;
    }
    if ((flags & 0x000800U) != 0) {  // composition-time-offsets-present
        stride += 4;
    }
    if (static_cast<std::uint64_t>(count) * stride + at > body.size()) {
        return false;
    }
    if (!has_size && s.traf_default_sample_size == 0) {
        // No per-sample size and no default to fall back on: nothing in the
        // fragment says where one sample ends and the next begins.
        return false;
    }
    if (!s.traf_is_selected) {
        return true;  // another track's fragment: parsed, nothing to emit
    }

    const std::int64_t base = static_cast<std::int64_t>(s.traf_base_offset) + data_offset;
    if (base < 0) {
        return false;
    }
    auto offset = static_cast<std::uint64_t>(base);
    if (s.samples.size() + count > s.options.max_samples) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t size =
            has_size ? get_u32(body, at + (static_cast<std::size_t>(i) * stride) + size_at)
                     : s.traf_default_sample_size;
        s.samples.push_back(SampleRef{offset, size});
        offset += size;
    }
    return true;
}

// Hands over every pending sample whose bytes are inside `window`, which
// starts at absolute offset `window_pos`. Samples come out in the order the
// table declares them, which for audio is both decode and presentation
// order.
[[nodiscard]] std::expected<void, DemuxError> drain_samples(ReaderState& s,
                                                            std::span<const std::byte> window,
                                                            std::uint64_t window_pos,
                                                            const Reader::SampleFn& on_sample) {
    const std::uint64_t window_end = window_pos + window.size();
    while (s.next_sample < s.samples.size()) {
        const auto& ref = s.samples[s.next_sample];
        if (ref.offset < window_pos) {
            // The bytes this sample names are already behind the window -
            // only possible when the table arrived after the data it
            // indexes, which a stream cannot go back for.
            return std::unexpected(DemuxError::kMoovAfterMdat);
        }
        const std::uint64_t end = ref.offset + ref.size;
        if (end > window_end) {
            break;  // not here yet
        }
        on_sample(window.subspan(static_cast<std::size_t>(ref.offset - window_pos), ref.size));
        ++s.next_sample;
        ++s.samples_read;
    }
    return {};
}

// Parses as much of `window` as it can. `window_pos` is the absolute file
// offset of window[0]; s.parse_pos is where the parser is, in the same
// absolute terms. Returns nothing - progress is recorded in s.parse_pos, and
// the caller decides what it may now discard.
std::expected<void, DemuxError> walk(ReaderState& s, std::span<const std::byte> window,
                                     std::uint64_t window_pos) {
    const std::uint64_t window_end = window_pos + window.size();

    for (;;) {
        if (s.parse_pos >= window_end) {
            return {};  // need more input (or done)
        }
        const auto at = static_cast<std::size_t>(s.parse_pos - window_pos);

        BoxHeader box{};
        const BoxRead got = read_box_header(window, at, box);
        if (got == BoxRead::kNeedMore) {
            return {};
        }
        if (got == BoxRead::kBad) {
            return std::unexpected(s.saw_box ? DemuxError::kMalformed : DemuxError::kNotIsobmff);
        }
        // A file that does not begin with a recognisable top-level box is
        // not ISOBMFF. ftyp/styp is the normal opener; a bare moov or mdat
        // occurs in the wild too, so the test is "is this one of the box
        // types a file can start with", not "is it ftyp".
        if (!s.saw_box) {
            const bool plausible = box.type == kFtyp || box.type == kStyp || box.type == kMoov ||
                                   box.type == kMoof || box.type == kMdat;
            if (!plausible) {
                return std::unexpected(DemuxError::kNotIsobmff);
            }
            s.saw_box = true;
        }

        const std::uint64_t body_bytes = box.to_eof ? 0 : box.size - box.header_bytes;
        const std::uint64_t box_end = box.to_eof ? 0 : s.parse_pos + box.size;

        if (box.type == kMdat) {
            s.saw_mdat = true;
        } else if (box.type == kMoov && s.saw_mdat) {
            s.moov_after_mdat = true;
        }

        if (is_container(box.type)) {
            if (s.open.size() >= s.options.max_depth) {
                return std::unexpected(DemuxError::kLimitExceeded);
            }
            if (box.type == kMoof) {
                s.current_moof_start = s.parse_pos;
            }
            if (box.type == kTrak) {
                ++s.traks_seen;
                if (s.traks_seen > s.options.max_chunks) {
                    return std::unexpected(DemuxError::kLimitExceeded);
                }
                s.pending = PendingTrack{};
                s.in_trak = true;
            }
            s.open.push_back(ReaderState::Open{box.type, box_end, box.to_eof});
            s.parse_pos += box.header_bytes;
            if (const auto closed = close_finished(s, s.options); !closed) {
                return std::unexpected(closed.error());
            }
            continue;
        }

        if (!is_wanted_leaf(box.type)) {
            // Skipped by length alone - never held. mdat lands here, which
            // is exactly right: its bytes reach the caller through
            // drain_samples, not through this parser.
            if (box.to_eof) {
                // Runs to the end of the file: there is no box after it, so
                // parking the parser past every possible offset ends the
                // walk without pretending to know where the file stops.
                s.parse_pos = UINT64_MAX;
                return {};
            }
            s.parse_pos = box_end;
            if (const auto closed = close_finished(s, s.options); !closed) {
                return std::unexpected(closed.error());
            }
            continue;
        }

        // A leaf worth reading has to be here in full.
        if (box.to_eof || body_bytes > s.options.max_box_bytes) {
            return std::unexpected(DemuxError::kLimitExceeded);
        }
        if (box_end > window_end) {
            return {};  // need more input
        }
        const auto body = window.subspan(at + box.header_bytes,
                                         static_cast<std::size_t>(body_bytes));

        bool ok = true;
        switch (box.type) {
            case kTkhd:
                ok = parse_tkhd(body, s.pending);
                break;
            case kMdhd:
                ok = parse_mdhd(body, s.pending);
                break;
            case kStsd:
                ok = parse_stsd(body, s.pending);
                break;
            case kStsc:
                ok = parse_stsc(body, s.options, s.pending);
                break;
            case kStsz:
                ok = parse_stsz(body, s.options, s.pending);
                break;
            case kStz2:
                ok = parse_stz2(body, s.options, s.pending);
                break;
            case kStco:
            case kCo64:
                ok = parse_chunk_offsets(box.type, body, s.options, s.pending);
                break;
            case kTrex:
                // Body layout past the FullBox header (offset 0): track_ID(4),
                // default_sample_description_index(8), default_sample_duration(12),
                // default_sample_size(16), default_sample_flags(20) - ISO/IEC
                // 14496-12 §8.8.3. Confirmed against this project's own writer
                // (fragment.cpp's build_trex) and the reader test helper's
                // independent read_trex().
                if (body.size() >= 20) {
                    s.trex_track_id = get_u32(body, 4);
                    s.trex_default_sample_size = get_u32(body, 16);
                    s.have_trex = true;
                }
                break;
            case kTfhd:
                ok = parse_tfhd(body, s);
                break;
            case kTrun:
                ok = parse_trun(body, s);
                break;
            default:
                break;
        }
        if (!ok) {
            return std::unexpected(DemuxError::kMalformed);
        }

        s.parse_pos = box_end;
        if (const auto closed = close_finished(s, s.options); !closed) {
            return std::unexpected(closed.error());
        }
    }
}

// The verdict both entry points reach once no more bytes are coming.
[[nodiscard]] std::expected<void, DemuxError> finish_verdict(const ReaderState& s) {
    if (!s.saw_box) {
        return std::unexpected(DemuxError::kNotIsobmff);
    }
    if (!s.track_found) {
        // A file that never got as far as a trak was cut short; one that had
        // traks but none we can use is a different answer.
        return std::unexpected(s.traks_seen > 0 ? DemuxError::kNoAudioTrack
                                                : DemuxError::kTruncated);
    }
    return {};
}

}  // namespace

std::expected<Demuxed, DemuxError> demux(std::span<const std::byte> file,
                                         const ReadOptions& options) {
    detail::ReaderState s;
    s.options = options;

    const auto walked = walk(s, file, 0);
    if (!walked) {
        return std::unexpected(walked.error());
    }
    const auto verdict = finish_verdict(s);
    if (!verdict) {
        return std::unexpected(verdict.error());
    }

    Demuxed out;
    // Samples are views into `file`, so collecting them costs one pointer
    // pair each and copies no audio - the zero-copy promise in the header.
    // A sample whose declared range runs past the end of the file is
    // dropped, not fabricated: a truncated download is the ordinary case,
    // and the samples before the cut are all real.
    out.samples.reserve(s.samples.size());
    const auto collect = [&out](std::span<const std::byte> sample) {
        out.samples.push_back(sample);
    };
    const auto drained = drain_samples(s, file, 0, collect);
    if (!drained) {
        return std::unexpected(drained.error());
    }
    out.track = s.track;
    return out;
}

Reader::Reader(const ReadOptions& options) : state_(std::make_unique<detail::ReaderState>()) {
    state_->options = options;
}

Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;
Reader::~Reader() = default;

const ReadTrack& Reader::track() const { return state_->track; }
bool Reader::track_found() const { return state_->track_found; }
std::size_t Reader::samples_read() const { return state_->samples_read; }

std::expected<void, DemuxError> Reader::push(std::span<const std::byte> chunk,
                                             const SampleFn& on_sample) {
    auto& s = *state_;
    s.buffer.insert(s.buffer.end(), chunk.begin(), chunk.end());

    const auto walked = walk(s, s.buffer, s.window_pos);
    if (!walked) {
        return std::unexpected(walked.error());
    }
    const auto drained = drain_samples(s, s.buffer, s.window_pos, on_sample);
    if (!drained) {
        return std::unexpected(drained.error());
    }

    // Discard everything neither the parser nor the next sample still needs.
    // The parser may be far ahead (it skips mdat by arithmetic, never by
    // holding it) while samples are still being read out of those very
    // bytes, so the low-water mark is the LOWER of the two - which is what
    // keeps peak memory at a chunk plus a sample rather than the file.
    const std::uint64_t next_needed =
        s.next_sample < s.samples.size() ? s.samples[s.next_sample].offset : s.parse_pos;
    const std::uint64_t keep_from = std::min(s.parse_pos, next_needed);
    if (keep_from > s.window_pos) {
        const auto drop = std::min<std::uint64_t>(keep_from - s.window_pos, s.buffer.size());
        s.buffer.erase(s.buffer.begin(), s.buffer.begin() + static_cast<std::ptrdiff_t>(drop));
        s.window_pos += drop;
    }
    return {};
}

std::expected<void, DemuxError> Reader::finish() {
    auto& s = *state_;
    if (s.moov_after_mdat && s.next_sample < s.samples.size()) {
        // The table turned up behind the data. demux() reads this file
        // perfectly; a stream cannot, and saying which is more useful than
        // "no samples".
        return std::unexpected(DemuxError::kMoovAfterMdat);
    }
    return finish_verdict(s);
}

}  // namespace mp4
