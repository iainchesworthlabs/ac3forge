#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

// Reading the shape of an AC-3 or E-AC-3 elementary stream back off the wire.
//
// This is the inverse of the encoder's framing: given a bare byte stream, find
// the access-unit boundaries and work out what the stream actually carries.
// A muxer needs exactly this - a container has to know where packets begin and
// how many channels to declare - and deriving it from the bitstream beats
// asking the caller, who can be wrong.
//
// Both formats put bsid at bit 40, deliberately, so a reader can tell them
// apart before committing to a layout: AC-3 spends its first 40 bits on
// syncword, crc1, fscod and frmsizecod, and E-AC-3 on syncword, strmtyp,
// substreamid, frmsiz, fscod, numblkscod, acmod and lfeon.

namespace ac3::io {

enum class StreamKind : std::uint8_t {
    kAc3,   // bsid <= 10
    kEac3,  // bsid 16 (Annex E)
};

enum class ScanError : std::uint8_t {
    kEmpty,
    kLostSync,
    kUnsupportedBsid,
    kReservedValue,
    kTruncated,
    // A dependent substream with no independent one ahead of it: its channels
    // have nothing to extend and no programme to belong to (E2.3.1.2).
    kOrphanDependent,
};

[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(ScanError error);

// One programme carried by the stream: an independent substream (§E2.3.1.2
// numbers them I0-I7) together with the dependents that extend it, across
// every frame period the stream covers.
//
// Broadcast DD+ uses the extra independent substreams for the services A/52
// §5.4.2.2 names - a second language, an audio description, a commentary -
// each of which is a self-contained programme a receiver picks ONE of. They
// are not layers of one soundfield the way dependents are, so their access
// units must never be concatenated into a single timeline: I0's units and
// I1's units are two parallel sequences, each running at one unit per frame
// period.
struct ScannedProgramme {
    // §E2.3.1.2's substreamid, ascending in the order the programmes first
    // appear. Always 0 for AC-3, which has no substream layer.
    int substreamid = 0;
    Acmod acmod = Acmod::k2_0;  // of this programme's independent substream
    bool lfe = false;
    // Channels this PROGRAMME renders, folding in every dependent's chanmap.
    int channels = 0;
    int bsid = 0;
    // §5.4.2.2's service type, 0 (not indicated) unless infomdate carried
    // one. This is what tells a receiver that a programme is a complete main
    // service (bsmod 0-1) rather than an associated one to be mixed against
    // it (bsmod 2-7) - the whole reason a stream carries more than one.
    int bsmod = 0;
    // Substreams in this programme's first access unit, its independent one
    // included; always 1 for AC-3.
    std::size_t substreams_per_unit = 0;
    // As ScannedStream::oba_complexity_index below, for this programme alone.
    std::optional<int> oba_complexity_index = std::nullopt;
    // One entry per frame period: this programme's independent substream and
    // the dependents that follow it, concatenated exactly as they sit on the
    // wire. Spans point into the caller's buffer, and are NOT contiguous with
    // each other once a second programme is present.
    std::vector<std::span<const std::byte>> access_units{};
};

// One syncframe's bit stream information, read straight off the wire without
// decoding any audio.
//
// This is the bounded, always-affordable half of reading a stream: syncinfo
// plus the whole of bsi (Table 5.2 for AC-3, Table E1.2 for E-AC-3), stopping
// at the first audio block. Everything here is a transmitted field or an
// immediate consequence of one - nothing is derived from the audio, and
// nothing needs the frame to decode, so a frame whose audio a decoder would
// refuse still reports its header truthfully. `ac3cli probe` is built on
// exactly that property; scan() below is the same walk with only the first
// access unit's answers kept.
struct FrameHeader {
    StreamKind kind = StreamKind::kAc3;
    // The whole syncframe, from its sync word: §5.4.1's frame_size_bytes for
    // AC-3, (frmsiz + 1) * 2 for E-AC-3.
    std::size_t bytes = 0;
    int bsid = 0;
    // §5.4.2.1 / Annex E's infomdate payload. 0 ("not indicated") when the
    // frame carried no bsmod at all, matching ScannedStream::bsmod.
    int bsmod = 0;
    SampleRate sample_rate = SampleRate::k48000;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    int dialnorm = 31;
    // §5.4.2.9: std::nullopt where compre was clear, so "no word" and "a word
    // that says unity" stay distinguishable - the same convention
    // DecodedFrame::compr keeps.
    std::optional<std::uint8_t> compr = std::nullopt;
    // Ch2's own pair (§5.4.2.16-18), present only for acmod 1+1.
    std::optional<int> dialnorm2 = std::nullopt;
    std::optional<std::uint8_t> compr2 = std::nullopt;

    // --- E-AC-3 only (Table E1.2) ------------------------------------------
    eac3::StreamType strmtyp = eac3::StreamType::kIndependent;
    int substreamid = 0;
    // §E2.3.1.4. Reported as 0x3 for a reduced-rate frame, which transmits no
    // numblkscod at all and is implicitly six blocks - the same convention the
    // decoder's own Bsi keeps, with `reduced_rate` below saying which of the
    // two produced it.
    int numblkscod = 3;
    // §E2.3.1.3: fscod was 0x3 and the rate came from fscod2 (24/22.05/16 kHz),
    // a case AC-3 has no counterpart for.
    bool reduced_rate = false;
    // §E2.3.1.8: only a dependent substream may carry one.
    std::optional<std::uint16_t> chanmap = std::nullopt;
    // TS 103 420 §8.3.2.2's complexity_index_type_a, when this substream's own
    // addbsi carried the flag - see ScannedStream::oba_complexity_index.
    std::optional<int> oba_complexity_index = std::nullopt;

    // --- AC-3 only ---------------------------------------------------------
    // Table 5.18's index into kBitratesKbps, i.e. frmsizecod >> 1.
    int bit_rate_code = 0;
    // The rate that index names. E-AC-3 has no such field - its rate is
    // whatever `bytes` works out to over the frame's own duration.
    std::uint32_t bitrate_kbps = 0;

    // Full-bandwidth channels plus the LFE, as this syncframe codes them.
    [[nodiscard]] int coded_channels() const {
        return fullbw_channel_count(acmod) + (lfe ? 1 : 0);
    }
};

// Reads the header of the syncframe starting at `at`. `at` must begin with a
// sync word and hold at least the whole of bsi; it may be longer (the rest of
// the stream is fine) - FrameHeader::bytes says where the frame itself ends,
// which is not checked against `at.size()` here because a caller walking a
// stream needs that length in order to do the checking.
[[nodiscard]] AC3FORGE_EXPORT std::expected<FrameHeader, ScanError> read_frame_header(
    std::span<const std::byte> at);

struct ScannedStream {
    StreamKind kind = StreamKind::kAc3;
    SampleRate sample_rate = SampleRate::k48000;
    Acmod acmod = Acmod::k2_0;  // of the first (or only) substream
    bool lfe = false;
    // Channels the stream RENDERS, which for E-AC-3 folds in every dependent
    // substream's chanmap and so is not the bed's channel count.
    int channels = 0;
    // One entry per access unit: an AC-3 syncframe, or an E-AC-3 independent
    // substream together with the dependents that follow it. Spans point into
    // the caller's buffer.
    //
    // THE FIRST PROGRAMME'S units only - identical to
    // programmes.front().access_units, and for a single-programme stream
    // (every stream this encoder produced before ScannedProgramme existed,
    // and effectively all consumer content) that is every access unit there
    // is. A second independent substream's units are a parallel sequence,
    // not later entries here: appending them would hand a muxer or a decoder
    // two programmes spliced into one timeline. Pick a programme out of
    // `programmes` below to get at the others.
    std::vector<std::span<const std::byte>> access_units{};
    // Substreams in the first access unit; always 1 for AC-3.
    std::size_t substreams_per_unit = 0;
    // Every programme the stream carries, in ascending substreamid order and
    // never empty on success. One entry is the ordinary case; §E2.3.1.2 allows
    // up to eight. The scalar summary fields above all describe
    // programmes.front().
    std::vector<ScannedProgramme> programmes{};

    // The raw syntax values below exist for build_codec_config_box() (see
    // ac3/io/dec3.hpp): an ISOBMFF dac3/dec3 box wants bsid/bsmod/bit-rate
    // straight off the bitstream, not just the derived channel summary
    // above, and a container muxer has no business re-deriving them itself.
    // Every one of them is captured from the same first-access-unit walk
    // that fills in acmod/lfe/sample_rate above.

    // A/52 §5.4.1.3 / Annex E §E2.3.1.6.
    int bsid = 0;
    // §5.4.2.1 / Annex E's infomdate payload (0 when infomdate was clear,
    // matching "not indicated" - see Table 5.5's own bsmod semantics).
    int bsmod = 0;
    // AC-3 only: Table 5.18's index into kBitratesKbps (0-18), exactly what
    // AC3SpecificBox's bit_rate_code reports. Meaningless for E-AC-3, which
    // has no equivalent fixed-table field (see build_codec_config_box()).
    int bit_rate_code = 0;

    // TS 103 420 §8.3.1/§8.3.2.2: flag_ec3_extension_type_a and, when it is
    // set, complexity_index_type_a - read out of the first substream's
    // addbsi that carries them (see encoder/eac3_frame.hpp's
    // oba_complexity_index for the write side). This is the only Atmos/JOC
    // marker readable without decoding the EMDF container itself, and what
    // a dec3 box's own Atmos extension echoes verbatim. std::nullopt for a
    // stream that never sets the flag - AC-3 included, since addbsi's
    // object-audio use is E-AC-3 only.
    std::optional<int> oba_complexity_index = std::nullopt;
};

[[nodiscard]] AC3FORGE_EXPORT std::expected<ScannedStream, ScanError> scan(
    std::span<const std::byte> stream);

}  // namespace ac3::io
