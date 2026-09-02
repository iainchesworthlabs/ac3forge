#include "mpegts/reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ts_detail.hpp"

namespace mpegts {

namespace {

using namespace detail;

// The three packet grids in the wild. 188 is ISO/IEC 13818-1's own; 192 is
// M2TS (a Blu-ray/AVCHD rip, each packet prefixed by a 4-byte arrival
// timestamp); 204 is a DVB recording that kept its Reed-Solomon parity. The
// sync byte sits at the START of the 188 and 204 forms and 4 bytes into the
// 192 one, so a grid is identified by where 0x47 repeats, not by file size.
struct Grid {
    std::size_t stride = 0;  // bytes from one packet to the next
    std::size_t offset = 0;  // where the sync byte sits within a stride
};

constexpr std::array<Grid, 3> kGrids{Grid{188, 0}, Grid{192, 4}, Grid{204, 0}};

// How many consecutive packets must line up before the grid is believed.
// Five is enough that a stray 0x47 in payload cannot fake it (the odds of
// four more landing exactly a stride apart are about one in 2^32) and few
// enough that a capture starting mid-packet still locks within one PES.
constexpr int kSyncConfirmations = 5;

[[nodiscard]] std::uint8_t byte_at(std::span<const std::byte> data, std::size_t at) {
    return std::to_integer<std::uint8_t>(data[at]);
}

// Finds the packet grid: the first position where a sync byte repeats at a
// consistent stride. Returns the grid and the absolute offset of the first
// packet's own first byte (which for M2TS is 4 before the sync byte).
struct SyncLock {
    Grid grid;
    std::size_t start = 0;
    bool found = false;
};

[[nodiscard]] SyncLock find_sync(std::span<const std::byte> data, std::size_t limit) {
    const std::size_t search_end = std::min(data.size(), limit);
    for (std::size_t at = 0; at < search_end; ++at) {
        if (byte_at(data, at) != kSyncByte) {
            continue;
        }
        for (const auto grid : kGrids) {
            if (at < grid.offset) {
                continue;
            }
            bool ok = true;
            for (int i = 1; i < kSyncConfirmations; ++i) {
                const std::size_t next = at + (grid.stride * static_cast<std::size_t>(i));
                if (next >= data.size()) {
                    // Fewer than kSyncConfirmations packets are present. A
                    // short file is not a broken one, so accept what lined
                    // up rather than demanding a length the input may not
                    // have - but only if at least two did, which still rules
                    // out a lone stray 0x47.
                    ok = i >= 2;
                    break;
                }
                if (byte_at(data, next) != kSyncByte) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                return SyncLock{grid, at - grid.offset, true};
            }
        }
    }
    return SyncLock{};
}

// --- PSI ---------------------------------------------------------------------

// A section being reassembled across packets. PAT and PMT as this project
// writes them fit in one packet; a real multiplexer's may not, and a PMT
// with a long descriptor loop routinely does not.
struct SectionBuffer {
    std::vector<std::byte> bytes;
    std::size_t want = 0;  // total section length once known, 0 while unknown
    bool collecting = false;

    void reset() {
        bytes.clear();
        want = 0;
        collecting = false;
    }
};

// True once `bytes` holds a whole, CRC-valid section. A section whose CRC
// fails is thrown away rather than parsed: a transport stream is designed to
// survive bit errors, so a damaged PMT is an ordinary event, and believing
// one would mean locking onto a wrong PID for the rest of the file.
[[nodiscard]] bool section_complete(const SectionBuffer& section) {
    return section.want != 0 && section.bytes.size() >= section.want;
}

[[nodiscard]] bool section_crc_ok(std::span<const std::byte> section) {
    // Annex B: the CRC covers the section including its own CRC field, and
    // the result over the whole thing is zero when it matches.
    return section.size() >= 4 && crc32_mpeg2(section) == 0;
}

// ISO/IEC 13818-1 §2.4.4.3: table_id(8), then section_syntax_indicator(1),
// '0'(1), reserved(2), section_length(12) - the length counting from just
// after itself to the end of CRC_32.
[[nodiscard]] std::size_t section_total_length(std::span<const std::byte> head) {
    const std::size_t declared =
        ((static_cast<std::size_t>(byte_at(head, 1)) & 0x0FU) << 8U) | byte_at(head, 2);
    return declared + 3;
}

}  // namespace

std::string_view describe(DemuxError error) {
    switch (error) {
        case DemuxError::kNotTransportStream:
            return "not a transport stream: no 188/192/204-byte sync grid found";
        case DemuxError::kNoProgramme:
            return "no PAT, or no PMT for the programme it named";
        case DemuxError::kNoAudioStream:
            return "the programme map holds no AC-3/E-AC-3 elementary stream";
        case DemuxError::kMalformed:
            return "malformed PES or PSI section layout";
        case DemuxError::kLimitExceeded:
            return "a PES packet or PSI section beyond the reader's limits";
    }
    return "unknown error";
}

namespace detail {

// Everything the walk carries between calls: demux() keeps one on the stack
// and runs it once, Reader owns one across push()es, so both drive the same
// parser.
struct ReaderState {
    ReadOptions options;

    Grid grid{};
    bool synced = false;
    // Bytes consumed looking for the grid, so the search gives up at
    // max_sync_search_bytes across chunks rather than per chunk.
    std::uint64_t searched = 0;

    bool saw_pat = false;
    std::uint16_t pmt_pid = 0;
    std::uint16_t program_number = 0;
    bool have_pmt_pid = false;

    ReadStream stream;
    bool stream_found = false;

    SectionBuffer pat;
    SectionBuffer pmt;

    // The PES packet being reassembled on the audio PID.
    std::vector<std::byte> pes;
    bool pes_open = false;
    // The declared PES_packet_length, or 0 for the unbounded form that ends
    // only at the next payload_unit_start_indicator.
    std::size_t pes_want = 0;

    std::size_t payloads_read = 0;

    // Incremental input only: whole packets are parsed and dropped, so this
    // never holds more than one packet plus one chunk's remainder.
    std::vector<std::byte> buffer;
};

}  // namespace detail

namespace {

using detail::ReaderState;

// Hands one completed PES packet's payload to the caller. The PES header is
// stripped here, not by the caller: where the payload starts is
// PES-syntax, not codec-syntax (ISO/IEC 13818-1 §2.4.3.7).
void emit_pes(ReaderState& s, const Reader::PayloadFn& on_payload) {
    if (!s.pes_open) {
        return;
    }
    s.pes_open = false;
    const auto pes = std::span<const std::byte>{s.pes};
    // packet_start_code_prefix(3) + stream_id(1) + PES_packet_length(2), then
    // for the stream ids that carry one, flags(2) + PES_header_data_length(1).
    constexpr std::size_t kMinPesHeader = 9;
    if (pes.size() < kMinPesHeader) {
        s.pes.clear();
        return;
    }
    const std::size_t header_data_length = byte_at(pes, 8);
    const std::size_t payload_at = kMinPesHeader + header_data_length;
    if (payload_at >= pes.size()) {
        s.pes.clear();
        return;  // a header that swallows its own packet carries nothing
    }
    on_payload(pes.subspan(payload_at));
    ++s.payloads_read;
    s.pes.clear();
}

// Reads a PMT section that has already passed its CRC, looking for the first
// elementary stream this reader can use. Returns false when the section is
// well-formed but names nothing usable, which is not an error - a multi-
// programme stream has PMTs for programmes we do not want.
[[nodiscard]] bool select_from_pmt(std::span<const std::byte> section, ReaderState& s) {
    // table_id(1) + section_length(2) + program_number(2) + version(1) +
    // section_number(1) + last_section_number(1) + PCR_PID(2) +
    // program_info_length(2) = 12, then the descriptor loop.
    if (section.size() < 12 + 4) {
        return false;
    }
    const auto program_number =
        static_cast<std::uint16_t>((byte_at(section, 3) << 8U) | byte_at(section, 4));
    if (s.options.program_number != 0 && program_number != s.options.program_number) {
        return false;
    }
    const std::size_t program_info_length =
        ((static_cast<std::size_t>(byte_at(section, 10)) & 0x0FU) << 8U) | byte_at(section, 11);
    std::size_t at = 12 + program_info_length;
    // The CRC_32 is the last four bytes and is not part of the ES loop.
    const std::size_t loop_end = section.size() - 4;
    if (at > loop_end) {
        return false;
    }

    while (at + 5 <= loop_end) {
        const std::uint8_t stream_type = byte_at(section, at);
        const auto pid = static_cast<std::uint16_t>(
            ((static_cast<std::uint16_t>(byte_at(section, at + 1)) & 0x1FU) << 8U) |
            byte_at(section, at + 2));
        const std::size_t es_info_length =
            ((static_cast<std::size_t>(byte_at(section, at + 3)) & 0x0FU) << 8U) |
            byte_at(section, at + 4);
        const std::size_t descriptors_at = at + 5;
        if (descriptors_at + es_info_length > loop_end) {
            return false;
        }

        bool eac3 = false;
        bool ac4 = false;
        bool matched = false;
        CodecSignalling signalling = CodecSignalling::kAtscStreamType;

        // ATSC names the codec in stream_type itself.
        if (stream_type == kStreamTypeAtscAc3 || stream_type == kStreamTypeAtscEac3) {
            eac3 = stream_type == kStreamTypeAtscEac3;
            matched = true;
            signalling = CodecSignalling::kAtscStreamType;
        }

        // DVB and the registration form both live in the descriptor loop.
        for (std::size_t d = descriptors_at; !matched && d + 2 <= descriptors_at + es_info_length;) {
            const std::uint8_t tag = byte_at(section, d);
            const std::size_t length = byte_at(section, d + 1);
            if (d + 2 + length > descriptors_at + es_info_length) {
                return false;
            }
            if (stream_type == kStreamTypePrivateData &&
                (tag == kTagAc3Descriptor || tag == kTagEnhancedAc3Descriptor)) {
                eac3 = tag == kTagEnhancedAc3Descriptor;
                matched = true;
                signalling = CodecSignalling::kDvbDescriptor;
                break;
            }
            // EN 300 468 Annex D.7: AC-4 is the extension descriptor (0x7F)
            // whose first payload byte - the descriptor_tag_extension - is
            // 0x15. The flag byte after it is configuration this reader does
            // not need; presence is the identification.
            if (stream_type == kStreamTypePrivateData && tag == 0x7F && length >= 1 &&
                byte_at(section, d + 2) == 0x15) {
                ac4 = true;
                matched = true;
                signalling = CodecSignalling::kDvbExtensionDescriptor;
                break;
            }
            if (tag == kTagRegistrationDescriptor && length >= 4) {
                const std::array<std::uint8_t, 4> id{byte_at(section, d + 2),
                                                     byte_at(section, d + 3),
                                                     byte_at(section, d + 4),
                                                     byte_at(section, d + 5)};
                const bool ac3_id = id == std::array<std::uint8_t, 4>{'A', 'C', '-', '3'};
                const bool eac3_id = id == std::array<std::uint8_t, 4>{'E', 'A', 'C', '3'};
                if (ac3_id || eac3_id) {
                    eac3 = eac3_id;
                    matched = true;
                    signalling = CodecSignalling::kRegistrationDescriptor;
                    break;
                }
            }
            d += 2 + length;
        }

        if (matched) {
            s.stream = ReadStream{.program_number = program_number,
                                  .pmt_pid = s.pmt_pid,
                                  .elementary_pid = pid,
                                  .stream_type = stream_type,
                                  .eac3 = eac3,
                                  .ac4 = ac4,
                                  .signalling = signalling,
                                  .packet_size = s.grid.stride};
            s.stream_found = true;
            return true;
        }
        at = descriptors_at + es_info_length;
    }
    return false;
}

// Reads a PAT section that has already passed its CRC. Takes the first
// programme with a non-zero program_number (0 is the network information
// table, not a programme), or the one options.program_number asks for.
void select_from_pat(std::span<const std::byte> section, ReaderState& s) {
    // table_id(1) + section_length(2) + transport_stream_id(2) + version(1) +
    // section_number(1) + last_section_number(1) = 8, then the programme
    // loop, then CRC_32.
    if (section.size() < 8 + 4) {
        return;
    }
    const std::size_t loop_end = section.size() - 4;
    for (std::size_t at = 8; at + 4 <= loop_end; at += 4) {
        const auto program_number =
            static_cast<std::uint16_t>((byte_at(section, at) << 8U) | byte_at(section, at + 1));
        if (program_number == 0) {
            continue;  // network_PID, not a programme
        }
        if (s.options.program_number != 0 && program_number != s.options.program_number) {
            continue;
        }
        s.pmt_pid = static_cast<std::uint16_t>(
            ((static_cast<std::uint16_t>(byte_at(section, at + 2)) & 0x1FU) << 8U) |
            byte_at(section, at + 3));
        s.program_number = program_number;
        s.have_pmt_pid = true;
        return;
    }
}

// Feeds one packet's payload into a PSI section buffer, honouring the
// pointer_field a payload-unit-start packet begins with.
void feed_section(SectionBuffer& section, std::span<const std::byte> payload, bool unit_start,
                  std::uint32_t max_bytes) {
    std::size_t at = 0;
    if (unit_start) {
        if (payload.empty()) {
            return;
        }
        // §2.4.4.1: the pointer_field says how many bytes of a PREVIOUS
        // section's tail come first. This reader only ever wants the section
        // that starts here, so that tail is skipped rather than completed -
        // it belongs to a section whose head arrived before we were looking.
        const std::size_t pointer = byte_at(payload, 0);
        at = 1 + pointer;
        if (at >= payload.size()) {
            return;
        }
        section.reset();
        section.collecting = true;
    }
    if (!section.collecting) {
        return;
    }
    const auto rest = payload.subspan(at);
    section.bytes.insert(section.bytes.end(), rest.begin(), rest.end());
    if (section.want == 0 && section.bytes.size() >= 3) {
        section.want = section_total_length(section.bytes);
        if (section.want > max_bytes) {
            section.reset();
            return;
        }
    }
    if (section.bytes.size() > max_bytes) {
        section.reset();
    }
}

// Parses exactly one 188-byte packet (the timestamp prefix and any RS parity
// having already been stripped by the caller).
std::expected<void, DemuxError> parse_packet(ReaderState& s, std::span<const std::byte> packet,
                                             const Reader::PayloadFn& on_payload) {
    if (packet.size() != kTsPacketSize || byte_at(packet, 0) != kSyncByte) {
        // A packet that is not where the grid says it is: the stream has
        // slipped. Dropping it and carrying on is what a receiver does -
        // resynchronising is the whole point of the grid.
        return {};
    }
    const bool unit_start = (byte_at(packet, 1) & 0x40U) != 0;
    const auto pid = static_cast<std::uint16_t>(
        ((static_cast<std::uint16_t>(byte_at(packet, 1)) & 0x1FU) << 8U) | byte_at(packet, 2));
    if (pid == kNullPid) {
        return {};
    }
    const std::uint8_t adaptation_control = (byte_at(packet, 3) >> 4U) & 0x03U;
    if ((adaptation_control & 0x01U) == 0) {
        return {};  // adaptation field only, no payload
    }
    std::size_t at = 4;
    if ((adaptation_control & 0x02U) != 0) {
        const std::size_t adaptation_length = byte_at(packet, 4);
        at = 5 + adaptation_length;
        if (at > kTsPacketSize) {
            return {};  // a length that runs off the packet: drop it
        }
    }
    if (at >= kTsPacketSize) {
        return {};
    }
    const auto payload = packet.subspan(at);

    if (pid == kPatPid) {
        feed_section(s.pat, payload, unit_start, s.options.max_section_bytes);
        if (section_complete(s.pat)) {
            const auto section = std::span<const std::byte>{s.pat.bytes}.first(s.pat.want);
            if (section_crc_ok(section) && byte_at(section, 0) == 0x00) {
                s.saw_pat = true;
                if (!s.have_pmt_pid) {
                    select_from_pat(section, s);
                }
            }
            s.pat.reset();
        }
        return {};
    }

    if (s.have_pmt_pid && pid == s.pmt_pid && !s.stream_found) {
        feed_section(s.pmt, payload, unit_start, s.options.max_section_bytes);
        if (section_complete(s.pmt)) {
            const auto section = std::span<const std::byte>{s.pmt.bytes}.first(s.pmt.want);
            if (section_crc_ok(section) && byte_at(section, 0) == 0x02) {
                (void)select_from_pmt(section, s);
            }
            s.pmt.reset();
        }
        return {};
    }

    if (!s.stream_found || pid != s.stream.elementary_pid) {
        return {};
    }

    // The audio PID.
    if (unit_start) {
        // A new PES starts here, which is also what ends the previous one
        // when it used the unbounded length form.
        emit_pes(s, on_payload);
        if (payload.size() < 6) {
            return {};
        }
        // packet_start_code_prefix must be 0x000001, or this is not a PES.
        if (byte_at(payload, 0) != 0x00 || byte_at(payload, 1) != 0x00 ||
            byte_at(payload, 2) != 0x01) {
            return {};
        }
        s.pes_want =
            (static_cast<std::size_t>(byte_at(payload, 4)) << 8U) | byte_at(payload, 5);
        // §2.4.3.7: a PES_packet_length of 0 is the unbounded form. It
        // counts from after the length field, so the whole packet is 6 more.
        s.pes_want = s.pes_want == 0 ? 0 : s.pes_want + 6;
        s.pes.clear();
        s.pes_open = true;
    }
    if (!s.pes_open) {
        return {};  // mid-PES bytes with no start seen: we tuned in late
    }
    s.pes.insert(s.pes.end(), payload.begin(), payload.end());
    if (s.pes.size() > s.options.max_pes_bytes) {
        // Only reachable through the unbounded form - a bounded PES cannot
        // declare more than 64 KiB. A stream that never sends another
        // payload_unit_start_indicator would otherwise grow this forever.
        return std::unexpected(DemuxError::kLimitExceeded);
    }
    if (s.pes_want != 0 && s.pes.size() >= s.pes_want) {
        s.pes.resize(s.pes_want);
        emit_pes(s, on_payload);
    }
    return {};
}

// Parses whole packets out of `window`, returning how many bytes it
// consumed. Whatever is left is a partial packet.
std::expected<std::size_t, DemuxError> walk(ReaderState& s, std::span<const std::byte> window,
                                            const Reader::PayloadFn& on_payload) {
    std::size_t at = 0;
    if (!s.synced) {
        const auto remaining_search =
            s.options.max_sync_search_bytes > s.searched
                ? static_cast<std::size_t>(s.options.max_sync_search_bytes - s.searched)
                : std::size_t{0};
        const auto lock = find_sync(window, remaining_search);
        if (!lock.found) {
            // Keep the tail that might still hold the start of a grid, and
            // count the rest against the search budget.
            const std::size_t keep = std::min<std::size_t>(window.size(), 204 * kSyncConfirmations);
            const std::size_t drop = window.size() - keep;
            s.searched += drop;
            if (s.searched >= s.options.max_sync_search_bytes) {
                return std::unexpected(DemuxError::kNotTransportStream);
            }
            return drop;
        }
        s.synced = true;
        s.grid = lock.grid;
        at = lock.start;
    }

    while (at + s.grid.stride <= window.size()) {
        const auto packet = window.subspan(at + s.grid.offset, kTsPacketSize);
        const auto parsed = parse_packet(s, packet, on_payload);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        at += s.grid.stride;
    }
    return at;
}

[[nodiscard]] std::expected<void, DemuxError> finish_verdict(const ReaderState& s) {
    if (!s.synced) {
        return std::unexpected(DemuxError::kNotTransportStream);
    }
    if (!s.saw_pat || !s.have_pmt_pid) {
        return std::unexpected(DemuxError::kNoProgramme);
    }
    if (!s.stream_found) {
        return std::unexpected(DemuxError::kNoAudioStream);
    }
    return {};
}

}  // namespace

std::expected<Demuxed, DemuxError> demux(std::span<const std::byte> file,
                                         const ReadOptions& options) {
    detail::ReaderState s;
    s.options = options;

    Demuxed out;
    // Unlike the Matroska and MP4 readers this cannot be zero-copy: a PES
    // packet is sliced across 188-byte packets with headers in between, so
    // its payload is nowhere contiguous in the file. The reassembly buffer
    // is the only place a whole payload exists - and it is reused per
    // packet, so a span into it would dangle the moment the next one
    // arrives. Copying each out is what makes the returned views outlive the
    // walk; it is the format's cost, not a design choice.
    std::vector<std::vector<std::byte>> owned;
    const auto keep = [&owned](std::span<const std::byte> payload) {
        owned.emplace_back(payload.begin(), payload.end());
    };

    const auto walked = walk(s, file, keep);
    if (!walked) {
        return std::unexpected(walked.error());
    }
    // The last PES of a capture using the unbounded length form is only
    // complete when the input stops.
    emit_pes(s, keep);

    const auto verdict = finish_verdict(s);
    if (!verdict) {
        return std::unexpected(verdict.error());
    }

    out.stream = s.stream;
    out.storage = std::move(owned);
    out.payloads.reserve(out.storage.size());
    for (const auto& payload : out.storage) {
        out.payloads.emplace_back(payload);
    }
    return out;
}

Reader::Reader(const ReadOptions& options) : state_(std::make_unique<detail::ReaderState>()) {
    state_->options = options;
}

Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;
Reader::~Reader() = default;

const ReadStream& Reader::stream() const { return state_->stream; }
bool Reader::stream_found() const { return state_->stream_found; }
std::size_t Reader::payloads_read() const { return state_->payloads_read; }

std::expected<void, DemuxError> Reader::push(std::span<const std::byte> chunk,
                                             const PayloadFn& on_payload) {
    auto& s = *state_;
    s.buffer.insert(s.buffer.end(), chunk.begin(), chunk.end());
    const auto consumed = walk(s, s.buffer, on_payload);
    if (!consumed) {
        return std::unexpected(consumed.error());
    }
    s.buffer.erase(s.buffer.begin(), s.buffer.begin() + static_cast<std::ptrdiff_t>(*consumed));
    return {};
}

std::expected<void, DemuxError> Reader::finish(const PayloadFn& on_payload) {
    auto& s = *state_;
    // This finish() really does emit, unlike the Matroska and MP4 ones: a
    // PES using the unbounded length form ends at the next
    // payload_unit_start_indicator or at end-of-input, and for the last one
    // in a capture that is here.
    emit_pes(s, on_payload);
    return finish_verdict(s);
}

}  // namespace mpegts
