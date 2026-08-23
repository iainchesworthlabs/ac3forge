#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

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
};

[[nodiscard]] AC3FORGE_EXPORT std::expected<ScannedStream, ScanError> scan(
    std::span<const std::byte> stream);

}  // namespace ac3::io
