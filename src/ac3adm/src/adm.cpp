#include "ac3adm/ac3adm.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <istream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <adm/adm.hpp>
#include <adm/errors.hpp>
#include <adm/parse.hpp>
#include <adm/utilities/id_assignment.hpp>
#include <adm/write.hpp>
#include <bw64/bw64.hpp>

#include "adm_model.hpp"

// Every `bw64::`/`adm::` symbol below is a vendored third-party library
// (libbw64/libadm respectively, see src/ac3adm/CMakeLists.txt); every
// `ac3adm::` symbol is this module's own. Both libraries report failure by
// throwing std::runtime_error (or, for libadm's XML/schema errors, the
// adm::error::AdmException hierarchy) - this project's own convention is
// std::expected for stream-level/recoverable failure (CONTRIBUTING.md), so
// every call into either library is wrapped here at this one boundary and
// translated into AdmError rather than letting an exception escape this
// module's public API.

namespace ac3adm {

std::string_view describe(AdmError error) {
    switch (error) {
        case AdmError::kCannotOpen: return "cannot open file";
        case AdmError::kNotRiff: return "not a well-formed RIFF/RF64/BW64 WAVE file";
        case AdmError::kMalformedXml: return "axml chunk is not well-formed XML";
        case AdmError::kMalformedAdm: return "axml chunk XML is not a valid ADM document";
        case AdmError::kOther: return "unexpected failure reading the BW64/ADM file";
    }
    return "unknown error";
}

namespace {

// A unique path under the system temp directory for parse_bw64(std::istream&)'s spool file (see
// its own comment below). NOT derived from the istream's own address: an earlier version did
// exactly that (reinterpret_cast<std::uintptr_t>(&in)), which looked unique enough in a single
// process but is not - Windows does not vary a given call frame's stack address between separate
// launches of the same binary much, if at all, so two of this project's own ctest entries
// (each ac3tests.exe test case is its own process, and ctest -j runs many of them concurrently)
// landed on the exact same temp filename and raced on it, one process's write clobbering the
// other's read mid-parse. Caught via a real, intermittent ctest failure under -j8 that a single
// direct run of the same test could not reproduce - the actual symptom (not a hypothesis) that
// justified this fix. A high-resolution clock reading XORed with a random_device draw and a
// monotonic in-process counter is unique both across concurrent processes and across repeated
// calls within one process, without needing a platform-specific PID call.
std::filesystem::path make_temp_path() {
    static std::atomic<std::uint64_t> counter{0};
    std::random_device rd;
    const auto unique = (static_cast<std::uint64_t>(rd()) << 32) ^
                        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                        counter.fetch_add(1);
    return std::filesystem::temp_directory_path() / ("ac3adm_" + std::to_string(unique) + ".wav");
}

// Trims the trailing padding libbw64's bw64::AudioId fixed-width uid()/trackRef()/packRef()
// fields carry. NOT just ASCII space: BS.2088-1 §8.2 pads an unused chna slot's ID fields (and
// any "not required" field, e.g. packRef when a stream references a pack directly - §8.3.2's own
// worked example allocates 32 slots and only populates 4) with NUL characters ("null strings...
// N null characters (ASCII value zero)"), and this is normal, spec-documented content, not a
// degenerate edge case. An earlier version of this function only trimmed ' ' (0x20), reasoning
// from AudioId's own constructor (chunks.hpp) memset-ing its buffers to spaces before copying -
// but that memset is a write-side default for constructing an AudioId programmatically from a
// shorter string; libbw64's own parseAudioId() (parser.hpp) never goes through a short string on
// the read path - it reads exactly 12/14/11 raw bytes off the wire and passes them, already at
// full width, straight into that same constructor, so the copy step overwrites the memset
// completely and whatever padding byte was actually in the file (NUL, per the spec, for the
// common unused-slot case) survives untouched into uid()/trackRef()/packRef(). Trimming only
// space left a real, common-case file's unused chna slots coming back as fixed-width strings
// full of embedded NUL bytes rather than the empty string this module's own model.hpp documents
// ("may be empty, §8.2") - trimming both padding characters here covers the documented NUL case
// and any space-padded content without weakening either.
std::string trim_padding(std::string field) {
    static constexpr std::string_view kPaddingChars(" \0", 2);
    const auto last = field.find_last_not_of(kPaddingChars);
    field.resize(last == std::string::npos ? 0 : last + 1);
    return field;
}

std::vector<ChnaEntry> read_chna(const bw64::Bw64Reader& reader) {
    std::vector<ChnaEntry> entries;
    const auto chna_chunk = reader.chnaChunk();
    if (!chna_chunk) {
        return entries;
    }
    for (const auto& audio_id : chna_chunk->audioIds()) {
        ChnaEntry entry;
        entry.track_index = audio_id.trackIndex();
        entry.uid = trim_padding(audio_id.uid());
        entry.track_ref = trim_padding(audio_id.trackRef());
        entry.pack_ref = trim_padding(audio_id.packRef());
        entries.push_back(std::move(entry));
    }
    return entries;
}

// `file_bytes` is the size of the file `reader` was opened on - see the
// frame-count clamp below for why the PCM read needs it.
PcmAudio read_pcm(bw64::Bw64Reader& reader, std::uint64_t file_bytes) {
    PcmAudio audio;
    audio.sample_rate = reader.sampleRate();
    audio.bits_per_sample = reader.bitDepth();
    const auto channel_count = reader.channels();
    if (channel_count == 0) {
        return audio;
    }
    // The block alignment is recomputed here rather than read back through
    // bw64::Bw64Reader::blockAlignment(), because numberOfFrames() divides by
    // it: a <fmt > declaring fewer than 8 bits per channel makes it zero, so
    // asking the reader for a frame count first would be the division that
    // crashes.
    const auto block_align = static_cast<std::uint64_t>(channel_count) * reader.bitDepth() / 8;
    if (block_align == 0) {
        return audio;
    }
    // libbw64's own copy of that same figure is a uint16_t, so a <fmt > whose channel count and
    // sample width multiply past 65,535 wraps it - and WAVE's nBlockAlign field is 16 bits wide
    // as well, so the file's declared value matches the wrapped one and libbw64's own
    // "blockAlignment is X but should be Y" check passes it. Found against the 0.10.0 pin, where
    // that meant 32,768 channels at 16 bits wrapped to 0 (numberOfFrames(), the call immediately
    // below, dividing by it - a SIGFPE, on an uninstrumented build too) and 32,769 wrapped to 2
    // (sizing read()'s buffer at two bytes a frame while its decodePcmSamples call read 65,538 of
    // them - a heap overread the length of a whole frame, which an uninstrumented ac3adm ran as a
    // clean execution and returned as audio). Neither file can be read on its own terms, since
    // the container has no way to state a block alignment this large, so the PCM is left empty
    // the way the two degenerate cases above leave it - a value this comparison can still reach.
    //
    // The pinned fork now guards the same thing one layer further in, and more strictly:
    // FormatInfoChunk::blockAlignment() is utils::safeCast<uint16_t>, which THROWS rather than
    // wraps, and that call happens inside parseFormatInfoChunk's own sanity check - before a
    // Bw64Reader is ever constructed. Both fixtures above now fail the whole open (kCannotOpen)
    // rather than reaching this function at all, confirmed by re-running them after the re-pin
    // (tests/adm/test_adm.cpp's own case for this). This check stays regardless, the same
    // defense-in-depth reasoning as chunk_sizes_fit()'s own comment above parse_bw64_path: it is
    // this project's own code, and does not depend on the pinned dependency continuing to throw
    // here rather than wrap.
    if (reader.blockAlignment() != block_align) {
        return audio;
    }
    // numberOfFrames() is the <data> chunk's DECLARED size over that
    // alignment - what the file claims, not what it holds. A sixty-byte file
    // is free to claim four gigabytes of PCM (or, in RF64, sixteen exabytes
    // through <ds64>), and sizing a buffer straight from that figure is an
    // out-of-memory that any hostile - or merely truncated - file triggers at
    // will. fuzz_adm_parse found exactly that within a minute of first being
    // pointed at this function.
    //
    // The file's own size is the bound: <data> cannot hold more than the file
    // does. It is a generous bound rather than a tight one, since the RIFF
    // header and the fmt/chna/axml chunks all take space away from <data>,
    // but it is finite and it comes from the same bytes the reader was handed
    // rather than from a field inside them. A well-formed file is unaffected:
    // its declared size is at most its real one, so the clamp never binds.
    const auto frame_count = std::min(reader.numberOfFrames(), file_bytes / block_align);
    std::vector<float> interleaved(static_cast<std::size_t>(frame_count) * channel_count);
    reader.seek(0);
    reader.read(interleaved.data(), frame_count);

    audio.channels.assign(channel_count, std::vector<float>(frame_count));
    for (std::uint64_t frame = 0; frame < frame_count; ++frame) {
        for (std::uint16_t channel = 0; channel < channel_count; ++channel) {
            audio.channels[channel][frame] =
                interleaved[static_cast<std::size_t>(frame) * channel_count + channel];
        }
    }
    return audio;
}

// The XML half of read_adm_model below, factored out so the float-PCM
// fallback reader (parse_float_pcm_path) - which never constructs a
// bw64::Bw64Reader at all, because libbw64 refuses to open such a file - runs
// the identical libadm parse over the identical bytes rather than a second,
// subtly different copy of it.
}  // namespace

namespace detail {

std::expected<AdmModel, AdmError> parse_axml(const std::string& xml) {
    std::shared_ptr<adm::Document> document;
    try {
        std::istringstream xml_stream(xml);
        // recursive_node_search: without it, libadm's default parser only accepts
        // <audioFormatExtended> wrapped in the full EBUCore <ebuCoreMain><coreMetadata>
        // <format> structure (BS.2076-2 Annex 2's own worked examples use exactly that
        // wrapper) and throws XmlParsingError("audioFormatExtended node not found") on
        // anything else - confirmed by hitting this directly. Real-world ADM BWF masters
        // are not all produced by tools that add that wrapper; some embed a bare
        // <audioFormatExtended> root instead. Both are the same ADM content once found,
        // so accepting either here (rather than rejecting the bare form) is the more
        // robust choice for a production ingest reader.
        document = adm::parseXml(xml_stream, adm::xml::ParserOptions::recursive_node_search);
    } catch (const adm::error::AdmException&) {
        // Covers every adm::error:: exception libadm defines - AdmException is the common
        // base every one of them derives from (duplicate IDs, an unresolved reference, an
        // invalid enumerated value, the audioFormatExtended root not found, ...). Confirmed
        // reachable in practice for e.g. a duplicate element ID
        // (adm::error::XmlParsingDuplicateId); this is what AdmError::kMalformedAdm means.
        return std::unexpected(AdmError::kMalformedAdm);
    } catch (const std::exception&) {
        // Anything else - genuinely malformed XML (an unterminated tag, say) that never
        // reaches one of libadm's own adm::error:: types, since the lower-level XML
        // tokenizer it uses internally is a private/vendored dependency of libadm's own
        // (never exposed through any public libadm header, so this module cannot catch its
        // exact exception type without reaching past libadm's own public API boundary) -
        // BUT ALSO, confirmed directly rather than assumed, a well-formed document simply
        // missing a mandatory ADM attribute or element: libadm's own mandatory-attribute
        // check (xml_parser_helper.hpp's parseAttribute()) throws a plain, untyped
        // std::runtime_error too, indistinguishable by C++ exception type from a genuine
        // XML syntax error. AdmError::kMalformedXml covers this whole bucket - see its own
        // doc comment in ac3adm.hpp for the same caveat.
        return std::unexpected(AdmError::kMalformedXml);
    }

    return build_adm_model(document);
}

}  // namespace detail

namespace {

std::expected<AdmModel, AdmError> read_adm_model(const bw64::Bw64Reader& reader) {
    const auto axml_chunk = reader.axmlChunk();
    // BS.2088-1 §9 rule 2: ADM metadata is optional - a file with no <axml>
    // chunk at all is still a valid BW64 file, just one with an empty
    // AdmModel (see ac3adm/model.hpp's AdmDocument comment).
    if (!axml_chunk) {
        return AdmModel{};
    }
    // bw64::AxmlChunk has no data()/text() accessor of its own (its raw bytes are
    // private) - write() to a stream is the only public way to get the XML content
    // back out, so that is used here to build the string parse_axml wants.
    std::ostringstream xml_out;
    axml_chunk->write(xml_out);
    return detail::parse_axml(xml_out.str());
}

// A structural pre-check over the top-level chunk table - ids and declared
// lengths only, nothing that duplicates the parsing libbw64 is vendored to
// do.
//
// It exists because libbw64 materialises every chunk it reads EXCEPT <data>
// into a std::vector sized straight from the chunk header (chunks.hpp's
// UnknownChunk does `data_.resize(size); stream.read(...)`, and the axml and
// chna chunks do the same), during readFile() itself - before any ac3forge
// code gets a say. A 99-byte file whose <axml> header claims four gigabytes
// asks for four gigabytes. fuzz_adm_parse reported exactly that, twice, at
// two different chunk ids. On a real system the std::bad_alloc that usually
// follows is caught by parse_bw64_path's own try/catch below and reported as
// kCannotOpen, so this is resource exhaustion rather than memory corruption -
// but a library API handed an untrusted file should not be relying on the
// allocator as its bounds check.
//
// One rule: a chunk whose declared size runs past the end of the file is
// refused, unless it is <data>.
//
// <data> is exempt for a real reason: a recording truncated mid-<data> is an
// ordinary file, libbw64 reads it as far as it goes, and refusing it here
// would break a working case. RF64's 0xFFFFFFFF "resolve through <ds64>"
// escape (BS.2088-1 §4) lands in the same branch, since an escape IS a size
// past the end of the file.
//
// Stopping the walk there was not enough, though, and two of <ds64>'s own
// fields are read on the way past because of it:
//
//   - <data>'s 64-bit size, so the walk can step over an escaped <data> and
//     go on checking the chunks after it. Returning at <data> left those
//     unchecked, while libbw64 - which resolves the same 64-bit size through
//     reader.hpp's getChunkSize64 - stepped over <data> and allocated them.
//     fuzz_adm_parse reached a 1.7 GB <UnknownChunk> that way, behind an
//     RF64 <data> declaring 1.8 GB in its 32-bit header.
//   - <ds64>'s own tableLength, which is refused when the chunk is too short
//     to hold the table it declares. libbw64 reads that many 12-byte entries
//     with no bound of its own (parser.hpp's parseDataSize64Chunk), so a
//     28-byte <ds64> claiming 4.26 billion entries is a loop of 4.26 billion
//     reads against a stream that ended - a hang rather than an allocation,
//     and the first thing mutation found once ac3adm was instrumented.
//
// The table must also END on a chunk boundary, for a reason of the same kind:
// a trailing fragment too short to be a header is one libbw64 reads regardless,
// with the size field left holding whatever was on the stack. See the return
// at the bottom.
//
// Not covered here, deliberately: <ds64>'s table can also carry a 64-bit size for any OTHER
// chunk id, whose own 32-bit header is then perfectly plausible, and libbw64 prefers that value.
// Following it would mean reading the table's entries and not just its length, which this
// function still does not do - but the pinned libbw64 now closes this itself, one layer down: its
// own chunk-header scan resolves every chunk's size through the same table before any chunk is
// materialised, and refuses one that then runs past the real end of the file (patched to still
// allow <data> to, for the same reason this function does - see patch_libbw64.cmake). This
// function stays as an independent check ahead of that rather than being trimmed down to only
// what libbw64 itself does not also catch: it is this project's own code, fuzzed directly, and
// does not depend on a third-party dependency's pin continuing to get this right.
bool chunk_sizes_fit(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return true;  // libbw64 reports the open failure itself
    }
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end < 0) {
        return true;
    }
    const auto file_size = static_cast<std::uint64_t>(end);
    // "RIFF"/"RF64"/"BW64" + 32-bit size + "WAVE": anything shorter is not a
    // chunk table at all, and libbw64's own rejection describes it better.
    constexpr std::uint64_t kRiffHeaderBytes = 12;
    if (file_size < kRiffHeaderBytes) {
        return true;
    }
    // §4's own layout for the chunk <ds64> is: riffSize, dataSize, sampleCount,
    // then tableLength - four fields ahead of the table itself.
    constexpr std::size_t kDs64PrefixBytes = 28;
    constexpr std::uint64_t kDs64TableEntryBytes = 12;
    const auto little_endian = [](const unsigned char* bytes, std::size_t width) {
        std::uint64_t value = 0;
        for (std::size_t byte = 0; byte < width; ++byte) {
            value |= static_cast<std::uint64_t>(bytes[byte]) << (8 * byte);
        }
        return value;
    };

    std::uint64_t offset = kRiffHeaderBytes;
    bool data_size_known = false;
    std::uint64_t ds64_data_size = 0;
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    while (offset + 8 <= file_size) {
        std::array<char, 4> id{};
        std::array<unsigned char, 4> length{};
        in.read(id.data(), 4);
        in.read(reinterpret_cast<char*>(length.data()), 4);
        if (!in) {
            return true;
        }
        offset += 8;
        std::uint64_t declared = little_endian(length.data(), length.size());
        const std::string_view chunk_id(id.data(), id.size());

        // <ds64> is mandatory and first in an RF64/BW64 file, so its own two
        // useful fields are read here, while the stream is sitting on them -
        // see this function's own comment for what each is for.
        if (chunk_id == "ds64" && !data_size_known && declared >= kDs64PrefixBytes &&
            declared <= file_size - offset) {
            std::array<unsigned char, kDs64PrefixBytes> prefix{};
            in.read(reinterpret_cast<char*>(prefix.data()), kDs64PrefixBytes);
            if (!in) {
                return true;
            }
            ds64_data_size = little_endian(prefix.data() + 8, 8);
            data_size_known = true;
            const auto table_length = little_endian(prefix.data() + 24, 4);
            if (declared - kDs64PrefixBytes < table_length * kDs64TableEntryBytes) {
                return false;
            }
        }

        if (declared > file_size - offset) {
            // Past the end of the file: only a trailing <data> can honestly
            // be that, and only <data> is not buffered whole.
            if (chunk_id != "data") {
                return false;
            }
            // Truncated mid-<data>, with no <ds64> saying otherwise: nothing
            // after it is reachable anyway, since libbw64's own walk runs off
            // the end of the file at the same point.
            if (!data_size_known || ds64_data_size > file_size - offset) {
                return true;
            }
            declared = ds64_data_size;  // §4's escape, resolved: keep checking
        }
        offset += declared + (declared % 2);  // §4's pad byte
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    }
    // Landing short of the end leaves between one and seven bytes where
    // libbw64 expects a chunk header, and its own walk reads one anyway: it
    // continues while peek() is not EOF, and parseHeader() takes the id and
    // the 32-bit size through a readValue() that does not check whether the
    // read succeeded (parser/reader.hpp). A partial header leaves that size
    // holding whatever was on the stack, and the chunk is then allocated at
    // that size - fuzz_adm_parse reached malloc(4278190080) from a 19-byte
    // file this way, the first byte of the size being the only part of it
    // that came from the stack rather than the file. A table that ends on a
    // chunk boundary cannot do this; one that ends past the boundary (a final
    // odd-length chunk written without §4's pad byte, which real writers do)
    // is fine too, since libbw64 seeks past the end and stops.
    return offset >= file_size;
}

std::expected<AdmDocument, AdmError> parse_bw64_path(const std::string& path) {
    // Ahead of everything else: an untrusted file's chunk sizes are checked
    // before libbw64's own allocator ever touches them.
    if (!chunk_sizes_fit(path)) {
        return std::unexpected(AdmError::kNotRiff);
    }
    // Integer PCM and IEEE float both go through the same libbw64 read here -
    // see model.hpp's PcmAudio comment for why this module used to need a
    // second, hand-rolled container walk for float and no longer does.
    std::unique_ptr<bw64::Bw64Reader> reader;
    try {
        reader = bw64::readFile(path);
    } catch (const std::exception&) {
        // libbw64 reports "could not open", "malformed container", "unsupported <fmt >
        // formatTag" and "missing fmt/data chunk" all through the same std::runtime_error
        // hierarchy (reader.hpp), with no distinguishing exception type - kCannotOpen covers the
        // whole family here since a caller's next move (check the path/format) is the same
        // either way, and libbw64 does not label a chunk it dislikes clearly enough to justify
        // inventing a false-precision mapping to a more specific AdmError from the exception
        // text alone.
        return std::unexpected(AdmError::kCannotOpen);
    }

    // The bound read_pcm clamps the declared frame count against. Taken from
    // the filesystem rather than from any field in the file, which is the
    // whole point; a stat failure leaves it at zero, which reads no PCM at
    // all rather than trusting the declaration.
    std::error_code size_error;
    const auto file_bytes = std::filesystem::file_size(path, size_error);

    AdmDocument document;
    try {
        document.chna = read_chna(*reader);
        document.audio = read_pcm(*reader, size_error ? 0 : file_bytes);
    } catch (const std::exception&) {
        return std::unexpected(AdmError::kOther);
    }

    auto model = read_adm_model(*reader);
    if (!model) {
        return std::unexpected(model.error());
    }
    document.model = std::move(*model);
    return document;
}

}  // namespace

std::expected<AdmDocument, AdmError> parse_bw64(const std::string& path) {
    return parse_bw64_path(path);
}

std::expected<AdmDocument, AdmError> parse_bw64(std::istream& in) {
    if (!in) {
        return std::unexpected(AdmError::kCannotOpen);
    }
    // See ac3adm.hpp's own comment on this overload: libbw64 opens a file
    // by path internally, so an in-memory/stream source has to be spooled
    // to a real temporary file first.
    const auto temp_path = make_temp_path();
    {
        // noreplace: the system temp directory is shared/world-writable, and make_temp_path()'s
        // name is ours alone to use - fail instead of writing through a symlink another local
        // user pre-planted at this exact (astronomically unlikely to guess) name.
        std::ofstream out(temp_path, std::ios::binary | std::ios::noreplace);
        if (!out) {
            return std::unexpected(AdmError::kCannotOpen);
        }
        out << in.rdbuf();
        if (!out) {
            return std::unexpected(AdmError::kCannotOpen);
        }
    }

    auto result = parse_bw64_path(temp_path.string());
    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);  // best-effort cleanup
    return result;
}

std::string_view describe(AdmWriteError error) {
    switch (error) {
        case AdmWriteError::kInvalidDocument: return "AdmModel has an unresolved reference or an unsupported element type";
        case AdmWriteError::kCannotOpen: return "cannot open path for writing";
        case AdmWriteError::kOther: return "unexpected failure writing the BW64/ADM file";
    }
    return "unknown error";
}

namespace {

std::vector<float> interleave(const PcmAudio& audio) {
    const auto frame_count = audio.frame_count();
    const auto channel_count = audio.channels.size();
    std::vector<float> interleaved(frame_count * channel_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        for (std::size_t channel = 0; channel < channel_count; ++channel) {
            interleaved[frame * channel_count + channel] = audio.channels[channel][frame];
        }
    }
    return interleaved;
}

// Resolves one ChnaEntry's `uid` (an AdmModel-level correlation key, per ac3adm.hpp's own
// write_bw64 doc comment) to the bw64::AudioId it describes: the REAL, reassignIds()-assigned
// AudioTrackUidId, plus the real AudioTrackFormatId/AudioPackFormatId of whichever of the two
// (or neither, for a plain-PCM-shortcut AudioTrackUid) that track uid ended up referencing.
std::expected<bw64::AudioId, AdmWriteError> to_audio_id(
    const ChnaEntry& entry, const std::unordered_map<std::string, std::shared_ptr<adm::AudioTrackUid>>& track_uids_by_key) {
    const auto it = track_uids_by_key.find(entry.uid);
    if (it == track_uids_by_key.end()) {
        return std::unexpected(AdmWriteError::kInvalidDocument);
    }
    const auto& track_uid = it->second;
    std::string track_ref;
    if (const auto track_format = track_uid->getReference<adm::AudioTrackFormat>()) {
        track_ref = adm::formatId(track_format->get<adm::AudioTrackFormatId>());
    }
    std::string pack_ref;
    if (const auto pack_format = track_uid->getReference<adm::AudioPackFormat>()) {
        pack_ref = adm::formatId(pack_format->get<adm::AudioPackFormatId>());
    }
    return bw64::AudioId(entry.track_index, adm::formatId(track_uid->get<adm::AudioTrackUidId>()), track_ref, pack_ref);
}

}  // namespace

std::expected<void, AdmWriteError> write_bw64(const std::string& path, const AdmDocument& document) {
    // kWriteBitDepth goes to both halves of the file from here - every audioTrackUID's bitDepth
    // and, below, the <fmt > chunk - so the two cannot drift apart.
    auto built = detail::build_libadm_document(document.model, kWriteBitDepth);
    if (!built) {
        return std::unexpected(built.error());
    }
    // reassignIds() BEFORE resolving chna: it is the source of every real, final ID this
    // function (and the AudioId rows it builds below) reports - see ac3adm.hpp's own write_bw64
    // doc comment on why the caller's own AdmModel ID strings never appear in the written file.
    adm::reassignIds(built->document);

    std::vector<bw64::AudioId> audio_ids;
    audio_ids.reserve(document.chna.size());
    for (const auto& entry : document.chna) {
        auto audio_id = to_audio_id(entry, built->track_uids_by_key);
        if (!audio_id) {
            return std::unexpected(audio_id.error());
        }
        audio_ids.push_back(std::move(*audio_id));
    }
    auto chna_chunk = std::make_shared<bw64::ChnaChunk>(std::move(audio_ids));

    std::ostringstream xml;
    adm::writeXml(xml, built->document);
    auto axml_chunk = std::make_shared<bw64::AxmlChunk>(xml.str());

    if (document.audio.channels.empty()) {
        return std::unexpected(AdmWriteError::kInvalidDocument);
    }
    try {
        // bw64::Bw64Writer's own constructor takes sampleRate as uint16_t (writer.hpp) - a real
        // limit of libbw64 0.10.0's API, harmless here since it still comfortably covers every
        // AC-3/E-AC-3 rate this project ever decodes (max 48 kHz, well under 65536).
        auto writer = bw64::writeFile(path, static_cast<std::uint16_t>(document.audio.channels.size()),
                                      static_cast<std::uint16_t>(document.audio.sample_rate), kWriteBitDepth, chna_chunk,
                                      axml_chunk);
        auto interleaved = interleave(document.audio);
        writer->write(interleaved.data(), document.audio.frame_count());
        // ~Bw64Writer (writer's destructor, at scope exit) finalizes the file: writes the <axml>
        // chunk queued above, then patches the RIFF/data chunk sizes now that every sample has
        // gone out - the same "close on scope exit" shape ac3::io::WavStreamWriter's own callers
        // rely on elsewhere in this project.
    } catch (const std::exception&) {
        return std::unexpected(AdmWriteError::kOther);
    }
    return {};
}

}  // namespace ac3adm
