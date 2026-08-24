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
//
// A stream can also be BOTH at once, which is what kAc3CoreEac3Extension
// below is for - see its own comment.

namespace ac3::io {

enum class StreamKind : std::uint8_t {
    kAc3,   // bsid <= 10
    kEac3,  // bsid 16 (Annex E)
    // §E2.3.1.2: "If an AC-3 bit stream is present in the E-AC-3 bit stream,
    // then the AC-3 bit stream shall be processed as an independent substream
    // assigned substream ID 0." A legacy-core delivery takes that literally -
    // each access unit is an AC-3 syncframe (bsid <= 10) carrying the 5.1 bed,
    // immediately followed by one or more E-AC-3 dependent substreams (bsid
    // 16, strmtyp 1) whose chanmap channels replace and extend it per §E3.8.2.
    // There is no Annex E INDEPENDENT substream anywhere in such a stream: the
    // AC-3 frame is the independent substream.
    //
    // Deliberately its own kind rather than folded into either of the two
    // above. It is not kAc3 - the dependents are Annex E syntax an AC-3
    // reader cannot parse, and an AC3SpecificBox cannot describe them. It is
    // not kEac3 either - the access unit does not begin with an Annex E
    // syncframe, so anything that walks substreams by strmtyp would misread
    // the core's crc1 as a stream type. Callers that only handle the two
    // plain kinds should refuse this one explicitly rather than let it fall
    // through a two-way test.
    kAc3CoreEac3Extension,
};

enum class ScanError : std::uint8_t {
    kEmpty,
    kLostSync,
    kUnsupportedBsid,
    kReservedValue,
    kTruncated,
    // Every syncframe parsed, but the way they are arranged is a shape this
    // scanner does not model - an Annex E independent substream following an
    // AC-3 core (§E3.8.4's mixture of programmes), or a dependent with no
    // independent substream ahead of it to extend (including one belonging to
    // a DIFFERENT programme than any seen so far - §E2.3.1.2's own numbering
    // gives a dependent no way to name its parent except by adjacency).
    // Distinct from kUnsupportedBsid, which is about one frame this reader
    // cannot read at all rather than about how readable frames sit together.
    kUnsupportedStructure,
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
    // substream together with the dependents that follow it - or, for
    // kAc3CoreEac3Extension, the AC-3 core together with its dependents, which
    // is the same rule with the core standing in for the independent
    // substream. Spans point into the caller's buffer.
    //
    // THE FIRST PROGRAMME'S units only - identical to
    // programmes.front().access_units, and for a single-programme stream
    // (every stream this encoder produced before ScannedProgramme existed,
    // every kAc3CoreEac3Extension stream, and effectively all consumer
    // content) that is every access unit there is. A second independent
    // substream's units are a parallel sequence, not later entries here:
    // appending them would hand a muxer or a decoder two programmes spliced
    // into one timeline. Pick a programme out of `programmes` below to get
    // at the others.
    std::vector<std::span<const std::byte>> access_units{};
    // Substreams in the first access unit; always 1 for AC-3. The AC-3 core of
    // a kAc3CoreEac3Extension stream counts as one of them, on §E2.3.1.2's own
    // terms - a two-frame core-plus-dependent unit reports 2, not 1.
    std::size_t substreams_per_unit = 0;
    // Every programme the stream carries, in ascending substreamid order and
    // never empty on success. One entry is the ordinary case (always true for
    // AC-3 and kAc3CoreEac3Extension, which have no second independent
    // substream to carry a second one); §E2.3.1.2 allows up to eight for
    // E-AC-3. The scalar summary fields above all describe
    // programmes.front().
    std::vector<ScannedProgramme> programmes{};

    // The raw syntax values below exist for build_codec_config_box() (see
    // ac3/io/dec3.hpp): an ISOBMFF dac3/dec3 box wants bsid/bsmod/bit-rate
    // straight off the bitstream, not just the derived channel summary
    // above, and a container muxer has no business re-deriving them itself.
    // Every one of them is captured from the same first-access-unit walk
    // that fills in acmod/lfe/sample_rate above.
    //
    // For kAc3CoreEac3Extension all three describe the AC-3 CORE, since that
    // is the independent substream. Neither codec-config box has a defined
    // way to say "AC-3 core plus Annex E dependents", so
    // build_codec_config_box() refuses that kind outright rather than emit an
    // AC3SpecificBox that cannot mention the dependents or an EC3SpecificBox
    // whose bsid field would claim a core frame is Annex E syntax.

    // A/52 §5.4.1.3 / Annex E §E2.3.1.6.
    int bsid = 0;
    // §5.4.2.1 / Annex E's infomdate payload (0 when infomdate was clear,
    // matching "not indicated" - see Table 5.5's own bsmod semantics).
    int bsmod = 0;
    // AC-3 only (kAc3CoreEac3Extension's core included): Table 5.18's index
    // into kBitratesKbps (0-18), exactly what AC3SpecificBox's bit_rate_code
    // reports. Meaningless for E-AC-3, which has no equivalent fixed-table
    // field (see build_codec_config_box()).
    int bit_rate_code = 0;

    // TS 103 420 §8.3.1/§8.3.2.2: flag_ec3_extension_type_a and, when it is
    // set, complexity_index_type_a - read out of the first substream's
    // addbsi that carries them (see encoder/eac3_frame.hpp's
    // oba_complexity_index for the write side). This is the only Atmos/JOC
    // marker readable without decoding the EMDF container itself, and what
    // a dec3 box's own Atmos extension echoes verbatim. std::nullopt for a
    // stream that never sets the flag - plain AC-3 always, since addbsi's
    // object-audio use is E-AC-3 only. A kAc3CoreEac3Extension stream can
    // still carry one: the core cannot, but its dependents can, and that is
    // where a legacy-core Atmos delivery actually puts it.
    std::optional<int> oba_complexity_index = std::nullopt;

    // The stream's rendered channel LOCATIONS as one ATSC A/52-2018 Table
    // E2.5 custom-channel-map word: bit 0 (Left) in the most significant bit
    // through bit 15 (LFE) in the least, six of the sixteen naming a PAIR
    // rather than one channel (see ac3::eac3::chanmap). `channels` above is
    // this word's channel count and nothing more - the scan already unions
    // the independent substream's acmod/lfeon with every dependent's own
    // chanmap to compute it (§E3.8.2), so keeping the word itself costs
    // nothing and answers questions a bare count cannot: which locations,
    // not how many.
    //
    // For AC-3 there are no dependents to union, so this is just acmod/lfeon
    // expressed in the same vocabulary. 1+1 (dual mono) has no Table E2.5
    // location at all - Ch1/Ch2 are independent programmes rather than
    // directions - and stands in as Left|Right there, the same placeholder
    // ac3::eac3::chanmap::acmod_map() already uses for the channel count's
    // sake.
    //
    // Written for ac3::io::dash_channel_configuration() (ac3/io/dec3.hpp),
    // whose DASH AudioChannelConfiguration @value IS this word in hex.
    std::uint16_t channel_map = 0;
};

[[nodiscard]] AC3FORGE_EXPORT std::expected<ScannedStream, ScanError> scan(
    std::span<const std::byte> stream);

}  // namespace ac3::io
