#include "mpegts/mpegts.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <optional>

namespace mpegts {

namespace {

using Bytes = std::vector<std::byte>;

constexpr std::size_t kTsPacketSize = 188;
constexpr std::uint8_t kSyncByte = 0x47;
constexpr std::uint16_t kPatPid = 0x0000;

void put_byte(Bytes& out, std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }

void put_be16(Bytes& out, std::uint16_t value) {
    put_byte(out, static_cast<std::uint8_t>(value >> 8));
    put_byte(out, static_cast<std::uint8_t>(value & 0xFF));
}

void put_be32(Bytes& out, std::uint32_t value) {
    put_byte(out, static_cast<std::uint8_t>(value >> 24));
    put_byte(out, static_cast<std::uint8_t>(value >> 16));
    put_byte(out, static_cast<std::uint8_t>(value >> 8));
    put_byte(out, static_cast<std::uint8_t>(value & 0xFF));
}

// ISO/IEC 13818-1 Annex B: the CRC_32 field every PSI section ends with is
// the non-reflected CRC-32/MPEG-2 variant - generator polynomial
// x^32+x^26+x^23+x^22+x^16+x^12+x^11+x^10+x^8+x^7+x^5+x^4+x^2+x+1 (0x04C11DB7),
// initial value all-ones, no output XOR, most-significant-bit-first. This is
// NOT the same algorithm as the reflected CRC-32 (poly 0xEDB88320, e.g.
// zlib/PNG's) that "CRC-32" alone often means - transcribing that instead is
// a real, easy-to-make mistake, so this is self-checked below against the
// standard "123456789" test vector rather than merely trusted.
constexpr std::uint32_t crc32_mpeg2(std::span<const std::byte> data) {
    std::uint32_t crc = 0xFFFF'FFFFu;
    for (const auto b : data) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b)) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000'0000u) ? (crc << 1) ^ 0x04C1'1DB7u : (crc << 1);
        }
    }
    return crc;
}

constexpr std::array<std::byte, 9> kCrc32CheckVector = {
    std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'},
    std::byte{'6'}, std::byte{'7'}, std::byte{'8'}, std::byte{'9'}};
static_assert(crc32_mpeg2(kCrc32CheckVector) == 0x0376'E6E7u,
              "CRC-32/MPEG-2 self-check against the standard \"123456789\" test vector failed - "
              "a wrong polynomial or bit order here corrupts every PAT/PMT section silently, "
              "since nothing but a real demuxer's CRC check would ever notice.");

// --- PSI section builders ---------------------------------------------------
// Table/section field names and widths throughout follow ISO/IEC 13818-1
// §2.4.4 (program_association_section, Table 2-30) and §2.4.4.8
// (TS_program_map_section, Table 2-33).

Bytes build_pat_section(std::uint16_t transport_stream_id, std::uint16_t program_number,
                        std::uint16_t pmt_pid) {
    Bytes body;
    put_be16(body, transport_stream_id);
    // reserved(2)='11', version_number(5)=0, current_next_indicator(1)=1 -
    // version_number never changes here because this module never rewrites
    // a PAT/PMT mid-stream, so 0 for the life of the file is correct, not a
    // placeholder.
    put_byte(body, 0xC1);
    put_byte(body, 0x00);  // section_number
    put_byte(body, 0x00);  // last_section_number
    put_be16(body, program_number);
    // reserved(3)='111', program_map_PID(13).
    put_be16(body, static_cast<std::uint16_t>(0xE000u | (pmt_pid & 0x1FFFu)));

    Bytes section;
    put_byte(section, 0x00);  // table_id: program_association_section
    // section_syntax_indicator(1)=1, '0'(1), reserved(2)='11', then the
    // 12-bit section_length counting everything from here to the end of
    // CRC_32 inclusive.
    const auto section_length = static_cast<std::uint16_t>(body.size() + 4);
    put_be16(section, static_cast<std::uint16_t>(0xB000u | (section_length & 0x0FFFu)));
    section.insert(section.end(), body.begin(), body.end());
    put_be32(section, crc32_mpeg2(section));
    return section;
}

// --- PMT identification: stream_type + descriptor ---------------------------
//
// Everything below exists twice, once per registry, because ATSC and DVB
// identify the very same elementary stream differently - see mpegts.hpp's
// header comment for the split and why a stream commits to one of them. The
// underlying A/52 field values arrive once, as ServiceInfo, and each
// registry's own tables map them onto its own bit layout.

// ISO/IEC 13818-1 Table 2-34: 0x06 is "ITU-T Rec. H.222.0 | ISO/IEC 13818-1
// PES packets containing private data" - not an MPEG audio stream_type at
// all. This is deliberate: DVB does not register a stream_type of its own
// for AC-3/E-AC-3 (unlike ATSC's 0x81/0x87 below), so a DVB-conformant
// demuxer is expected to find 0x06 plus one of the DVB descriptors in the
// PMT and treat the PID as AC-3/E-AC-3 on that basis alone (ETSI EN 300 468
// D.2/D.4, and A/52:2018 Annex A §A3's own note on the two systems'
// opposite choices).
constexpr std::uint8_t kStreamTypePrivateData = 0x06;
// A/52:2018 Annex A §A4.1: "The value of stream_type for AC-3 shall be
// 0x81."
constexpr std::uint8_t kStreamTypeAtscAc3 = 0x81;
// A/52:2018 Annex G §G3.1: "E-AC-3 bit streams shall be identified
// with a stream_type value of 0x87 when transmitted as PES streams
// conforming to ATSC-published standards."
constexpr std::uint8_t kStreamTypeAtscEac3 = 0x87;

// ISO/IEC 13818-1 Table 2-22's audio stream_id range ('110x xxxx') does not
// apply to either profile: both put the audio in PES private data, which
// Table 2-19 puts under stream_id 0xBD (private_stream_1) - DVB by way of
// stream_type 0x06, ATSC by saying so outright (A/52 Annex A §A4.2 and
// Annex G §G3.2, both "shall be 0xBD (indicating private_stream_1)").
// Unlike a DVD-Video private_stream_1, neither carries an extra sub-stream-id
// byte in front of the payload (that convention is specific to DVD-Video's
// own multiplexing rules) - the PES payload here is the raw AC-3/E-AC-3
// access unit, nothing else.
constexpr std::uint8_t kPesStreamIdPrivateStream1 = 0xBD;

// ETSI EN 300 468 Annex D.2, Table D.6.
constexpr std::uint8_t kTagDvbAc3Descriptor = 0x6A;
// ETSI EN 300 468 Annex D.4, Table D.7.
constexpr std::uint8_t kTagDvbEnhancedAc3Descriptor = 0x7A;
// A/52:2018 Annex A §A4.3, Table A4.1: "The value for the AC-3
// descriptor tag is 0x81."
constexpr std::uint8_t kTagAtscAc3Descriptor = 0x81;
// A/52:2018 Annex G §G3.5, Table G.1: "The value assigned to the
// E-AC-3_audio_descriptor() tag is 0xCC."
constexpr std::uint8_t kTagAtscEac3Descriptor = 0xCC;

// A/52 Table 5.7's bsmod values are, field for field, EN 300 468 Table D.4's
// service type flags and A/52 Table G.2's audio_service_type - the same eight
// service kinds in the same eight codes - so one accessor feeds all three.
// A stream that never transmitted bsmod (an E-AC-3 one with no infomdate)
// has no "not indicated" code to fall back on in either table, so it reads
// as complete main, which is what an ordinary programme is.
[[nodiscard]] int service_type(const ServiceInfo& s) {
    return s.bsmod_present ? (s.bsmod & 0x7) : 0;
}
[[nodiscard]] int service_type(const SubstreamService& s) {
    return s.bsmod_present ? (s.bsmod & 0x7) : 0;
}

// A/52 Table 5.7 splits bsmod 0b111 by acmod: 1/0 (acmod 0b001) is voice
// over, anything wider is karaoke. Both registries inherit the split - it is
// why Table D.4 and Table G.2 each list 0b111 twice, with opposite
// full-service restrictions.
[[nodiscard]] bool is_voiceover(int bsmod, int acmod) { return bsmod == 0x7 && acmod == 0x1; }

// EN 300 468 Table D.4 and A/52 Table G.2 pin the full-service flag for four
// of the eight service types and leave the rest to the author. Where they
// pin it, honour them; where they do not, an unqualified visually-impaired,
// hearing-impaired or commentary service is normally complete on its own.
[[nodiscard]] bool full_service(int bsmod, int acmod, std::optional<bool> override_value) {
    if (override_value) {
        return *override_value;
    }
    switch (bsmod) {
        case 0x0: return true;   // CM   - must be set to 1
        case 0x1: return false;  // ME   - must be set to 0
        case 0x4: return false;  // D    - must be set to 0
        case 0x6: return true;   // E    - must be set to 1
        case 0x7: return !is_voiceover(bsmod, acmod);  // VO must be 0, karaoke must be 1
        default: return true;    // VI, HI, C - unconstrained
    }
}

// The number-of-channels flags. EN 300 468 Table D.5, A/52 Table G.3 and
// (for the per-substream byte) Tables D.10 and G.6 are the same ladder with
// different ceilings, so one function walks it and the caller says which
// rungs its own table actually has.
//
// `channels` is the RENDERED count, dependent substreams folded in, which is
// the only thing that separates "> 2 channels" from "> 5.1 channels" - the
// bed's own acmod cannot, since a 5.1 bed with two height dependents still
// reads acmod 3/2.
struct ChannelFlagLimits {
    bool wide = false;      // 0b101 available (> 5.1 / > 3/2 + LFE)
    bool programmes = false;  // 0b110 available (multiple independent substreams)
    bool dual_mono = true;  // 0b001 available - reserved in A/52 Table G.6
};

[[nodiscard]] std::optional<int> channel_flags(int acmod, bool lfe, int channels, int dsurmod,
                                               bool multiple_programmes,
                                               const ChannelFlagLimits& limits) {
    if (multiple_programmes && limits.programmes) {
        return 0x6;
    }
    if (acmod == 0x0) {  // 1+1
        return limits.dual_mono ? std::optional<int>{0x1} : std::nullopt;
    }
    if (acmod == 0x1) {  // 1/0
        return 0x0;
    }
    if (acmod == 0x2 && !lfe) {
        // Both tables' own note: 0b011 when dsurmod says Dolby Surround
        // encoded, 0b010 for any other value or none at all.
        return dsurmod == 0x2 ? 0x3 : 0x2;
    }
    if (limits.wide && channels > 6) {
        return 0x5;
    }
    return 0x4;
}

void put_descriptor_header(Bytes& d, std::uint8_t tag, std::size_t payload_bytes) {
    put_byte(d, tag);
    put_byte(d, static_cast<std::uint8_t>(payload_bytes));
}

// ETSI EN 300 468 Table D.1: b7 Enhanced AC-3 flag (Table D.2), b6 full
// service flag (Table D.3), b5-b3 service type flags (Table D.4), b2-b0
// number of channels flags (Table D.5). Also the layout of the AC-3
// component_descriptor's own component_type, which is where D.3/D.5 send a
// reader for these fields' meaning.
[[nodiscard]] std::optional<std::uint8_t> dvb_component_type(const ServiceInfo& s, bool eac3) {
    const int svc = service_type(s);
    const bool multiple = eac3 && (s.independent_substreams & 0xFEu) != 0;
    const auto flags = channel_flags(s.acmod, s.lfe, s.channels, s.dsurmod, multiple,
                                     ChannelFlagLimits{.wide = eac3, .programmes = eac3});
    if (!flags) {
        return std::nullopt;
    }
    const unsigned value = (eac3 ? 0x80u : 0x00u) |
                           (full_service(svc, s.acmod, s.full_service) ? 0x40u : 0x00u) |
                           (static_cast<unsigned>(svc) << 3) | static_cast<unsigned>(*flags);
    return static_cast<std::uint8_t>(value);
}

// ETSI EN 300 468 Table D.8: b7 mixing metadata flag (Table D.9), b6 full
// service flag (Table D.3), b5-b3 service type flags (Table D.4), b2-b0
// number of channels flags (Table D.10 - Table D.5 without its "multiple
// programmes" value, which describes a whole stream rather than one
// substream).
[[nodiscard]] std::optional<std::uint8_t> dvb_substream_byte(const SubstreamService& s) {
    const int svc = service_type(s);
    // A substream's own bed is all its acmod describes, so no "> 5.1" rung.
    const auto flags = channel_flags(s.acmod, s.lfe, /*channels=*/0, s.dsurmod,
                                     /*multiple_programmes=*/false, ChannelFlagLimits{});
    if (!flags) {
        return std::nullopt;
    }
    const unsigned value = (s.mix_metadata ? 0x80u : 0x00u) |
                           (full_service(svc, s.acmod, std::nullopt) ? 0x40u : 0x00u) |
                           (static_cast<unsigned>(svc) << 3) | static_cast<unsigned>(*flags);
    return static_cast<std::uint8_t>(value);
}

// A/52:2018 Annex G Table G.4: b7 reserved ('1'), b6 substream_priority,
// b5-b3 audio service type flags (Table G.5), b2-b0 number of channels flags
// (Table G.6).
//
// Both of those sub-tables are narrower than their DVB counterparts, and
// deliberately so - a substream 1-3 carries an ASSOCIATED service, so
// Table G.5 reserves complete main (0b000) and emergency (0b110), and
// Table G.6 reserves 1+1 (0b001) along with everything above 3/2 + LFE.
// A substream this project cannot describe inside those limits gets its
// field OMITTED (and its flag left clear) rather than a reserved or
// approximated value: a receiver already handles an absent optional field,
// where a reserved one it "may ignore" tells it something untrue about the
// stream. The alternative reading - that substream1_flag "shall be included"
// whenever the substream exists - would require transmitting exactly such a
// value, which is the worse of the two.
[[nodiscard]] std::optional<std::uint8_t> atsc_substream_byte(const SubstreamService& s) {
    const int svc = service_type(s);
    if (svc == 0x0 || svc == 0x6) {
        return std::nullopt;  // Table G.5: both reserved for a substream
    }
    const auto flags = channel_flags(s.acmod, s.lfe, /*channels=*/0, s.dsurmod,
                                     /*multiple_programmes=*/false,
                                     ChannelFlagLimits{.dual_mono = false});
    if (!flags) {
        return std::nullopt;  // Table G.6 reserves 1+1
    }
    const unsigned value = 0x80u | (s.substream_priority ? 0x40u : 0x00u) |
                           (static_cast<unsigned>(svc) << 3) | static_cast<unsigned>(*flags);
    return static_cast<std::uint8_t>(value);
}

// ETSI EN 300 468 Table D.6. The four flags are mandatory; each optional
// field that follows is present exactly when its flag is set. bsid's three
// most significant bits "should always be set to 0b000", the five least
// significant carrying the elementary stream's own bsid.
Bytes build_dvb_ac3_descriptor(const ServiceInfo& s) {
    const auto component_type = dvb_component_type(s, /*eac3=*/false);
    Bytes body;
    unsigned flags = 0;
    if (component_type) {
        flags |= 0x80u;
    }
    flags |= 0x40u;  // bsid_flag
    if (s.mainid) {
        flags |= 0x20u;
    }
    if (s.asvc) {
        flags |= 0x10u;
    }
    // reserved_flags (b3-b0) "should always be set to 0b0".
    put_byte(body, static_cast<std::uint8_t>(flags));
    if (component_type) {
        put_byte(body, *component_type);
    }
    put_byte(body, static_cast<std::uint8_t>(s.bsid & 0x1F));
    if (s.mainid) {
        put_byte(body, static_cast<std::uint8_t>(*s.mainid & 0x7));
    }
    if (s.asvc) {
        put_byte(body, *s.asvc);
    }

    Bytes d;
    put_descriptor_header(d, kTagDvbAc3Descriptor, body.size());
    d.insert(d.end(), body.begin(), body.end());
    return d;
}

// ETSI EN 300 468 Table D.7. Same shape as the AC-3 descriptor with four more
// flags in the same byte - mixinfoexists, which unlike its neighbours is a
// plain value bit and not a presence flag, and substream1-3, which are.
Bytes build_dvb_eac3_descriptor(const ServiceInfo& s) {
    const auto component_type = dvb_component_type(s, /*eac3=*/true);
    std::array<std::optional<std::uint8_t>, 3> substreams{};
    for (std::size_t i = 0; i < substreams.size(); ++i) {
        if (s.associated_substreams[i].present) {
            substreams[i] = dvb_substream_byte(s.associated_substreams[i]);
        }
    }

    Bytes body;
    unsigned flags = 0;
    if (component_type) {
        flags |= 0x80u;
    }
    flags |= 0x40u;  // bsid_flag
    if (s.mainid) {
        flags |= 0x20u;
    }
    if (s.asvc) {
        flags |= 0x10u;
    }
    if (s.mix_metadata) {
        flags |= 0x08u;  // mixinfoexists
    }
    for (std::size_t i = 0; i < substreams.size(); ++i) {
        if (substreams[i]) {
            flags |= 0x04u >> i;  // substream1_flag, substream2_flag, substream3_flag
        }
    }
    put_byte(body, static_cast<std::uint8_t>(flags));
    if (component_type) {
        put_byte(body, *component_type);
    }
    put_byte(body, static_cast<std::uint8_t>(s.bsid & 0x1F));
    if (s.mainid) {
        put_byte(body, static_cast<std::uint8_t>(*s.mainid & 0x7));
    }
    if (s.asvc) {
        put_byte(body, *s.asvc);
    }
    for (const auto& substream : substreams) {
        if (substream) {
            put_byte(body, *substream);
        }
    }

    Bytes d;
    put_descriptor_header(d, kTagDvbEnhancedAc3Descriptor, body.size());
    d.insert(d.end(), body.begin(), body.end());
    return d;
}

// A/52:2018 Annex A Table A4.1. Unlike DVB's flag-gated layout this one is a
// fixed prefix followed by conditional fields, and "horizontal lines in the
// table indicate allowable termination points": the first of those sits
// immediately before langcod, which the standard states outright ("This
// field is immediately after the first allowed termination point in the
// descriptor").
//
// So the three-byte form below - sample_rate_code/bsid, bit_rate_code/
// surround_mode, bsmod/num_channels/full_svc - is a complete, conformant
// descriptor on its own, and it is what this module writes unless the caller
// supplied a service association. It can only be extended past that point by
// also transmitting langcod (deprecated, "shall be set to 0xFF") and then
// the whole mainid/priority or asvcflags branch, so the extended form is
// written only when there is a real association to put in it - never to pad
// out a longer descriptor with an invented main-service number.
Bytes build_atsc_ac3_descriptor(const ServiceInfo& s) {
    const int svc = service_type(s);
    const int acmod = s.acmod & 0x7;
    // Table A4.5: with the msb clear the low three bits ARE acmod, which is
    // strictly more informative than the "maximum number of channels" form
    // the msb-set half of the table offers, and always available here.
    const unsigned num_channels = static_cast<unsigned>(acmod);
    // Table A4.1 branches on `bsmod < 2` literally, not on "is this a main
    // service" - so bsmod 0b111 with acmod > 1, which Table 5.7 calls a MAIN
    // audio service (karaoke), still takes the asvcflags branch. Following
    // the standard's own condition rather than the semantics it usually
    // implies is deliberate: a receiver parses this descriptor by that
    // condition, so anything else desynchronises it.
    const bool main_service = svc < 0x2;
    const bool extended = main_service ? s.mainid.has_value() : s.asvc.has_value();

    Bytes body;
    // Table A4.2's sample_rate_code 0b000/0b001/0b010 are fscod's own 48,
    // 44.1 and 32 kHz; its "one of these two/three" values exist for an
    // announcement of a future stream, which this is not.
    put_byte(body, static_cast<std::uint8_t>(((s.sample_rate_code & 0x7) << 5) | (s.bsid & 0x1F)));
    // Table A4.3: the low five bits index the nominal rates and the msb says
    // "upper limit" rather than "exact". A muxer wrapping a real stream
    // knows the exact rate, so the msb stays clear.
    put_byte(body,
             static_cast<std::uint8_t>(((s.bit_rate_code & 0x3F) << 2) | (s.dsurmod & 0x3)));
    put_byte(body, static_cast<std::uint8_t>((static_cast<unsigned>(svc) << 5) |
                                             (num_channels << 1) |
                                             (full_service(svc, acmod, s.full_service) ? 1u : 0u)));
    if (extended) {
        put_byte(body, 0xFF);  // langcod, deprecated: "shall be set to 0xFF"
        if (num_channels == 0) {
            put_byte(body, 0xFF);  // langcod2, same deprecation
        }
        if (main_service) {
            // Table A4.6 priority, then three reserved bits set to '111'.
            put_byte(body, static_cast<std::uint8_t>(
                               ((static_cast<unsigned>(*s.mainid) & 0x7u) << 5) |
                               ((static_cast<unsigned>(s.priority) & 0x3u) << 3) | 0x7u));
        } else {
            put_byte(body, *s.asvc);
        }
        // textlen = 0 with text_code = 1 (ISO Latin-1): no descriptive text,
        // and the encoding bit still has to say something.
        put_byte(body, 0x01);
        // language_flag = 0, language_flag_2 = 0, reserved = '111111'. Past
        // the mainid/asvcflags branch the syntax is unconditional to here, so
        // the extended form runs to the end of the defined structure rather
        // than stopping at a termination point this module cannot confirm.
        put_byte(body, 0x3F);
    }

    Bytes d;
    put_descriptor_header(d, kTagAtscAc3Descriptor, body.size());
    d.insert(d.end(), body.begin(), body.end());
    return d;
}

// A/52:2018 Annex G Table G.1. Two flag/value bytes, then - once anything
// optional follows - a third carrying the language flags and bsid, whose
// five bits are zero_bits when bsid_flag is clear rather than absent. bsid is
// always known here, so that third byte is always written and always real.
Bytes build_atsc_eac3_descriptor(const ServiceInfo& s) {
    const int svc = service_type(s);
    const auto flags_value = channel_flags(s.acmod, s.lfe, s.channels, s.dsurmod,
                                           /*multiple_programmes=*/false,
                                           ChannelFlagLimits{.wide = true});
    std::array<std::optional<std::uint8_t>, 3> substreams{};
    for (std::size_t i = 0; i < substreams.size(); ++i) {
        if (s.associated_substreams[i].present) {
            substreams[i] = atsc_substream_byte(s.associated_substreams[i]);
        }
    }

    Bytes body;
    // Byte 1: reserved '1', then bsid_flag, mainid_flag, asvc_flag,
    // mixinfoexists, substream1_flag, substream2_flag, substream3_flag.
    unsigned flags = 0x80u | 0x40u;  // reserved '1', bsid_flag
    if (s.mainid) {
        flags |= 0x20u;
    }
    if (s.asvc) {
        flags |= 0x10u;
    }
    if (s.mix_metadata) {
        flags |= 0x08u;
    }
    for (std::size_t i = 0; i < substreams.size(); ++i) {
        if (substreams[i]) {
            flags |= 0x04u >> i;
        }
    }
    put_byte(body, static_cast<std::uint8_t>(flags));
    // Byte 2: reserved '1', full_service_flag, audio_service_type (Table
    // G.2), number_of_channels (Table G.3). Table G.3 has no encoding for
    // 1+1 beyond 0b001, which channel_flags supplies; nothing it can produce
    // lands on a reserved value.
    put_byte(body, static_cast<std::uint8_t>(
                       0x80u | (full_service(svc, s.acmod, s.full_service) ? 0x40u : 0x00u) |
                       (static_cast<unsigned>(svc) << 3) |
                       static_cast<unsigned>(flags_value.value_or(0x4))));
    // Byte 3: language_flag = 0, language_flag_2 = 0, reserved, bsid.
    put_byte(body, static_cast<std::uint8_t>(0x20u | (s.bsid & 0x1F)));
    if (s.mainid) {
        // reserved '111', priority (Table A4.6, which Annex G reuses), mainid.
        put_byte(body, static_cast<std::uint8_t>(0xE0u |
                                                 ((static_cast<unsigned>(s.priority) & 0x3u) << 3) |
                                                 (static_cast<unsigned>(*s.mainid) & 0x7u)));
    }
    if (s.asvc) {
        put_byte(body, *s.asvc);
    }
    for (const auto& substream : substreams) {
        if (substream) {
            put_byte(body, *substream);
        }
    }

    Bytes d;
    put_descriptor_header(d, kTagAtscEac3Descriptor, body.size());
    d.insert(d.end(), body.begin(), body.end());
    return d;
}

[[nodiscard]] Bytes build_descriptor(const AudioTrack& track, BroadcastProfile profile) {
    const bool eac3 = track.codec == AudioCodec::kEac3;
    if (profile == BroadcastProfile::kAtsc) {
        return eac3 ? build_atsc_eac3_descriptor(track.service)
                    : build_atsc_ac3_descriptor(track.service);
    }
    return eac3 ? build_dvb_eac3_descriptor(track.service)
                : build_dvb_ac3_descriptor(track.service);
}

[[nodiscard]] std::uint8_t stream_type_for(const AudioTrack& track, BroadcastProfile profile) {
    if (profile != BroadcastProfile::kAtsc) {
        return kStreamTypePrivateData;
    }
    return track.codec == AudioCodec::kEac3 ? kStreamTypeAtscEac3 : kStreamTypeAtscAc3;
}

Bytes build_pmt_section(std::uint16_t program_number, std::uint16_t audio_pid,
                        std::uint8_t stream_type, const Bytes& descriptor) {
    Bytes es_loop;
    put_byte(es_loop, stream_type);
    // reserved(3)='111', elementary_PID(13).
    put_be16(es_loop, static_cast<std::uint16_t>(0xE000u | (audio_pid & 0x1FFFu)));
    // reserved(4)='1111', ES_info_length(12).
    put_be16(es_loop, static_cast<std::uint16_t>(0xF000u | (descriptor.size() & 0x0FFFu)));
    es_loop.insert(es_loop.end(), descriptor.begin(), descriptor.end());

    Bytes body;
    put_be16(body, program_number);
    put_byte(body, 0xC1);  // reserved(2)='11', version_number(5)=0, current_next_indicator(1)=1
    put_byte(body, 0x00);  // section_number
    put_byte(body, 0x00);  // last_section_number
    // reserved(3)='111', PCR_PID(13) - the audio PID, since it is the only
    // PID this program has and mux() always stamps a PCR on it (see
    // mpegts.hpp's header comment on why once per access unit is enough).
    put_be16(body, static_cast<std::uint16_t>(0xE000u | (audio_pid & 0x1FFFu)));
    put_be16(body, 0xF000u);  // reserved(4)='1111', program_info_length(12)=0: no top-level descriptors
    body.insert(body.end(), es_loop.begin(), es_loop.end());

    Bytes section;
    put_byte(section, 0x02);  // table_id: TS_program_map_section
    const auto section_length = static_cast<std::uint16_t>(body.size() + 4);
    put_be16(section, static_cast<std::uint16_t>(0xB000u | (section_length & 0x0FFFu)));
    section.insert(section.end(), body.begin(), body.end());
    put_be32(section, crc32_mpeg2(section));
    return section;
}

// --- TS packet assembly -----------------------------------------------------

// A PSI section (PAT or PMT) as this module ever builds one is always small
// enough - one program, one stream - to fit in a single TS packet with the
// mandatory pointer_field, so unlike a PES access unit this never needs to
// span packets. If a future change ever grows a section past that, this is
// a programming error (a caller/build-time bug, not a malformed input this
// module was asked to mux), so it asserts rather than returning a
// std::expected error nothing else in this file could plausibly trigger.
void write_psi_packet(Bytes& out, std::uint16_t pid, std::uint8_t& cc, const Bytes& section) {
    assert(section.size() + 1 <= kTsPacketSize - 4);

    Bytes pkt;
    pkt.reserve(kTsPacketSize);
    put_byte(pkt, kSyncByte);
    // transport_error_indicator(1)=0, payload_unit_start_indicator(1)=1 (a
    // PSI section always starts its own packet here), transport_priority(1)=0,
    // PID(13).
    put_be16(pkt, static_cast<std::uint16_t>(0x4000u | (pid & 0x1FFFu)));
    // transport_scrambling_control(2)='00', adaptation_field_control(2)='01'
    // (payload only), continuity_counter(4).
    put_byte(pkt, static_cast<std::uint8_t>(0x10u | (cc & 0x0Fu)));
    cc = static_cast<std::uint8_t>((cc + 1) & 0x0Fu);
    put_byte(pkt, 0x00);  // pointer_field: the section starts immediately after it
    pkt.insert(pkt.end(), section.begin(), section.end());
    // ISO/IEC 13818-1 §2.4.4.3's note: a stuffing byte of 0xFF immediately
    // following the last section in a packet's payload is not itself a
    // valid table_id, so a demuxer that keeps reading past the section it
    // wanted stops there rather than misinterpreting stuffing as another
    // section header.
    while (pkt.size() < kTsPacketSize) {
        put_byte(pkt, 0xFF);
    }
    assert(pkt.size() == kTsPacketSize);
    out.insert(out.end(), pkt.begin(), pkt.end());
}

// program_clock_reference: 33-bit base (90 kHz) + 6 reserved bits (all 1) +
// 9-bit extension (27 MHz remainder, 0-299) - ISO/IEC 13818-1 §2.4.2.2,
// Table 2-6. This module's whole timing model runs off the same 90 kHz
// count PTS uses (see mux()'s stamp_90k), so the extension is always 0:
// there is no finer-grained clock anywhere in this module to put there, and
// 0 is exactly as valid as any other value in that field.
void write_pcr(Bytes& pkt, std::uint64_t pcr_base_90k) {
    const std::uint64_t base = pcr_base_90k & 0x1'FFFF'FFFFull;  // 33 bits
    const std::uint64_t word = (base << 15) | (0x3Full << 9);    // reserved(6)=111111, extension(9)=0
    for (int i = 5; i >= 0; --i) {
        put_byte(pkt, static_cast<std::uint8_t>(word >> (8 * i)));
    }
}

// PTS/DTS field encoding, ISO/IEC 13818-1 §2.4.3.7, Table 2-21 - the 5-byte
// '0010 PTS[32..30] marker PTS[29..15] marker PTS[14..0] marker' layout used
// for a PTS-only PES header (prefix 0b0010; DTS-also would be 0b0011/0b0001
// and is never needed here, since audio has no reordering to signal).
void write_pts(Bytes& pkt, std::uint64_t pts_90k) {
    const std::uint64_t v = pts_90k & 0x1'FFFF'FFFFull;  // 33 bits
    put_byte(pkt, static_cast<std::uint8_t>(0x20u | (((v >> 30) & 0x7u) << 1) | 0x1u));
    put_byte(pkt, static_cast<std::uint8_t>((v >> 22) & 0xFFu));
    put_byte(pkt, static_cast<std::uint8_t>((((v >> 15) & 0x7Fu) << 1) | 0x1u));
    put_byte(pkt, static_cast<std::uint8_t>((v >> 7) & 0xFFu));
    put_byte(pkt, static_cast<std::uint8_t>(((v & 0x7Fu) << 1) | 0x1u));
}

// Writes an adaptation field of exactly `total_len` bytes (the value that
// goes in adaptation_field_length itself, i.e. NOT counting that length byte
// - ISO/IEC 13818-1 §2.4.3.4, Table 2-6). total_len==0 is that table's own
// special case: "the value 0 is for inserting a single stuffing byte in the
// adaptation field of a Transport Stream packet" - just the length byte, no
// flags byte at all - used when a packet needs exactly one pad byte to reach
// 188 and nothing else (no PCR, no random_access marking).
void append_adaptation_field(Bytes& pkt, std::size_t total_len, bool pcr_present,
                             std::uint64_t pcr_base_90k, bool random_access) {
    put_byte(pkt, static_cast<std::uint8_t>(total_len));
    if (total_len == 0) {
        return;
    }
    std::uint8_t flags = 0;
    if (random_access) {
        flags |= 0x40u;  // random_access_indicator
    }
    if (pcr_present) {
        flags |= 0x10u;  // PCR_flag
    }
    put_byte(pkt, flags);
    std::size_t used = 1;
    if (pcr_present) {
        write_pcr(pkt, pcr_base_90k);
        used += 6;
    }
    // Remaining budget, if any, is pure stuffing - ISO/IEC 13818-1 §2.4.3.5:
    // "stuffing_byte - This is a fixed 8-bit value equal to '1111 1111'".
    while (used < total_len) {
        put_byte(pkt, 0xFF);
        ++used;
    }
}

// Builds one complete PES packet (header + the raw access unit as payload) -
// ISO/IEC 13818-1 §2.4.3.6/§2.4.3.7. Returns MuxError::kFrameTooLarge if the
// access unit would overflow PES_packet_length's 16 bits; every legal AC-3/
// E-AC-3 access unit is far smaller than that ceiling, so this is a defensive
// check rather than something real material is expected to hit.
std::expected<Bytes, MuxError> build_pes_packet(std::span<const std::byte> access_unit,
                                                std::uint64_t pts_90k) {
    constexpr std::size_t kHeaderAfterLengthField = 3 + 5;  // flags(3) + PTS(5)
    // start_code_prefix(3) + stream_id(1) + PES_packet_length field(2), then
    // kHeaderAfterLengthField.
    constexpr std::size_t kFixedHeaderBytes = 6 + kHeaderAfterLengthField;
    const std::size_t pes_packet_length = kHeaderAfterLengthField + access_unit.size();
    if (pes_packet_length > 0xFFFFu) {
        return std::unexpected(MuxError::kFrameTooLarge);
    }

    Bytes pes;
    pes.reserve(kFixedHeaderBytes + access_unit.size());
    put_byte(pes, 0x00);
    put_byte(pes, 0x00);
    put_byte(pes, 0x01);  // packet_start_code_prefix
    put_byte(pes, kPesStreamIdPrivateStream1);
    put_be16(pes, static_cast<std::uint16_t>(pes_packet_length));
    // '10'(2, fixed marker) + PES_scrambling_control(2)='00' +
    // PES_priority(1)=0 + data_alignment_indicator(1)=1 (the payload is
    // exactly one complete access unit, starting right here) +
    // copyright(1)=0 + original_or_copy(1)=0.
    put_byte(pes, 0x84);
    // PTS_DTS_flags(2)='10' (PTS only - audio never reorders, so no DTS) +
    // ESCR/ES_rate/DSM_trick_mode/additional_copy_info/PES_CRC/
    // PES_extension flags, all 0.
    put_byte(pes, 0x80);
    put_byte(pes, 0x05);  // PES_header_data_length: just the 5-byte PTS
    write_pts(pes, pts_90k);
    pes.insert(pes.end(), access_unit.begin(), access_unit.end());
    return pes;
}

// Splits one PES packet across as many 188-byte TS packets as it needs.
// ISO/IEC 13818-1 §2.4.3.3: continuity_counter increments once per packet
// that carries payload on this PID, first packet or not. The first packet
// carries PUSI, and an adaptation field stamping PCR and
// random_access_indicator (every AC-3/E-AC-3 access unit here is
// independently decodable, dependent substreams and all, so this is always
// true, not an approximation). Whichever packet turns out to be the last one
// gets whatever adaptation-field stuffing it needs to land on exactly 188
// bytes - a Transport Stream packet has no other way to be short.
void emit_pes_packets(Bytes& out, std::uint16_t pid, std::uint8_t& cc,
                      std::span<const std::byte> pes, std::uint64_t pcr_base_90k) {
    std::size_t offset = 0;
    bool first = true;
    while (offset < pes.size()) {
        Bytes pkt;
        pkt.reserve(kTsPacketSize);
        put_byte(pkt, kSyncByte);
        put_be16(pkt, static_cast<std::uint16_t>((first ? 0x4000u : 0x0000u) | (pid & 0x1FFFu)));

        const std::size_t remaining = pes.size() - offset;
        std::size_t take = 0;
        bool has_adaptation = false;
        std::size_t adapt_len = 0;

        if (first) {
            // Reserve 1 (adaptation_field_length byte) + 1 (flags) + 6 (PCR)
            // = 8 of the 184-byte payload budget; 183 - take always lands
            // exactly on 7 (no stuffing) when take is the full 176, and
            // above 7 (with stuffing) whenever this is also the last packet.
            take = std::min<std::size_t>(remaining, 176);
            has_adaptation = true;
            adapt_len = 183 - take;
        } else if (remaining >= 184) {
            take = 184;
            has_adaptation = false;
        } else {
            take = remaining;
            has_adaptation = true;
            adapt_len = 183 - take;
        }

        // transport_scrambling_control(2)='00',
        // adaptation_field_control(2)='11' (both) or '01' (payload only),
        // continuity_counter(4).
        put_byte(pkt, static_cast<std::uint8_t>((has_adaptation ? 0x30u : 0x10u) | (cc & 0x0Fu)));
        cc = static_cast<std::uint8_t>((cc + 1) & 0x0Fu);

        if (has_adaptation) {
            append_adaptation_field(pkt, adapt_len, /*pcr_present=*/first, pcr_base_90k,
                                    /*random_access=*/first);
        }
        pkt.insert(pkt.end(), pes.begin() + static_cast<std::ptrdiff_t>(offset),
                  pes.begin() + static_cast<std::ptrdiff_t>(offset + take));
        assert(pkt.size() == kTsPacketSize);
        out.insert(out.end(), pkt.begin(), pkt.end());

        offset += take;
        first = false;
    }
}

}  // namespace

std::string_view describe(MuxError error) {
    switch (error) {
        case MuxError::kNoFrames:
            return "no frames to mux";
        case MuxError::kInvalidTrack:
            return "invalid track: channels, sample rate and samples_per_frame are required";
        case MuxError::kInvalidOptions:
            return "invalid options: pmt_pid and audio_pid must differ and neither may be 0x0000";
        case MuxError::kFrameTooLarge:
            return "access unit too large for one PES packet";
    }
    return "unknown error";
}

std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::span<const std::byte>> frames,
    const MuxOptions& options) {
    if (frames.empty()) {
        return std::unexpected(MuxError::kNoFrames);
    }
    if (track.channels <= 0 || track.sample_rate == 0 || track.samples_per_frame == 0) {
        return std::unexpected(MuxError::kInvalidTrack);
    }
    if (options.pmt_pid == options.audio_pid || options.pmt_pid == kPatPid ||
        options.audio_pid == kPatPid) {
        return std::unexpected(MuxError::kInvalidOptions);
    }

    const Bytes descriptor = build_descriptor(track, options.profile);
    const Bytes pat_section =
        build_pat_section(options.transport_stream_id, options.program_number, options.pmt_pid);
    const Bytes pmt_section =
        build_pmt_section(options.program_number, options.audio_pid,
                          stream_type_for(track, options.profile), descriptor);

    // PTS and PCR share one 90 kHz clock derived from the cumulative sample
    // count, the same way matroska::mux derives its millisecond timestamps -
    // see that module's own comment on why the cumulative count is used
    // rather than a per-frame increment (a frame duration that is not a
    // whole number of clock ticks - 1536 samples at 44.1 kHz, for one -
    // rounds without the error ever accumulating).
    const auto stamp_90k = [&](std::size_t index) {
        return static_cast<std::uint64_t>(index) * track.samples_per_frame * 90'000ull /
               track.sample_rate;
    };

    Bytes out;
    std::uint8_t pat_cc = 0;
    std::uint8_t pmt_cc = 0;
    std::uint8_t audio_cc = 0;
    const std::uint32_t psi_period = std::max<std::uint32_t>(options.psi_repeat_every_au, 1);

    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (i % psi_period == 0) {
            write_psi_packet(out, kPatPid, pat_cc, pat_section);
            write_psi_packet(out, options.pmt_pid, pmt_cc, pmt_section);
        }

        const auto pts = stamp_90k(i);
        auto pes = build_pes_packet(frames[i], pts);
        if (!pes) {
            return std::unexpected(pes.error());
        }
        emit_pes_packets(out, options.audio_pid, audio_cc, *pes, pts);
    }

    return out;
}

std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::vector<std::byte>> frames,
    const MuxOptions& options) {
    const std::vector<std::span<const std::byte>> views(frames.begin(), frames.end());
    return mux(track, views, options);
}

std::expected<Writer, MuxError> Writer::create(const AudioTrack& track,
                                               const MuxOptions& options) {
    // The same refusals as mux(), minus kNoFrames - an incremental writer
    // cannot know yet whether frames will arrive.
    if (track.channels <= 0 || track.sample_rate == 0 || track.samples_per_frame == 0) {
        return std::unexpected(MuxError::kInvalidTrack);
    }
    if (options.pmt_pid == options.audio_pid || options.pmt_pid == kPatPid ||
        options.audio_pid == kPatPid) {
        return std::unexpected(MuxError::kInvalidOptions);
    }
    const Bytes descriptor = build_descriptor(track, options.profile);
    return Writer(
        track, options,
        build_pat_section(options.transport_stream_id, options.program_number, options.pmt_pid),
        build_pmt_section(options.program_number, options.audio_pid,
                          stream_type_for(track, options.profile), descriptor));
}

Writer::Writer(AudioTrack track, MuxOptions options, std::vector<std::byte> pat_section,
               std::vector<std::byte> pmt_section)
    : track_(std::move(track)),
      options_(std::move(options)),
      pat_section_(std::move(pat_section)),
      pmt_section_(std::move(pmt_section)) {}

std::expected<std::vector<std::byte>, MuxError> Writer::push(
    std::span<const std::byte> access_unit) {
    // One iteration of mux()'s own loop, with the cross-unit state - the
    // continuity counters and the index the 90 kHz clock derives from -
    // living on this object instead of the stack. Same statements, same
    // order, so the concatenated output is mux()'s, byte for byte.
    Bytes out;
    const std::uint32_t psi_period = std::max<std::uint32_t>(options_.psi_repeat_every_au, 1);
    if (index_ % psi_period == 0) {
        write_psi_packet(out, kPatPid, pat_cc_, pat_section_);
        write_psi_packet(out, options_.pmt_pid, pmt_cc_, pmt_section_);
    }
    const std::uint64_t pts = static_cast<std::uint64_t>(index_) * track_.samples_per_frame *
                              90'000ull / track_.sample_rate;
    auto pes = build_pes_packet(access_unit, pts);
    if (!pes) {
        return std::unexpected(pes.error());
    }
    emit_pes_packets(out, options_.audio_pid, audio_cc_, *pes, pts);
    ++index_;
    return out;
}

std::vector<std::byte> Writer::finalize() { return {}; }

}  // namespace mpegts
