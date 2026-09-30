#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/export.hpp"

// IEC 61937 ("S/PDIF burst") packing: an AC-3 or E-AC-3 access unit disguised
// as 16-bit stereo PCM so AV receivers accept it over S/PDIF or HDMI.
//
// AC-3: each burst is exactly 6144 bytes (1536 stereo 16-bit sample frames —
// one AC-3 frame duration at any AC-3 sample rate): the four preamble words
// Pa 0xF872, Pb 0x4E1F, Pc (data type 1 = AC-3, with bsmod in bits 8..10), Pd
// (payload length in BITS), then the frame bytes packed big-endian into
// words, zero-padded to the burst length. Words are emitted little-endian,
// ready for a PCM16 container; byte-exact against FFmpeg's spdif muxer as the
// oracle.
//
// E-AC-3: verified against two independent primary sources (FFmpeg's
// libavformat/spdifenc.c spdif_header_eac3, and Microsoft's own "Representing
// Formats for IEC 61937 Transmissions" — the two agree). The burst is fixed
// at 24576 bytes (4x AC-3's, matching WASAPI's requirement that the carrier
// clock run at 4x the content sample rate for Dolby Digital Plus), Pc is data
// type 0x15 with no extra bits, and Pd is the payload length in BYTES rather
// than bits — unlike AC-3's Pd, the detail most likely to be copied wrong
// from the AC-3 shape. Annex E lets one syncframe cover as few as one of the
// six blocks a burst period spans (numblkscod, Table E2.4), so
// Eac3BurstPacker accumulates consecutive access units until their block
// counts reach six before emitting a burst.
//
// AC-4: written from IEC 61937-14:2017, with IEC 61937-1:2021 (and its 2024
// corrigendum) for the burst format and IEC 61937-2:2021+AMD1:2026 for the
// data type, which is 24 with a subdata type in Pc bits 5 and 6. Each frame
// travels alone in a burst whose repetition period is the frame's duration,
// so the period follows the stream's frame rate instead of being fixed; see
// the AC-4 section below.

namespace ac3::iec61937 {

inline constexpr std::size_t kBurstBytes = 6144;
inline constexpr std::size_t kEac3BurstBytes = 24576;

// IEC 61937-2 Table 2 data types, as Pc bits 0 to 6 carry them: the
// conventional data type in bits 0 to 4 and the subdata type in bits 5 and 6
// (IEC 61937-1 6.1.8.2; IEC 61937-2 4.2). Only the ones this project both
// writes and reads are named; anything else a carrier holds is skipped, not
// decoded.
enum class BurstDataType : std::uint8_t {
    kAc3 = 0x01,
    kEac3 = 0x15,
    // Data type 24 (IEC 61937-14 Table 2). The subdata type sets the link:
    // the base sampling frequency for AC-4 and AC-4 LD, four and sixteen times
    // it for the two high-bit-rate types (clauses 5.3.1, 5.3.3, 5.3.5, 5.3.7).
    kAc4 = 0x18,       // subdata type 0
    kAc4Hbr4 = 0x38,   // 1
    kAc4Hbr16 = 0x58,  // 2
    kAc4Ld = 0x78,     // 3: low delay, at 48 kHz only
};

[[nodiscard]] constexpr bool is_ac4(BurstDataType type) {
    return (static_cast<unsigned>(type) & 0x1FU) == 24U;
}

// "AC-3", "E-AC-3", "AC-4", "AC-4 HBR4", "AC-4 HBR16" or "AC-4 LD".
[[nodiscard]] AC3FORGE_EXPORT std::string_view data_type_name(BurstDataType type);

enum class WrapError : std::uint8_t {
    kNotAFrame,  // missing sync word or truncated header
    // Cannot happen for legal AC-3 sizes; guarded anyway. For AC-4, a frame
    // longer than its burst type allows at its frame rate (IEC 61937-14
    // Tables 9, 15, 21 and 26).
    kFrameTooLarge,
    // AC-4: a base sampling frequency and frame rate the burst type has no
    // repetition period for (AC-4 LD at 25 fps, say).
    kUnsupportedRate,
    // AC-4: a frame whose base sampling frequency is not the stream's first,
    // which would change the link's rate under a receiver locked to it.
    kRateChanged,
};

// Wrap exactly one AC-3 syncframe into one 6144-byte burst.
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<std::byte>, WrapError> wrap_frame(
    std::span<const std::byte> frame);

// Accumulates E-AC-3 access units into IEC 61937 bursts. Feed it whole access
// units (ac3::split_access_units's granularity — the independent substream's
// syncframe plus every dependent's, concatenated exactly as split_access_units
// returns them) rather than lone syncframes: a dependent's channels only
// reach the burst if its bytes are included, and a decoder finds them by the
// same concatenation the elementary stream already uses.
class AC3FORGE_EXPORT Eac3BurstPacker {
   public:
    // Real work, not =default, because Impl below is incomplete here - same
    // reason ac3::io::WavStreamReader's default ctor gives.
    Eac3BurstPacker();
    // Declared (and defined in iec61937.cpp, where Impl below is complete)
    // rather than implicit: a dllexport class generates every implicit
    // special member whether or not called, and the unique_ptr member makes
    // the implicit copy deleted - which is fine - but move-assignment's
    // implicit reset() needs Impl complete, so it cannot stay implicit once
    // Impl is only forward-declared here.
    ~Eac3BurstPacker();
    Eac3BurstPacker(const Eac3BurstPacker&) = delete;
    Eac3BurstPacker& operator=(const Eac3BurstPacker&) = delete;
    Eac3BurstPacker(Eac3BurstPacker&&) noexcept;
    Eac3BurstPacker& operator=(Eac3BurstPacker&&) noexcept;

    // Returns a completed burst once enough access units have accumulated to
    // cover six blocks, or std::nullopt if more are still needed. bsid, fscod
    // and numblkscod are read from the leading (independent) substream's
    // header, which every substream of an access unit shares.
    [[nodiscard]] std::expected<std::optional<std::vector<std::byte>>, WrapError> push(
        std::span<const std::byte> access_unit);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Wrap a whole stream's worth of ALREADY-SPLIT units into one concatenated
// IEC 61937 payload - one AC-3 frame per unit (ac3::split_frames's
// granularity), or one whole E-AC-3 access unit per unit
// (ac3::split_access_units's granularity), matching `eac3`. For a caller
// that already has its frames/access units in hand - e.g. a GUI's freshly
// encoded output - rather than a raw elementary-stream buffer it would
// otherwise have to split itself first. ac3cli's own `spdif`/`play` commands
// split a raw buffer and wrap frame-by-frame instead (see main.cpp); both
// paths bottom out in wrap_frame/Eac3BurstPacker above, so they cannot
// disagree about how a unit becomes a burst.
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<std::byte>, WrapError> wrap_stream(
    std::span<const std::span<const std::byte>> units, bool eac3);

// ---------------------------------------------------------------------------
// AC-4 (IEC 61937-14:2017).
//
// One AC-4 frame to a data-burst, whole and alone: Part 14 Annex A's sync
// frame, a syncword (0xAC40, or 0xAC41 when a CRC word follows the frame), the
// frame's size and the raw_ac4_frame, which is what an .ac4 file holds back to
// back. The burst's repetition period is the frame's duration in IEC 60958
// frames at the link rate (Tables 5, 11, 17 and 23), so it follows the
// stream's frame rate. At 29.97, 59.94 and 119.88 fps a frame is not a whole
// number of IEC 60958 frames, and the periods of five bursts run in the
// sequence Tables 6, 12, 18 and 24 give. Pc bits 8 to 11 carry a code for the
// period (Tables 7, 8, 13, 14, 19, 20 and 25), and Pd the frame's length: in
// bits for AC-4 and AC-4 LD, in bytes for HBR4, and in 8-byte units for
// HBR16, whose payload is zero-padded to a whole unit.
//
// Two readings are taken where the texts leave a choice; iec61937.cpp gives
// the reasons:
//
// - Which frame of a stream is data-burst 0 of a sequence. Part 14 numbers
//   the five without saying. A frame is placed by its phase in the five-frame
//   cycle ETSI TS 103 190-2 clause 5.11 locks the decoder's sample rate
//   converter to, from its sequence_counter, so a stream packed from any frame
//   gives each frame the same period.
// - Pd's unit for AC-4 and AC-4 LD. Part 14 (clauses 5.3.1 and 5.3.7, Tables
//   9 and 26) says bits; IEC 61937-2 Table 2 says bytes. The packer writes
//   bits, and the reader takes either, since the sync frame says its own size.
// ---------------------------------------------------------------------------

// What IEC 61937-14's tables give one AC-4 burst type at one frame rate.
struct Ac4BurstTiming {
    // Pc bits 8 to 11.
    std::uint8_t code = 0;
    // The IEC 60958 frame rate, which is what the link runs at.
    std::uint32_t link_rate_hz = 0;
    // The repetition periods of data-bursts 0 to 4 of a sequence, in IEC 60958
    // frames: all five the same at the integer frame rates.
    std::array<std::uint16_t, 5> periods{};
    // The largest Pd each of those bursts may carry, in the type's own unit.
    std::array<std::uint16_t, 5> max_length{};
};

// The row of `type`'s tables for a stream's fs_index (0 for 44.1 kHz, 1 for
// 48 kHz) and frame_rate_index (ETSI TS 103 190-1 Tables 83 and 84). Nothing
// when the type has none: AC-4 LD outside 100, 119.88 and 120 fps, and every
// type at 44.1 kHz outside frame_rate_index 13, which is the only index Table
// 84 defines there.
[[nodiscard]] AC3FORGE_EXPORT std::optional<Ac4BurstTiming> ac4_burst_timing(BurstDataType type,
                                                                             int fs_index,
                                                                             int frame_rate_index);

// The row a burst's own Pc code names, for a reader, which knows no frame rate.
// Code 13 names the same periods at both base sampling frequencies, and every
// other code only a 48 kHz one, so the code alone decides the period. AC-4 LD's
// code 14 (256 IEC 60958 frames, 187.5 fps) is here though no frame_rate_index
// of TS 103 190-1 V1.4.1 reaches it: Table 83 reserves index 14.
[[nodiscard]] AC3FORGE_EXPORT std::optional<Ac4BurstTiming> ac4_burst_timing_for_code(
    BurstDataType type, int code);

// The smallest of AC-4, AC-4 HBR4 and AC-4 HBR16 that carries sync frames of
// up to `frame_bytes` bytes at this frame rate in every burst of a sequence: the
// burst type a stream's largest frame needs, chosen before the link opens,
// since the link's rate goes with it. Nothing when even HBR16's does not, or
// the rate has no row.
[[nodiscard]] AC3FORGE_EXPORT std::optional<BurstDataType> ac4_burst_type_for(
    std::size_t frame_bytes, int fs_index, int frame_rate_index);

// What the head of an AC-4 sync frame says: its length and the fields of its
// table of contents a packer needs (ETSI TS 103 190-1 4.2.3.1, TS 103 190-2
// 6.2.1.1: bitstream_version, sequence_counter, wait_frames, fs_index,
// frame_rate_index, in that order).
struct Ac4SyncFrame {
    // The whole sync frame: syncword, frame_size, raw_ac4_frame and, with
    // syncword 0xAC41, the CRC word.
    std::size_t bytes = 0;
    bool crc = false;
    int sequence_counter = 0;
    int fs_index = 0;
    int frame_rate_index = 0;
};

// Nothing unless `bytes` is exactly one sync frame whose table of contents
// can be read as far as frame_rate_index.
[[nodiscard]] AC3FORGE_EXPORT std::optional<Ac4SyncFrame> read_ac4_sync_frame(
    std::span<const std::byte> bytes);

// Packs one stream's AC-4 sync frames into data-bursts of one type, a frame to
// a burst. One packer per stream: it remembers the stream's base sampling
// frequency, which sets the link's rate, and the last frame's phase, which a
// frame whose sequence_counter is 0 continues from (TS 103 190-2 5.11).
class AC3FORGE_EXPORT Ac4BurstPacker {
   public:
    explicit Ac4BurstPacker(BurstDataType type = BurstDataType::kAc4);

    // One whole sync frame in, its data-burst out: Pa, Pb, Pc and Pd, then the
    // frame big-endian within little-endian 16-bit words, as wrap_frame writes
    // AC-3, with an odd last byte in the high half of its word and zero in the
    // low (IEC 61937-1 6.1.2), then zeros to the burst's repetition period.
    // The burst is period x 4 bytes long. A type other than the four AC-4 ones
    // refuses everything with kNotAFrame.
    [[nodiscard]] std::expected<std::vector<std::byte>, WrapError> push(
        std::span<const std::byte> sync_frame);

    // What the last successful push() packed.
    struct Packed {
        std::uint16_t pc = 0;
        std::uint16_t pd = 0;
        // In IEC 60958 frames.
        std::uint32_t period = 0;
        // Which data-burst of Tables 6, 12, 18 and 24 it is: 0 to 4.
        int sequence_index = 0;
        // The payload Pd describes: the sync frame, and for HBR16 the zeros
        // that fill its last 8-byte unit.
        std::size_t payload_bytes = 0;
        std::uint32_t link_rate_hz = 0;
    };
    [[nodiscard]] const std::optional<Packed>& last() const { return last_; }
    [[nodiscard]] BurstDataType type() const { return type_; }

   private:
    BurstDataType type_;
    std::optional<int> fs_index_;
    std::optional<int> phase_;
    std::optional<Packed> last_;
};

// A whole stream's sync frames (as ac4::scan finds them in an .ac4 file) as one
// carrier, the AC-4 counterpart of wrap_stream.
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<std::byte>, WrapError> wrap_ac4_stream(
    std::span<const std::span<const std::byte>> frames, BurstDataType type = BurstDataType::kAc4);

// ---------------------------------------------------------------------------
// De-framing: recovering the elementary stream from a burst carrier.
//
// The inverse of everything above, and the only way to check it against
// itself: a wrapped stream that will not unwrap back to the bytes that went
// in is wrong somewhere, and until this existed nothing in the project read a
// burst back. It is also what a capture of a real player's S/PDIF or HDMI
// output needs - that arrives as "PCM" whose 16-bit words are somebody else's
// bursts, and the elementary stream inside is the part worth keeping.
//
// Everything here treats its input as hostile. A burst carrier is by
// definition something that came off a wire or out of a capture device, so no
// length taken from Pd is trusted further than the repetition period allows,
// and a preamble that does not lead to a syncframe is treated as a false
// match to resync past rather than as a fatal error.
// ---------------------------------------------------------------------------

// How a 16-bit IEC 61937 word is laid out in the carrier's bytes.
//
// wrap_frame/Eac3BurstPacker emit little-endian words, because their output is
// destined for a PCM16 WAV; the same bursts observed on the wire, or captured
// by a device that hands over big-endian PCM, put the same words the other way
// round. The two are told apart by the preamble itself rather than guessed at:
// Pa/Pb is 0xF872 0x4E1F, which is the byte string 72 F8 1F 4E little-endian
// and F8 72 4E 1F big-endian, and neither string occurs inside the other.
enum class WordOrder : std::uint8_t {
    kLittleEndian,
    kBigEndian,
};

enum class UnwrapError : std::uint8_t {
    kNoSync,           // no Pa/Pb preamble anywhere in the carrier
    kTruncatedBurst,   // input ended part-way through a burst payload
    kPayloadTooLarge,  // Pd claims more than the repetition period can hold
};

[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(UnwrapError error);

// The repetition period a data type's bursts occupy, in carrier bytes:
// 6144 for AC-3 (1536 sample frames), 24576 for E-AC-3 (6144 of them, the
// 4x carrier). Also the hard cap on how much payload one burst may claim.
// An AC-4 type's period follows the stream's frame rate, and this is the
// longest it has (IEC 61937-14 Tables 5, 11, 17 and 23): 8192 for AC-4, 32768
// for HBR4, 131072 for HBR16 and 1920 for AC-4 LD.
[[nodiscard]] AC3FORGE_EXPORT std::size_t repetition_period(BurstDataType type);

// What one burst's four preamble words said.
struct BurstHeader {
    BurstDataType data_type = BurstDataType::kAc3;
    // Pc bits 8..12. AC-3 carries bsmod here (bits 8..10); E-AC-3 carries
    // nothing and this is 0; AC-4 carries its repetition period's code in bits
    // 8..11 (IEC 61937-14 Tables 7, 8, 13, 14, 19, 20 and 25).
    std::uint8_t data_type_dependent = 0;
    std::uint8_t stream_number = 0;  // Pc bits 13..15
    bool error_flag = false;         // Pc bit 7
    // Pd as it was written, in whichever unit its data type uses.
    std::uint16_t pd = 0;
    // Payload length in ELEMENTARY-STREAM BYTES, whichever unit Pd used:
    // bits for AC-3, bytes for E-AC-3 (see Eac3BurstPacker's own note - it is
    // the detail most often copied wrong between the two). For AC-4 it is the
    // sync frame's own length, which leaves out HBR16's padding.
    std::size_t payload_bytes = 0;
    // Where the burst's Pa starts, counted in carrier bytes from the first
    // byte the reader was given. For a data type whose reference point is bit
    // 0 of Pa, as every AC-4 type's is (IEC 61937-14 Table 2), the distance
    // from one burst's offset to the next's is its repetition period, measured
    // as a receiver would measure it.
    std::uint64_t offset = 0;
};

// Feed carrier bytes in whatever sized chunks the source produces; take
// elementary-stream bytes out. One burst's payload is the largest thing this
// ever holds, so a whole session's memory is bounded by the chunk size plus
// one repetition period however long the capture runs.
class AC3FORGE_EXPORT BurstReader {
   public:
    // Appends every complete burst payload this chunk finished to `out`,
    // in carrier order, as elementary-stream bytes ready to be written as
    // .ac3/.ec3. `out` is never cleared - a caller that is streaming to a
    // file drains it itself between calls.
    //
    // Fails only on a burst whose Pd overruns its repetition period, which
    // is a carrier no decoder could follow either. A preamble that is not
    // followed by a syncframe is a false match inside payload or stuffing,
    // counted in false_syncs() and resynced past.
    [[nodiscard]] std::expected<void, UnwrapError> push(std::span<const std::byte> carrier,
                                                        std::vector<std::byte>& out);

    // No more carrier is coming. Fails if the last burst was cut off
    // mid-payload, which a truncated capture or a half-written file gives.
    [[nodiscard]] std::expected<void, UnwrapError> finish() const;

    [[nodiscard]] std::size_t bursts() const { return bursts_; }
    // Bursts of a data type BurstDataType does not name - another codec's
    // passthrough, or IEC 61937's own null/pause bursts.
    [[nodiscard]] std::size_t skipped_bursts() const { return skipped_bursts_; }
    [[nodiscard]] std::size_t false_syncs() const { return false_syncs_; }
    // Set once the first real burst is read, and unchanged after: a carrier
    // that changed either mid-stream would be a different stream.
    [[nodiscard]] std::optional<BurstDataType> data_type() const { return data_type_; }
    [[nodiscard]] std::optional<WordOrder> word_order() const { return order_; }
    // The last burst's header, for a caller that wants bsmod or the error
    // flag rather than only the payload.
    [[nodiscard]] const std::optional<BurstHeader>& last_header() const { return last_header_; }

   private:
    enum class State : std::uint8_t { kSyncing, kPayload };

    void compact();

    std::vector<std::byte> buffer_;  // carrier bytes not yet resolved
    std::size_t pos_ = 0;            // read cursor into buffer_
    // Carrier bytes compact() has dropped from the front of buffer_, so a
    // position in it plus this is a position in the whole carrier.
    std::uint64_t dropped_ = 0;
    State state_ = State::kSyncing;
    std::size_t payload_needed_ = 0;  // carrier bytes still wanted for this burst
    std::size_t payload_bytes_ = 0;   // elementary bytes this burst yields
    // False while stepping over a burst of a data type this does not decode:
    // the same consume-N-bytes state, with nothing emitted at the end of it.
    bool emitting_ = false;
    // The committed burst's word order, held plainly rather than read back out
    // of order_ below: unpacking happens on a later push() than the header
    // read, and "order_ is engaged whenever emitting_ is true" is an invariant
    // no analyser can see. A value nothing has to check cannot be checked
    // wrongly.
    WordOrder payload_order_ = WordOrder::kLittleEndian;
    std::optional<BurstDataType> data_type_;
    std::optional<WordOrder> order_;
    std::optional<BurstHeader> last_header_;
    std::size_t bursts_ = 0;
    std::size_t skipped_bursts_ = 0;
    std::size_t false_syncs_ = 0;
};

// Batch form, mirroring wrap_stream: every burst in `carrier`, concatenated
// into one elementary stream. For a caller that already holds the whole
// carrier - a test, or a GUI with a file in hand - rather than one streaming
// it. ac3cli's own `unspdif` uses BurstReader directly so that a two-hour
// capture costs the same as a two-second one.
//
// kNoSync means no burst was found at all, which separates "this is ordinary
// PCM" from "this is a carrier with nothing in it we decode".
[[nodiscard]] AC3FORGE_EXPORT std::expected<std::vector<std::byte>, UnwrapError> unwrap_stream(
    std::span<const std::byte> carrier);

// ---------------------------------------------------------------------------
// Capture-side recognition.
// ---------------------------------------------------------------------------

// Carrier bytes from the interleaved float frames a capture backend
// delivers, appended to `out`: the PCM16 words that were divided by 32768 on
// the way in, multiplied back. Only the first two channels are read - IEC
// 61937 is a stereo carrier - and `channels` is the capture's own count.
//
// Exposed rather than kept inside PassthroughDetector because a recorder
// needs the identical conversion once detection has said yes: if the two
// disagreed by a rounding step, a session would detect a bitstream and then
// record a different one.
AC3FORGE_EXPORT void carrier_from_capture(std::span<const float> interleaved,
                                          std::uint16_t channels, std::vector<std::byte>& out);

// Is this capture actually a bitstream?
//
// An endpoint fed IEC 61937 hands its samples over as ordinary PCM, because
// that is what a burst carrier is pretending to be: nothing in the capture
// API says "this is Dolby Digital", and encoding it as if it were audio
// produces a stream of noise. The bursts are recognisable, though - a
// preamble every repetition period, a syncframe behind it - and that is what
// this answers, so a recorder can keep the elementary stream instead.
//
// Fed the same interleaved float frames ac3::audio::Capture delivers. A
// backend that converts int16 to float by dividing by 32768 (which is what
// every backend here does) loses nothing, so the words come back exactly;
// a capture that is genuinely float32-native has been through a mixer and
// its bursts are already destroyed, which shows up here as no detection.
class AC3FORGE_EXPORT PassthroughDetector {
   public:
    // How much carrier to look at before giving up. Two E-AC-3 repetition
    // periods, so even the worst case - starting mid-burst on the longer of
    // the two data types - still contains a whole one. That covers every
    // AC-4 and AC-4 HBR4 period too (at most 8192 and 32768 bytes). AC-4
    // HBR16's run to 131072, but it travels on an eight-channel link, which a
    // capture read two channels at a time never shows whole anyway.
    static constexpr std::size_t kInspectBytes = 2 * kEac3BurstBytes;

    // `channels` is the capture's channel count; only the first two carry a
    // burst, IEC 61937 being a stereo carrier. Cheap once decided: after a
    // verdict either way this does nothing at all.
    void push(std::span<const float> interleaved, std::uint16_t channels);

    // Set once a burst has been both located and confirmed to hold a
    // syncframe. Still nullopt while undecided.
    [[nodiscard]] std::optional<BurstDataType> detected() const { return detected_; }
    // True once kInspectBytes went by without one: this capture is PCM.
    [[nodiscard]] bool decided() const {
        return detected_.has_value() || inspected_ >= kInspectBytes;
    }
    [[nodiscard]] std::optional<WordOrder> word_order() const { return order_; }
    [[nodiscard]] std::size_t inspected_bytes() const { return inspected_; }

    // The carrier bytes seen so far, kept so a recorder that only finds out
    // mid-buffer can still unwrap the bursts it already went past instead of
    // dropping the first fraction of a second. Bounded by kInspectBytes.
    [[nodiscard]] std::span<const std::byte> buffered() const { return buffered_; }
    void clear_buffer() { buffered_.clear(); }

   private:
    std::vector<std::byte> buffered_;
    std::size_t inspected_ = 0;
    std::optional<BurstDataType> detected_;
    std::optional<WordOrder> order_;
};

}  // namespace ac3::iec61937
