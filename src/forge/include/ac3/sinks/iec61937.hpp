#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
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

namespace ac3::iec61937 {

inline constexpr std::size_t kBurstBytes = 6144;
inline constexpr std::size_t kEac3BurstBytes = 24576;

enum class WrapError : std::uint8_t {
    kNotAFrame,      // missing sync word or truncated header
    kFrameTooLarge,  // cannot happen for legal AC-3 sizes; guarded anyway
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
    // Returns a completed burst once enough access units have accumulated to
    // cover six blocks, or std::nullopt if more are still needed. bsid, fscod
    // and numblkscod are read from the leading (independent) substream's
    // header, which every substream of an access unit shares.
    [[nodiscard]] std::expected<std::optional<std::vector<std::byte>>, WrapError> push(
        std::span<const std::byte> access_unit);

   private:
    std::vector<std::byte> pending_;
    int blocks_pending_ = 0;
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

// IEC 61937-2 Table 2 data types. Only the two this project both writes and
// reads are named; anything else a carrier holds is skipped, not decoded.
enum class BurstDataType : std::uint8_t {
    kAc3 = 0x01,
    kEac3 = 0x15,
};

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
[[nodiscard]] AC3FORGE_EXPORT std::size_t repetition_period(BurstDataType type);

// What one burst's four preamble words said.
struct BurstHeader {
    BurstDataType data_type = BurstDataType::kAc3;
    // Pc bits 8..12. AC-3 carries bsmod here (bits 8..10); E-AC-3 carries
    // nothing and this is 0.
    std::uint8_t data_type_dependent = 0;
    std::uint8_t stream_number = 0;  // Pc bits 13..15
    bool error_flag = false;         // Pc bit 7
    // Payload length in ELEMENTARY-STREAM BYTES, whichever unit Pd used:
    // bits for AC-3, bytes for E-AC-3 (see Eac3BurstPacker's own note - it is
    // the detail most often copied wrong between the two).
    std::size_t payload_bytes = 0;
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
    // Bursts whose data type is neither 0x01 nor 0x15 - another codec's
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
    State state_ = State::kSyncing;
    std::size_t payload_needed_ = 0;  // carrier bytes still wanted for this burst
    std::size_t payload_bytes_ = 0;   // elementary bytes this burst yields
    // False while stepping over a burst of a data type this does not decode:
    // the same consume-N-bytes state, with nothing emitted at the end of it.
    bool emitting_ = false;
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
    // the two data types - still contains a whole one.
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
