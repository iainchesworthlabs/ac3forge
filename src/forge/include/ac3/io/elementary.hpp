#pragma once

#include <array>
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
};

[[nodiscard]] AC3FORGE_EXPORT std::string_view describe(ScanError error);

// One independent substream other than substream 0, as its own bsi describes
// it. Both MPEG-TS registries carry a byte per such substream saying what
// kind of audio it holds - ETSI EN 300 468 Table D.8 and A/52:2018 Annex G
// Table G.4 - and every field either table needs is read on the same walk
// that already sizes the substream.
//
// The channel description here is the substream's OWN bed (acmod/lfe). A
// non-zero independent substream that brought dependents of its own would
// render wider than that, but working out which dependent belongs to which
// independent is exactly the per-programme model ROADMAP.md's DC5 adds; this
// is the honest subset available before it.
struct SubstreamService {
    bool present = false;
    int bsmod = 0;
    bool bsmod_present = false;
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    // The four Annex G §3.5 mixinfoexists conditions, for THIS
    // substream - see ScannedStream::mix_metadata, which is the same
    // question asked of independent substream 0.
    bool mix_metadata = false;
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
    // Whether bsmod was actually transmitted - see ScannedStream::bsmod_present,
    // the same distinction one level out. Always true for AC-3 (§5.4.2.2 puts
    // it in every syncframe's bsi unconditionally); for E-AC-3 only when
    // infomdate was set, since Annex E moved it into that optional payload.
    bool bsmod_present = false;
    // §5.4.2.8 / §E2.3.2.3 dsurmod: 0 = not indicated, 1 = NOT Dolby Surround
    // encoded, 2 = Dolby Surround encoded. Only transmitted when acmod is
    // 2/0, so 0 for every other layout - which reads identically to "not
    // indicated" either way.
    int dsurmod = 0;
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
    // The four Annex G §3.5 mixinfoexists conditions (pgmscle, extpgmscle,
    // mixdef > 0, paninfoe) - see ScannedStream::mix_metadata, the same
    // question asked one level out. Always false for AC-3, which has no
    // mixing metadata element at all.
    bool mix_metadata = false;

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
    std::vector<std::span<const std::byte>> access_units{};
    // Substreams in the first access unit; always 1 for AC-3.
    std::size_t substreams_per_unit = 0;

    // The raw syntax values below exist for build_codec_config_box() (see
    // ac3/io/dec3.hpp): an ISOBMFF dac3/dec3 box wants bsid/bsmod/bit-rate
    // straight off the bitstream, not just the derived channel summary
    // above, and a container muxer has no business re-deriving them itself.
    // Every one of them is captured from the same first-access-unit walk
    // that fills in acmod/lfe/sample_rate above.

    // A/52 §5.4.1.3 / Annex E §E2.3.1.6.
    int bsid = 0;
    // §5.4.2.2 Table 5.7, or Annex E's infomdate payload (§E2.3.2.1). 0 when
    // the stream never carried it - which for E-AC-3 is the ordinary case,
    // since bsmod rides inside infomdate rather than unconditionally; see
    // bsmod_present below, which is what tells "the stream said complete
    // main" apart from "the stream never said".
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

    // The service granularity below exists for the MPEG-TS PMT descriptors
    // (see mpegts::ServiceInfo): both the DVB AC3/enhanced_AC-3 descriptors
    // (ETSI EN 300 468 Annex D.3/D.5) and the ATSC AC-3/E-AC-3 audio
    // descriptors (A/52:2018 Annex A Table A4.1, Annex G Table G.1) carry
    // optional identification fields whose values come from exactly these
    // bitstream fields, and a muxer has no business re-deriving them. Like
    // bsid/bsmod/bit_rate_code above, every one is captured on the same
    // first-access-unit walk - except independent_substreams, which is a
    // whole-stream observation (see its own comment).

    // Whether bsmod was transmitted at all. Always true for AC-3 (§5.4.2.2
    // puts it in every syncframe's bsi); for E-AC-3 only when infomdate was
    // set, since Annex E moved it into that optional payload (§E2.3.2).
    bool bsmod_present = false;
    // §5.4.2.8 / §E2.3.2.3 dsurmod: 0 = not indicated, 1 = NOT Dolby
    // Surround encoded, 2 = Dolby Surround encoded. Only transmitted when
    // acmod is 2/0 (§5.4.2's own condition), so 0 for every other layout -
    // which reads identically to "not indicated", the value both descriptor
    // registries want in that case anyway.
    int dsurmod = 0;
    // The four conditions A/52 Annex G §3.5 lists for the ATSC descriptor's
    // mixinfoexists bit, and that ETSI EN 300 468 D.5 words as "contains
    // metadata in independent substream 0 to control mixing with another
    // AC-3 or Enhanced AC-3 stream": pgmscle, extpgmscle, mixdef > 0 or
    // paninfoe set in the first independent substream's mixing metadata
    // (Table E1.2). False for AC-3, which has no mixing metadata element.
    bool mix_metadata = false;
    // Bit n set when an independent substream with substreamid n
    // (§E2.3.1.2) appears ANYWHERE in the stream, not only in the first
    // access unit - which is what the descriptors' substream1-3 fields
    // describe ("the E-AC-3 stream contains an additional programme carried
    // in independent substream 1"). 0 for AC-3, which has no substreams;
    // 0b0000'0001 for the ordinary single-programme E-AC-3 stream.
    //
    // This is an OBSERVATION of the substream ids present, deliberately not
    // a change to how access units are grouped - scan() still starts a new
    // access unit at every independent substream regardless of its id (see
    // ROADMAP.md's DC5, which is where that grouping gets fixed and where a
    // real per-programme model belongs).
    std::uint8_t independent_substreams = 0;
    // Independent substreams 1, 2 and 3 (index 0, 1, 2 here) - the ones both
    // MPEG-TS registries can name individually. Substream 0 is not repeated
    // here: it is the stream's main service, already described by acmod/lfe/
    // bsmod/mix_metadata above. Entries whose `present` is false were never
    // seen. Ids 4-7 are legal in the syntax and show up in
    // independent_substreams above, but no descriptor field names them.
    std::array<SubstreamService, 3> associated_substreams{};

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
