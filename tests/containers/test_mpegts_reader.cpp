#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "mpegts/mpegts.hpp"
#include "mpegts/reader.hpp"

// The same two halves the Matroska and MP4 reader tests have.
//
// Round-tripping mux()/Writer answers "does the reader understand what this
// project writes" - and for MPEG-TS that is a narrower question than usual,
// because the writer implements only the DVB signalling profile and only the
// 188-byte grid. So the rest build transport streams by hand: ATSC's own
// stream_types, a registration descriptor, the M2TS and 204-byte grids a
// disc rip and a satellite capture arrive on, the unbounded PES length
// broadcast uses, a stream that starts mid-packet, and sections whose CRC
// does not match.

namespace {

using Bytes = std::vector<std::byte>;

void put_u8(Bytes& out, std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }

void put_u16(Bytes& out, std::uint16_t value) {
    put_u8(out, static_cast<std::uint8_t>(value >> 8));
    put_u8(out, static_cast<std::uint8_t>(value & 0xFF));
}

void put_bytes(Bytes& out, std::span<const std::byte> in) {
    out.insert(out.end(), in.begin(), in.end());
}

// An independent CRC-32/MPEG-2, transcribed from the polynomial rather than
// shared with ts_detail.hpp - a test that stamps its sections with the same
// code the reader checks them with proves only that the two agree.
std::uint32_t crc32(std::span<const std::byte> data) {
    std::uint32_t crc = 0xFFFF'FFFFU;
    for (const auto b : data) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b)) << 24U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000'0000U) != 0 ? (crc << 1U) ^ 0x04C1'1DB7U : (crc << 1U);
        }
    }
    return crc;
}

void append_crc(Bytes& section) {
    const std::uint32_t value = crc32(section);
    put_u8(section, static_cast<std::uint8_t>(value >> 24U));
    put_u8(section, static_cast<std::uint8_t>((value >> 16U) & 0xFF));
    put_u8(section, static_cast<std::uint8_t>((value >> 8U) & 0xFF));
    put_u8(section, static_cast<std::uint8_t>(value & 0xFF));
}

// One 188-byte TS packet carrying `payload`, padded with an adaptation-field
// stuffing tail when the payload is short.
Bytes ts_packet(std::uint16_t pid, bool unit_start, std::uint8_t& cc,
                std::span<const std::byte> payload) {
    Bytes pkt;
    put_u8(pkt, 0x47);
    put_u16(pkt, static_cast<std::uint16_t>((unit_start ? 0x4000U : 0U) | (pid & 0x1FFFU)));
    const std::size_t budget = 184;
    const bool needs_stuffing = payload.size() < budget;
    put_u8(pkt, static_cast<std::uint8_t>((needs_stuffing ? 0x30U : 0x10U) | (cc & 0x0FU)));
    cc = static_cast<std::uint8_t>((cc + 1) & 0x0FU);
    if (needs_stuffing) {
        const std::size_t adaptation = budget - payload.size() - 1;
        put_u8(pkt, static_cast<std::uint8_t>(adaptation));
        if (adaptation > 0) {
            put_u8(pkt, 0x00);  // flags: nothing set
            for (std::size_t i = 1; i < adaptation; ++i) {
                put_u8(pkt, 0xFF);
            }
        }
    }
    put_bytes(pkt, payload);
    REQUIRE(pkt.size() == 188);
    return pkt;
}

Bytes psi_packet(std::uint16_t pid, std::uint8_t& cc, const Bytes& section) {
    Bytes payload;
    put_u8(payload, 0x00);  // pointer_field
    put_bytes(payload, section);
    return ts_packet(pid, true, cc, payload);
}

Bytes pat_section(std::uint16_t program_number, std::uint16_t pmt_pid) {
    Bytes body;
    put_u16(body, 1);      // transport_stream_id
    put_u8(body, 0xC1);    // version 0, current
    put_u8(body, 0x00);    // section_number
    put_u8(body, 0x00);    // last_section_number
    put_u16(body, program_number);
    put_u16(body, static_cast<std::uint16_t>(0xE000U | (pmt_pid & 0x1FFFU)));

    Bytes section;
    put_u8(section, 0x00);  // table_id
    put_u16(section, static_cast<std::uint16_t>(0xB000U | ((body.size() + 4) & 0x0FFFU)));
    put_bytes(section, body);
    append_crc(section);
    return section;
}

struct EsSpec {
    std::uint8_t stream_type = 0x81;
    std::uint16_t pid = 0x0100;
    Bytes descriptors;
};

Bytes pmt_section(std::uint16_t program_number, std::span<const EsSpec> streams) {
    Bytes es_loop;
    for (const auto& es : streams) {
        put_u8(es_loop, es.stream_type);
        put_u16(es_loop, static_cast<std::uint16_t>(0xE000U | (es.pid & 0x1FFFU)));
        put_u16(es_loop,
                static_cast<std::uint16_t>(0xF000U | (es.descriptors.size() & 0x0FFFU)));
        put_bytes(es_loop, es.descriptors);
    }

    Bytes body;
    put_u16(body, program_number);
    put_u8(body, 0xC1);
    put_u8(body, 0x00);
    put_u8(body, 0x00);
    put_u16(body, 0xE100);  // PCR_PID
    put_u16(body, 0xF000);  // program_info_length 0
    put_bytes(body, es_loop);

    Bytes section;
    put_u8(section, 0x02);  // table_id
    put_u16(section, static_cast<std::uint16_t>(0xB000U | ((body.size() + 4) & 0x0FFFU)));
    put_bytes(section, body);
    append_crc(section);
    return section;
}

// A PES packet carrying `payload`. length == 0 selects the unbounded form
// broadcast uses, which ends only when the next one starts.
Bytes pes_packet(std::span<const std::byte> payload, bool unbounded) {
    Bytes pes;
    put_u8(pes, 0x00);
    put_u8(pes, 0x00);
    put_u8(pes, 0x01);
    put_u8(pes, 0xBD);  // private_stream_1
    const std::size_t declared = unbounded ? 0 : 3 + payload.size();
    put_u16(pes, static_cast<std::uint16_t>(declared));
    put_u8(pes, 0x84);  // '10' marker + data_alignment_indicator
    put_u8(pes, 0x00);  // no PTS
    put_u8(pes, 0x00);  // PES_header_data_length
    put_bytes(pes, payload);
    return pes;
}

// Slices one PES packet across as many TS packets as it needs.
void emit_pes(Bytes& out, std::uint16_t pid, std::uint8_t& cc, const Bytes& pes) {
    std::size_t at = 0;
    bool first = true;
    while (at < pes.size()) {
        const std::size_t take = std::min<std::size_t>(184, pes.size() - at);
        const auto slice = std::span<const std::byte>{pes}.subspan(at, take);
        const auto pkt = ts_packet(pid, first, cc, slice);
        out.insert(out.end(), pkt.begin(), pkt.end());
        at += take;
        first = false;
    }
}

Bytes frame_of(std::size_t size, std::uint8_t fill) {
    return Bytes(size, static_cast<std::byte>(fill));
}

std::vector<std::span<const std::byte>> views_of(const std::vector<Bytes>& frames) {
    return {frames.begin(), frames.end()};
}

// The elementary stream a set of payloads concatenates to - which is the
// reader's actual contract for MPEG-TS, since a PES payload is not promised
// to be exactly one access unit.
Bytes concat(std::span<const std::span<const std::byte>> payloads) {
    Bytes out;
    for (const auto& payload : payloads) {
        out.insert(out.end(), payload.begin(), payload.end());
    }
    return out;
}

Bytes concat(const std::vector<Bytes>& frames) {
    Bytes out;
    for (const auto& frame : frames) {
        out.insert(out.end(), frame.begin(), frame.end());
    }
    return out;
}

// Re-wraps a 188-byte stream onto one of the other two grids a real capture
// arrives on: M2TS prefixes each packet with a 4-byte arrival timestamp,
// and a DVB recording that kept its error correction appends 16 parity
// bytes.
Bytes to_m2ts(std::span<const std::byte> ts) {
    Bytes out;
    for (std::size_t at = 0; at + 188 <= ts.size(); at += 188) {
        put_u8(out, 0x12);  // an arrival timestamp; its value is nothing to us
        put_u8(out, 0x34);
        put_u8(out, 0x56);
        put_u8(out, 0x78);
        put_bytes(out, ts.subspan(at, 188));
    }
    return out;
}

Bytes to_204(std::span<const std::byte> ts) {
    Bytes out;
    for (std::size_t at = 0; at + 188 <= ts.size(); at += 188) {
        put_bytes(out, ts.subspan(at, 188));
        for (int i = 0; i < 16; ++i) {
            put_u8(out, 0xA5);  // stand-in Reed-Solomon parity
        }
    }
    return out;
}

Bytes read_in_chunks(std::span<const std::byte> file, std::size_t chunk) {
    mpegts::Reader reader{};
    Bytes got;
    const auto sink = [&got](std::span<const std::byte> payload) {
        got.insert(got.end(), payload.begin(), payload.end());
    };
    for (std::size_t offset = 0; offset < file.size(); offset += chunk) {
        const auto take = std::min(chunk, file.size() - offset);
        REQUIRE(reader.push(file.subspan(offset, take), sink).has_value());
    }
    REQUIRE(reader.finish(sink).has_value());
    return got;
}

// A complete hand-built stream: PAT, PMT, then one PES per frame.
Bytes build_stream(std::span<const EsSpec> streams, const std::vector<Bytes>& frames,
                   bool unbounded_pes = false, std::uint16_t program_number = 1) {
    std::uint8_t pat_cc = 0;
    std::uint8_t pmt_cc = 0;
    std::uint8_t es_cc = 0;
    constexpr std::uint16_t kPmtPid = 0x1000;

    Bytes out;
    const auto pat = psi_packet(0x0000, pat_cc, pat_section(program_number, kPmtPid));
    out.insert(out.end(), pat.begin(), pat.end());
    const auto pmt = psi_packet(kPmtPid, pmt_cc, pmt_section(program_number, streams));
    out.insert(out.end(), pmt.begin(), pmt.end());
    for (const auto& frame : frames) {
        emit_pes(out, streams.front().pid, es_cc, pes_packet(frame, unbounded_pes));
    }
    return out;
}

}  // namespace

TEST_CASE("MPEG-TS round-trips mux()'s access units", "[mpegts][reader]") {
    const std::vector<Bytes> frames{frame_of(700, 0x11), frame_of(512, 0x22),
                                    frame_of(1024, 0x33), frame_of(64, 0x44)};
    const mpegts::AudioTrack track{.codec = mpegts::AudioCodec::kEac3,
                                   .sample_rate = 48000,
                                   .channels = 6,
                                   .samples_per_frame = 1536};
    const auto file = mpegts::mux(track, views_of(frames));
    REQUIRE(file.has_value());

    const auto out = mpegts::demux(*file);
    REQUIRE(out.has_value());
    CHECK(out->stream.eac3);
    CHECK(out->stream.packet_size == 188);
    // The writer is the DVB profile: private data plus a descriptor.
    CHECK(out->stream.stream_type == 0x06);
    CHECK(out->stream.signalling == mpegts::CodecSignalling::kDvbDescriptor);
    CHECK(out->stream.elementary_pid == 0x0100);
    // mux() writes one access unit per PES, so here - and only here - the
    // payloads line up one-to-one with the frames. The contract is the
    // concatenation, which is what is checked.
    CHECK(concat(out->payloads) == concat(frames));
}

TEST_CASE("MPEG-TS round-trips an AC-3 track and the Writer's own output", "[mpegts][reader]") {
    const std::vector<Bytes> frames{frame_of(400, 0xA1), frame_of(400, 0xA2),
                                    frame_of(400, 0xA3)};
    const mpegts::AudioTrack track{.codec = mpegts::AudioCodec::kAc3,
                                   .sample_rate = 48000,
                                   .channels = 2,
                                   .samples_per_frame = 1536};

    auto writer = mpegts::Writer::create(track);
    REQUIRE(writer.has_value());
    Bytes file;
    for (const auto& frame : frames) {
        auto pushed = writer->push(frame);
        REQUIRE(pushed.has_value());
        file.insert(file.end(), pushed->begin(), pushed->end());
    }
    const auto tail = writer->finalize();
    file.insert(file.end(), tail.begin(), tail.end());

    const auto out = mpegts::demux(file);
    REQUIRE(out.has_value());
    CHECK_FALSE(out->stream.eac3);
    CHECK(concat(out->payloads) == concat(frames));
}

TEST_CASE("MPEG-TS Reader over arbitrary chunk boundaries matches demux()", "[mpegts][reader]") {
    const std::vector<Bytes> frames{frame_of(700, 0x11), frame_of(3, 0x22), frame_of(1500, 0x33),
                                    frame_of(64, 0x44)};
    const auto file = mpegts::mux(mpegts::AudioTrack{.codec = mpegts::AudioCodec::kEac3,
                                                     .sample_rate = 48000,
                                                     .channels = 6,
                                                     .samples_per_frame = 1536},
                                  views_of(frames));
    REQUIRE(file.has_value());

    for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{188},
                                    std::size_t{997}, file->size()}) {
        INFO("chunk size " << chunk);
        CHECK(read_in_chunks(*file, chunk) == concat(frames));
    }
}

TEST_CASE("MPEG-TS reads every way a PMT names the codec", "[mpegts][reader]") {
    const std::vector<Bytes> frames{frame_of(300, 0x51), frame_of(300, 0x52)};

    SECTION("ATSC stream_type 0x81 is AC-3") {
        const std::array<EsSpec, 1> streams{EsSpec{.stream_type = 0x81, .pid = 0x0100, .descriptors = {}}};
        const auto out = mpegts::demux(build_stream(streams, frames));
        REQUIRE(out.has_value());
        CHECK_FALSE(out->stream.eac3);
        CHECK(out->stream.signalling == mpegts::CodecSignalling::kAtscStreamType);
        CHECK(concat(out->payloads) == concat(frames));
    }

    SECTION("ATSC stream_type 0x87 is E-AC-3") {
        const std::array<EsSpec, 1> streams{EsSpec{.stream_type = 0x87, .pid = 0x0100, .descriptors = {}}};
        const auto out = mpegts::demux(build_stream(streams, frames));
        REQUIRE(out.has_value());
        CHECK(out->stream.eac3);
        CHECK(out->stream.signalling == mpegts::CodecSignalling::kAtscStreamType);
    }

    SECTION("DVB's Enhanced_AC3_descriptor on a private-data stream_type") {
        Bytes descriptors;
        put_u8(descriptors, 0x7A);
        put_u8(descriptors, 1);
        put_u8(descriptors, 0x00);
        const std::array<EsSpec, 1> streams{
            EsSpec{.stream_type = 0x06, .pid = 0x0100, .descriptors = descriptors}};
        const auto out = mpegts::demux(build_stream(streams, frames));
        REQUIRE(out.has_value());
        CHECK(out->stream.eac3);
        CHECK(out->stream.signalling == mpegts::CodecSignalling::kDvbDescriptor);
    }

    SECTION("a registration descriptor's 'EAC3' format identifier") {
        Bytes descriptors;
        put_u8(descriptors, 0x05);
        put_u8(descriptors, 4);
        for (const char c : std::string{"EAC3"}) {
            put_u8(descriptors, static_cast<std::uint8_t>(c));
        }
        const std::array<EsSpec, 1> streams{
            EsSpec{.stream_type = 0x87, .pid = 0x0100, .descriptors = descriptors}};
        const auto out = mpegts::demux(build_stream(streams, frames));
        REQUIRE(out.has_value());
        CHECK(out->stream.eac3);
    }

    SECTION("a stream this reader has no use for") {
        const std::array<EsSpec, 1> streams{EsSpec{.stream_type = 0x0F, .pid = 0x0100, .descriptors = {}}};  // AAC ADTS
        const auto out = mpegts::demux(build_stream(streams, frames));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mpegts::DemuxError::kNoAudioStream);
    }

    SECTION("the audio stream is picked out of several") {
        Bytes ac3_descriptor;
        put_u8(ac3_descriptor, 0x6A);
        put_u8(ac3_descriptor, 1);
        put_u8(ac3_descriptor, 0x00);
        const std::array<EsSpec, 3> streams{
            EsSpec{.stream_type = 0x1B, .pid = 0x0101, .descriptors = {}},  // H.264 video
            EsSpec{.stream_type = 0x0F, .pid = 0x0102, .descriptors = {}},  // AAC
            EsSpec{.stream_type = 0x06, .pid = 0x0103, .descriptors = ac3_descriptor},
        };
        // build_stream puts the PES on streams.front()'s PID, so reorder:
        // the AC-3 one has to be first for the frames to land on it.
        const std::array<EsSpec, 3> reordered{streams[2], streams[0], streams[1]};
        const auto out = mpegts::demux(build_stream(reordered, frames));
        REQUIRE(out.has_value());
        CHECK(out->stream.elementary_pid == 0x0103);
        CHECK(concat(out->payloads) == concat(frames));
    }
}

TEST_CASE("MPEG-TS reads the M2TS and 204-byte grids", "[mpegts][reader]") {
    const std::vector<Bytes> frames{frame_of(500, 0x71), frame_of(500, 0x72),
                                    frame_of(500, 0x73), frame_of(500, 0x74),
                                    frame_of(500, 0x75), frame_of(500, 0x76)};
    const auto plain = mpegts::mux(mpegts::AudioTrack{.codec = mpegts::AudioCodec::kEac3,
                                                      .sample_rate = 48000,
                                                      .channels = 6,
                                                      .samples_per_frame = 1536},
                                   views_of(frames));
    REQUIRE(plain.has_value());

    SECTION("M2TS, as a Blu-ray or AVCHD rip arrives") {
        const auto file = to_m2ts(*plain);
        const auto out = mpegts::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->stream.packet_size == 192);
        CHECK(concat(out->payloads) == concat(frames));
        CHECK(read_in_chunks(file, 333) == concat(frames));
    }

    SECTION("204 bytes, as a capture that kept its parity arrives") {
        const auto file = to_204(*plain);
        const auto out = mpegts::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->stream.packet_size == 204);
        CHECK(concat(out->payloads) == concat(frames));
    }
}

TEST_CASE("MPEG-TS reads the unbounded PES length broadcast uses", "[mpegts][reader]") {
    // PES_packet_length == 0: the packet ends when the next one starts, or
    // when the stream does. The last one is only complete at finish().
    const std::vector<Bytes> frames{frame_of(900, 0x81), frame_of(900, 0x82),
                                    frame_of(900, 0x83)};
    const std::array<EsSpec, 1> streams{EsSpec{.stream_type = 0x87, .pid = 0x0100, .descriptors = {}}};
    const auto file = build_stream(streams, frames, /*unbounded_pes=*/true);

    const auto out = mpegts::demux(file);
    REQUIRE(out.has_value());
    CHECK(concat(out->payloads) == concat(frames));
    // And streamed, where the final packet depends on finish() emitting.
    CHECK(read_in_chunks(file, 188) == concat(frames));
}

TEST_CASE("MPEG-TS locks onto a capture that starts mid-stream", "[mpegts][reader]") {
    // Tuning in late is the normal way a transport stream is acquired: the
    // recording starts wherever the tuner was, not on a packet boundary.
    const std::vector<Bytes> frames{frame_of(600, 0x91), frame_of(600, 0x92)};
    const std::array<EsSpec, 1> streams{EsSpec{.stream_type = 0x87, .pid = 0x0100, .descriptors = {}}};
    auto file = build_stream(streams, frames);

    Bytes prefixed;
    // 77 bytes of whatever the tuner had, including a decoy sync byte that
    // is not on the grid.
    for (int i = 0; i < 77; ++i) {
        put_u8(prefixed, i == 30 ? 0x47 : static_cast<std::uint8_t>(i));
    }
    prefixed.insert(prefixed.end(), file.begin(), file.end());

    const auto out = mpegts::demux(prefixed);
    REQUIRE(out.has_value());
    CHECK(concat(out->payloads) == concat(frames));
}

TEST_CASE("MPEG-TS refuses what it cannot read", "[mpegts][reader]") {
    SECTION("empty input") {
        const auto out = mpegts::demux({});
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mpegts::DemuxError::kNotTransportStream);
    }

    SECTION("a Matroska file") {
        Bytes ebml{std::byte{0x1A}, std::byte{0x45}, std::byte{0xDF}, std::byte{0xA3}};
        ebml.resize(4096, std::byte{0x00});
        const auto out = mpegts::demux(ebml);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mpegts::DemuxError::kNotTransportStream);
    }

    SECTION("a sync grid but no PAT") {
        std::uint8_t cc = 0;
        Bytes file;
        for (int i = 0; i < 8; ++i) {
            const auto pkt = ts_packet(0x0100, false, cc, frame_of(184, 0x5A));
            file.insert(file.end(), pkt.begin(), pkt.end());
        }
        const auto out = mpegts::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mpegts::DemuxError::kNoProgramme);
    }

    SECTION("a PAT whose CRC does not match is not believed") {
        const std::vector<Bytes> frames{frame_of(300, 0x61)};
        const std::array<EsSpec, 1> streams{EsSpec{.stream_type = 0x87, .pid = 0x0100, .descriptors = {}}};
        auto file = build_stream(streams, frames);
        // The PAT is the first packet, and - being far smaller than 184
        // bytes - ts_packet() pads it with adaptation-field stuffing ahead
        // of the payload, so the section does not start at a fixed offset
        // from the packet's own start. The packet's LAST byte is always the
        // CRC_32's own last byte regardless of that padding, which is what
        // is flipped here - exactly what a bit error looks like.
        file[187] = static_cast<std::byte>(std::to_integer<std::uint8_t>(file[187]) ^ 0xFF);
        const auto out = mpegts::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mpegts::DemuxError::kNoProgramme);
    }

    SECTION("an unbounded PES that never ends is bounded anyway") {
        // The one place a transport stream can ask for unbounded memory: a
        // PES_packet_length of 0 with no further payload_unit_start_indicator
        // ever sent.
        std::uint8_t pat_cc = 0;
        std::uint8_t pmt_cc = 0;
        std::uint8_t es_cc = 0;
        const std::array<EsSpec, 1> streams{EsSpec{.stream_type = 0x87, .pid = 0x0100, .descriptors = {}}};
        Bytes file;
        const auto pat = psi_packet(0x0000, pat_cc, pat_section(1, 0x1000));
        file.insert(file.end(), pat.begin(), pat.end());
        const auto pmt = psi_packet(0x1000, pmt_cc, pmt_section(1, streams));
        file.insert(file.end(), pmt.begin(), pmt.end());
        // One PES start, then thousands of continuation packets.
        emit_pes(file, 0x0100, es_cc, pes_packet(frame_of(100, 0x01), /*unbounded=*/true));
        for (int i = 0; i < 2000; ++i) {
            const auto pkt = ts_packet(0x0100, false, es_cc, frame_of(184, 0xCC));
            file.insert(file.end(), pkt.begin(), pkt.end());
        }
        const auto out = mpegts::demux(file, mpegts::ReadOptions{.max_pes_bytes = 64 * 1024});
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mpegts::DemuxError::kLimitExceeded);
    }
}

TEST_CASE("MPEG-TS describe() names every demux error", "[mpegts][reader]") {
    for (const auto error :
         {mpegts::DemuxError::kNotTransportStream, mpegts::DemuxError::kNoProgramme,
          mpegts::DemuxError::kNoAudioStream, mpegts::DemuxError::kMalformed,
          mpegts::DemuxError::kLimitExceeded}) {
        CHECK_FALSE(mpegts::describe(error).empty());
        CHECK(mpegts::describe(error) != "unknown error");
    }
    // DemuxError has an explicit uint8_t underlying type, so a value outside
    // every enumerator is well-defined to construct and switch on - describe()'s
    // fallthrough is reachable this way without invoking any UB to get there.
    CHECK(mpegts::describe(static_cast<mpegts::DemuxError>(200)) == "unknown error");
}

// --- error-path and less-common-shape coverage -----------------------------
//
// Everything below builds a transport stream by hand (bypassing mux()/
// Writer, and often build_stream()'s own PMT/PES shortcuts) to reach a
// hostile-or-merely-unusual layout the round-trip and "reads every way a PMT
// names the codec" tests above never touch.

TEST_CASE("MPEG-TS select_from_pmt handles malformed and foreign shapes", "[mpegts][reader]") {
    const std::vector<Bytes> frames{frame_of(200, 0x81)};

    SECTION("a PMT section too short to hold its own fixed fields") {
        std::uint8_t pat_cc = 0;
        std::uint8_t pmt_cc = 0;
        Bytes file;
        const auto pat = psi_packet(0x0000, pat_cc, pat_section(1, 0x1000));
        file.insert(file.end(), pat.begin(), pat.end());
        Bytes short_body{std::byte{0x00}, std::byte{0x01}};
        Bytes section;
        put_u8(section, 0x02);
        put_u16(section, static_cast<std::uint16_t>(0xB000U | ((short_body.size() + 4) & 0x0FFFU)));
        put_bytes(section, short_body);
        append_crc(section);
        const auto pmt = psi_packet(0x1000, pmt_cc, section);
        file.insert(file.end(), pmt.begin(), pmt.end());
        const auto out = mpegts::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mpegts::DemuxError::kNoAudioStream);
    }

    SECTION("a private-data stream with a descriptor tag this reader does not know") {
        Bytes descriptors;
        put_u8(descriptors, 0x9B);  // not the AC-3/Enhanced-AC-3 tag
        put_u8(descriptors, 1);
        put_u8(descriptors, 0x00);
        const std::array<EsSpec, 1> streams{
            EsSpec{.stream_type = 0x06, .pid = 0x0100, .descriptors = descriptors}};
        const auto out = mpegts::demux(build_stream(streams, frames));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mpegts::DemuxError::kNoAudioStream);
    }

    SECTION("a registration descriptor shorter than a 4-byte format_identifier") {
        // stream_type 0x06 (private data), not an ATSC one - 0x81/0x87 would
        // match on stream_type alone before the descriptor loop is ever
        // reached, making the malformed descriptor below irrelevant.
        Bytes descriptors;
        put_u8(descriptors, 0x05);
        put_u8(descriptors, 2);
        put_u8(descriptors, 'A');
        put_u8(descriptors, 'C');
        const std::array<EsSpec, 1> streams{
            EsSpec{.stream_type = 0x06, .pid = 0x0100, .descriptors = descriptors}};
        const auto out = mpegts::demux(build_stream(streams, frames));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mpegts::DemuxError::kNoAudioStream);
    }

    SECTION("a registration descriptor naming a format this reader does not recognise") {
        Bytes descriptors;
        put_u8(descriptors, 0x05);
        put_u8(descriptors, 4);
        for (const char c : std::string{"MPGA"}) {
            put_u8(descriptors, static_cast<std::uint8_t>(c));
        }
        const std::array<EsSpec, 1> streams{
            EsSpec{.stream_type = 0x06, .pid = 0x0100, .descriptors = descriptors}};
        const auto out = mpegts::demux(build_stream(streams, frames));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mpegts::DemuxError::kNoAudioStream);
    }

    SECTION("options.program_number rejects a PMT whose own section disagrees with the PAT") {
        // The PAT names programme 5 at pid 0x1000 (matching options below),
        // but the PMT section actually sitting at 0x1000 declares itself
        // programme 7 - select_from_pmt's own check catches what the PAT
        // upstream could not.
        std::uint8_t pat_cc = 0;
        std::uint8_t pmt_cc = 0;
        Bytes file;
        const auto pat = psi_packet(0x0000, pat_cc, pat_section(5, 0x1000));
        file.insert(file.end(), pat.begin(), pat.end());
        const std::array<EsSpec, 1> streams{EsSpec{.stream_type = 0x81, .pid = 0x0100, .descriptors = {}}};
        const auto pmt = psi_packet(0x1000, pmt_cc, pmt_section(7, streams));
        file.insert(file.end(), pmt.begin(), pmt.end());
        const auto out = mpegts::demux(file, mpegts::ReadOptions{.program_number = 5});
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mpegts::DemuxError::kNoAudioStream);
    }
}

TEST_CASE("MPEG-TS select_from_pat handles more than one programme entry", "[mpegts][reader]") {
    std::uint8_t pat_cc = 0;
    std::uint8_t pmt_cc = 0;
    std::uint8_t es_cc = 0;
    Bytes body;
    put_u16(body, 1);
    put_u8(body, 0xC1);
    put_u8(body, 0x00);
    put_u8(body, 0x00);
    put_u16(body, 0);          // program_number 0: the network_PID, not a programme
    put_u16(body, 0xE0AB);     // network_PID, irrelevant to a reader
    put_u16(body, 3);          // program_number 3
    put_u16(body, 0xE000U | 0x1000U);
    Bytes section;
    put_u8(section, 0x00);
    put_u16(section, static_cast<std::uint16_t>(0xB000U | ((body.size() + 4) & 0x0FFFU)));
    put_bytes(section, body);
    append_crc(section);

    Bytes file;
    const auto pat = psi_packet(0x0000, pat_cc, section);
    file.insert(file.end(), pat.begin(), pat.end());
    const std::array<EsSpec, 1> streams{EsSpec{.stream_type = 0x81, .pid = 0x0100, .descriptors = {}}};
    const auto pmt = psi_packet(0x1000, pmt_cc, pmt_section(3, streams));
    file.insert(file.end(), pmt.begin(), pmt.end());
    const std::vector<Bytes> frames{frame_of(200, 0x91)};
    emit_pes(file, 0x0100, es_cc, pes_packet(frames.front(), false));

    const auto out = mpegts::demux(file);
    REQUIRE(out.has_value());
    CHECK(out->stream.program_number == 3);
    CHECK(concat(out->payloads) == concat(frames));
}

TEST_CASE("MPEG-TS parse_packet drops what it cannot use without losing the rest",
         "[mpegts][reader]") {
    const auto with_extra_packet_before = [](Bytes extra) {
        const std::array<EsSpec, 1> streams{EsSpec{.stream_type = 0x87, .pid = 0x0100, .descriptors = {}}};
        const std::vector<Bytes> frames{frame_of(200, 0xA1)};
        auto file = build_stream(streams, frames);
        extra.insert(extra.end(), file.begin(), file.end());
        return std::pair{extra, frames};
    };

    SECTION("a null-PID packet is skipped") {
        std::uint8_t cc = 0;
        auto [file, frames] = with_extra_packet_before(ts_packet(0x1FFF, false, cc, frame_of(184, 0xFF)));
        const auto out = mpegts::demux(file);
        REQUIRE(out.has_value());
        CHECK(concat(out->payloads) == concat(frames));
    }

    SECTION("an adaptation-field-only packet (no payload) is skipped") {
        Bytes pkt;
        put_u8(pkt, 0x47);
        put_u16(pkt, 0x0000);
        put_u8(pkt, 0x20);  // adaptation_field_control = 0b10: adaptation only
        put_u8(pkt, 183);   // adaptation_length fills the rest of the packet
        put_u8(pkt, 0x00);
        Bytes stuffing(182, std::byte{0xFF});
        pkt.insert(pkt.end(), stuffing.begin(), stuffing.end());
        REQUIRE(pkt.size() == 188);
        auto [file, frames] = with_extra_packet_before(pkt);
        const auto out = mpegts::demux(file);
        REQUIRE(out.has_value());
        CHECK(concat(out->payloads) == concat(frames));
    }

    SECTION("an adaptation_length that runs off the packet is dropped") {
        Bytes pkt;
        put_u8(pkt, 0x47);
        put_u16(pkt, 0x0000);
        put_u8(pkt, 0x30);  // adaptation_field_control = 0b11: both
        put_u8(pkt, 250);   // far past what 188 bytes can hold
        Bytes rest(183, std::byte{0x00});
        pkt.insert(pkt.end(), rest.begin(), rest.end());
        REQUIRE(pkt.size() == 188);
        auto [file, frames] = with_extra_packet_before(pkt);
        const auto out = mpegts::demux(file);
        REQUIRE(out.has_value());
        CHECK(concat(out->payloads) == concat(frames));
    }

    SECTION("an adaptation field that consumes the whole packet leaves no payload") {
        Bytes pkt;
        put_u8(pkt, 0x47);
        put_u16(pkt, 0x0000);
        put_u8(pkt, 0x30);
        put_u8(pkt, 183);  // 5 + 183 == 188: no room left for a payload byte
        Bytes rest(183, std::byte{0x00});
        pkt.insert(pkt.end(), rest.begin(), rest.end());
        REQUIRE(pkt.size() == 188);
        auto [file, frames] = with_extra_packet_before(pkt);
        const auto out = mpegts::demux(file);
        REQUIRE(out.has_value());
        CHECK(concat(out->payloads) == concat(frames));
    }

    SECTION("a packet whose sync byte has slipped is dropped, not fatal") {
        // Still a full 188 bytes (a genuinely wrong-sized slice can't reach
        // parse_packet at all - walk() only ever hands it a fixed-size
        // window), just with byte 0 corrupted, so the grid stays aligned
        // for every packet after it.
        std::uint8_t cc = 0;
        auto slipped = ts_packet(0x0100, false, cc, frame_of(184, 0x00));
        slipped[0] = std::byte{0x00};
        auto [file, frames] = with_extra_packet_before(slipped);
        const auto out = mpegts::demux(file);
        REQUIRE(out.has_value());
        CHECK(concat(out->payloads) == concat(frames));
    }
}

TEST_CASE("MPEG-TS PES reassembly handles a malformed or empty start", "[mpegts][reader]") {
    const std::array<EsSpec, 1> streams{EsSpec{.stream_type = 0x87, .pid = 0x0100, .descriptors = {}}};

    SECTION("a PES payload under 6 bytes at unit_start carries no start code to check") {
        std::uint8_t pat_cc = 0;
        std::uint8_t pmt_cc = 0;
        std::uint8_t es_cc = 0;
        Bytes file;
        const auto pat = psi_packet(0x0000, pat_cc, pat_section(1, 0x1000));
        file.insert(file.end(), pat.begin(), pat.end());
        const auto pmt = psi_packet(0x1000, pmt_cc, pmt_section(1, streams));
        file.insert(file.end(), pmt.begin(), pmt.end());
        const auto short_start = ts_packet(0x0100, true, es_cc, frame_of(3, 0x00));
        file.insert(file.end(), short_start.begin(), short_start.end());
        const std::vector<Bytes> frames{frame_of(200, 0xB1)};
        emit_pes(file, 0x0100, es_cc, pes_packet(frames.front(), false));

        const auto out = mpegts::demux(file);
        REQUIRE(out.has_value());
        CHECK(concat(out->payloads) == concat(frames));
    }

    SECTION("a payload_unit_start packet without a real PES start code is dropped") {
        std::uint8_t pat_cc = 0;
        std::uint8_t pmt_cc = 0;
        std::uint8_t es_cc = 0;
        Bytes file;
        const auto pat = psi_packet(0x0000, pat_cc, pat_section(1, 0x1000));
        file.insert(file.end(), pat.begin(), pat.end());
        const auto pmt = psi_packet(0x1000, pmt_cc, pmt_section(1, streams));
        file.insert(file.end(), pmt.begin(), pmt.end());
        const auto bad_start = ts_packet(0x0100, true, es_cc, frame_of(184, 0xEE));
        file.insert(file.end(), bad_start.begin(), bad_start.end());
        const std::vector<Bytes> frames{frame_of(200, 0xB2)};
        emit_pes(file, 0x0100, es_cc, pes_packet(frames.front(), false));

        const auto out = mpegts::demux(file);
        REQUIRE(out.has_value());
        CHECK(concat(out->payloads) == concat(frames));
    }

    SECTION("a non-unit-start packet before any PES has opened is dropped") {
        std::uint8_t pat_cc = 0;
        std::uint8_t pmt_cc = 0;
        std::uint8_t es_cc = 0;
        Bytes file;
        const auto pat = psi_packet(0x0000, pat_cc, pat_section(1, 0x1000));
        file.insert(file.end(), pat.begin(), pat.end());
        const auto pmt = psi_packet(0x1000, pmt_cc, pmt_section(1, streams));
        file.insert(file.end(), pmt.begin(), pmt.end());
        const auto stray = ts_packet(0x0100, false, es_cc, frame_of(184, 0xDD));
        file.insert(file.end(), stray.begin(), stray.end());
        const std::vector<Bytes> frames{frame_of(200, 0xB3)};
        emit_pes(file, 0x0100, es_cc, pes_packet(frames.front(), false));

        const auto out = mpegts::demux(file);
        REQUIRE(out.has_value());
        CHECK(concat(out->payloads) == concat(frames));
    }

    SECTION("a PES that never reaches its own 9-byte header before the stream ends") {
        std::uint8_t pat_cc = 0;
        std::uint8_t pmt_cc = 0;
        std::uint8_t es_cc = 0;
        Bytes file;
        const auto pat = psi_packet(0x0000, pat_cc, pat_section(1, 0x1000));
        file.insert(file.end(), pat.begin(), pat.end());
        const auto pmt = psi_packet(0x1000, pmt_cc, pmt_section(1, streams));
        file.insert(file.end(), pmt.begin(), pmt.end());
        // Exactly 6 bytes: enough to pass the start-code check and open a
        // PES (unbounded form, PES_packet_length == 0), but nowhere near
        // emit_pes's own 9-byte minimum header - the stream simply ends
        // with the PES still open and too short to say anything about.
        Bytes truncated_pes{std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
                            std::byte{0xBD}, std::byte{0x00}, std::byte{0x00}};
        const auto pkt = ts_packet(0x0100, true, es_cc, truncated_pes);
        file.insert(file.end(), pkt.begin(), pkt.end());

        const auto out = mpegts::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->payloads.empty());
    }

    SECTION("a PES header whose own fields fill the packet leaves no payload") {
        std::uint8_t pat_cc = 0;
        std::uint8_t pmt_cc = 0;
        std::uint8_t es_cc = 0;
        Bytes file;
        const auto pat = psi_packet(0x0000, pat_cc, pat_section(1, 0x1000));
        file.insert(file.end(), pat.begin(), pat.end());
        const auto pmt = psi_packet(0x1000, pmt_cc, pmt_section(1, streams));
        file.insert(file.end(), pmt.begin(), pmt.end());
        // PES_packet_length == 3, header_data_length == 0: the declared
        // length (9 bytes total: the 6 fixed bytes + this 3) is exactly the
        // fixed header's own size, so pes_want is reached the instant the
        // header itself arrives and emit_pes finds nothing past offset 9.
        Bytes header_only{std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0xBD},
                          std::byte{0x00}, std::byte{0x03}, std::byte{0x84}, std::byte{0x00},
                          std::byte{0x00}};
        const auto pkt = ts_packet(0x0100, true, es_cc, header_only);
        file.insert(file.end(), pkt.begin(), pkt.end());
        const std::vector<Bytes> frames{frame_of(200, 0xB4)};
        emit_pes(file, 0x0100, es_cc, pes_packet(frames.front(), false));

        const auto out = mpegts::demux(file);
        REQUIRE(out.has_value());
        CHECK(concat(out->payloads) == concat(frames));
    }
}

TEST_CASE("MPEG-TS finish_verdict distinguishes no-PAT from a PAT naming no programme",
         "[mpegts][reader]") {
    // A PAT that parses fine (real CRC, table_id) but whose only entry is
    // program_number 0 (the network_PID, not a programme) - saw_pat becomes
    // true without ever setting have_pmt_pid, unlike the "no PAT at all"
    // case in the "refuses what it cannot read" TEST_CASE above.
    std::uint8_t pat_cc = 0;
    Bytes body;
    put_u16(body, 1);
    put_u8(body, 0xC1);
    put_u8(body, 0x00);
    put_u8(body, 0x00);
    put_u16(body, 0);       // program_number 0
    put_u16(body, 0xE0AB);  // network_PID
    Bytes section;
    put_u8(section, 0x00);
    put_u16(section, static_cast<std::uint16_t>(0xB000U | ((body.size() + 4) & 0x0FFFU)));
    put_bytes(section, body);
    append_crc(section);

    Bytes file;
    const auto pat = psi_packet(0x0000, pat_cc, section);
    file.insert(file.end(), pat.begin(), pat.end());
    // find_sync needs at least two packets on the grid to lock at all (see
    // the "very short capture" test below) - one more filler packet, not a
    // second PAT, so the ONLY thing that ever set saw_pat is the section above.
    std::uint8_t filler_cc = 0;
    const auto filler = ts_packet(0x2000, false, filler_cc, frame_of(184, 0x5A));
    file.insert(file.end(), filler.begin(), filler.end());

    const auto out = mpegts::demux(file);
    REQUIRE_FALSE(out.has_value());
    CHECK(out.error() == mpegts::DemuxError::kNoProgramme);
}

TEST_CASE("MPEG-TS find_sync locks onto a very short capture", "[mpegts][reader]") {
    // Fewer than kSyncConfirmations (5) packets are present - only 3 here -
    // which is still believed once at least two lined up.
    std::uint8_t cc = 0;
    Bytes file;
    for (int i = 0; i < 3; ++i) {
        const auto pkt = ts_packet(0x0100, false, cc, frame_of(184, 0x33));
        file.insert(file.end(), pkt.begin(), pkt.end());
    }
    const auto out = mpegts::demux(file);
    // No PAT ever arrives, but sync itself must have locked (not
    // kNotTransportStream) for the verdict to reach the programme check.
    REQUIRE_FALSE(out.has_value());
    CHECK(out.error() == mpegts::DemuxError::kNoProgramme);
}

TEST_CASE("MPEG-TS gives up on a sync search past its own budget", "[mpegts][reader]") {
    Bytes file(2048, std::byte{0x00});  // no 0x47 anywhere
    const auto out = mpegts::demux(file, mpegts::ReadOptions{.max_sync_search_bytes = 512});
    REQUIRE_FALSE(out.has_value());
    CHECK(out.error() == mpegts::DemuxError::kNotTransportStream);
}

TEST_CASE("MPEG-TS Reader surfaces a walk error directly from push()", "[mpegts][reader]") {
    // walk() always keeps a 204*kSyncConfirmations (1020-byte) tail that
    // might still hold the start of a grid, counting only what it drops
    // beyond that against the search budget - so the pushed chunk has to be
    // bigger than that tail for one push() to exceed a small budget outright.
    mpegts::Reader reader{mpegts::ReadOptions{.max_sync_search_bytes = 64}};
    const Bytes garbage(1200, std::byte{0x00});  // no 0x47 anywhere
    const auto sink = [](std::span<const std::byte>) {};
    const auto pushed = reader.push(garbage, sink);
    REQUIRE_FALSE(pushed.has_value());
    CHECK(pushed.error() == mpegts::DemuxError::kNotTransportStream);
}
