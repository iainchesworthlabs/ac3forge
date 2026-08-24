#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "mpegts/export.hpp"

// The read side of mpegts::mux()/mpegts::Writer: pulling one programme's
// audio back out of a transport stream.
//
// A container reader and nothing more, the same way the writer beside it is
// a container writer and nothing more (see mpegts/mpegts.hpp): it locks to
// the packet grid, follows PAT to PMT to an elementary PID, reassembles PES,
// and hands the payloads back as opaque bytes. It links nothing from
// ac3::forge.
//
// WHAT A "FRAME" IS HERE, AND WHY IT DIFFERS FROM THE SIBLINGS. A Matroska
// SimpleBlock and an MP4 sample each hold exactly one access unit, so those
// readers hand back access units. A PES packet does not promise that: it may
// carry one access unit (which is what mux() writes), several, or - with the
// unbounded PES_packet_length broadcast uses - a run of them ending only
// when the next one starts. So this reader hands back PES PAYLOADS, and what
// they concatenate to is the elementary stream. That is exactly what
// ac3::io::scan wants, and re-framing them into access units is its job, not
// this module's: doing it here would mean knowing what an AC-3 syncframe is.
//
// BOTH SIGNALLING PROFILES, unlike the writer. mux() implements DVB
// (stream_type 0x06 plus ETSI EN 300 468 Annex D's AC3_descriptor /
// Enhanced_AC3_descriptor) and says why in its own header. A reader has no
// such luxury: an ATSC broadcast capture or a North American disc rip uses
// stream_type 0x81/0x87 instead, and a third family of streams names the
// codec through a registration_descriptor's 'AC-3'/'EAC3' format_identifier.
// All three are recognised, because all three are what arrives.
//
// PACKET SIZES. 188 bytes is the ISO/IEC 13818-1 packet. A Blu-ray or AVCHD
// rip is M2TS: the same packets with a 4-byte arrival timestamp in front of
// each, so 192. A DVB recording off a satellite card may carry 16 bytes of
// Reed-Solomon parity after each, so 204. The grid size is detected from the
// sync-byte spacing rather than assumed.
//
// UNTRUSTED INPUT. A transport stream is the format most likely to arrive
// damaged - it is designed to be tuned into mid-flight and to survive bit
// errors - so "malformed" is the normal case rather than the exceptional
// one. Every PSI section's CRC is checked before it is believed, a section
// or PES that outruns its declared length is dropped rather than trusted,
// and ReadOptions bounds what may be buffered. fuzz/fuzz_mpegts_demux.cpp
// drives both entry points with arbitrary bytes.

namespace mpegts {

namespace detail {
// Reader's parse state, defined in src/mpegts/src/reader.cpp - a
// namespace-scope type rather than a private nested one so the walker's own
// free functions there can name it.
struct ReaderState;
}  // namespace detail

enum class DemuxError : std::uint8_t {
    kNotTransportStream,  // no 188/192/204-byte sync grid anywhere in the input
    kNoProgramme,         // never found a PAT, or a PMT for the programme it named
    kNoAudioStream,       // the PMT held no AC-3/E-AC-3 elementary stream
    kMalformed,           // a PES or section layout that cannot be parsed
    kLimitExceeded,       // a PES or section beyond ReadOptions
};

[[nodiscard]] MPEGTS_EXPORT std::string_view describe(DemuxError error);

// How the PMT named the codec. Reported rather than resolved because the
// three are not interchangeable claims: an ATSC stream_type is a statement
// about the payload, a DVB descriptor is a statement about a private-data
// PID, and a registration descriptor is a format identifier. A caller
// remuxing back out wants to know which it was.
enum class CodecSignalling : std::uint8_t {
    kAtscStreamType,          // stream_type 0x81 / 0x87
    kDvbDescriptor,           // stream_type 0x06 + AC3_descriptor / Enhanced_AC3_descriptor
    kRegistrationDescriptor,  // format_identifier 'AC-3' / 'EAC3'
};

struct ReadStream {
    std::uint16_t program_number = 0;
    std::uint16_t pmt_pid = 0;
    std::uint16_t elementary_pid = 0;
    std::uint8_t stream_type = 0;
    bool eac3 = false;  // E-AC-3 rather than AC-3, per whichever signalling was found
    CodecSignalling signalling = CodecSignalling::kAtscStreamType;
    // The detected grid: 188 (TS), 192 (M2TS) or 204 (TS with RS parity).
    std::size_t packet_size = 188;
};

struct ReadOptions {
    // The programme to extract. 0 takes the first one whose PMT names an
    // AC-3/E-AC-3 stream, which for a single-programme capture is the only
    // one there is.
    std::uint16_t program_number = 0;
    // The largest PES packet the reader will assemble. PES_packet_length is
    // 16 bits, so a bounded PES cannot exceed 64 KiB - but the UNBOUNDED
    // form (length 0) has no ceiling of its own and ends only at the next
    // payload_unit_start_indicator, which a hostile stream simply never
    // sends. This is that ceiling.
    std::uint32_t max_pes_bytes = 1U << 20;  // 1 MiB
    // The largest PSI section. ISO/IEC 13818-1's own section_length is 12
    // bits, so 4093 bytes of body is the format's ceiling; this exists so
    // the buffer is bounded by a named constant rather than by luck.
    std::uint32_t max_section_bytes = 4096;
    // How far into the input the reader will look for the packet grid before
    // giving up on it being a transport stream at all.
    std::uint32_t max_sync_search_bytes = 1U << 20;  // 1 MiB
};

struct Demuxed {
    ReadStream stream;
    // Each entry is one PES payload, NOT necessarily one access unit - see
    // the header comment. Concatenated, in order, they are the elementary
    // stream.
    //
    // OWNED, unlike the Matroska and MP4 readers' zero-copy results, and
    // that is the format's doing rather than a design choice: a PES packet
    // is sliced across 188-byte packets with headers in between, so its
    // payload is nowhere contiguous in the file and something has to
    // reassemble it. `payloads` views into `storage` below, so the two move
    // together and a caller can keep using the same span-list shape the
    // sibling readers return.
    std::vector<std::vector<std::byte>> storage;
    std::vector<std::span<const std::byte>> payloads;
};

// Reads a complete transport stream held in one buffer.
[[nodiscard]] MPEGTS_EXPORT std::expected<Demuxed, DemuxError> demux(
    std::span<const std::byte> file, const ReadOptions& options = {});

// Incrementally reads payloads out of a transport stream as its bytes
// arrive - mpegts::Writer's mirror image, and the natural shape for the one
// container here that was designed to be read as a stream in the first
// place.
//
// Move-only: the parse state lives behind a pointer.
class MPEGTS_EXPORT Reader {
   public:
    using PayloadFn = std::function<void(std::span<const std::byte>)>;

    explicit Reader(const ReadOptions& options = {});
    Reader(Reader&&) noexcept;
    Reader& operator=(Reader&&) noexcept;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader();

    [[nodiscard]] std::expected<void, DemuxError> push(std::span<const std::byte> chunk,
                                                       const PayloadFn& on_payload);

    // Call once, when the input ends. Unlike the Matroska and MP4 readers'
    // finish(), this one CAN emit: a PES packet with the unbounded length
    // broadcast uses is terminated by the next one starting or by the stream
    // stopping, so the last one in a capture is only complete here.
    [[nodiscard]] std::expected<void, DemuxError> finish(const PayloadFn& on_payload);

    [[nodiscard]] const ReadStream& stream() const;
    [[nodiscard]] bool stream_found() const;
    [[nodiscard]] std::size_t payloads_read() const;

   private:
    std::unique_ptr<detail::ReaderState> state_;
};

}  // namespace mpegts
