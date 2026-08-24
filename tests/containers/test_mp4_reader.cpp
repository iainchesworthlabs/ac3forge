#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mp4/mp4.hpp"
#include "mp4/reader.hpp"

// Two halves, the same split the Matroska reader's tests make.
//
// Round-tripping mux()/fragment() answers "does the reader understand what
// this project writes", which a reader and writer sharing a misunderstanding
// would also pass. So the rest build ISOBMFF by hand out of shapes the
// writers never emit - co64 instead of stco, stz2 instead of stsz, several
// samples per chunk, a 64-bit largesize header, an mdat declared to run to
// end-of-file, moov placed after mdat, and a pile of sample tables that lie.

namespace {

using Bytes = std::vector<std::byte>;

// --- a hand ISOBMFF writer, independent of src/mp4/src ----------------------

void put_u8(Bytes& out, std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }

void put_u16(Bytes& out, std::uint16_t value) {
    put_u8(out, static_cast<std::uint8_t>(value >> 8));
    put_u8(out, static_cast<std::uint8_t>(value & 0xFF));
}

void put_u32(Bytes& out, std::uint32_t value) {
    put_u16(out, static_cast<std::uint16_t>(value >> 16));
    put_u16(out, static_cast<std::uint16_t>(value & 0xFFFF));
}

void put_u64(Bytes& out, std::uint64_t value) {
    put_u32(out, static_cast<std::uint32_t>(value >> 32));
    put_u32(out, static_cast<std::uint32_t>(value & 0xFFFF'FFFFU));
}

void put_fourcc(Bytes& out, std::string_view text) {
    for (const char c : text) {
        put_u8(out, static_cast<std::uint8_t>(c));
    }
}

void put_bytes(Bytes& out, std::span<const std::byte> in) { out.insert(out.end(), in.begin(), in.end()); }

Bytes box(std::string_view type, std::span<const std::byte> body) {
    Bytes out;
    put_u32(out, static_cast<std::uint32_t>(8 + body.size()));
    put_fourcc(out, type);
    put_bytes(out, body);
    return out;
}

// The 64-bit largesize form (size field == 1, then a u64) - never written by
// this project, common in files with a large mdat.
Bytes large_box(std::string_view type, std::span<const std::byte> body) {
    Bytes out;
    put_u32(out, 1);
    put_fourcc(out, type);
    put_u64(out, 16 + body.size());
    put_bytes(out, body);
    return out;
}

Bytes fullbox(std::string_view type, std::uint8_t version, std::uint32_t flags,
              std::span<const std::byte> body) {
    Bytes full;
    put_u8(full, version);
    put_u8(full, static_cast<std::uint8_t>(flags >> 16));
    put_u8(full, static_cast<std::uint8_t>((flags >> 8) & 0xFF));
    put_u8(full, static_cast<std::uint8_t>(flags & 0xFF));
    put_bytes(full, body);
    return box(type, full);
}

Bytes frame_of(std::size_t size, std::uint8_t fill) {
    return Bytes(size, static_cast<std::byte>(fill));
}

std::vector<std::span<const std::byte>> views_of(const std::vector<Bytes>& frames) {
    return {frames.begin(), frames.end()};
}

std::vector<Bytes> owned(std::span<const std::span<const std::byte>> frames) {
    std::vector<Bytes> out;
    out.reserve(frames.size());
    for (const auto& f : frames) {
        out.emplace_back(f.begin(), f.end());
    }
    return out;
}

std::vector<Bytes> read_in_chunks(std::span<const std::byte> file, std::size_t chunk) {
    mp4::Reader reader{};
    std::vector<Bytes> got;
    const auto sink = [&got](std::span<const std::byte> sample) {
        got.emplace_back(sample.begin(), sample.end());
    };
    for (std::size_t offset = 0; offset < file.size(); offset += chunk) {
        const auto take = std::min(chunk, file.size() - offset);
        REQUIRE(reader.push(file.subspan(offset, take), sink).has_value());
    }
    REQUIRE(reader.finish().has_value());
    return got;
}

// A dec3 payload matching what ac3::io::build_codec_config_box writes for a
// 5.1 JOC stream: data_rate 448, one independent substream, fscod 0, bsid 16,
// acmod 7, lfeon, and TS 103 420's Atmos extension with complexity index 16.
// Hand-packed here rather than built with the writer, for the same reason the
// EBML above is hand-packed.
Bytes atmos_dec3() {
    // 448(13) | 0(3) -> 0b0000111000000_000
    Bytes out;
    put_u16(out, static_cast<std::uint16_t>((448U << 3U) | 0U));
    // fscod(2)=0 bsid(5)=16 reserved(1)=0 -> 00 10000 0
    put_u8(out, 0b0010'0000);
    // asvc(1)=0 bsmod(3)=0 acmod(3)=7 lfeon(1)=1 -> 0 000 111 1
    put_u8(out, 0b0000'1111);
    // reserved(3)=0 num_dep_sub(4)=0 reserved(1)=0 -> 000 0000 0
    put_u8(out, 0b0000'0000);
    // reserved(7)=0 flag_ec3_extension_type_a(1)=1, then complexity(8)=16.
    put_u8(out, 0x01);
    put_u8(out, 16);
    return out;
}

// A minimal plain moov describing `sizes` samples laid out back-to-back from
// `mdat_data_offset`, with `samples_per_chunk` samples in every chunk.
struct MoovSpec {
    std::vector<std::uint32_t> sizes;
    std::uint32_t samples_per_chunk = 1;
    std::uint64_t mdat_data_offset = 0;
    std::string entry{"ec-3"};
    bool co64 = false;
    bool stz2 = false;
    std::uint32_t track_id = 1;
    std::uint32_t channels = 6;
    std::uint32_t sample_rate = 48000;
};

Bytes build_stsd(const MoovSpec& spec) {
    Bytes entry_body;
    put_u32(entry_body, 0);
    put_u16(entry_body, 0);  // SampleEntry reserved[6]
    put_u16(entry_body, 1);  // data_reference_index
    put_u32(entry_body, 0);
    put_u32(entry_body, 0);  // AudioSampleEntry reserved[2]
    put_u16(entry_body, static_cast<std::uint16_t>(spec.channels));
    put_u16(entry_body, 16);  // samplesize
    put_u16(entry_body, 0);   // pre_defined
    put_u16(entry_body, 0);   // reserved
    put_u32(entry_body, spec.sample_rate << 16U);
    const auto config = atmos_dec3();
    put_bytes(entry_body, box(spec.entry == "ac-3" ? "dac3" : "dec3", config));

    Bytes body;
    put_u32(body, 1);  // entry_count
    put_bytes(body, box(spec.entry, entry_body));
    return fullbox("stsd", 0, 0, body);
}

Bytes build_moov(const MoovSpec& spec) {
    Bytes stbl;
    put_bytes(stbl, build_stsd(spec));

    // stts: one run, every sample the same duration.
    Bytes stts;
    put_u32(stts, 1);
    put_u32(stts, static_cast<std::uint32_t>(spec.sizes.size()));
    put_u32(stts, 1536);
    put_bytes(stbl, fullbox("stts", 0, 0, stts));

    Bytes stsc;
    put_u32(stsc, 1);  // entry_count
    put_u32(stsc, 1);  // first_chunk
    put_u32(stsc, spec.samples_per_chunk);
    put_u32(stsc, 1);  // sample_description_index
    put_bytes(stbl, fullbox("stsc", 0, 0, stsc));

    if (spec.stz2) {
        Bytes stz2;
        put_u32(stz2, 0);   // reserved(24) + field_size(8), filled below
        stz2[0] = std::byte{0};
        stz2[1] = std::byte{0};
        stz2[2] = std::byte{0};
        stz2[3] = std::byte{16};  // field_size
        put_u32(stz2, static_cast<std::uint32_t>(spec.sizes.size()));
        for (const auto size : spec.sizes) {
            put_u16(stz2, static_cast<std::uint16_t>(size));
        }
        put_bytes(stbl, fullbox("stz2", 0, 0, stz2));
    } else {
        Bytes stsz;
        put_u32(stsz, 0);  // sample_size: per-sample table follows
        put_u32(stsz, static_cast<std::uint32_t>(spec.sizes.size()));
        for (const auto size : spec.sizes) {
            put_u32(stsz, size);
        }
        put_bytes(stbl, fullbox("stsz", 0, 0, stsz));
    }

    // Chunk offsets: samples are contiguous, so each chunk starts where the
    // previous one's samples ended.
    std::vector<std::uint64_t> offsets;
    std::uint64_t cursor = spec.mdat_data_offset;
    for (std::size_t i = 0; i < spec.sizes.size(); i += spec.samples_per_chunk) {
        offsets.push_back(cursor);
        for (std::size_t j = i; j < spec.sizes.size() && j < i + spec.samples_per_chunk; ++j) {
            cursor += spec.sizes[j];
        }
    }
    Bytes co;
    put_u32(co, static_cast<std::uint32_t>(offsets.size()));
    for (const auto offset : offsets) {
        if (spec.co64) {
            put_u64(co, offset);
        } else {
            put_u32(co, static_cast<std::uint32_t>(offset));
        }
    }
    put_bytes(stbl, fullbox(spec.co64 ? "co64" : "stco", 0, 0, co));

    Bytes minf;
    put_bytes(minf, box("stbl", stbl));

    Bytes mdhd;
    put_u32(mdhd, 0);
    put_u32(mdhd, 0);                 // creation/modification
    put_u32(mdhd, spec.sample_rate);  // timescale
    put_u32(mdhd, 0);                 // duration
    put_u16(mdhd, 0x15C7);            // "eng"
    put_u16(mdhd, 0);
    Bytes mdia;
    put_bytes(mdia, fullbox("mdhd", 0, 0, mdhd));
    put_bytes(mdia, box("minf", minf));

    Bytes tkhd;
    put_u32(tkhd, 0);
    put_u32(tkhd, 0);              // creation/modification
    put_u32(tkhd, spec.track_id);  // track_ID
    put_u32(tkhd, 0);              // reserved
    put_u32(tkhd, 0);              // duration
    for (int i = 0; i < 15; ++i) {
        put_u32(tkhd, 0);  // the rest, unread here
    }
    Bytes trak;
    put_bytes(trak, fullbox("tkhd", 0, 0x000007, tkhd));
    put_bytes(trak, box("mdia", mdia));

    Bytes moov;
    put_bytes(moov, box("trak", trak));
    return box("moov", moov);
}

Bytes ftyp() {
    Bytes body;
    put_fourcc(body, "isom");
    put_u32(body, 0);
    put_fourcc(body, "isom");
    return box("ftyp", body);
}

}  // namespace

TEST_CASE("MP4 round-trips mux()'s frames back byte-for-byte", "[mp4][reader]") {
    const std::vector<Bytes> frames{frame_of(700, 0x11), frame_of(512, 0x22),
                                    frame_of(1024, 0x33), frame_of(64, 0x44)};
    const mp4::AudioTrack track{.codec_id = std::string{mp4::kCodecEac3},
                                .sample_rate = 48000,
                                .channels = 6,
                                .samples_per_frame = 1536,
                                .codec_config = atmos_dec3(),
                                .language = "eng"};
    const auto file = mp4::mux(track, views_of(frames));
    REQUIRE(file.has_value());

    const auto out = mp4::demux(*file);
    REQUIRE(out.has_value());
    CHECK(out->track.codec_id == "ec-3");
    CHECK(out->track.sample_rate == 48000);
    CHECK(out->track.channels == 6);
    CHECK(out->track.timescale == 48000);
    CHECK(out->track.language == "eng");
    CHECK(out->track.track_id == 1);
    CHECK(owned(out->samples) == frames);

    // The dec3 box survives the round trip, Atmos extension included - the
    // exact signalling an FFmpeg remux is known to drop.
    CHECK(out->track.codec_config.eac3);
    CHECK(out->track.codec_config.bsid == 16);
    CHECK(out->track.codec_config.acmod == 7);
    CHECK(out->track.codec_config.lfeon);
    CHECK(out->track.codec_config.data_rate_kbps == 448);
    REQUIRE(out->track.codec_config.oba_complexity_index.has_value());
    CHECK(*out->track.codec_config.oba_complexity_index == 16);
    CHECK(out->track.codec_config.payload == atmos_dec3());
}

TEST_CASE("MP4 round-trips fragment()'s media segments", "[mp4][reader][fragment]") {
    std::vector<Bytes> frames;
    for (int i = 0; i < 20; ++i) {
        frames.push_back(frame_of(200 + (static_cast<std::size_t>(i) * 7),
                                  static_cast<std::uint8_t>(0x40 + i)));
    }
    const mp4::AudioTrack track{.codec_id = std::string{mp4::kCodecEac3},
                                .sample_rate = 48000,
                                .channels = 6,
                                .samples_per_frame = 1536,
                                .codec_config = atmos_dec3(),
                                .language = "und"};
    const auto out = mp4::fragment(track, views_of(frames),
                                   mp4::FragmentOptions{.frames_per_fragment = 6});
    REQUIRE(out.has_value());

    // A CMAF track is delivered as the init segment followed by every media
    // segment, which is exactly how a player concatenates them.
    Bytes whole = out->init_segment;
    for (const auto& segment : out->media_segments) {
        whole.insert(whole.end(), segment.bytes.begin(), segment.bytes.end());
    }

    const auto read = mp4::demux(whole);
    REQUIRE(read.has_value());
    CHECK(read->track.codec_id == "ec-3");
    CHECK(owned(read->samples) == frames);
    CHECK(read_in_chunks(whole, 97) == frames);
}

TEST_CASE("MP4 Reader over arbitrary chunk boundaries matches demux()", "[mp4][reader]") {
    const std::vector<Bytes> frames{frame_of(700, 0x11), frame_of(3, 0x22), frame_of(1500, 0x33),
                                    frame_of(64, 0x44), frame_of(900, 0x55)};
    const auto file = mp4::mux(mp4::AudioTrack{.codec_id = std::string{mp4::kCodecEac3},
                                               .sample_rate = 48000,
                                               .channels = 6,
                                               .samples_per_frame = 1536,
                                               .codec_config = atmos_dec3(),
                                               .language = "und"},
                               views_of(frames));
    REQUIRE(file.has_value());

    for (const std::size_t chunk :
         {std::size_t{1}, std::size_t{3}, std::size_t{7}, std::size_t{64}, std::size_t{997},
          file->size()}) {
        INFO("chunk size " << chunk);
        CHECK(read_in_chunks(*file, chunk) == frames);
    }
}

TEST_CASE("MP4 reads sample tables this project never writes", "[mp4][reader]") {
    const std::vector<std::uint32_t> sizes{100, 250, 75, 400, 33, 180};
    std::vector<Bytes> expect;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        expect.push_back(frame_of(sizes[i], static_cast<std::uint8_t>(0xA0 + i)));
    }

    const auto assemble = [&](MoovSpec spec, bool large_mdat) {
        // Two passes, the same chicken-and-egg mp4::mux itself solves: moov's
        // size fixes where mdat's data starts, and stco has to name that.
        spec.mdat_data_offset = 0;
        const auto measured = build_moov(spec);
        const std::size_t mdat_header = large_mdat ? 16 : 8;
        spec.mdat_data_offset = ftyp().size() + measured.size() + mdat_header;
        const auto moov = build_moov(spec);
        REQUIRE(moov.size() == measured.size());

        Bytes payload;
        for (const auto& frame : expect) {
            payload.insert(payload.end(), frame.begin(), frame.end());
        }
        Bytes file = ftyp();
        file.insert(file.end(), moov.begin(), moov.end());
        const auto mdat = large_mdat ? large_box("mdat", payload) : box("mdat", payload);
        file.insert(file.end(), mdat.begin(), mdat.end());
        return file;
    };

    SECTION("co64 rather than stco") {
        const auto file = assemble(MoovSpec{.sizes = sizes, .co64 = true}, false);
        const auto out = mp4::demux(file);
        REQUIRE(out.has_value());
        CHECK(owned(out->samples) == expect);
    }

    SECTION("stz2's compact 16-bit sizes rather than stsz") {
        const auto file = assemble(MoovSpec{.sizes = sizes, .stz2 = true}, false);
        const auto out = mp4::demux(file);
        REQUIRE(out.has_value());
        CHECK(owned(out->samples) == expect);
    }

    SECTION("three samples per chunk rather than one") {
        const auto file = assemble(MoovSpec{.sizes = sizes, .samples_per_chunk = 3}, false);
        const auto out = mp4::demux(file);
        REQUIRE(out.has_value());
        CHECK(owned(out->samples) == expect);
        CHECK(read_in_chunks(file, 64) == expect);
    }

    SECTION("a 64-bit largesize mdat header") {
        const auto file = assemble(MoovSpec{.sizes = sizes}, true);
        const auto out = mp4::demux(file);
        REQUIRE(out.has_value());
        CHECK(owned(out->samples) == expect);
    }

    SECTION("an 'ac-3' entry with a dac3 box") {
        const auto file = assemble(MoovSpec{.sizes = sizes, .entry = "ac-3"}, false);
        const auto out = mp4::demux(file);
        REQUIRE(out.has_value());
        CHECK(out->track.codec_id == "ac-3");
        CHECK_FALSE(out->track.codec_config.eac3);
        CHECK(owned(out->samples) == expect);
    }
}

TEST_CASE("MP4 with moov after mdat reads batch but not streamed", "[mp4][reader]") {
    // A muxer that did not rewrite the file for "faststart" leaves the sample
    // table behind the data it indexes. demux() reaches backwards happily; a
    // stream cannot, and says so rather than returning nothing.
    const std::vector<std::uint32_t> sizes{120, 340, 90};
    std::vector<Bytes> expect;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        expect.push_back(frame_of(sizes[i], static_cast<std::uint8_t>(0xC0 + i)));
    }
    Bytes payload;
    for (const auto& frame : expect) {
        payload.insert(payload.end(), frame.begin(), frame.end());
    }

    MoovSpec spec{.sizes = sizes};
    spec.mdat_data_offset = ftyp().size() + 8;
    const auto moov = build_moov(spec);

    Bytes file = ftyp();
    const auto mdat = box("mdat", payload);
    file.insert(file.end(), mdat.begin(), mdat.end());
    file.insert(file.end(), moov.begin(), moov.end());

    SECTION("demux() reads it") {
        const auto out = mp4::demux(file);
        REQUIRE(out.has_value());
        CHECK(owned(out->samples) == expect);
    }

    SECTION("Reader reports the layout instead of guessing") {
        mp4::Reader reader{};
        const auto sink = [](std::span<const std::byte>) {};
        auto failed = false;
        for (std::size_t offset = 0; offset < file.size(); offset += 16) {
            const auto take = std::min<std::size_t>(16, file.size() - offset);
            const auto pushed = reader.push(std::span{file}.subspan(offset, take), sink);
            if (!pushed) {
                CHECK(pushed.error() == mp4::DemuxError::kMoovAfterMdat);
                failed = true;
                break;
            }
        }
        if (!failed) {
            const auto done = reader.finish();
            REQUIRE_FALSE(done.has_value());
            CHECK(done.error() == mp4::DemuxError::kMoovAfterMdat);
        }
    }
}

TEST_CASE("MP4 refuses what is not an MP4", "[mp4][reader]") {
    SECTION("empty input") {
        const auto out = mp4::demux({});
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kNotIsobmff);
    }

    SECTION("a Matroska file's EBML header") {
        const Bytes ebml{std::byte{0x1A}, std::byte{0x45}, std::byte{0xDF}, std::byte{0xA3},
                         std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
        const auto out = mp4::demux(ebml);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kNotIsobmff);
    }

    SECTION("ftyp alone, no track") {
        const auto file = ftyp();
        const auto out = mp4::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kTruncated);
    }

    SECTION("a track this reader has no use for") {
        MoovSpec spec{.sizes = {10}, .entry = "mp4a"};
        spec.mdat_data_offset = 0;
        Bytes file = ftyp();
        const auto moov = build_moov(spec);
        file.insert(file.end(), moov.begin(), moov.end());
        const auto out = mp4::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kNoAudioTrack);
    }
}

TEST_CASE("MP4 rejects sample tables that lie", "[mp4][reader]") {
    const auto with_moov = [](const Bytes& moov) {
        Bytes file = ftyp();
        file.insert(file.end(), moov.begin(), moov.end());
        Bytes mdat_body(64, std::byte{0xFF});
        const auto mdat = box("mdat", mdat_body);
        file.insert(file.end(), mdat.begin(), mdat.end());
        return file;
    };

    SECTION("a box smaller than its own header cannot be a length") {
        Bytes file = ftyp();
        put_u32(file, 3);  // a size of 3, inside an 8-byte header
        put_fourcc(file, "moov");
        const auto out = mp4::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("an stsz claiming more samples than its body holds") {
        Bytes stsz;
        put_u32(stsz, 0);
        put_u32(stsz, 1'000'000);  // count
        put_u32(stsz, 10);         // ...and exactly one size behind it
        Bytes stbl;
        put_bytes(stbl, build_stsd(MoovSpec{}));
        put_bytes(stbl, fullbox("stsz", 0, 0, stsz));
        Bytes minf;
        put_bytes(minf, box("stbl", stbl));
        Bytes mdia;
        put_bytes(mdia, box("minf", minf));
        Bytes trak;
        put_bytes(trak, box("mdia", mdia));
        Bytes moov;
        put_bytes(moov, box("trak", trak));
        const auto out = mp4::demux(with_moov(box("moov", moov)));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("an stsz sample count past max_samples") {
        Bytes stsz;
        put_u32(stsz, 4);          // uniform size, so no table to overrun
        put_u32(stsz, 5'000'000);  // ...but more samples than the limit allows
        Bytes stbl;
        put_bytes(stbl, build_stsd(MoovSpec{}));
        put_bytes(stbl, fullbox("stsz", 0, 0, stsz));
        Bytes minf;
        put_bytes(minf, box("stbl", stbl));
        Bytes mdia;
        put_bytes(mdia, box("minf", minf));
        Bytes trak;
        put_bytes(trak, box("mdia", mdia));
        Bytes moov;
        put_bytes(moov, box("trak", trak));
        const auto out = mp4::demux(with_moov(box("moov", moov)));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("an stsc whose first entry does not start at chunk 1") {
        MoovSpec spec{.sizes = {10, 20}};
        auto moov = build_moov(spec);
        // Rewrite the stsc run's first_chunk from 1 to 7 in place. The field
        // is the first u32 of the entry, 8 bytes into the FullBox body.
        bool patched = false;
        for (std::size_t i = 0; i + 4 <= moov.size(); ++i) {
            if (moov[i] == std::byte{'s'} && moov[i + 1] == std::byte{'t'} &&
                moov[i + 2] == std::byte{'s'} && moov[i + 3] == std::byte{'c'}) {
                moov[i + 4 + 8 + 3] = std::byte{7};
                patched = true;
                break;
            }
        }
        REQUIRE(patched);
        const auto out = mp4::demux(with_moov(moov));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("a chunk offset past the end of the file drops the sample, not the file") {
        // stco naming an offset nothing backs is the single most common
        // corruption in a truncated download. Every sample whose bytes ARE
        // present is still real, so those come back and the rest do not.
        MoovSpec spec{.sizes = {16, 16}};
        spec.mdat_data_offset = 1'000'000;
        const auto out = mp4::demux(with_moov(build_moov(spec)));
        REQUIRE(out.has_value());
        CHECK(out->samples.empty());
    }

    SECTION("nesting past max_depth") {
        Bytes inner;
        for (int i = 0; i < 40; ++i) {
            inner = box("traf", inner);
        }
        Bytes file = ftyp();
        const auto moof = box("moof", inner);
        file.insert(file.end(), moof.begin(), moof.end());
        const auto out = mp4::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kLimitExceeded);
    }
}

TEST_CASE("MP4 describe() names every demux error", "[mp4][reader]") {
    for (const auto error :
         {mp4::DemuxError::kNotIsobmff, mp4::DemuxError::kTruncated, mp4::DemuxError::kMalformed,
          mp4::DemuxError::kNoAudioTrack, mp4::DemuxError::kLimitExceeded,
          mp4::DemuxError::kMoovAfterMdat}) {
        CHECK_FALSE(mp4::describe(error).empty());
        CHECK(mp4::describe(error) != "unknown error");
    }
}
