#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
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
    // Overrides used by tests that need a dec3/tkhd/mdhd byte shape build_moov
    // itself never produces (a short/version-1/malformed box) - when unset,
    // build_stsd()/build_moov() fall back to their normal construction.
    std::optional<Bytes> dec3_payload = std::nullopt;
    std::optional<Bytes> tkhd_override = std::nullopt;
    std::optional<Bytes> mdhd_override = std::nullopt;
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
    const auto config = spec.dec3_payload.value_or(atmos_dec3());
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
    put_bytes(mdia, spec.mdhd_override ? *spec.mdhd_override : fullbox("mdhd", 0, 0, mdhd));
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
    put_bytes(trak, spec.tkhd_override ? *spec.tkhd_override : fullbox("tkhd", 0, 0x000007, tkhd));
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

// A stsd built directly from raw entry bytes rather than through build_stsd()
// - for tests that need a malformed or foreign sample entry shape build_stsd
// itself can never produce.
Bytes raw_stsd(std::uint32_t entry_count, const Bytes& entries) {
    Bytes body;
    put_u32(body, entry_count);
    put_bytes(body, entries);
    return fullbox("stsd", 0, 0, body);
}

// A moov holding one trak whose stbl's ONLY child is the given stsd bytes -
// tkhd/mdia/mdhd are the usual well-formed ones (spec.track_id/sample_rate),
// so a test can isolate a malformed stsd without also having to fabricate a
// matching sample table around it.
Bytes moov_with_stsd(const MoovSpec& spec, const Bytes& stsd) {
    Bytes stbl;
    put_bytes(stbl, stsd);
    Bytes minf;
    put_bytes(minf, box("stbl", stbl));

    Bytes mdhd;
    put_u32(mdhd, 0);
    put_u32(mdhd, 0);
    put_u32(mdhd, spec.sample_rate);
    put_u32(mdhd, 0);
    put_u16(mdhd, 0x15C7);
    put_u16(mdhd, 0);
    Bytes mdia;
    put_bytes(mdia, fullbox("mdhd", 0, 0, mdhd));
    put_bytes(mdia, box("minf", minf));

    Bytes tkhd;
    put_u32(tkhd, 0);
    put_u32(tkhd, 0);
    put_u32(tkhd, spec.track_id);
    put_u32(tkhd, 0);
    put_u32(tkhd, 0);
    put_u32(tkhd, 0);
    for (int i = 0; i < 15; ++i) {
        put_u32(tkhd, 0);
    }
    Bytes trak;
    put_bytes(trak, fullbox("tkhd", 0, 0x000007, tkhd));
    put_bytes(trak, box("mdia", mdia));

    Bytes moov;
    put_bytes(moov, box("trak", trak));
    return box("moov", moov);
}

// --- fragmented-file hand builders --------------------------------------
//
// mp4::fragment() itself only ever writes ONE shape: tfhd with
// default-base-is-moof and nothing else, trun with data-offset-present plus
// sample-size-present and nothing else. Every other optional tfhd/trun field
// ISO/IEC 14496-12 §8.8.7/§8.8.8 allows is legal in a file this project did
// not write (a real packager's fragmenter, or a hand-edited one), so the
// reader has to handle them too - these builders make that content directly
// rather than only through fragment()'s own narrow writer.

// A fragmented-file moov: one trak with an EMPTY sample table (stsd only -
// every sample lives in a moof/trun instead) plus mvex/trex, the same shape
// build_init_segment() in fragment.cpp writes.
Bytes build_fragmented_moov(const MoovSpec& spec, std::uint32_t trex_track_id,
                            std::uint32_t trex_default_duration,
                            std::uint32_t trex_default_size) {
    Bytes stbl;
    put_bytes(stbl, build_stsd(spec));
    Bytes empty_stsc;
    put_u32(empty_stsc, 0);
    put_bytes(stbl, fullbox("stsc", 0, 0, empty_stsc));
    Bytes empty_stsz;
    put_u32(empty_stsz, 0);
    put_u32(empty_stsz, 0);
    put_bytes(stbl, fullbox("stsz", 0, 0, empty_stsz));
    Bytes empty_stco;
    put_u32(empty_stco, 0);
    put_bytes(stbl, fullbox("stco", 0, 0, empty_stco));

    Bytes minf;
    put_bytes(minf, box("stbl", stbl));

    Bytes mdhd;
    put_u32(mdhd, 0);
    put_u32(mdhd, 0);
    put_u32(mdhd, spec.sample_rate);
    put_u32(mdhd, 0);
    put_u16(mdhd, 0x15C7);
    put_u16(mdhd, 0);
    Bytes mdia;
    put_bytes(mdia, fullbox("mdhd", 0, 0, mdhd));
    put_bytes(mdia, box("minf", minf));

    Bytes tkhd;
    put_u32(tkhd, 0);
    put_u32(tkhd, 0);
    put_u32(tkhd, spec.track_id);
    put_u32(tkhd, 0);
    put_u32(tkhd, 0);
    put_u32(tkhd, 0);
    for (int i = 0; i < 15; ++i) {
        put_u32(tkhd, 0);
    }
    Bytes trak;
    put_bytes(trak, fullbox("tkhd", 0, 0x000007, tkhd));
    put_bytes(trak, box("mdia", mdia));

    Bytes trex;
    put_u32(trex, trex_track_id);
    put_u32(trex, 1);
    put_u32(trex, trex_default_duration);
    put_u32(trex, trex_default_size);
    put_u32(trex, 0x02000000);
    Bytes mvex;
    put_bytes(mvex, fullbox("trex", 0, 0, trex));

    Bytes moov;
    put_bytes(moov, box("trak", trak));
    put_bytes(moov, box("mvex", mvex));
    return box("moov", moov);
}

Bytes build_tfhd_flagged(std::uint32_t flags, std::uint32_t track_id,
                         std::optional<std::uint64_t> base_data_offset = std::nullopt,
                         std::optional<std::uint32_t> sample_description_index = std::nullopt,
                         std::optional<std::uint32_t> default_sample_duration = std::nullopt,
                         std::optional<std::uint32_t> default_sample_size = std::nullopt) {
    Bytes body;
    put_u32(body, track_id);
    if (base_data_offset) {
        put_u64(body, *base_data_offset);
    }
    if (sample_description_index) {
        put_u32(body, *sample_description_index);
    }
    if (default_sample_duration) {
        put_u32(body, *default_sample_duration);
    }
    if (default_sample_size) {
        put_u32(body, *default_sample_size);
    }
    return fullbox("tfhd", 0, flags, body);
}

// One trun sample's optional per-sample fields, in §8.8.8.2's fixed order.
struct TrunSample {
    std::optional<std::uint32_t> duration = std::nullopt;
    std::optional<std::uint32_t> size = std::nullopt;
    std::optional<std::uint32_t> flags = std::nullopt;
    std::optional<std::int32_t> cts = std::nullopt;
};

Bytes build_trun_flagged(std::uint32_t flags, std::optional<std::int32_t> data_offset,
                         std::optional<std::uint32_t> first_sample_flags,
                         const std::vector<TrunSample>& samples) {
    Bytes body;
    put_u32(body, static_cast<std::uint32_t>(samples.size()));
    if (data_offset) {
        put_u32(body, static_cast<std::uint32_t>(*data_offset));
    }
    if (first_sample_flags) {
        put_u32(body, *first_sample_flags);
    }
    for (const auto& s : samples) {
        if (s.duration) {
            put_u32(body, *s.duration);
        }
        if (s.size) {
            put_u32(body, *s.size);
        }
        if (s.flags) {
            put_u32(body, *s.flags);
        }
        if (s.cts) {
            put_u32(body, static_cast<std::uint32_t>(*s.cts));
        }
    }
    return fullbox("trun", 0, flags, body);
}

Bytes build_moof_with(std::uint32_t sequence_number, const Bytes& tfhd, const Bytes& trun) {
    Bytes traf_body;
    put_bytes(traf_body, tfhd);
    put_bytes(traf_body, trun);
    Bytes mfhd_body;
    put_u32(mfhd_body, sequence_number);
    Bytes moof_body;
    put_bytes(moof_body, fullbox("mfhd", 0, 0, mfhd_body));
    put_bytes(moof_body, box("traf", traf_body));
    return box("moof", moof_body);
}

// Builds a moof whose trun uses default-base-is-moof (0x020000, always set
// here) plus data-offset-present, with the data_offset computed the same
// two-pass way fragment.cpp's build_moof() does: measure the moof with a
// placeholder 0, then rebuild with the real distance from moof's own start
// to the mdat payload that follows it (moof size + the 8-byte mdat header).
Bytes build_moof_relative(std::uint32_t track_id, std::uint32_t tfhd_extra_flags,
                          std::uint32_t trun_extra_flags,
                          const std::vector<TrunSample>& samples,
                          std::optional<std::uint32_t> first_sample_flags = std::nullopt) {
    // first-sample-flags-present (0x000004) has to be part of the flags
    // whenever a value is actually supplied, or build_trun_flagged writes 4
    // bytes the parser's own flag check never advances past - misaligning
    // every per-sample field that follows.
    const std::uint32_t fsf_flag = first_sample_flags ? 0x000004U : 0U;
    const auto flags = 0x000001U | fsf_flag | trun_extra_flags;
    const auto tfhd = build_tfhd_flagged(0x020000U | tfhd_extra_flags, track_id);
    const auto measured = build_trun_flagged(flags, 0, first_sample_flags, samples);
    const auto moof_measured = build_moof_with(1, tfhd, measured);
    const auto data_offset = static_cast<std::int32_t>(moof_measured.size() + 8);
    const auto trun = build_trun_flagged(flags, data_offset, first_sample_flags, samples);
    return build_moof_with(1, tfhd, trun);
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

TEST_CASE("MP4 trun without explicit sizes falls back to trex's default_sample_size",
          "[mp4][reader][fragment]") {
    // §8.8.8.2: a trun with sample-size-present (0x000200) clear carries no
    // per-sample size at all - every sample in the run is
    // mvex/trex::default_sample_size (§8.8.3), which tfhd here does not
    // override. This is the shape a real fragmented file uses when every
    // sample happens to be the same size; fragment() never emits it (it
    // always writes explicit per-sample sizes), so it has to be hand-built.
    constexpr std::uint32_t kTrackId = 1;
    constexpr std::uint32_t kDefaultSampleSize = 96;

    // build_moov()'s body is exactly box("trak", trak) - reuse its trak
    // (which is what makes track_found true) and add a real mvex/trex
    // alongside it, since MoovSpec itself has no fragmented-track support.
    // close_finished() calls build_sample_refs() on moov's own stbl
    // unconditionally, regardless of a later moof - a non-empty sizes here
    // would add a real (if fictitious) sample ahead of the fragmented ones.
    const MoovSpec spec{.sizes = {}, .track_id = kTrackId};
    const Bytes moov_box = build_moov(spec);
    const std::span<const std::byte> trak_box(moov_box.data() + 8, moov_box.size() - 8);

    Bytes trex_body;
    put_u32(trex_body, kTrackId);              // track_ID
    put_u32(trex_body, 1);                     // default_sample_description_index
    put_u32(trex_body, 1536);                  // default_sample_duration
    put_u32(trex_body, kDefaultSampleSize);    // default_sample_size
    put_u32(trex_body, 0x02000000);            // default_sample_flags
    const Bytes mvex = box("mvex", fullbox("trex", 0, 0, trex_body));

    Bytes moov_body;
    put_bytes(moov_body, trak_box);
    put_bytes(moov_body, mvex);
    const Bytes moov = box("moov", moov_body);

    // tfhd: base-data-offset-present only, with a placeholder base offset
    // patched in below once the mdat payload's absolute position is known.
    // No default-sample-size-present, so trex's value must survive
    // unmodified into parse_trun's fallback.
    Bytes tfhd_body;
    put_u32(tfhd_body, kTrackId);
    put_u64(tfhd_body, 0);  // base_data_offset placeholder
    const Bytes tfhd = fullbox("tfhd", 0, 0x000001, tfhd_body);

    // trun: flags = 0, so no data-offset, no per-sample duration, size, or
    // flags fields at all - just a bare sample_count.
    Bytes trun_body;
    put_u32(trun_body, 3);  // sample_count
    const Bytes trun = fullbox("trun", 0, 0x000000, trun_body);

    Bytes traf_body;
    put_bytes(traf_body, tfhd);
    put_bytes(traf_body, trun);
    const Bytes moof = box("moof", box("traf", traf_body));

    const std::vector<Bytes> expect{frame_of(kDefaultSampleSize, 0xB1),
                                     frame_of(kDefaultSampleSize, 0xB2),
                                     frame_of(kDefaultSampleSize, 0xB3)};
    Bytes payload;
    for (const auto& frame : expect) {
        put_bytes(payload, frame);
    }
    const Bytes mdat = box("mdat", payload);

    Bytes file = ftyp();
    put_bytes(file, moov);
    put_bytes(file, moof);
    const std::size_t mdat_payload_offset = file.size() + 8;
    put_bytes(file, mdat);

    // Patch tfhd's base_data_offset placeholder now that mdat's absolute
    // position is known - a fixed-width field, so this changes no box's
    // declared size.
    const std::size_t tfhd_base_offset_at =
        ftyp().size() + moov.size() + 8 /* moof header */ + 8 /* traf header */ +
        12 /* tfhd header: size+fourcc+version/flags */ + 4 /* track_ID */;
    Bytes patched_offset;
    put_u64(patched_offset, mdat_payload_offset);
    std::copy(patched_offset.begin(), patched_offset.end(),
              file.begin() + static_cast<std::ptrdiff_t>(tfhd_base_offset_at));

    const auto out = mp4::demux(file);
    REQUIRE(out.has_value());
    CHECK(owned(out->samples) == expect);
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

// --- error-path and less-common-encoding coverage -------------------------
//
// Everything below exercises box shapes the round-trip and "sample tables
// this project never writes" tests above never reach: a hostile or merely
// unusual file, not one this project's own writers would ever produce.

TEST_CASE("MP4 dec3 reads less common encodings", "[mp4][reader]") {
    const auto with_dec3 = [](const Bytes& payload) {
        MoovSpec spec{.sizes = {32}};
        spec.dec3_payload = payload;
        spec.mdat_data_offset = 0;
        const auto measured = build_moov(spec);
        spec.mdat_data_offset = ftyp().size() + measured.size() + 8;
        const auto moov = build_moov(spec);
        Bytes file = ftyp();
        file.insert(file.end(), moov.begin(), moov.end());
        const auto mdat = box("mdat", frame_of(32, 0xAA));
        file.insert(file.end(), mdat.begin(), mdat.end());
        return file;
    };

    SECTION("a box shorter than its fixed fields zero-extends rather than reading garbage") {
        const auto full = atmos_dec3();
        const Bytes truncated(full.begin(), full.begin() + 3);
        const auto out = mp4::demux(with_dec3(truncated));
        REQUIRE(out.has_value());
        CHECK(out->track.codec_config.fscod == 0);
        CHECK(out->track.codec_config.bsid == 16);
        CHECK_FALSE(out->track.codec_config.oba_complexity_index.has_value());
    }

    SECTION("a dependent-substream stream carries a channel location") {
        // data_rate=448,num_ind_sub=0 | fscod=0,bsid=16,reserved=0 |
        // asvc=0,bsmod=0,acmod=7,lfeon=1 | reserved(3)=0,num_dep_sub=3,chan_loc
        // MSB=0 | chan_loc low 8 bits=0xAA (170) | no Atmos extension.
        const Bytes payload{std::byte{0x0E}, std::byte{0x00}, std::byte{0x20}, std::byte{0x0F},
                            std::byte{0x06}, std::byte{0xAA}};
        const auto out = mp4::demux(with_dec3(payload));
        REQUIRE(out.has_value());
        CHECK(out->track.codec_config.num_dep_sub == 3);
        CHECK(out->track.codec_config.chan_loc == 170);
    }

    SECTION("a dec3 with no Atmos extension byte at all") {
        const auto full = atmos_dec3();
        const Bytes no_extension(full.begin(), full.begin() + 5);
        const auto out = mp4::demux(with_dec3(no_extension));
        REQUIRE(out.has_value());
        CHECK_FALSE(out->track.codec_config.oba_complexity_index.has_value());
    }

    SECTION("the extension byte present but flag_ec3_extension_type_a clear") {
        auto full = atmos_dec3();
        full[5] = std::byte{0x00};  // reserved(7) + flag_ec3_extension_type_a(1) = 0
        const Bytes no_flag(full.begin(), full.begin() + 6);
        const auto out = mp4::demux(with_dec3(no_flag));
        REQUIRE(out.has_value());
        CHECK_FALSE(out->track.codec_config.oba_complexity_index.has_value());
    }

    SECTION("flag_ec3_extension_type_a set but the box ends before the complexity byte") {
        const auto full = atmos_dec3();
        const Bytes no_complexity(full.begin(), full.begin() + 6);
        const auto out = mp4::demux(with_dec3(no_complexity));
        REQUIRE(out.has_value());
        CHECK_FALSE(out->track.codec_config.oba_complexity_index.has_value());
    }
}

TEST_CASE("MP4 mdhd language falls back to und for an unpacked code", "[mp4][reader]") {
    // 5 bits/letter: 'e'=5 is valid, 27 is not (only 1-26 map to a-z) - so
    // the second letter trips unpack_language's fallback before a third is
    // ever read.
    constexpr std::uint16_t kInvalidPacked = (5U << 10U) | (27U << 5U);
    Bytes mdhd_body;
    put_u32(mdhd_body, 0);
    put_u32(mdhd_body, 0);
    put_u32(mdhd_body, 48000);
    put_u32(mdhd_body, 0);
    put_u16(mdhd_body, kInvalidPacked);
    put_u16(mdhd_body, 0);

    MoovSpec spec{.sizes = {32}};
    spec.mdhd_override = fullbox("mdhd", 0, 0, mdhd_body);
    spec.mdat_data_offset = 0;
    const auto measured = build_moov(spec);
    spec.mdat_data_offset = ftyp().size() + measured.size() + 8;
    Bytes file = ftyp();
    const auto moov = build_moov(spec);
    file.insert(file.end(), moov.begin(), moov.end());
    const auto mdat = box("mdat", frame_of(32, 0xAA));
    file.insert(file.end(), mdat.begin(), mdat.end());

    const auto out = mp4::demux(file);
    REQUIRE(out.has_value());
    CHECK(out->track.language == "und");
}

TEST_CASE("MP4 reads version-1 tkhd/mdhd and rejects ones too short to read", "[mp4][reader]") {
    const auto assemble = [](MoovSpec spec) {
        spec.mdat_data_offset = 0;
        const auto measured = build_moov(spec);
        spec.mdat_data_offset = ftyp().size() + measured.size() + 8;
        const auto moov = build_moov(spec);
        Bytes file = ftyp();
        file.insert(file.end(), moov.begin(), moov.end());
        const auto mdat = box("mdat", frame_of(32, 0xAA));
        file.insert(file.end(), mdat.begin(), mdat.end());
        return file;
    };

    SECTION("a version-1 tkhd (64-bit creation/modification times)") {
        Bytes body;
        put_u8(body, 1);
        put_u8(body, 0);
        put_u8(body, 0);
        put_u8(body, 0);
        put_u64(body, 0);  // creation_time
        put_u64(body, 0);  // modification_time
        put_u32(body, 42);  // track_ID, at offset 20 for version 1
        put_u32(body, 0);
        put_u64(body, 0);

        MoovSpec spec{.sizes = {32}, .track_id = 42};
        spec.tkhd_override = box("tkhd", body);
        const auto out = mp4::demux(assemble(spec));
        REQUIRE(out.has_value());
        CHECK(out->track.track_id == 42);
    }

    SECTION("a tkhd body under 4 bytes is malformed") {
        MoovSpec spec{.sizes = {32}};
        spec.tkhd_override = box("tkhd", Bytes(2, std::byte{0}));
        const auto out = mp4::demux(assemble(spec));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("a tkhd too short to reach track_ID is malformed") {
        MoovSpec spec{.sizes = {32}};
        spec.tkhd_override = box("tkhd", Bytes(8, std::byte{0}));  // version 0, < 12+4
        const auto out = mp4::demux(assemble(spec));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("a version-1 mdhd (64-bit durations, language further out)") {
        Bytes body;
        put_u8(body, 1);
        put_u8(body, 0);
        put_u8(body, 0);
        put_u8(body, 0);
        put_u64(body, 0);      // creation_time
        put_u64(body, 0);      // modification_time
        put_u32(body, 44100);  // timescale, at offset 20 for version 1
        put_u64(body, 0);      // duration
        put_u16(body, 0x15C7);  // "eng", at offset 32
        put_u16(body, 0);

        MoovSpec spec{.sizes = {32}};
        spec.mdhd_override = box("mdhd", body);
        const auto out = mp4::demux(assemble(spec));
        REQUIRE(out.has_value());
        CHECK(out->track.timescale == 44100);
        CHECK(out->track.language == "eng");
    }

    SECTION("an mdhd body under 4 bytes is malformed") {
        MoovSpec spec{.sizes = {32}};
        spec.mdhd_override = box("mdhd", Bytes(2, std::byte{0}));
        const auto out = mp4::demux(assemble(spec));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("an mdhd too short to reach timescale is malformed") {
        MoovSpec spec{.sizes = {32}};
        spec.mdhd_override = box("mdhd", Bytes(8, std::byte{0}));  // version 0, < 12+4
        const auto out = mp4::demux(assemble(spec));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("an mdhd long enough for timescale but not language keeps the default") {
        Bytes body;
        put_u8(body, 0);
        put_u8(body, 0);
        put_u8(body, 0);
        put_u8(body, 0);
        put_u32(body, 0);
        put_u32(body, 0);
        put_u32(body, 44100);  // timescale, offset 12 for version 0
        put_u16(body, 0);      // 18 bytes total, < language_at(20)+2

        MoovSpec spec{.sizes = {32}};
        spec.mdhd_override = box("mdhd", body);
        const auto out = mp4::demux(assemble(spec));
        REQUIRE(out.has_value());
        CHECK(out->track.timescale == 44100);
        CHECK(out->track.language == "und");
    }
}

TEST_CASE("MP4 stsd handles malformed and foreign sample entries", "[mp4][reader]") {
    const auto with_stsd = [](const Bytes& stsd) {
        MoovSpec spec{.sizes = {32}};
        Bytes file = ftyp();
        const auto moov = moov_with_stsd(spec, stsd);
        file.insert(file.end(), moov.begin(), moov.end());
        const auto mdat = box("mdat", frame_of(32, 0xAA));
        file.insert(file.end(), mdat.begin(), mdat.end());
        return file;
    };

    SECTION("a stsd body under 8 bytes is malformed") {
        // Bypasses raw_stsd(), which always writes a full 8-byte
        // verflags+entry_count body - this needs fewer bytes than that.
        const auto out = mp4::demux(with_stsd(box("stsd", Bytes(4, std::byte{0}))));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("entry_count claims more entries than the body actually holds") {
        // A single foreign entry ("mp4a") that exactly fills the body, but
        // entry_count says two - the loop runs out of room for a second
        // header rather than reading past the end.
        Bytes entries;
        put_u32(entries, 8);
        put_fourcc(entries, "mp4a");
        const auto out = mp4::demux(with_stsd(raw_stsd(2, entries)));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kNoAudioTrack);
    }

    SECTION("an entry whose own header cannot be read is malformed") {
        Bytes entries;
        put_u32(entries, 3);  // a size smaller than any box header
        put_fourcc(entries, "ec-3");
        const auto out = mp4::demux(with_stsd(raw_stsd(1, entries)));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("an entry declared to run to end-of-file is malformed") {
        Bytes entries;
        put_u32(entries, 0);  // size == 0: to_eof
        put_fourcc(entries, "ec-3");
        const auto out = mp4::demux(with_stsd(raw_stsd(1, entries)));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("an entry's declared size overruns the stsd body") {
        Bytes entries;
        put_u32(entries, 100);
        put_fourcc(entries, "ec-3");
        const auto out = mp4::demux(with_stsd(raw_stsd(1, entries)));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("an ec-3 entry too short for its fixed AudioSampleEntry fields") {
        Bytes entries;
        put_u32(entries, 20);  // 8 header + 12, short of the 28 fields need
        put_fourcc(entries, "ec-3");
        put_bytes(entries, Bytes(12, std::byte{0}));
        const auto out = mp4::demux(with_stsd(raw_stsd(1, entries)));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("an ec-3 entry with no child boxes at all still has an entry") {
        Bytes entries;
        put_u32(entries, 8 + 28);  // header + fixed fields, nothing after
        put_fourcc(entries, "ec-3");
        put_bytes(entries, Bytes(28, std::byte{0}));
        const auto out = mp4::demux(with_stsd(raw_stsd(1, entries)));
        REQUIRE(out.has_value());
        CHECK(out->track.codec_id == "ec-3");
        CHECK(out->track.codec_config.payload.empty());
    }

    SECTION("a child box whose own header cannot be read is malformed") {
        Bytes entries;
        put_u32(entries, 8 + 28 + 8);
        put_fourcc(entries, "ec-3");
        put_bytes(entries, Bytes(28, std::byte{0}));
        put_u32(entries, 2);  // a child size smaller than any box header
        put_fourcc(entries, "dec3");
        const auto out = mp4::demux(with_stsd(raw_stsd(1, entries)));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("a child box's declared size overruns the sample entry") {
        Bytes entries;
        put_u32(entries, 8 + 28 + 8);
        put_fourcc(entries, "ec-3");
        put_bytes(entries, Bytes(28, std::byte{0}));
        put_u32(entries, 100);  // far past the entry's own end
        put_fourcc(entries, "dec3");
        const auto out = mp4::demux(with_stsd(raw_stsd(1, entries)));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("a foreign child box is skipped before the real dec3 child") {
        const auto dec3 = atmos_dec3();
        Bytes entries;
        put_u32(entries, static_cast<std::uint32_t>(8 + 28 + 8 + (8 + dec3.size())));
        put_fourcc(entries, "ec-3");
        put_bytes(entries, Bytes(28, std::byte{0}));
        put_u32(entries, 8);  // an empty, unrelated child box first
        put_fourcc(entries, "esds");
        put_u32(entries, static_cast<std::uint32_t>(8 + dec3.size()));
        put_fourcc(entries, "dec3");
        put_bytes(entries, dec3);
        const auto out = mp4::demux(with_stsd(raw_stsd(1, entries)));
        REQUIRE(out.has_value());
        CHECK(out->track.codec_config.bsid == 16);
    }
}

TEST_CASE("MP4 build_sample_refs handles every sample-table shape", "[mp4][reader]") {
    const auto assemble = [](MoovSpec spec, bool omit_stsc = false) {
        spec.mdat_data_offset = 0;
        const auto measured = build_moov(spec);
        spec.mdat_data_offset = ftyp().size() + measured.size() + 8;
        auto moov = build_moov(spec);
        if (omit_stsc) {
            // Cut the stsc box (fullbox("stsc",...) with entry_count=0/1) out
            // of the already-built moov in place, rather than adding a whole
            // separate no-stsc builder: find it by fourcc and its own
            // declared size, then erase exactly that many bytes.
            for (std::size_t i = 0; i + 8 <= moov.size(); ++i) {
                if (moov[i + 4] == std::byte{'s'} && moov[i + 5] == std::byte{'t'} &&
                    moov[i + 6] == std::byte{'s'} && moov[i + 7] == std::byte{'c'}) {
                    const auto size = static_cast<std::size_t>(
                        (std::to_integer<unsigned>(moov[i]) << 24U) |
                        (std::to_integer<unsigned>(moov[i + 1]) << 16U) |
                        (std::to_integer<unsigned>(moov[i + 2]) << 8U) |
                        std::to_integer<unsigned>(moov[i + 3]));
                    moov.erase(moov.begin() + static_cast<std::ptrdiff_t>(i),
                              moov.begin() + static_cast<std::ptrdiff_t>(i + size));
                    break;
                }
            }
        }
        Bytes payload;
        for (const auto size : spec.sizes) {
            payload.insert(payload.end(), Bytes(size, std::byte{0xBB}).begin(),
                           Bytes(size, std::byte{0xBB}).end());
        }
        Bytes file = ftyp();
        file.insert(file.end(), moov.begin(), moov.end());
        const auto mdat = box("mdat", payload);
        file.insert(file.end(), mdat.begin(), mdat.end());
        return file;
    };

    SECTION("a sample entry describing zero samples is not an error") {
        // The fragmented init-segment shape: stsd present, every other table
        // empty - legal, and contributes nothing rather than failing.
        const auto out = mp4::demux(assemble(MoovSpec{.sizes = {}}));
        REQUIRE(out.has_value());
        CHECK(out->samples.empty());
    }

    SECTION("sizes and offsets with no stsc at all is malformed") {
        const auto out = mp4::demux(assemble(MoovSpec{.sizes = {16, 16}}, /*omit_stsc=*/true));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }
}

TEST_CASE("MP4 explicit ReadOptions::track_id selects a specific track", "[mp4][reader]") {
    MoovSpec spec{.sizes = {32}, .track_id = 7};
    spec.mdat_data_offset = 0;
    const auto measured = build_moov(spec);
    spec.mdat_data_offset = ftyp().size() + measured.size() + 8;
    const auto moov = build_moov(spec);
    Bytes file = ftyp();
    file.insert(file.end(), moov.begin(), moov.end());
    const auto mdat = box("mdat", frame_of(32, 0xAA));
    file.insert(file.end(), mdat.begin(), mdat.end());

    SECTION("the matching track_id is found") {
        const auto out = mp4::demux(file, mp4::ReadOptions{.track_id = 7});
        REQUIRE(out.has_value());
        CHECK(out->track.track_id == 7);
    }

    SECTION("a non-matching track_id is not found") {
        const auto out = mp4::demux(file, mp4::ReadOptions{.track_id = 9});
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kNoAudioTrack);
    }
}

TEST_CASE("MP4 fragmented reader across tfhd/trun flag combinations", "[mp4][reader]") {
    constexpr std::uint32_t kTrackId = 1;
    const MoovSpec spec{.sizes = {}, .track_id = kTrackId};

    const auto assemble = [&](std::uint32_t trex_duration, std::uint32_t trex_size,
                              const Bytes& moof, const Bytes& payload) {
        Bytes file = ftyp();
        const auto moov = build_fragmented_moov(spec, kTrackId, trex_duration, trex_size);
        file.insert(file.end(), moov.begin(), moov.end());
        file.insert(file.end(), moof.begin(), moof.end());
        const auto mdat = box("mdat", payload);
        file.insert(file.end(), mdat.begin(), mdat.end());
        return file;
    };

    SECTION("base-data-offset-present names the sample bytes directly") {
        const auto moov = build_fragmented_moov(spec, kTrackId, 0, 0);
        const auto trun =
            build_trun_flagged(0x000201U, 0, std::nullopt, {TrunSample{.size = 40}});
        // moof's size doesn't depend on base_data_offset's VALUE (always an
        // 8-byte field), so build once with a placeholder to measure, then
        // again with the real absolute offset - the same two-pass shape
        // fragment.cpp's own build_moof() uses for its (relative) offset.
        const auto placeholder_tfhd = build_tfhd_flagged(0x000001U, kTrackId, std::uint64_t{0});
        const auto moof_size = build_moof_with(1, placeholder_tfhd, trun).size();
        const auto abs_offset =
            static_cast<std::uint64_t>(ftyp().size() + moov.size() + moof_size + 8);
        const auto tfhd = build_tfhd_flagged(0x000001U, kTrackId, abs_offset);
        const auto moof = build_moof_with(1, tfhd, trun);
        const auto out = mp4::demux(assemble(0, 0, moof, frame_of(40, 0x21)));
        REQUIRE(out.has_value());
        REQUIRE(out->samples.size() == 1);
        CHECK(owned(out->samples).front() == frame_of(40, 0x21));
    }

    SECTION("sample-description-index-present and default-sample-duration-present are skipped") {
        const auto moof =
            build_moof_relative(kTrackId, 0x000002U | 0x000008U, 0x000200U,
                                {TrunSample{.size = 40}});
        const auto out = mp4::demux(assemble(1536, 0, moof, frame_of(40, 0x22)));
        REQUIRE(out.has_value());
        REQUIRE(out->samples.size() == 1);
        CHECK(owned(out->samples).front() == frame_of(40, 0x22));
    }

    SECTION("default-sample-size-present in tfhd covers samples with no per-sample size") {
        const auto tfhd =
            build_tfhd_flagged(0x020000U | 0x000010U, kTrackId, std::nullopt, std::nullopt,
                               std::nullopt, std::uint32_t{40});
        const auto measured = build_trun_flagged(0x000001U, 0, std::nullopt,
                                                  {TrunSample{}, TrunSample{}});
        const auto moof_measured = build_moof_with(1, tfhd, measured);
        const auto data_offset = static_cast<std::int32_t>(moof_measured.size() + 8);
        const auto trun = build_trun_flagged(0x000001U, data_offset, std::nullopt,
                                             {TrunSample{}, TrunSample{}});
        const auto moof = build_moof_with(1, tfhd, trun);
        Bytes payload;
        put_bytes(payload, frame_of(40, 0x31));
        put_bytes(payload, frame_of(40, 0x32));
        const auto out = mp4::demux(assemble(0, 0, moof, payload));
        REQUIRE(out.has_value());
        REQUIRE(out->samples.size() == 2);
        CHECK(owned(out->samples)[0] == frame_of(40, 0x31));
        CHECK(owned(out->samples)[1] == frame_of(40, 0x32));
    }

    SECTION("trex's own default_sample_size covers samples when tfhd sets none") {
        const auto moof = build_moof_relative(kTrackId, 0, 0x000000U, {TrunSample{}});
        const auto out = mp4::demux(assemble(1536, 40, moof, frame_of(40, 0x41)));
        REQUIRE(out.has_value());
        REQUIRE(out->samples.size() == 1);
        CHECK(owned(out->samples).front() == frame_of(40, 0x41));
    }

    SECTION("neither a default nor a per-sample size is malformed") {
        const auto moof = build_moof_relative(kTrackId, 0, 0x000000U, {TrunSample{}});
        const auto out = mp4::demux(assemble(1536, 0, moof, frame_of(40, 0x42)));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("per-sample duration, flags and composition-time-offset fields are skipped") {
        const auto moof = build_moof_relative(
            kTrackId, 0, 0x000100U | 0x000200U | 0x000400U | 0x000800U,
            {TrunSample{.duration = 1536, .size = 40, .flags = 0, .cts = 0}});
        const auto out = mp4::demux(assemble(0, 0, moof, frame_of(40, 0x51)));
        REQUIRE(out.has_value());
        REQUIRE(out->samples.size() == 1);
        CHECK(owned(out->samples).front() == frame_of(40, 0x51));
    }

    SECTION("first-sample-flags-present is skipped") {
        const auto moof = build_moof_relative(kTrackId, 0, 0x000200U, {TrunSample{.size = 40}},
                                              /*first_sample_flags=*/0x02000000U);
        const auto out = mp4::demux(assemble(0, 0, moof, frame_of(40, 0x52)));
        REQUIRE(out.has_value());
        REQUIRE(out->samples.size() == 1);
        CHECK(owned(out->samples).front() == frame_of(40, 0x52));
    }

    SECTION("a foreign track's traf is parsed but nothing is emitted for it") {
        const auto tfhd = build_tfhd_flagged(0x020000U, /*track_id=*/99);
        const auto trun =
            build_trun_flagged(0x000201U, 0, std::nullopt, {TrunSample{.size = 40}});
        const auto moof = build_moof_with(1, tfhd, trun);
        const auto out = mp4::demux(assemble(1536, 0, moof, frame_of(40, 0x61)));
        REQUIRE(out.has_value());
        CHECK(out->samples.empty());
    }

    SECTION("a negative base offset is malformed") {
        const auto tfhd = build_tfhd_flagged(0x020000U, kTrackId);
        const auto trun = build_trun_flagged(0x000201U, -1000, std::nullopt,
                                             {TrunSample{.size = 40}});
        const auto moof = build_moof_with(1, tfhd, trun);
        const auto out = mp4::demux(assemble(1536, 0, moof, frame_of(40, 0x62)));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("a trun sample count past max_samples is malformed") {
        const auto tfhd = build_tfhd_flagged(0x020000U, kTrackId);
        Bytes trun_body;
        put_u32(trun_body, 5'000'000);  // count, with no matching per-sample bytes
        put_u32(trun_body, 0);
        const auto trun = fullbox("trun", 0, 0x000001U, trun_body);
        const auto moof = build_moof_with(1, tfhd, trun);
        const auto out = mp4::demux(assemble(1536, 0, moof, {}));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("a trun body too short for its declared per-sample stride is malformed") {
        const auto tfhd = build_tfhd_flagged(0x020000U, kTrackId);
        Bytes trun_body;
        put_u32(trun_body, 100);  // count claims far more samples than follow
        put_u32(trun_body, 0);
        put_u32(trun_body, 40);  // just one sample's worth of size data
        const auto trun = fullbox("trun", 0, 0x000201U, trun_body);
        const auto moof = build_moof_with(1, tfhd, trun);
        const auto out = mp4::demux(assemble(1536, 0, moof, {}));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }

    SECTION("a tfhd body under 8 bytes is malformed") {
        const auto tfhd = box("tfhd", Bytes(6, std::byte{0}));
        const auto trun =
            build_trun_flagged(0x000201U, 0, std::nullopt, {TrunSample{.size = 40}});
        const auto moof = build_moof_with(1, tfhd, trun);
        const auto out = mp4::demux(assemble(1536, 0, moof, frame_of(40, 0x63)));
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kMalformed);
    }
}

TEST_CASE("MP4 walk() handles box-shape edge cases", "[mp4][reader]") {
    SECTION("an unreadable box header before any box has been seen is kNotIsobmff") {
        Bytes file;
        put_u32(file, 3);  // a size smaller than any box header, as the FIRST bytes
        put_fourcc(file, "xxxx");
        const auto out = mp4::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kNotIsobmff);
    }

    SECTION("a bare mdat with no ftyp is still a plausible opener") {
        // mdat comes FIRST here, so the samples it holds start right after
        // its own 8-byte header - not after a moov that comes later.
        MoovSpec spec{.sizes = {16}, .mdat_data_offset = 8};
        const auto moov = build_moov(spec);
        const auto mdat = box("mdat", frame_of(16, 0x71));
        Bytes file = mdat;
        file.insert(file.end(), moov.begin(), moov.end());
        const auto out = mp4::demux(file);
        REQUIRE(out.has_value());
        CHECK(owned(out->samples) == std::vector<Bytes>{frame_of(16, 0x71)});
    }

    SECTION("more trak boxes than max_chunks is a limit, not a crash") {
        MoovSpec a{.sizes = {16}, .track_id = 1};
        MoovSpec b{.sizes = {16}, .entry = "mp4a", .track_id = 2};
        Bytes moov;
        // Two independent trak boxes, hand-assembled from two full moovs'
        // worth of trak content - only trak count matters for this gate.
        const auto extract_trak = [](const Bytes& full_moov) {
            // full_moov is box("moov", box("trak", ...)) with nothing else
            // inside moov's body, so the trak box is everything after moov's
            // own 8-byte header.
            return Bytes(full_moov.begin() + 8, full_moov.end());
        };
        put_bytes(moov, extract_trak(build_moov(a)));
        put_bytes(moov, extract_trak(build_moov(b)));
        Bytes file = ftyp();
        const auto moov_box = box("moov", moov);
        file.insert(file.end(), moov_box.begin(), moov_box.end());
        const auto mdat = box("mdat", frame_of(16, 0x72));
        file.insert(file.end(), mdat.begin(), mdat.end());

        const auto out = mp4::demux(file, mp4::ReadOptions{.max_chunks = 1});
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kLimitExceeded);
    }

    SECTION("a leaf box larger than max_box_bytes is a limit") {
        Bytes file = ftyp();
        Bytes moov_body;
        Bytes trak_body;
        Bytes oversized_stsz_header;
        put_u32(oversized_stsz_header, 128U << 20U);  // declared size > 64 MiB default
        put_fourcc(oversized_stsz_header, "stsz");
        Bytes mdia_body;
        Bytes minf_body;
        Bytes stbl_body;
        put_bytes(stbl_body, oversized_stsz_header);
        put_bytes(minf_body, box("stbl", stbl_body));
        put_bytes(mdia_body, box("minf", minf_body));
        put_bytes(trak_body, box("mdia", mdia_body));
        put_bytes(moov_body, box("trak", trak_body));
        const auto moov = box("moov", moov_body);
        file.insert(file.end(), moov.begin(), moov.end());

        const auto out = mp4::demux(file);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == mp4::DemuxError::kLimitExceeded);
    }
}

TEST_CASE("MP4 Reader surfaces a walk error directly from push()", "[mp4][reader]") {
    mp4::Reader reader{};
    Bytes bad;
    put_u32(bad, 3);  // a size smaller than any box header
    put_fourcc(bad, "xxxx");
    const auto sink = [](std::span<const std::byte>) {};
    const auto pushed = reader.push(bad, sink);
    REQUIRE_FALSE(pushed.has_value());
    CHECK(pushed.error() == mp4::DemuxError::kNotIsobmff);
}
