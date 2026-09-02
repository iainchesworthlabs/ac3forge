#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/elementary.hpp"
#include "mpegts/mpegts.hpp"

// These tests read the muxer's output back with an independent TS/PSI walker
// rather than comparing against bytes this same code produced. A muxer
// checked only against itself proves nothing about whether a player can
// open the stream - the same reasoning test_matroska.cpp documents for its
// own EBML walker.

namespace {

using Bytes = std::vector<std::byte>;

Bytes frame_of(std::size_t size, std::uint8_t fill) {
    return Bytes(size, static_cast<std::byte>(fill));
}

constexpr std::size_t kTsPacketSize = 188;

// ISO/IEC 13818-1 Annex B's CRC_32, transcribed independently of
// src/mpegts/src/mpegts.cpp's own copy (same reasoning as that file's own
// comment on why this is worth self-checking rather than trusting by
// construction): non-reflected CRC-32/MPEG-2, poly 0x04C11DB7, init
// all-ones, no output XOR.
std::uint32_t crc32_mpeg2(std::span<const std::byte> data) {
    std::uint32_t crc = 0xFFFF'FFFFu;
    for (const auto b : data) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b)) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000'0000u) ? (crc << 1) ^ 0x04C1'1DB7u : (crc << 1);
        }
    }
    return crc;
}

struct TsPacket {
    std::uint16_t pid = 0;
    bool pusi = false;
    std::uint8_t cc = 0;
    bool has_payload = false;
    bool pcr_present = false;
    bool random_access = false;
    std::uint64_t pcr_base = 0;
    std::span<const std::byte> payload;
};

std::vector<TsPacket> parse_packets(std::span<const std::byte> ts) {
    REQUIRE(ts.size() % kTsPacketSize == 0);
    std::vector<TsPacket> out;
    for (std::size_t off = 0; off < ts.size(); off += kTsPacketSize) {
        const auto pkt = ts.subspan(off, kTsPacketSize);
        REQUIRE(std::to_integer<std::uint8_t>(pkt[0]) == 0x47);
        const auto b1 = std::to_integer<std::uint8_t>(pkt[1]);
        const auto b2 = std::to_integer<std::uint8_t>(pkt[2]);
        const auto b3 = std::to_integer<std::uint8_t>(pkt[3]);

        TsPacket p;
        p.pusi = (b1 & 0x40u) != 0;
        p.pid = static_cast<std::uint16_t>(((b1 & 0x1Fu) << 8) | b2);
        const auto afc = static_cast<std::uint8_t>((b3 >> 4) & 0x3u);
        p.cc = static_cast<std::uint8_t>(b3 & 0x0Fu);
        const bool has_adaptation = (afc & 0x2u) != 0;
        p.has_payload = (afc & 0x1u) != 0;

        std::size_t offset = 4;
        if (has_adaptation) {
            const auto adapt_len = std::to_integer<std::uint8_t>(pkt[4]);
            if (adapt_len > 0) {
                const auto flags = std::to_integer<std::uint8_t>(pkt[5]);
                p.random_access = (flags & 0x40u) != 0;
                p.pcr_present = (flags & 0x10u) != 0;
                if (p.pcr_present) {
                    std::uint64_t word = 0;
                    for (std::size_t i = 0; i < 6; ++i) {
                        word = (word << 8) | std::to_integer<std::uint8_t>(pkt[6 + i]);
                    }
                    p.pcr_base = (word >> 15) & 0x1'FFFF'FFFFull;
                }
            }
            offset += 1 + static_cast<std::size_t>(adapt_len);
        }
        if (p.has_payload) {
            REQUIRE(offset <= pkt.size());
            p.payload = pkt.subspan(offset);
        }
        out.push_back(p);
    }
    return out;
}

// Extracts one PSI section from a PUSI packet's payload (pointer_field, then
// the section) and validates its CRC_32 covers the section correctly.
Bytes section_from_psi_packet(const TsPacket& pkt) {
    REQUIRE(pkt.pusi);
    REQUIRE(pkt.has_payload);
    const auto pointer_field = std::to_integer<std::uint8_t>(pkt.payload[0]);
    const auto section_start = pkt.payload.subspan(1 + static_cast<std::size_t>(pointer_field));
    // section_length is the low 12 bits of bytes[1..2]; the section runs
    // from table_id through CRC_32 inclusive, i.e. 3 + section_length bytes.
    const auto len_hi = std::to_integer<std::uint8_t>(section_start[1]);
    const auto len_lo = std::to_integer<std::uint8_t>(section_start[2]);
    const std::size_t section_length = ((len_hi & 0x0Fu) << 8) | len_lo;
    const Bytes section(section_start.begin(), section_start.begin() + 3 +
                                                     static_cast<std::ptrdiff_t>(section_length));
    REQUIRE(crc32_mpeg2(section) == 0);
    return section;
}

std::vector<const TsPacket*> packets_on_pid(const std::vector<TsPacket>& packets,
                                            std::uint16_t pid) {
    std::vector<const TsPacket*> out;
    for (const auto& p : packets) {
        if (p.pid == pid) {
            out.push_back(&p);
        }
    }
    return out;
}

// Reassembles every PES-wrapped access unit carried on `pid` and returns
// each one's raw payload (the bytes after the full PES header), in order.
// The PES header this module always writes is a fixed 14 bytes: start code
// (3) + stream_id (1) + PES_packet_length (2) + flags (3) + PTS (5).
std::vector<Bytes> reassemble_pes_payloads(const std::vector<const TsPacket*>& on_pid) {
    constexpr std::size_t kPesHeaderBytes = 14;
    std::vector<Bytes> out;
    Bytes current;
    std::size_t want = 0;  // access-unit bytes expected for the PES packet in progress
    bool in_progress = false;

    for (const auto* p : on_pid) {
        if (!p->has_payload) {
            continue;
        }
        if (p->pusi) {
            if (in_progress) {
                // A well-formed stream never starts a new PES before the
                // previous one's declared length is satisfied.
                REQUIRE(current.size() == want);
                out.push_back(current);
            }
            REQUIRE(p->payload.size() >= kPesHeaderBytes);
            REQUIRE(std::to_integer<std::uint8_t>(p->payload[0]) == 0x00);
            REQUIRE(std::to_integer<std::uint8_t>(p->payload[1]) == 0x00);
            REQUIRE(std::to_integer<std::uint8_t>(p->payload[2]) == 0x01);
            REQUIRE(std::to_integer<std::uint8_t>(p->payload[3]) == 0xBD);  // private_stream_1
            const auto len_hi = std::to_integer<std::uint8_t>(p->payload[4]);
            const auto len_lo = std::to_integer<std::uint8_t>(p->payload[5]);
            const std::size_t pes_packet_length =
                (static_cast<std::size_t>(len_hi) << 8) | len_lo;
            REQUIRE(pes_packet_length >= 8);  // flags(3) + PTS(5), the fixed part it always covers
            want = pes_packet_length - 8;
            REQUIRE(std::to_integer<std::uint8_t>(p->payload[8]) == 0x05);  // PES_header_data_length
            const auto access_unit_part = p->payload.subspan(kPesHeaderBytes);
            current.assign(access_unit_part.begin(), access_unit_part.end());
            in_progress = true;
        } else {
            REQUIRE(in_progress);
            current.insert(current.end(), p->payload.begin(), p->payload.end());
        }
    }
    if (in_progress) {
        REQUIRE(current.size() == want);
        out.push_back(current);
    }
    return out;
}

// The ES loop's first descriptor, as {tag, body}: the PMT this module writes
// always has one program with one elementary stream and no program-level
// descriptors, so the loop starts at a fixed offset (see the PAT/PMT test's
// own walk of the same bytes).
struct Descriptor {
    std::uint8_t tag = 0;
    Bytes body;
};

Descriptor first_descriptor(const Bytes& pmt) {
    const std::size_t es_info_length =
        ((std::to_integer<std::size_t>(pmt[15]) & 0x0Fu) << 8) |
        std::to_integer<std::size_t>(pmt[16]);
    REQUIRE(es_info_length >= 2);
    const std::size_t length = std::to_integer<std::size_t>(pmt[18]);
    REQUIRE(es_info_length >= 2 + length);
    Descriptor d;
    d.tag = std::to_integer<std::uint8_t>(pmt[17]);
    d.body.assign(pmt.begin() + 19, pmt.begin() + 19 + static_cast<std::ptrdiff_t>(length));
    return d;
}

std::uint8_t stream_type_of(const Bytes& pmt) { return std::to_integer<std::uint8_t>(pmt[12]); }

// The descriptor following `previous` in the same ES loop - for the one
// codec (AC-4) whose signalling is a registration descriptor AND a DVB
// extension descriptor side by side.
Descriptor descriptor_after(const Bytes& pmt, const Descriptor& previous) {
    const std::size_t at = 17 + 2 + previous.body.size();
    const std::size_t length = std::to_integer<std::size_t>(pmt[at + 1]);
    Descriptor d;
    d.tag = std::to_integer<std::uint8_t>(pmt[at]);
    d.body.assign(pmt.begin() + static_cast<std::ptrdiff_t>(at + 2),
                  pmt.begin() + static_cast<std::ptrdiff_t>(at + 2 + length));
    return d;
}

Bytes pmt_of(std::span<const std::byte> file) {
    const auto packets = parse_packets(file);
    return section_from_psi_packet(*packets_on_pid(packets, 0x1000).front());
}

// A 3/2 + LFE complete-main service at 448 kbps, 48 kHz - the values
// ac3::io::scan reads off this project's own 5.1 output, spelled out here so
// the descriptor assertions below are against known inputs rather than
// whatever an encoder happened to produce.
mpegts::ServiceInfo five_one_service(int bsid) {
    return mpegts::ServiceInfo{.bsmod = 0,
                               .bsmod_present = true,
                               .acmod = 7,
                               .lfe = true,
                               .channels = 6,
                               .bsid = bsid,
                               .dsurmod = 0,
                               .bit_rate_code = 15,  // Table A4.3: 448 kbit/s, exact
                               .sample_rate_code = 0};
}

}  // namespace

TEST_CASE("MPEG-TS output parses as well-formed TS packets with a valid PAT/PMT", "[mpegts]") {
    const std::vector<Bytes> frames{frame_of(1792, 0x11), frame_of(1792, 0x22),
                                    frame_of(1792, 0x33), frame_of(1792, 0x44),
                                    frame_of(1792, 0x55)};
    const auto file = mpegts::mux({.codec = mpegts::AudioCodec::kEac3, .channels = 6}, frames);
    REQUIRE(file.has_value());

    const auto packets = parse_packets(*file);
    REQUIRE_FALSE(packets.empty());

    const auto pat_packets = packets_on_pid(packets, 0x0000);
    REQUIRE_FALSE(pat_packets.empty());
    const auto pat = section_from_psi_packet(*pat_packets.front());
    CHECK(std::to_integer<std::uint8_t>(pat[0]) == 0x00);  // table_id: PAT
    // program_number at bytes[8..9], program_map_PID at bytes[10..11] low 13 bits.
    const std::uint16_t program_number = static_cast<std::uint16_t>(
        (std::to_integer<std::uint8_t>(pat[8]) << 8) | std::to_integer<std::uint8_t>(pat[9]));
    const std::uint16_t pmt_pid = static_cast<std::uint16_t>(
        ((std::to_integer<std::uint8_t>(pat[10]) & 0x1Fu) << 8) |
        std::to_integer<std::uint8_t>(pat[11]));
    CHECK(program_number == 1);
    CHECK(pmt_pid == 0x1000);

    const auto pmt_packets = packets_on_pid(packets, pmt_pid);
    REQUIRE_FALSE(pmt_packets.empty());
    const auto pmt = section_from_psi_packet(*pmt_packets.front());
    CHECK(std::to_integer<std::uint8_t>(pmt[0]) == 0x02);  // table_id: PMT
    const std::uint16_t pcr_pid = static_cast<std::uint16_t>(
        ((std::to_integer<std::uint8_t>(pmt[8]) & 0x1Fu) << 8) |
        std::to_integer<std::uint8_t>(pmt[9]));
    CHECK(pcr_pid == 0x0100);  // default audio_pid
    // ES loop starts right after program_info_length (bytes[10..11], 0 here).
    const std::uint8_t stream_type = std::to_integer<std::uint8_t>(pmt[12]);
    const std::uint16_t elementary_pid = static_cast<std::uint16_t>(
        ((std::to_integer<std::uint8_t>(pmt[13]) & 0x1Fu) << 8) |
        std::to_integer<std::uint8_t>(pmt[14]));
    CHECK(stream_type == 0x06);  // DVB private data - see mpegts.hpp
    CHECK(elementary_pid == 0x0100);
    const std::uint8_t descriptor_tag = std::to_integer<std::uint8_t>(pmt[17]);
    CHECK(descriptor_tag == 0x7A);  // Enhanced_AC3_descriptor for E-AC-3
}

TEST_CASE("MPEG-TS access units round-trip byte-for-byte through PES", "[mpegts]") {
    const std::vector<Bytes> frames{frame_of(1792, 0x11), frame_of(1792, 0x22),
                                    frame_of(1792, 0x33), frame_of(896, 0x44)};
    const auto file = mpegts::mux({.codec = mpegts::AudioCodec::kAc3, .channels = 2}, frames);
    REQUIRE(file.has_value());

    const auto packets = parse_packets(*file);
    const auto audio_ptrs = packets_on_pid(packets, 0x0100);
    const auto access_units = reassemble_pes_payloads(audio_ptrs);

    REQUIRE(access_units.size() == frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        CHECK(access_units[i] == frames[i]);
    }

    // Every access unit's first TS packet stamps PCR and marks a random
    // access point (mpegts.hpp's rationale: every AC-3/E-AC-3 access unit
    // here is independently decodable).
    std::size_t pusi_count = 0;
    for (const auto* p : audio_ptrs) {
        if (p->pusi) {
            CHECK(p->pcr_present);
            CHECK(p->random_access);
            ++pusi_count;
        }
    }
    CHECK(pusi_count == frames.size());
}

TEST_CASE("MPEG-TS descriptor identifies AC-3 vs Enhanced AC-3", "[mpegts]") {
    const std::vector<Bytes> frames{frame_of(64, 0xAB), frame_of(64, 0xCD), frame_of(64, 0xEF)};

    const auto ac3_file = mpegts::mux(
        {.codec = mpegts::AudioCodec::kAc3, .channels = 2,
         .service = {.acmod = 2, .channels = 2, .bsid = 8}},
        frames);
    REQUIRE(ac3_file.has_value());
    const auto ac3 = first_descriptor(pmt_of(*ac3_file));
    CHECK(ac3.tag == 0x6A);

    const auto eac3_file = mpegts::mux(
        {.codec = mpegts::AudioCodec::kEac3, .channels = 6, .service = five_one_service(16)},
        frames);
    REQUIRE(eac3_file.has_value());
    const auto eac3 = first_descriptor(pmt_of(*eac3_file));
    CHECK(eac3.tag == 0x7A);
}

// ETSI EN 300 468 Table D.6/D.7 with Table D.1's component_type: the DVB
// descriptors used to leave every optional field out because ac3::io::scan
// could not supply one. Every byte below is decoded against the standard's
// own field positions, not against what the builder happened to emit.
TEST_CASE("DVB descriptors carry component_type and bsid", "[mpegts]") {
    const std::vector<Bytes> frames{frame_of(64, 0xAB), frame_of(64, 0xCD)};

    SECTION("AC-3, 5.1 complete main") {
        const auto file = mpegts::mux(
            {.codec = mpegts::AudioCodec::kAc3, .channels = 6, .service = five_one_service(8)},
            frames);
        REQUIRE(file.has_value());
        const auto d = first_descriptor(pmt_of(*file));
        CHECK(d.tag == 0x6A);
        REQUIRE(d.body.size() == 3);
        // component_type_flag + bsid_flag set, mainid/asvc clear, the four
        // reserved_flags "always set to 0b0".
        CHECK(std::to_integer<std::uint8_t>(d.body[0]) == 0xC0);
        // Table D.1: b7 Enhanced AC-3 flag (0 = AC-3), b6 full service,
        // b5-b3 service type (000 = CM), b2-b0 channels (100 = > 2 channels;
        // 5.1 is not "> 5.1", which is Table D.5's next rung up).
        CHECK(std::to_integer<std::uint8_t>(d.body[1]) == 0x44);
        // "The three msb should always be set to 0b000."
        CHECK(std::to_integer<std::uint8_t>(d.body[2]) == 8);
    }

    SECTION("E-AC-3 sets the Enhanced AC-3 flag and can go past 5.1") {
        auto service = five_one_service(16);
        service.channels = 12;  // 7.1.4, two dependents' worth of extra channels
        const auto file = mpegts::mux(
            {.codec = mpegts::AudioCodec::kEac3, .channels = 12, .service = service}, frames);
        REQUIRE(file.has_value());
        const auto d = first_descriptor(pmt_of(*file));
        CHECK(d.tag == 0x7A);
        REQUIRE(d.body.size() == 3);
        CHECK(std::to_integer<std::uint8_t>(d.body[0]) == 0xC0);
        CHECK(std::to_integer<std::uint8_t>(d.body[1]) == 0xC5);  // E-AC-3, full, CM, > 5.1
        CHECK(std::to_integer<std::uint8_t>(d.body[2]) == 16);
    }

    SECTION("a Dolby Surround encoded stereo mix reads as Table D.5's 0b011") {
        const auto file = mpegts::mux(
            {.codec = mpegts::AudioCodec::kAc3, .channels = 2,
             .service = {.acmod = 2, .channels = 2, .bsid = 8, .dsurmod = 2}},
            frames);
        REQUIRE(file.has_value());
        const auto d = first_descriptor(pmt_of(*file));
        CHECK(std::to_integer<std::uint8_t>(d.body[1]) == 0x43);
    }

    SECTION("an associated service's full-service flag follows Table D.4") {
        // Music and effects: "full service flag ... set to 0b0".
        const auto file = mpegts::mux(
            {.codec = mpegts::AudioCodec::kAc3, .channels = 2,
             .service = {.bsmod = 1, .acmod = 2, .channels = 2, .bsid = 8}},
            frames);
        REQUIRE(file.has_value());
        const auto d = first_descriptor(pmt_of(*file));
        CHECK(std::to_integer<std::uint8_t>(d.body[1]) == 0x0A);  // AC-3, not full, ME, stereo
    }

    SECTION("mainid and asvc appear only when the caller supplies them") {
        auto service = five_one_service(16);
        service.mainid = 3;
        service.asvc = std::uint8_t{0x0A};
        service.mix_metadata = true;
        const auto file = mpegts::mux(
            {.codec = mpegts::AudioCodec::kEac3, .channels = 6, .service = service}, frames);
        REQUIRE(file.has_value());
        const auto d = first_descriptor(pmt_of(*file));
        REQUIRE(d.body.size() == 5);
        // component_type + bsid + mainid + asvc flags, plus mixinfoexists,
        // which Table D.7 makes a value bit rather than a presence flag.
        CHECK(std::to_integer<std::uint8_t>(d.body[0]) == 0xF8);
        CHECK(std::to_integer<std::uint8_t>(d.body[3]) == 3);
        CHECK(std::to_integer<std::uint8_t>(d.body[4]) == 0x0A);
    }

    SECTION("a second independent substream reads as Table D.5's multi-programme value") {
        auto service = five_one_service(16);
        service.independent_substreams = 0x03;  // I0 and I1
        service.associated_substreams[0] =
            mpegts::SubstreamService{.present = true, .bsmod = 2, .acmod = 1};  // VI, mono
        const auto file = mpegts::mux(
            {.codec = mpegts::AudioCodec::kEac3, .channels = 6, .service = service}, frames);
        REQUIRE(file.has_value());
        const auto d = first_descriptor(pmt_of(*file));
        REQUIRE(d.body.size() == 4);
        CHECK(std::to_integer<std::uint8_t>(d.body[0]) == 0xC4);  // + substream1_flag
        CHECK(std::to_integer<std::uint8_t>(d.body[1]) == 0xC6);  // E-AC-3, full, CM, multi-prog
        // Table D.8: mixing metadata 0, full service 1 (VI is unconstrained),
        // service type 010 (VI), channels 000 (mono).
        CHECK(std::to_integer<std::uint8_t>(d.body[3]) == 0x50);
    }
}

// A/52:2018 Annex A Table A4.1 and Annex G Table G.1, plus the stream_type
// values sections A4.1 and G3.1 assign. ATSC identifies the stream by
// stream_type and describes it in the descriptor; DVB does the reverse.
TEST_CASE("ATSC profile writes its own stream_type and descriptors", "[mpegts]") {
    const std::vector<Bytes> frames{frame_of(64, 0xAB), frame_of(64, 0xCD)};
    const mpegts::MuxOptions atsc{.profile = mpegts::BroadcastProfile::kAtsc};

    SECTION("AC-3: stream_type 0x81, AC-3_audio_stream_descriptor 0x81") {
        const auto file = mpegts::mux(
            {.codec = mpegts::AudioCodec::kAc3, .channels = 6, .service = five_one_service(8)},
            frames, atsc);
        REQUIRE(file.has_value());
        const auto pmt = pmt_of(*file);
        CHECK(stream_type_of(pmt) == 0x81);
        const auto d = first_descriptor(pmt);
        CHECK(d.tag == 0x81);
        // Three bytes: the descriptor terminates at the first allowed
        // termination point, which A/52 puts immediately before langcod.
        REQUIRE(d.body.size() == 3);
        // sample_rate_code (Table A4.2: 0b000 = 48 kHz) then bsid.
        CHECK(std::to_integer<std::uint8_t>(d.body[0]) == 0x08);
        // bit_rate_code (Table A4.3: index 15 = 448 kbit/s, msb clear for
        // "exact") then surround_mode (Table A4.4: 0b00, not indicated).
        CHECK(std::to_integer<std::uint8_t>(d.body[1]) == 0x3C);
        // bsmod, then Table A4.5's num_channels (msb clear, so the low three
        // bits ARE acmod: 0b111 = 3/2), then full_svc.
        CHECK(std::to_integer<std::uint8_t>(d.body[2]) == 0x0F);
    }

    SECTION("E-AC-3: stream_type 0x87, E-AC-3_audio_descriptor 0xCC") {
        const auto file = mpegts::mux(
            {.codec = mpegts::AudioCodec::kEac3, .channels = 6, .service = five_one_service(16)},
            frames, atsc);
        REQUIRE(file.has_value());
        const auto pmt = pmt_of(*file);
        CHECK(stream_type_of(pmt) == 0x87);
        const auto d = first_descriptor(pmt);
        CHECK(d.tag == 0xCC);
        REQUIRE(d.body.size() == 3);
        // reserved '1', bsid_flag, then mainid/asvc/mixinfoexists/substream1-3
        // all clear.
        CHECK(std::to_integer<std::uint8_t>(d.body[0]) == 0xC0);
        // reserved '1', full_service_flag, audio_service_type (Table G.2:
        // 000 = complete main), number_of_channels (Table G.3: 0b100 =
        // "> 2 channels; <= 3/2 + LFE").
        CHECK(std::to_integer<std::uint8_t>(d.body[1]) == 0xC4);
        // language_flag, language_flag_2, reserved, then bsid's five bits.
        CHECK(std::to_integer<std::uint8_t>(d.body[2]) == 0x30);
    }

    SECTION("a wider E-AC-3 programme reads as Table G.3's 0b101") {
        auto service = five_one_service(16);
        service.channels = 8;  // 7.1
        const auto file = mpegts::mux(
            {.codec = mpegts::AudioCodec::kEac3, .channels = 8, .service = service}, frames, atsc);
        REQUIRE(file.has_value());
        const auto d = first_descriptor(pmt_of(*file));
        CHECK(std::to_integer<std::uint8_t>(d.body[1]) == 0xC5);
    }

    SECTION("mainid extends the AC-3 descriptor past its first termination point") {
        auto service = five_one_service(8);
        service.mainid = 3;
        service.priority = 1;  // Table A4.6: primary audio
        const auto file = mpegts::mux(
            {.codec = mpegts::AudioCodec::kAc3, .channels = 6, .service = service}, frames, atsc);
        REQUIRE(file.has_value());
        const auto d = first_descriptor(pmt_of(*file));
        REQUIRE(d.body.size() == 7);
        CHECK(std::to_integer<std::uint8_t>(d.body[3]) == 0xFF);  // langcod, deprecated
        // mainid (3 bits), priority (2), reserved '111'.
        CHECK(std::to_integer<std::uint8_t>(d.body[4]) == 0x6F);
        CHECK(std::to_integer<std::uint8_t>(d.body[5]) == 0x01);  // textlen 0, text_code 1
        CHECK(std::to_integer<std::uint8_t>(d.body[6]) == 0x3F);  // no language, reserved '111111'
    }

    SECTION("an associated AC-3 service takes the asvcflags branch instead") {
        auto service = five_one_service(8);
        service.bsmod = 5;  // commentary
        service.asvc = std::uint8_t{0x81};
        const auto file = mpegts::mux(
            {.codec = mpegts::AudioCodec::kAc3, .channels = 6, .service = service}, frames, atsc);
        REQUIRE(file.has_value());
        const auto d = first_descriptor(pmt_of(*file));
        REQUIRE(d.body.size() == 7);
        CHECK(std::to_integer<std::uint8_t>(d.body[4]) == 0x81);  // asvcflags, not mainid
    }

    SECTION("a substream A/52 Table G.5 and G.6 cannot describe is omitted, not guessed") {
        auto service = five_one_service(16);
        service.independent_substreams = 0x03;
        // Complete main is "reserved" as a substream service type (Table G.5),
        // so there is no honest byte to write for this one.
        service.associated_substreams[0] =
            mpegts::SubstreamService{.present = true, .bsmod = 0, .acmod = 2};
        const auto file = mpegts::mux(
            {.codec = mpegts::AudioCodec::kEac3, .channels = 6, .service = service}, frames, atsc);
        REQUIRE(file.has_value());
        const auto d = first_descriptor(pmt_of(*file));
        REQUIRE(d.body.size() == 3);
        CHECK((std::to_integer<std::uint8_t>(d.body[0]) & 0x04u) == 0);  // substream1_flag clear
    }

    SECTION("a describable substream does get its Table G.4 byte") {
        auto service = five_one_service(16);
        service.independent_substreams = 0x03;
        service.associated_substreams[0] =
            mpegts::SubstreamService{.present = true, .bsmod = 2, .acmod = 1};  // VI, mono
        const auto file = mpegts::mux(
            {.codec = mpegts::AudioCodec::kEac3, .channels = 6, .service = service}, frames, atsc);
        REQUIRE(file.has_value());
        const auto d = first_descriptor(pmt_of(*file));
        REQUIRE(d.body.size() == 4);
        CHECK((std::to_integer<std::uint8_t>(d.body[0]) & 0x04u) != 0);
        // reserved '1', substream_priority 0, service type 010 (VI),
        // channels 000 (mono).
        CHECK(std::to_integer<std::uint8_t>(d.body[3]) == 0x90);
    }
}

// The narrow layouts and the one service type whose meaning depends on acmod.
// A/52 Table 5.7 splits bsmod 0b111 by acmod - 1/0 is voice over, anything
// wider is karaoke - and Table D.4/G.2 give the two OPPOSITE full-service
// restrictions, so getting the split wrong flips a bit in every descriptor
// that carries it.
TEST_CASE("the narrow layouts and the voiceover/karaoke split", "[mpegts]") {
    const std::vector<Bytes> frames{frame_of(64, 0xAB), frame_of(64, 0xCD)};
    const mpegts::MuxOptions atsc{.profile = mpegts::BroadcastProfile::kAtsc};

    SECTION("mono reads as 0b000 in both registries") {
        const mpegts::ServiceInfo mono{.acmod = 1, .channels = 1, .bsid = 8};
        const auto dvb = mpegts::mux({.codec = mpegts::AudioCodec::kAc3, .channels = 1,
                                      .service = mono},
                                     frames);
        REQUIRE(dvb.has_value());
        // AC-3, full service, complete main, mono.
        CHECK(std::to_integer<std::uint8_t>(first_descriptor(pmt_of(*dvb)).body[1]) == 0x40);

        const auto ac3_atsc = mpegts::mux({.codec = mpegts::AudioCodec::kAc3, .channels = 1,
                                           .service = mono},
                                          frames, atsc);
        REQUIRE(ac3_atsc.has_value());
        // Table A4.5: num_channels is acmod itself, 0b0001 for 1/0.
        CHECK(std::to_integer<std::uint8_t>(first_descriptor(pmt_of(*ac3_atsc)).body[2]) == 0x03);

        auto eac3_mono = mono;
        eac3_mono.bsid = 16;
        const auto eac3_atsc = mpegts::mux({.codec = mpegts::AudioCodec::kEac3, .channels = 1,
                                            .service = eac3_mono},
                                           frames, atsc);
        REQUIRE(eac3_atsc.has_value());
        // Table G.3: 0b000, mono.
        CHECK(std::to_integer<std::uint8_t>(first_descriptor(pmt_of(*eac3_atsc)).body[1]) == 0xC0);
    }

    SECTION("1+1 dual mono reads as 0b001") {
        const mpegts::ServiceInfo dual{.acmod = 0, .channels = 2, .bsid = 8};
        const auto dvb = mpegts::mux({.codec = mpegts::AudioCodec::kAc3, .channels = 2,
                                      .service = dual},
                                     frames);
        REQUIRE(dvb.has_value());
        CHECK(std::to_integer<std::uint8_t>(first_descriptor(pmt_of(*dvb)).body[1]) == 0x41);
    }

    SECTION("a 1+1 ATSC descriptor carries langcod2 when it is extended") {
        // Table A4.1 gates langcod2 on num_channels == 0b0000, which is
        // exactly acmod 1+1 - so the extended form is a byte longer here than
        // for any other layout.
        mpegts::ServiceInfo dual{.acmod = 0, .channels = 2, .bsid = 8};
        dual.mainid = 2;
        const auto file = mpegts::mux({.codec = mpegts::AudioCodec::kAc3, .channels = 2,
                                       .service = dual},
                                      frames, atsc);
        REQUIRE(file.has_value());
        const auto d = first_descriptor(pmt_of(*file));
        REQUIRE(d.body.size() == 8);
        CHECK(std::to_integer<std::uint8_t>(d.body[3]) == 0xFF);  // langcod
        CHECK(std::to_integer<std::uint8_t>(d.body[4]) == 0xFF);  // langcod2
        CHECK((std::to_integer<std::uint8_t>(d.body[5]) >> 5) == 2);  // mainid
    }

    SECTION("bsmod 0b111 is voice over at 1/0 and karaoke above it") {
        // Table D.4: voice over "set to 0b0", karaoke "set to 0b1" - the same
        // code, opposite full-service flags, decided by acmod alone.
        const auto voiceover = mpegts::mux(
            {.codec = mpegts::AudioCodec::kAc3, .channels = 1,
             .service = {.bsmod = 7, .acmod = 1, .channels = 1, .bsid = 8}},
            frames);
        REQUIRE(voiceover.has_value());
        CHECK(std::to_integer<std::uint8_t>(first_descriptor(pmt_of(*voiceover)).body[1]) == 0x38);

        const auto karaoke = mpegts::mux(
            {.codec = mpegts::AudioCodec::kAc3, .channels = 2,
             .service = {.bsmod = 7, .acmod = 2, .channels = 2, .bsid = 8}},
            frames);
        REQUIRE(karaoke.has_value());
        CHECK(std::to_integer<std::uint8_t>(first_descriptor(pmt_of(*karaoke)).body[1]) == 0x7A);
    }

    SECTION("an emergency service is full by Table D.4, a dialogue one is not") {
        const auto emergency = mpegts::mux(
            {.codec = mpegts::AudioCodec::kAc3, .channels = 1,
             .service = {.bsmod = 6, .acmod = 1, .channels = 1, .bsid = 8}},
            frames);
        REQUIRE(emergency.has_value());
        CHECK(std::to_integer<std::uint8_t>(first_descriptor(pmt_of(*emergency)).body[1]) == 0x70);

        const auto dialogue = mpegts::mux(
            {.codec = mpegts::AudioCodec::kAc3, .channels = 2,
             .service = {.bsmod = 4, .acmod = 2, .channels = 2, .bsid = 8}},
            frames);
        REQUIRE(dialogue.has_value());
        CHECK(std::to_integer<std::uint8_t>(first_descriptor(pmt_of(*dialogue)).body[1]) == 0x22);
    }

    SECTION("full_service can be overridden where the tables do not pin it") {
        auto service = five_one_service(8);
        service.bsmod = 2;  // visually impaired - unconstrained either way
        service.full_service = false;
        const auto partial = mpegts::mux({.codec = mpegts::AudioCodec::kAc3, .channels = 6,
                                          .service = service},
                                         frames);
        REQUIRE(partial.has_value());
        CHECK(std::to_integer<std::uint8_t>(first_descriptor(pmt_of(*partial)).body[1]) == 0x14);
    }
}

TEST_CASE("the DVB profile is what a caller gets without asking", "[mpegts]") {
    const std::vector<Bytes> frames{frame_of(64, 0xAB), frame_of(64, 0xCD)};
    const auto implicit =
        mpegts::mux({.codec = mpegts::AudioCodec::kEac3, .channels = 6,
                     .service = five_one_service(16)},
                    frames);
    const auto explicit_dvb =
        mpegts::mux({.codec = mpegts::AudioCodec::kEac3, .channels = 6,
                     .service = five_one_service(16)},
                    frames, {.profile = mpegts::BroadcastProfile::kDvb});
    REQUIRE(implicit.has_value());
    REQUIRE(explicit_dvb.has_value());
    CHECK(*implicit == *explicit_dvb);
    CHECK(stream_type_of(pmt_of(*implicit)) == 0x06);
}

TEST_CASE("MPEG-TS continuity counters increment per PID and wrap at 16", "[mpegts]") {
    // Small access units (well under 167 bytes) so each one is exactly one
    // TS packet, and psi_repeat_every_au=1 so PAT/PMT repeat every access
    // unit too - with 20 access units, every PID's continuity counter wraps
    // past 15 back to 0 at least once, which is the case a stuck or
    // never-incremented counter would fail on but a short run would not
    // exercise at all.
    std::vector<Bytes> frames;
    for (int i = 0; i < 20; ++i) {
        frames.push_back(frame_of(32, static_cast<std::uint8_t>(i)));
    }
    const auto file = mpegts::mux({.codec = mpegts::AudioCodec::kAc3, .channels = 2}, frames,
                                  {.psi_repeat_every_au = 1});
    REQUIRE(file.has_value());
    const auto packets = parse_packets(*file);

    for (const std::uint16_t pid : {std::uint16_t{0x0000}, std::uint16_t{0x1000},
                                    std::uint16_t{0x0100}}) {
        const auto on_pid = packets_on_pid(packets, pid);
        REQUIRE(on_pid.size() >= 17);  // enough to prove a wrap actually happened
        bool saw_wrap = false;
        for (std::size_t i = 1; i < on_pid.size(); ++i) {
            const auto expected = static_cast<std::uint8_t>((on_pid[i - 1]->cc + 1) & 0x0F);
            CHECK(on_pid[i]->cc == expected);
            if (on_pid[i]->cc == 0 && on_pid[i - 1]->cc == 15) {
                saw_wrap = true;
            }
        }
        CHECK(saw_wrap);
    }
}

TEST_CASE("MPEG-TS muxer rejects what it cannot describe", "[mpegts]") {
    const std::vector<Bytes> one{frame_of(16, 0)};
    // Explicit empty span: bare {} became ambiguous when the span-of-views
    // mux overload arrived alongside the owned-list one.
    CHECK(mpegts::mux({.channels = 2}, std::span<const Bytes>{}).error() ==
          mpegts::MuxError::kNoFrames);
    CHECK(mpegts::mux({.channels = 0}, one).error() == mpegts::MuxError::kInvalidTrack);
    CHECK(mpegts::mux({.sample_rate = 0, .channels = 2}, one).error() ==
          mpegts::MuxError::kInvalidTrack);
    CHECK(mpegts::mux({.channels = 2, .samples_per_frame = 0}, one).error() ==
          mpegts::MuxError::kInvalidTrack);
    CHECK(mpegts::mux({.channels = 2}, one, {.pmt_pid = 0x0100, .audio_pid = 0x0100}).error() ==
          mpegts::MuxError::kInvalidOptions);
    CHECK(mpegts::mux({.channels = 2}, one, {.pmt_pid = 0x0000}).error() ==
          mpegts::MuxError::kInvalidOptions);

    const std::vector<Bytes> huge{frame_of(70'000, 0)};
    CHECK(mpegts::mux({.channels = 2}, huge).error() == mpegts::MuxError::kFrameTooLarge);
}

TEST_CASE("MPEG-TS mux of real encoded AC-3 round-trips through ac3::io::scan", "[mpegts]") {
    // Real, non-silent, multi-frame material - a silent or single-frame
    // stream would exercise almost none of the encoder and none of the
    // frame-2-onward MDCT overlap state (CONTRIBUTING.md's validation
    // discipline). Different tones per channel so a channel-order mistake
    // would show up as wrong content, not just a wrong byte count.
    auto encoder = std::make_unique<ac3::FrameEncoder>(
        ac3::EncoderConfig{.bitrate_kbps = 192, .acmod = ac3::Acmod::k2_0});
    std::vector<std::vector<float>> pcm(2, std::vector<float>(ac3::kSamplesPerFrame));
    const std::vector<std::span<const float>> views{pcm[0], pcm[1]};

    Bytes elementary;
    constexpr int kFrames = 6;
    for (int frame = 0; frame < kFrames; ++frame) {
        for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
            const auto nf = static_cast<float>(n);
            pcm[0][static_cast<std::size_t>(n)] = 0.3F * std::sin(nf * 0.05F);
            pcm[1][static_cast<std::size_t>(n)] = 0.3F * std::sin(nf * 0.13F);
        }
        const auto encoded = encoder->encode_frame(views);
        REQUIRE(encoded.has_value());
        elementary.insert(elementary.end(), encoded->begin(), encoded->end());
    }

    const auto scanned = ac3::io::scan(elementary);
    REQUIRE(scanned.has_value());
    REQUIRE(scanned->access_units.size() == static_cast<std::size_t>(kFrames));
    REQUIRE(scanned->kind == ac3::io::StreamKind::kAc3);

    std::vector<Bytes> frames;
    for (const auto unit : scanned->access_units) {
        frames.emplace_back(unit.begin(), unit.end());
    }

    const mpegts::AudioTrack track{.codec = mpegts::AudioCodec::kAc3,
                                   .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
                                   .channels = scanned->channels,
                                   .samples_per_frame = ac3::kSamplesPerFrame};
    const auto file = mpegts::mux(track, frames);
    REQUIRE(file.has_value());

    const auto packets = parse_packets(*file);
    const auto pmt = section_from_psi_packet(*packets_on_pid(packets, 0x1000).front());
    CHECK(std::to_integer<std::uint8_t>(pmt[17]) == 0x6A);  // AC-3, not Enhanced AC-3

    const auto audio_ptrs = packets_on_pid(packets, 0x0100);
    const auto access_units = reassemble_pes_payloads(audio_ptrs);
    REQUIRE(access_units.size() == frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        CHECK(access_units[i] == frames[i]);
    }
}

TEST_CASE("mpegts::Writer's concatenated pushes are mux()'s bytes exactly", "[mpegts]") {
    // The Writer's whole contract: the only state mux()'s loop carries
    // across access units lives on the Writer, so pushing the same frames
    // one at a time reproduces the batch output byte for byte - PSI repeat
    // cadence, continuity counters, PTS/PCR clock and all. Frame count
    // deliberately spans several PSI repeats and varies frame sizes.
    const mpegts::MuxOptions options{.psi_repeat_every_au = 3};
    for (const auto codec : {mpegts::AudioCodec::kAc3, mpegts::AudioCodec::kEac3}) {
        const mpegts::AudioTrack track{
            .codec = codec, .sample_rate = 48000, .channels = 6, .samples_per_frame = 1536};
        std::vector<Bytes> frames;
        for (std::size_t i = 0; i < 11; ++i) {
            frames.push_back(frame_of(600 + 40 * i, static_cast<std::uint8_t>(0x20 + i)));
        }

        const auto batch = mpegts::mux(track, frames, options);
        REQUIRE(batch.has_value());

        auto writer = mpegts::Writer::create(track, options);
        REQUIRE(writer.has_value());
        Bytes streamed;
        for (const auto& frame : frames) {
            const auto packets = writer->push(frame);
            REQUIRE(packets.has_value());
            CHECK_FALSE(packets->empty());
            streamed.insert(streamed.end(), packets->begin(), packets->end());
        }
        const auto tail = writer->finalize();
        CHECK(tail.empty());
        streamed.insert(streamed.end(), tail.begin(), tail.end());

        CHECK(writer->frames_written() == frames.size());
        REQUIRE(streamed.size() == batch->size());
        CHECK(std::equal(streamed.begin(), streamed.end(), batch->begin(), batch->end()));
    }
}

TEST_CASE("mpegts::Writer refuses what mux() refuses", "[mpegts]") {
    CHECK(mpegts::Writer::create({.channels = 0}).error() == mpegts::MuxError::kInvalidTrack);
    const mpegts::AudioTrack ok{.sample_rate = 48000, .channels = 2, .samples_per_frame = 1536};
    CHECK(mpegts::Writer::create(ok, {.pmt_pid = 0x0031, .audio_pid = 0x0031}).error() ==
          mpegts::MuxError::kInvalidOptions);
}

// --------------------------------------------------------------------------
// AC-4 (roadmap IM4): EN 300 468 Annex D.7's DVB signalling - stream_type
// 0x06 plus the extension descriptor 0x7F/0x15 - with the ISO 13818-1 §2.6.8
// registration descriptor ('AC-4') riding beside it for interop (it is what
// FFmpeg's own TS demuxer keys AC-4 on, verified against ffprobe).

TEST_CASE("MPEG-TS AC-4 writes DVB extension + registration descriptors", "[mpegts][ac4]") {
    const std::vector<Bytes> frames{frame_of(320, 0x4A), frame_of(320, 0x4B)};
    const auto file = mpegts::mux(
        {.codec = mpegts::AudioCodec::kAc4, .channels = 2, .samples_per_frame = 2048}, frames);
    REQUIRE(file.has_value());

    const auto pmt = pmt_of(*file);
    const auto first = first_descriptor(pmt);
    // Registration first: tag 0x05, format_identifier 'AC-4'.
    CHECK(first.tag == 0x05);
    REQUIRE(first.body.size() == 4);
    CHECK(first.body[0] == std::byte{'A'});
    CHECK(first.body[1] == std::byte{'C'});
    CHECK(first.body[2] == std::byte{'-'});
    CHECK(first.body[3] == std::byte{'4'});

    // Then the normative DVB extension descriptor: 0x7F, extension 0x15,
    // one flag byte with ac4_config_flag and toc_flag both clear.
    const auto second = descriptor_after(pmt, first);
    CHECK(second.tag == 0x7F);
    REQUIRE(second.body.size() == 2);
    CHECK(second.body[0] == std::byte{0x15});
    CHECK(second.body[1] == std::byte{0x00});
}

TEST_CASE("MPEG-TS refuses AC-4 under the ATSC profile", "[mpegts][ac4]") {
    const std::vector<Bytes> frames{frame_of(64, 0x4C)};
    const auto file = mpegts::mux(
        {.codec = mpegts::AudioCodec::kAc4, .channels = 2, .samples_per_frame = 2048}, frames,
        mpegts::MuxOptions{.profile = mpegts::BroadcastProfile::kAtsc});
    REQUIRE_FALSE(file.has_value());
    // ATSC never registered AC-4 for 13818-1 transport (A/342-2 is ATSC
    // 3.0's ROUTE/MMT) - refused, not given an invented stream_type.
    CHECK(file.error() == mpegts::MuxError::kInvalidOptions);
}
