#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "mpegts/export.hpp"

// A minimal MPEG-2 Transport Stream (TS) muxer, per ISO/IEC 13818-1 (MPEG-2
// Systems), with the AC-3/Enhanced AC-3 identification DVB defines in ETSI
// EN 300 468 Annex D.
//
// This is a container writer and nothing more: it lays out 188-byte TS
// packets and takes each access unit as opaque bytes. It has NO dependency
// on ac3::forge beyond the caller telling it AC-3 vs. E-AC-3 (AudioCodec,
// below) - which is the point of keeping it a separate library, the same
// shape as matroska::matroska (src/matroska/). A caller muxing E-AC-3 hands
// over whole access units; the module knows nothing about what is inside
// them.
//
// Scope, deliberately narrow for a first, mergeable implementation:
//   - single program: one PAT, one PMT, one elementary stream, all on PIDs
//     the caller can override but that default to values clear of the
//     0x0000-0x001F reserved range (ISO/IEC 13818-1 Table 2-3);
//   - PAT and PMT are re-sent periodically so a receiver tuning in mid-
//     stream does not have to wait for the very first packet - common
//     broadcast practice, not something ISO/IEC 13818-1 itself mandates for
//     a stream this simple;
//   - PCR runs on the audio PID (there is no other PID to carry it), stamped
//     once per access unit - AC-3/E-AC-3's largest possible frame duration
//     (1536 samples at the slowest Annex E fscod2 rate, 16 kHz) is 96 ms,
//     comfortably inside the 100 ms bound §2.7.2 sets for how far apart two
//     PCR values may be, so "once per access unit" is enough on its own
//     without a separate timer;
//   - no video, no other elementary streams, no PID remapping, no seek aids.
// A general-purpose multiplexer is out of scope; this is enough to produce a
// stream a player or `ffprobe` recognizes as one AC-3/E-AC-3 programme.
//
// Broadcast profile: DVB or ATSC, chosen per stream (MuxOptions::profile).
// Both standards register AC-3/E-AC-3 for MPEG-TS carriage, but with
// different, non-interoperable signalling - a different stream_type, and a
// different descriptor registry - so a stream is written to satisfy ONE of
// them, never a bit of each:
//
//   - DVB (BroadcastProfile::kDvb, the default): stream_type 0x06, audio
//     carried as PES private data (ISO/IEC 13818-1 Table 2-34), identified
//     by the AC3_descriptor (tag 0x6A) or enhanced_AC-3_descriptor (tag
//     0x7A) ETSI EN 300 468 Annex D.3/D.5 define. DVB registers no
//     stream_type of its own, so the descriptor tag IS the identification
//     (EN 300 468 D.2, and A/52:2018 Annex A §A3's own note on the
//     two systems' opposite choices).
//   - ATSC (BroadcastProfile::kAtsc): stream_type 0x81 for AC-3 (A/52:2018
//     Annex A §A4.1) or 0x87 for E-AC-3 (Annex G §G3.1),
//     identified by the AC-3_audio_stream_descriptor (tag 0x81, Annex A
//     §A4.3, Table A4.1) or the E-AC-3_audio_descriptor (tag 0xCC,
//     Annex G §G3.5, Table G.1). ATSC chose the stream_type as the
//     unique identification and the descriptor as configuration detail - the
//     mirror image of DVB's choice.
//
// Both profiles' descriptors carry the same underlying A/52 field values
// (bsmod, acmod, dsurmod, bsid, the substream ids in use) in different bit
// layouts, so a caller supplies those values once, as ServiceInfo below, and
// this module maps them onto whichever registry's tables the chosen profile
// uses. That mapping - EN 300 468 Tables D.1-D.5, A/52 Tables A4.2-A4.6 and
// G.2-G.4 - is descriptor syntax, which is this module's job; reading those
// values off the bitstream is ac3::io::scan's, which is why ServiceInfo is
// plain integers and this module still has no dependency on ac3::forge.

namespace mpegts {

enum class AudioCodec : std::uint8_t {
    kAc3,
    kEac3,
    // AC-4 (ETSI TS 103 190). DVB only: EN 300 468 Annex D.7 signals it as
    // stream_type 0x06 plus the AC-4_descriptor - an extension_descriptor
    // (tag 0x7F) whose descriptor_tag_extension is 0x15. ATSC never
    // registered AC-4 for MPEG-2 TS at all (A/342-2 carries it over ATSC
    // 3.0's ROUTE/MMT transports, not 13818-1), so kAtsc plus kAc4 is
    // kInvalidOptions rather than an invented stream_type. The AC-3-shaped
    // ServiceOptions fields (bsid, mainid, components...) describe elements
    // the AC-4 descriptor does not carry and are ignored for this codec.
    kAc4,
};

// Which registry's stream_type and descriptor to write. See the header
// comment above for what each one actually emits.
enum class BroadcastProfile : std::uint8_t {
    kDvb,   // ETSI EN 300 468 Annex D.3/D.5, stream_type 0x06
    kAtsc,  // A/52:2018 Annex A / Annex G, stream_type 0x81 / 0x87
};

// One independent substream other than substream 0, as the descriptors'
// per-substream byte describes it: ETSI EN 300 468 Table D.8 and A/52:2018
// Annex G Table G.4 lay out the same four things in the same four bit
// positions, differing only in what the top bit means (DVB spends it on the
// substream's own mixing-metadata flag, ATSC on a decoding-priority flag)
// and in which service types and channel counts each one's sub-tables allow.
struct SubstreamService {
    // False leaves the descriptor's substreamN_flag clear and omits the
    // byte, which is what both registries require when no independent
    // substream with that id is in the stream.
    bool present = false;
    int bsmod = 0;
    bool bsmod_present = true;
    int acmod = 2;
    bool lfe = false;
    int dsurmod = 0;
    // DVB only (Table D.9): whether this substream carries mixing metadata.
    bool mix_metadata = false;
    // ATSC only (Table G.4): set on the ONE associated substream that has
    // the highest decoding priority when several carry the same service type
    // and language. False means "not highest", or - when no substream sets
    // it - "not specified".
    bool substream_priority = false;
};

// A/52 bitstream field values, exactly as ac3::io::scan reads them off the
// elementary stream, plus the handful of identification values that are an
// authoring decision rather than anything the bitstream carries. Every
// optional descriptor field either derives from one of these or is omitted;
// nothing here is guessed, and a value left at its default omits its own
// field rather than transmitting a made-up one.
//
// The defaults describe an ordinary single-programme main service, which is
// what a default-constructed AudioTrack produces: complete main, stereo, no
// surround-mode indication, no service associations.
struct ServiceInfo {
    // A/52 §5.4.2.2, Table 5.7 - identical in meaning to EN 300 468
    // Table D.4's service type flags and A/52 Table G.2's
    // audio_service_type, which is why one field feeds all three.
    // `bsmod_present` is false for an E-AC-3 stream that never sent
    // infomdate: neither registry has a "not indicated" service type to
    // write, so an absent bsmod falls back to complete main (0), which is
    // both tables' own value for an ordinary programme.
    int bsmod = 0;
    bool bsmod_present = true;
    // §5.4.2.3, Table 5.8 audio coding mode, and whether the LFE
    // channel is on (§5.4.2.10). With `channels` these drive the
    // number-of-channels field both registries carry - EN 300 468 Table D.5,
    // A/52 Table A4.5 (AC-3) and Table G.3 (E-AC-3).
    int acmod = 2;
    bool lfe = false;
    // Channels the stream RENDERS, dependent substreams included - what
    // separates EN 300 468 Table D.5's "> 2 channels" from its "> 5.1
    // channels", and A/52 Table G.3's 0b100 from its 0b101.
    int channels = 2;
    // §5.4.1.3 / E2.3.1.6. Carried as the descriptors' own bsid field
    // in both registries (8 or 6 for AC-3, 16 for E-AC-3).
    int bsid = 8;
    // §5.4.2.8 dsurmod: 0 = not indicated, 1 = not Dolby Surround
    // encoded, 2 = Dolby Surround encoded. Carried verbatim as ATSC's
    // surround_mode (Table A4.4) and, for a 2-channel stream, selects
    // between EN 300 468 Table D.5's 0b010 and 0b011 and A/52 Table G.3's
    // 0b010 and 0b011.
    int dsurmod = 0;
    // AC-3 only: Table 5.18's index into the nominal bit rates, which is
    // exactly ATSC's bit_rate_code (Table A4.3) with its "upper limit rather
    // than exact" msb clear. Neither registry's E-AC-3 descriptor has an
    // equivalent field.
    int bit_rate_code = 0;
    // §5.4.1.1 fscod, carried as ATSC's sample_rate_code (Table A4.2,
    // whose 0/1/2 are fscod's own 48/44.1/32 kHz). DVB's descriptors have no
    // sample-rate field.
    int sample_rate_code = 0;
    // A/52 Annex G §3.5 / EN 300 468 D.5 mixinfoexists. E-AC-3 only.
    bool mix_metadata = false;
    // Bit n set when independent substream n is present (§E2.3.1.2),
    // as ac3::io::ScannedStream::independent_substreams reports it. This is
    // what answers EN 300 468 Table D.5's "elementary stream contains
    // multiple programmes carried in independent substreams", which no
    // single substream's own description can.
    std::uint8_t independent_substreams = 0x01;
    // Independent substreams 1, 2 and 3 (index 0, 1, 2), the only ones
    // either registry names individually.
    std::array<SubstreamService, 3> associated_substreams{};

    // --- authoring decisions, not bitstream fields -------------------------

    // Whether this service is complete enough to present on its own (EN 300
    // 468 Table D.3's full service flag / A/52 Annex A's full_svc / Annex G's
    // full_service_flag). std::nullopt derives it from bsmod where Table D.4
    // and Table G.2 constrain it - CM and E must be full, ME, D and VO must
    // not - and assumes a full service everywhere else, which is what an
    // unqualified visually-impaired, hearing-impaired or commentary service
    // normally is.
    std::optional<bool> full_service = std::nullopt;
    // The main-service number this service either IS or is associated with
    // (0-7). std::nullopt omits mainid entirely rather than claiming service
    // 0: a single-programme mux has no association to describe, and an
    // invented number links the wrong services at a receiver.
    std::optional<int> mainid = std::nullopt;
    // A/52 Table A4.6 priority, transmitted alongside mainid: 1 = primary
    // audio, 2 = other audio, 3 = not specified (0 is reserved). Ignored
    // when mainid is absent, and by DVB, whose descriptors have no priority
    // field of their own.
    int priority = 3;
    // One bit per main service this ASSOCIATED service may be reproduced
    // with (bit 7 = main service 7). std::nullopt omits asvc. Only
    // meaningful for an associated service, i.e. bsmod >= 2.
    std::optional<std::uint8_t> asvc = std::nullopt;
};

enum class MuxError : std::uint8_t {
    kNoFrames,
    kInvalidTrack,    // zero/negative channels, zero sample rate or zero samples_per_frame
    kInvalidOptions,  // PMT PID and audio PID collide, or either collides with PID 0x0000 (PAT)
    kFrameTooLarge,   // a single access unit too large for one PES packet's 16-bit length field
};

[[nodiscard]] MPEGTS_EXPORT std::string_view describe(MuxError error);

struct AudioTrack {
    AudioCodec codec = AudioCodec::kEac3;
    std::uint32_t sample_rate = 48000;
    int channels = 2;
    // Samples one access unit represents, used to place PTS/PCR timestamps.
    // An AC-3 or E-AC-3 access unit is 1536.
    std::uint32_t samples_per_frame = 1536;
    // What the PMT descriptor says about the service this track carries. The
    // default is the ordinary single-programme main service ServiceInfo's
    // own defaults describe.
    ServiceInfo service{};
};

struct MuxOptions {
    // Which registry identifies the stream. DVB by default: it is what every
    // release before this one wrote, so an existing caller's output does not
    // change under it.
    BroadcastProfile profile = BroadcastProfile::kDvb;
    std::uint16_t program_number = 1;
    std::uint16_t transport_stream_id = 1;
    // Chosen clear of the 0x0000-0x001F reserved PID range (ISO/IEC 13818-1
    // Table 2-3) and of each other; mux() reports MuxError::kInvalidOptions
    // if a caller manages to collide them.
    std::uint16_t pmt_pid = 0x1000;
    std::uint16_t audio_pid = 0x0100;
    // How often PAT+PMT repeat, in access units (always sent once before the
    // very first one regardless of this value). Not a conformance
    // requirement for a stream this simple - just how quickly a receiver
    // that tunes in mid-stream finds the programme.
    std::uint32_t psi_repeat_every_au = 20;
};

// Mux access units into a complete .ts, returned as bytes. No file I/O here,
// so this stays testable without touching a disk. Access units arrive as
// views (matroska::mux's own reasoning); the vector-list overload below
// forwards for owned lists.
[[nodiscard]] MPEGTS_EXPORT std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::span<const std::byte>> frames,
    const MuxOptions& options = {});

[[nodiscard]] MPEGTS_EXPORT std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::vector<std::byte>> frames,
    const MuxOptions& options = {});

// Incrementally muxes access units into a transport stream as they arrive -
// matroska::Writer's sibling, for a session whose length is not known up
// front. A transport stream is the born-streamable container: the only
// state that crosses access units is three continuity counters and the
// index the 90 kHz clock derives from, so the bytes push() hands back,
// written in order, are IDENTICAL to what mux() produces for the same
// frames - that equality is this class's contract and its test.
//
// No file I/O, matching matroska::Writer: push() hands back bytes for the
// caller to write. Two shape differences from the Matroska sibling, both
// forced by the format itself: there is no header() (PSI - PAT and PMT -
// repeats every options.psi_repeat_every_au access units and rides inside
// push()'s own bytes, exactly where mux() puts it), and finalize() always
// returns empty (a transport stream has no trailer and no length field to
// patch) - it exists so a caller can treat the two writers uniformly.
class MPEGTS_EXPORT Writer {
   public:
    // Validates the track and options the same way mux() does.
    [[nodiscard]] static std::expected<Writer, MuxError> create(const AudioTrack& track,
                                                                 const MuxOptions& options = {});

    // The TS packets carrying this access unit (PSI first, on the repeat
    // cadence) - never empty on success. Write them in order as they come.
    [[nodiscard]] std::expected<std::vector<std::byte>, MuxError> push(
        std::span<const std::byte> access_unit);

    // Always empty - see the class comment. Safe to call exactly once, or
    // never; nothing is held back.
    [[nodiscard]] std::vector<std::byte> finalize();

    [[nodiscard]] std::size_t frames_written() const { return index_; }

   private:
    Writer(AudioTrack track, MuxOptions options, std::vector<std::byte> pat_section,
           std::vector<std::byte> pmt_section);

    AudioTrack track_;
    MuxOptions options_;
    std::vector<std::byte> pat_section_;
    std::vector<std::byte> pmt_section_;
    std::size_t index_ = 0;
    std::uint8_t pat_cc_ = 0;
    std::uint8_t pmt_cc_ = 0;
    std::uint8_t audio_cc_ = 0;
};

}  // namespace mpegts
