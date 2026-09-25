#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ac4/ac4.hpp"

namespace {

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

// Real Dolby Encoding Engine 6.5.4 output (tools/generators/gen_ac4_baseline.py),
// not a stream this project's own tooling produced - see docs/verification.md's
// AC-4 section and CONTRIBUTING.md's Oracles list, #3.
std::filesystem::path fixture_path() {
    return AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR "/ac4-stereo-64/dee.ac4";
}

}  // namespace

TEST_CASE("scan walks every sync frame of a real DEE AC-4 stream with CRCs intact", "[ac4]") {
    const auto data = read_file(fixture_path());
    const auto result = ac4::scan(data);

    CHECK_FALSE(result.stopped_at.has_value());
    REQUIRE(result.frames.size() == 73);
    for (const auto& frame : result.frames) {
        CAPTURE(frame.offset);
        CHECK(frame.sync_word == 0xAC41);
        REQUIRE(frame.crc_ok.has_value());
        CHECK(*frame.crc_ok);
    }
    // Annex G.3.2: frame_size is the trailing raw_ac4_frame()'s own byte
    // count, so consecutive frames' offsets have to be contiguous with no
    // gap or overlap.
    for (std::size_t i = 1; i < result.frames.size(); ++i) {
        const auto& prev = result.frames[i - 1];
        const std::size_t prev_total =
            prev.raw_ac4_frame.size() + 4 /* sync+frame_size */ + 2 /* crc */;
        CHECK(result.frames[i].offset == prev.offset + prev_total);
    }
}

TEST_CASE("parse_raw_frame reads a real stereo DEE frame's TOC and presentation", "[ac4]") {
    const auto data = read_file(fixture_path());
    const auto scanned = ac4::scan(data);
    REQUIRE(scanned.frames.size() == 73);

    // Frame 0. Every field below is cross-checked against MediaInfo's own
    // (dlb_ac4lib-based) reading of this exact fixture - see
    // docs/verification.md.
    const auto result = ac4::parse_raw_frame(scanned.frames[0].raw_ac4_frame);
    REQUIRE(result.has_value());
    const auto& toc = result->toc;

    CHECK(toc.bitstream_version == 2);
    CHECK(toc.sample_rate_hz == 48000);
    CHECK(toc.frame_rate_index == 13);  // Table 83's "(23,44)" row, 2048 samples/frame -
                                        // matches MediaInfo's "23.438 FPS (2048 SPF)"
                                        // for source material with no embedded frame rate.
    CHECK(toc.n_presentations == 1);
    CHECK(toc.payload_base == 1);

    REQUIRE(toc.presentations_v1.size() == 1);
    CHECK(toc.presentations_v1[0].group_refs == std::vector<int>{0});

    REQUIRE(toc.substream_groups.size() == 1);
    const auto& group = toc.substream_groups[0];
    CHECK(group.b_substreams_present);
    CHECK(group.b_channel_coded);
    CHECK_FALSE(group.oamd.has_value());
    REQUIRE(group.substreams.size() == 1);
    REQUIRE(group.substreams[0].kind == ac4::GroupSubstream::Kind::kChan);
    REQUIRE(group.substreams[0].chan.has_value());
    const auto& chan = *group.substreams[0].chan;
    CHECK(chan.channel_mode_name == "Stereo");
    REQUIRE(chan.ch_mode.has_value());
    CHECK(*chan.ch_mode == 1);
    REQUIRE(chan.substream_index.has_value());
    CHECK(*chan.substream_index == 1);

    CHECK(toc.n_substreams == 3);
    REQUIRE(result->substreams.size() == 3);
    // Table 15/50: which substream_index_table() row is audio is decided by
    // ac4_substream_info_chan()'s own substream_index (1 here), not by
    // table position - rows 0 and 2 are ac4_presentation_substream() and
    // emdf_payloads_substream(), different shapes this parser reports by
    // byte range only.
    CHECK_FALSE(result->substreams[0].is_audio);
    CHECK(result->substreams[1].is_audio);
    REQUIRE(result->substreams[1].audio_size.has_value());
    CHECK(*result->substreams[1].audio_size == 396);
    CHECK(result->substreams[1].size == 402);
    CHECK_FALSE(result->substreams[2].is_audio);

    // §4.3.3.12.4 Pseudocode 1: every substream's byte span has to land
    // fully inside the frame that declared it.
    std::size_t end = 0;
    for (const auto& sub : result->substreams) {
        CHECK(sub.offset + sub.size <= scanned.frames[0].raw_ac4_frame.size());
        end = std::max(end, sub.offset + sub.size);
    }
    CHECK(end <= scanned.frames[0].raw_ac4_frame.size());
}

TEST_CASE("parse_raw_frame agrees with itself across every frame of a real stream", "[ac4]") {
    // Not a per-field ground-truth check (that's the frame-0 test above) -
    // this proves the parser stays synchronised for 73 consecutive frames
    // of real, varying-size VBR content rather than only the one frame
    // that was used to debug it.
    const auto data = read_file(fixture_path());
    const auto scanned = ac4::scan(data);
    REQUIRE(scanned.frames.size() == 73);

    for (const auto& frame : scanned.frames) {
        CAPTURE(frame.offset);
        const auto result = ac4::parse_raw_frame(frame.raw_ac4_frame);
        REQUIRE(result.has_value());
        CHECK(result->toc.bitstream_version == 2);
        CHECK(result->toc.n_presentations == 1);
        REQUIRE(result->toc.substream_groups.size() == 1);
        REQUIRE(result->toc.substream_groups[0].substreams.size() == 1);
        const auto& sub0 = result->toc.substream_groups[0].substreams[0];
        REQUIRE(sub0.kind == ac4::GroupSubstream::Kind::kChan);
        REQUIRE(sub0.chan.has_value());
        CHECK(sub0.chan->channel_mode_name == "Stereo");
        std::size_t total = 0;
        for (const auto& sub : result->substreams) {
            total += sub.size;
        }
        CHECK(total <= frame.raw_ac4_frame.size());
    }
}

TEST_CASE("parse_raw_frame rejects a frame truncated inside the TOC", "[ac4]") {
    const auto data = read_file(fixture_path());
    const auto scanned = ac4::scan(data);
    REQUIRE(!scanned.frames.empty());
    const auto& raw = scanned.frames[0].raw_ac4_frame;

    for (const std::size_t cut : {std::size_t{0}, std::size_t{1}, std::size_t{5}, raw.size() / 2}) {
        CAPTURE(cut);
        const auto result = ac4::parse_raw_frame(raw.subspan(0, cut));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac4::Error::kTruncated);
    }
}

TEST_CASE("scan reports kLostSync at the offset of a corrupted sync word", "[ac4]") {
    auto data = read_file(fixture_path());
    const auto first = ac4::scan(data);
    REQUIRE(first.frames.size() > 1);
    const std::size_t second_frame_offset = first.frames[1].offset;

    data[second_frame_offset] = std::byte{0x00};  // was the high byte of 0xAC41
    const auto result = ac4::scan(data);
    REQUIRE(result.stopped_at.has_value());
    CHECK(*result.stopped_at == ac4::Error::kLostSync);
    CHECK(result.stopped_at_offset == second_frame_offset);
    // Everything before the corruption still parsed.
    CHECK(result.frames.size() == 1);
}

// --- Synthetic object/A-JOC/OAMD vectors ------------------------------------
//
// No real DEE-produced fixture reaches ac4_substream_info_ajoc()/
// ac4_substream_info_obj()/oamd_substream_info() - see ac4.hpp's module
// docs and docs/verification.md's AC-4 section. Each vector below is a
// hand-built bitstream assembled by the MSB-first BitWriter below (which
// shares no code with ac4::, so this is a genuine encode-side cross-check,
// not a tautology): a fixed, minimal TOC/presentation/single-substream-
// group preamble (write_ac4_object_coded_preamble(), traced field by field
// against parse_toc()/parse_presentation_v1_info()/
// parse_substream_group_info() as they stood when this was written) wraps
// one object/A-JOC substream payload per vector, reached through the same
// public parse_raw_frame() entry point real content uses - there is no
// separate way to unit-test the anonymous-namespace parse_* helpers
// directly.
namespace {

class BitWriter {
   public:
    // n may run past value's own 32 bits (padding a frame well beyond where
    // a test's real fields end, say) - bit positions at or above 32 are
    // simply 0, rather than shifting value by that many bits, which Sec.
    // [expr.shift] makes undefined once the shift count reaches the
    // operand's width.
    void put(std::uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            bits_.push_back(i < 32 && ((value >> i) & 1u) != 0);
        }
    }

    [[nodiscard]] std::vector<std::byte> bytes() const {
        std::vector<bool> padded = bits_;
        while (padded.size() % 8 != 0) {
            padded.push_back(false);
        }
        std::vector<std::byte> out(padded.size() / 8, std::byte{0});
        for (std::size_t i = 0; i < padded.size(); ++i) {
            if (padded[i]) {
                out[i / 8] |= static_cast<std::byte>(0x80u >> (i % 8));
            }
        }
        return out;
    }

   private:
    std::vector<bool> bits_;
};

// Table 3's variable_bits(n_bits) encoding of `value`, written from the
// syntax rather than from ac4.cpp's reader: every group but the last is
// followed by a continuation bit of 1, and each continuation adds
// 2^n_bits before the next group is read, so k continuations offset the
// groups' own base-2^n_bits value by the sum of 2^(n_bits*j), j = 1..k.
void put_variable_bits(BitWriter& w, int n_bits, std::uint32_t value) {
    int continuations = 0;
    std::uint64_t offset = 0;
    while (value - offset >= (std::uint64_t{1} << (n_bits * (continuations + 1)))) {
        ++continuations;
        offset += std::uint64_t{1} << (n_bits * continuations);
    }
    const std::uint64_t groups = value - offset;
    const std::uint64_t mask = (std::uint64_t{1} << n_bits) - 1;
    for (int i = continuations; i >= 0; --i) {
        w.put(static_cast<std::uint32_t>((groups >> (n_bits * i)) & mask), n_bits);
        w.put(i > 0 ? 1u : 0u, 1);
    }
}

// Every field up through ac4_presentation_v1_info() for a single
// presentation referencing a single substream group (group_index 0):
// bitstream_version 2, fs_index 0 (so every b_sf_multiplier field
// downstream is unread), frame_rate_index 5 (so frame_rate_multiply_info()
// reads no bits and frame_rate_factor resolves to 1, keeping every
// b_audio_ndot loop below to one iteration). 49 bits.
void write_ac4_object_coded_preamble(BitWriter& w) {
    w.put(2, 2);   // bitstream_version = 2
    w.put(0, 10);  // sequence_counter
    w.put(0, 1);   // b_wait_frames
    w.put(0, 1);   // fs_index = 0 (44100 Hz)
    w.put(5, 4);   // frame_rate_index = 5
    w.put(0, 1);   // b_iframe_global
    w.put(1, 1);   // b_single_presentation -> n_presentations = 1
    w.put(0, 1);   // b_payload_base = 0
    w.put(0, 1);   // b_program_id = 0
    // ac4_presentation_v1_info():
    w.put(1, 1);  // b_single_substream_group = 1
    w.put(0, 1);  // presentation_version terminator (unary 0 -> version 0)
    w.put(0, 3);  // md_compat
    w.put(0, 1);  // b_presentation_id = 0
    // frame_rate_multiply_info(frame_rate_index=5): reads 0 bits.
    w.put(0, 1);  // frame_rate_fractions_info: frame_rate_factor==1 branch reads 1 bit
    // emdf_info(): version(2)=0, key_id(3)=0, b_payloads_substream_info(1)=0,
    // emdf_reserved: primary(2)=0, secondary(2)=0.
    w.put(0, 2);
    w.put(0, 3);
    w.put(0, 1);
    w.put(0, 2);
    w.put(0, 2);
    w.put(0, 1);  // b_presentation_filter = 0
    w.put(0, 3);  // ac4_sgi_specifier(): group_index = 0
    w.put(0, 1);  // b_pre_virtualized
    w.put(0, 1);  // b_add_emdf_substreams = 0
    w.put(0, 1);  // b_alternative
    w.put(0, 1);  // b_pres_ndot
    w.put(0, 2);  // ac4_presentation_substream_info()'s substream_index_ref
}

// ac4_substream_group_info()'s own preamble for a single-substream,
// object-coded group: b_substreams_present=1 (every *_info element below
// reads its own substream_index explicitly), b_hsf_ext=0, b_single_substream
// =1 (n_lf_substreams=1, no count field), b_channel_coded=0. 4 bits.
void write_ac4_object_coded_group_preamble(BitWriter& w) {
    w.put(1, 1);  // b_substreams_present
    w.put(0, 1);  // b_hsf_ext
    w.put(1, 1);  // b_single_substream
    w.put(0, 1);  // b_channel_coded
}

// substream_index_table() for exactly one, zero-length substream:
// n_substreams=1 (direct, not the variable_bits(0) escape), b_size_present=1
// (n_substreams==1 makes this explicit), one substream_size entry
// (b_more_bits=0, substream_size=0). 14 bits. parse_raw_frame() never reads
// this synthetic frame's "audio" - Substream::is_audio only gates a header
// read when size >= 3 - so a zero-length entry is enough to round-trip.
void write_ac4_single_empty_substream_index_table(BitWriter& w) {
    w.put(1, 2);   // n_substreams = 1
    w.put(1, 1);   // b_size_present
    w.put(0, 1);   // b_more_bits
    w.put(0, 10);  // substream_size = 0
}

// Wraps one already-written substream-group payload (preamble, then
// caller's own b_ajoc/oamd/substream-info bits, then b_content_type=0) into
// a full frame and parses it. `write_payload` writes everything from
// ac4_substream_group_info()'s b_oamd_substream flag onward through its
// single substream's *_info() element - i.e. everything after
// write_ac4_object_coded_group_preamble()'s b_channel_coded=0 and before
// the trailing b_content_type flag this function appends itself.
ac4::RawFrame parse_wrapped_object_coded_group(
    const std::function<void(BitWriter&)>& write_payload) {
    BitWriter w;
    write_ac4_object_coded_preamble(w);
    write_ac4_object_coded_group_preamble(w);
    write_payload(w);
    w.put(0, 1);  // b_content_type = 0
    write_ac4_single_empty_substream_index_table(w);
    const auto data = w.bytes();
    auto result = ac4::parse_raw_frame(data);
    REQUIRE(result.has_value());
    return std::move(*result);
}

// substream_index_table() for one substream whose size is NOT transmitted:
// n_substreams=1, b_size_present=0, and no substream_size entry at all. 3
// bits. Table 14 reads b_size_present only when n_substreams == 1, so this
// is the one shape that leaves Toc::substream_sizes empty while
// Toc::n_substreams is 1 - which parse_raw_frame() used to index straight
// into, dereferencing element 0 of an empty vector.
void write_ac4_single_sizeless_substream_index_table(BitWriter& w) {
    w.put(1, 2);  // n_substreams = 1
    w.put(0, 1);  // b_size_present = 0
}

}  // namespace

// Regression: found by fuzz/fuzz_ac4_parse.cpp on its first run over the
// seed corpus, as a SEGV on address 0 inside parse_raw_frame(). Every
// substream_index_table() this suite built before it set b_size_present = 1,
// so the branch that omits the size table had never been parsed.
TEST_CASE("parse_raw_frame: a substream whose size is not transmitted runs to the end of the frame",
          "[ac4]") {
    BitWriter w;
    write_ac4_object_coded_preamble(w);
    write_ac4_object_coded_group_preamble(w);
    w.put(0, 1);  // b_oamd_substream = 0
    w.put(1, 1);  // b_ajoc = 1
    w.put(1, 1);  // b_lfe
    w.put(1, 1);  // b_static_dmx
    w.put(0, 1);  // b_oamd_common_data_present
    w.put(0, 4);  // n_fullband_upmix_signals_minus1 = 0 -> 1 signal
    w.put(1, 1);  // bed_dyn_obj_assignment(1): b_dyn_objects_only = 1
    w.put(0, 1);  // b_bitrate_info
    w.put(0, 1);  // b_audio_ndot
    w.put(1, 2);  // substream_index = 1
    w.put(0, 1);  // b_content_type = 0
    write_ac4_single_sizeless_substream_index_table(w);
    // Real payload after the TOC, which is the whole point: with no
    // transmitted size, what the substream covers is decided by where the
    // frame ends, so a frame that ends at the TOC would not test anything.
    auto data = w.bytes();
    const std::size_t toc_bytes = data.size();
    data.insert(data.end(), 8, std::byte{0});

    const auto result = ac4::parse_raw_frame(data);
    REQUIRE(result.has_value());
    REQUIRE(result->toc.n_substreams == 1);
    CHECK(result->toc.substream_sizes.empty());
    REQUIRE(result->substreams.size() == 1);
    // The one substream covers everything from payload_base to the end of
    // the frame, and never past it.
    CHECK(result->substreams[0].offset == toc_bytes);
    CHECK(result->substreams[0].size == data.size() - toc_bytes);
    CHECK(result->substreams[0].offset + result->substreams[0].size == data.size());
}

TEST_CASE("parse_substream_info_ajoc: static_dmx, minimal upmix", "[ac4]") {
    // b_lfe=1, b_static_dmx=1 (skips the dmx bed_dyn_obj_assignment() call
    // entirely, n_fullband_dmx_signals defaults to 5), b_oamd_common_data_
    // present=0, one upmix signal whose own bed_dyn_obj_assignment() is the
    // trivial b_dyn_objects_only=1 case, no bitrate info, substream_index=1.
    const auto frame = parse_wrapped_object_coded_group([](BitWriter& w) {
        w.put(0, 1);  // b_oamd_substream = 0
        w.put(1, 1);  // b_ajoc = 1
        w.put(1, 1);  // b_lfe
        w.put(1, 1);  // b_static_dmx
        w.put(0, 1);  // b_oamd_common_data_present
        w.put(0, 4);  // n_fullband_upmix_signals_minus1 = 0 -> 1 signal
        w.put(1, 1);  // bed_dyn_obj_assignment(1): b_dyn_objects_only = 1
        w.put(0, 1);  // b_bitrate_info
        w.put(0, 1);  // b_audio_ndot
        w.put(1, 2);  // substream_index = 1
    });

    REQUIRE(frame.toc.substream_groups.size() == 1);
    const auto& group = frame.toc.substream_groups[0];
    CHECK_FALSE(group.b_channel_coded);
    CHECK_FALSE(group.oamd.has_value());
    REQUIRE(group.substreams.size() == 1);
    REQUIRE(group.substreams[0].kind == ac4::GroupSubstream::Kind::kAjoc);
    REQUIRE(group.substreams[0].ajoc.has_value());
    const auto& ajoc = *group.substreams[0].ajoc;
    CHECK(ajoc.b_lfe);
    CHECK(ajoc.b_static_dmx);
    CHECK(ajoc.n_fullband_dmx_signals == 5);
    CHECK(ajoc.static_objects.empty());
    CHECK(ajoc.n_fullband_upmix_signals == 1);
    CHECK(ajoc.upmix_objects.empty());
    CHECK_FALSE(ajoc.sf_multiplier.has_value());
    CHECK_FALSE(ajoc.bitrate_kbps.has_value());
    REQUIRE(ajoc.substream_index.has_value());
    CHECK(*ajoc.substream_index == 1);
}

TEST_CASE("oamd_common_data: b_additional_data = 0 reads cleanly and the TOC continues",
          "[ac4]") {
    const auto frame = parse_wrapped_object_coded_group([](BitWriter& w) {
        w.put(0, 1);  // b_oamd_substream = 0
        w.put(1, 1);  // b_ajoc = 1
        w.put(1, 1);  // b_lfe
        w.put(1, 1);  // b_static_dmx (skip dmx assignment to keep this short)
        w.put(1, 1);  // b_oamd_common_data_present
        w.put(1, 1);  //   b_default_screen_size_ratio = 1 (skips the 5-bit code)
        w.put(1, 1);  //   b_bed_object_chan_distribute = 1
        w.put(0, 1);  //   b_additional_data = 0 -> oamd_common_data() ends here
        w.put(0, 4);  // n_fullband_upmix_signals_minus1 = 0 -> 1 signal
        w.put(1, 1);  // bed_dyn_obj_assignment(1): b_dyn_objects_only = 1
        w.put(0, 1);  // b_bitrate_info
        w.put(0, 1);  // b_audio_ndot
        w.put(1, 2);  // substream_index = 1
    });

    REQUIRE(frame.toc.substream_groups.size() == 1);
    REQUIRE(frame.toc.substream_groups[0].substreams.size() == 1);
    REQUIRE(frame.toc.substream_groups[0].substreams[0].ajoc.has_value());
    const auto& ajoc = *frame.toc.substream_groups[0].substreams[0].ajoc;
    REQUIRE(ajoc.oamd_common_data.has_value());
    const auto& oamd = *ajoc.oamd_common_data;
    CHECK(oamd.b_default_screen_size_ratio);
    CHECK_FALSE(oamd.master_screen_size_ratio_code.has_value());
    CHECK(oamd.b_bed_object_chan_distribute);
    CHECK_FALSE(oamd.trim.has_value());
    CHECK_FALSE(oamd.bed_render_info.has_value());
    CHECK_FALSE(oamd.headphone.has_value());
    // The bits after oamd_common_data() were read from the right place.
    CHECK(ajoc.n_fullband_upmix_signals == 1);
    CHECK(ajoc.upmix_objects.empty());
    REQUIRE(ajoc.substream_index.has_value());
    CHECK(*ajoc.substream_index == 1);
}

TEST_CASE("oamd_common_data: b_default_screen_size_ratio = 0 reads master_screen_size_ratio_code",
          "[ac4]") {
    const auto frame = parse_wrapped_object_coded_group([](BitWriter& w) {
        w.put(0, 1);   // b_oamd_substream = 0
        w.put(1, 1);   // b_ajoc = 1
        w.put(0, 1);   // b_lfe
        w.put(1, 1);   // b_static_dmx
        w.put(1, 1);   // b_oamd_common_data_present
        w.put(0, 1);   //   b_default_screen_size_ratio = 0
        w.put(19, 5);  //   master_screen_size_ratio_code = 19
        w.put(0, 1);   //   b_bed_object_chan_distribute = 0
        w.put(0, 1);   //   b_additional_data = 0
        w.put(0, 4);   // n_fullband_upmix_signals_minus1 = 0 -> 1 signal
        w.put(1, 1);   // bed_dyn_obj_assignment(1): b_dyn_objects_only = 1
        w.put(0, 1);   // b_bitrate_info
        w.put(0, 1);   // b_audio_ndot
        w.put(1, 2);   // substream_index = 1
    });

    const auto& oamd = *frame.toc.substream_groups[0].substreams[0].ajoc->oamd_common_data;
    CHECK_FALSE(oamd.b_default_screen_size_ratio);
    REQUIRE(oamd.master_screen_size_ratio_code.has_value());
    CHECK(*oamd.master_screen_size_ratio_code == 19);
    CHECK_FALSE(oamd.b_bed_object_chan_distribute);
}

TEST_CASE("oamd_common_data: add_data_bytes' budget covers trim, bed_render_info and headphone",
          "[ac4]") {
    // trim()/bed_render_info()/headphone() each read one bit (their own
    // presence flag, 0) and stop there; the byte budget (8 bits) is wider
    // than the 3 they spend between them, so the remaining 5 bits are read
    // as add_data - a raw range this parser does not interpret - rather
    // than left for headphone() (already read) or the fields after
    // oamd_common_data() to be misread from the wrong position.
    const auto frame = parse_wrapped_object_coded_group([](BitWriter& w) {
        w.put(0, 1);  // b_oamd_substream = 0
        w.put(1, 1);  // b_ajoc = 1
        w.put(0, 1);  // b_lfe
        w.put(1, 1);  // b_static_dmx
        w.put(1, 1);  // b_oamd_common_data_present
        w.put(1, 1);  //   b_default_screen_size_ratio = 1
        w.put(0, 1);  //   b_bed_object_chan_distribute = 0
        w.put(1, 1);  //   b_additional_data = 1
        w.put(0, 1);  //   add_data_bytes_minus1 = 0 -> add_data_bytes = 1 (8 bits)
        w.put(0, 1);  //   trim(): b_trim_present = 0            (1 of 8 bits)
        w.put(0, 1);  //   bed_render_info(): b_bed_render_info = 0  (1 of 8 bits)
        w.put(0, 1);  //   headphone(): b_headphone = 0          (1 of 8 bits)
        w.put(0, 5);  //   add_data: 5 raw bits, uninterpreted    (5 of 8 bits)
        w.put(0, 4);  // n_fullband_upmix_signals_minus1 = 0 -> 1 signal
        w.put(1, 1);  // bed_dyn_obj_assignment(1): b_dyn_objects_only = 1
        w.put(0, 1);  // b_bitrate_info
        w.put(0, 1);  // b_audio_ndot
        w.put(1, 2);  // substream_index = 1
    });

    const auto& ajoc = *frame.toc.substream_groups[0].substreams[0].ajoc;
    const auto& oamd = *ajoc.oamd_common_data;
    CHECK_FALSE(oamd.trim.has_value());
    CHECK_FALSE(oamd.bed_render_info.has_value());
    CHECK_FALSE(oamd.headphone.has_value());
    CHECK(ajoc.n_fullband_upmix_signals == 1);
    REQUIRE(ajoc.substream_index.has_value());
    CHECK(*ajoc.substream_index == 1);
}

TEST_CASE("oamd_common_data: a nested element reading past its add_data budget fails cleanly",
          "[ac4]") {
    // add_data_bytes declares an 8-bit budget, but trim() alone - once
    // global_trim_mode selects the NUM_TRIM_CONFIGS loop - reads 16 bits
    // (1+2+2+2 header, then 9 configs at 1 bit each for b_default_trim).
    // Plenty of real data follows, so this is the internal budget check in
    // oamd_common_data()'s spend() firing, not truncation against the
    // actual end of the frame.
    BitWriter w;
    write_ac4_object_coded_preamble(w);
    write_ac4_object_coded_group_preamble(w);
    w.put(0, 1);     // b_oamd_substream = 0
    w.put(1, 1);     // b_ajoc = 1
    w.put(0, 1);     // b_lfe
    w.put(1, 1);     // b_static_dmx
    w.put(1, 1);     // b_oamd_common_data_present
    w.put(1, 1);     //   b_default_screen_size_ratio = 1
    w.put(0, 1);     //   b_bed_object_chan_distribute = 0
    w.put(1, 1);     //   b_additional_data = 1
    w.put(0, 1);     //   add_data_bytes_minus1 = 0 -> add_data_bytes = 1 (8-bit budget)
    w.put(1, 1);     //   trim(): b_trim_present = 1
    w.put(0, 2);     //     warp_mode
    w.put(0, 2);     //     reserved
    w.put(0b10, 2);  //     global_trim_mode = 0b10 -> the NUM_TRIM_CONFIGS loop
    for (int i = 0; i < 9; ++i) {
        w.put(1, 1);  // configs[i]: b_default_trim = 1 (1 bit each, 9 total)
    }
    // 16 bits spent inside trim() alone, against an 8-bit budget: the
    // failure happens here, so nothing after this matters, but pad well
    // past it regardless - a bug that keeps reading anyway should hit real
    // (if meaningless) data rather than the reader's own overflow path,
    // keeping this test about the budget check, not about truncation.
    w.put(0, 64);
    const auto data = w.bytes();

    const auto result = ac4::parse_raw_frame(data);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac4::Error::kTruncated);
}

TEST_CASE("parse_bed_dyn_obj_assignment: nonstd flags exclude LFE (A-JOC dmx assignment)",
          "[ac4]") {
    // Regression vector for the array-index bug this parser had: Table 64's
    // array position is (16 - channel_order), so a plain 17-bit MSB-first
    // read has flag[16-i] at bit i, not flag[i] at bit i or flag[9-i]'s
    // 10-bit-case formula misapplied here. Orders 0,1,2,3 (L,R,C,LFE) are
    // all set (bits at positions 16,15,14,13); order 3 (LFE) must NOT
    // produce a BED object here - bed_dyn_obj_assignment()'s own
    // "if (i != 3 and i != 16)" guard excludes it - unlike
    // ac4_substream_info_obj()'s structurally similar branch (see the
    // "std bed flags include LFE" test below), which does add one.
    const auto frame = parse_wrapped_object_coded_group([](BitWriter& w) {
        w.put(0, 1);  // b_oamd_substream = 0
        w.put(1, 1);  // b_ajoc = 1
        w.put(0, 1);  // b_lfe
        w.put(0, 1);  // b_static_dmx = 0 -> dmx assignment is read
        w.put(1, 4);  // n_fullband_dmx_signals_minus1 = 1 -> n_signals = 2
        // bed_dyn_obj_assignment(2):
        w.put(0, 1);  // b_dyn_objects_only
        w.put(0, 1);  // b_isf
        w.put(0, 1);  // b_ch_assign_code
        w.put(1, 1);  // b_channel_assignment_flags_present
        w.put(1, 1);  // b_nonstd_bed_channel_assignment_flags_present
        // Table 64: array position (16 - channel_order); orders 0,1,2,3 ->
        // positions 16,15,14,13. Array position 0 is the FIRST bit
        // transmitted (ac4.cpp's own comment on this formula) - positions
        // 16,15,14,13 are therefore the LAST 4 of the 17 bits written, the
        // low 4 bits of this value.
        w.put(0b1111, 17);
        w.put(0, 1);  // b_oamd_common_data_present
        w.put(0, 4);  // n_fullband_upmix_signals_minus1 = 0 -> 1 signal
        w.put(1, 1);  // bed_dyn_obj_assignment(1): b_dyn_objects_only = 1
        w.put(0, 1);  // b_bitrate_info
        w.put(0, 1);  // b_audio_ndot
        w.put(0, 2);  // substream_index = 0
    });

    REQUIRE(frame.toc.substream_groups.size() == 1);
    REQUIRE(frame.toc.substream_groups[0].substreams.size() == 1);
    REQUIRE(frame.toc.substream_groups[0].substreams[0].ajoc.has_value());
    const auto& ajoc = *frame.toc.substream_groups[0].substreams[0].ajoc;
    CHECK(ajoc.n_fullband_dmx_signals == 2);
    REQUIRE(ajoc.static_objects.size() == 3);  // L, R, C - LFE excluded
    for (const auto& obj : ajoc.static_objects) {
        CHECK(obj.kind == ac4::ObjectKind::kBed);
        CHECK_FALSE(obj.lfe);
        CHECK(obj.ajoc_coded);
    }
    CHECK(ajoc.n_fullband_upmix_signals == 1);
    CHECK(ajoc.upmix_objects.empty());
    REQUIRE(ajoc.substream_index.has_value());
    CHECK(*ajoc.substream_index == 0);
}

TEST_CASE("parse_substream_info_obj: dynamic objects with an LFE bed object", "[ac4]") {
    const auto frame = parse_wrapped_object_coded_group([](BitWriter& w) {
        w.put(0, 1);  // b_oamd_substream = 0
        w.put(0, 1);  // b_ajoc = 0 -> ac4_substream_info_obj()
        w.put(2, 3);  // n_objects_code = 2 -> num_objects = 2
        w.put(1, 1);  // b_dynamic_objects
        w.put(1, 1);  // b_lfe
        w.put(0, 1);  // b_bitrate_info
        w.put(0, 1);  // b_audio_ndot
        w.put(1, 2);  // substream_index = 1
    });

    REQUIRE(frame.toc.substream_groups.size() == 1);
    const auto& group = frame.toc.substream_groups[0];
    REQUIRE(group.substreams.size() == 1);
    REQUIRE(group.substreams[0].kind == ac4::GroupSubstream::Kind::kObj);
    REQUIRE(group.substreams[0].obj.has_value());
    const auto& obj = *group.substreams[0].obj;
    CHECK(obj.b_dynamic_objects);
    REQUIRE(obj.objects.size() == 2);
    CHECK(obj.objects[0].kind == ac4::ObjectKind::kBed);
    CHECK(obj.objects[0].lfe);
    CHECK_FALSE(obj.objects[0].ajoc_coded);
    CHECK(obj.objects[1].kind == ac4::ObjectKind::kDyn);
    CHECK_FALSE(obj.objects[1].lfe);
    REQUIRE(obj.substream_index.has_value());
    CHECK(*obj.substream_index == 1);
}

TEST_CASE("parse_substream_info_obj: std bed flags include LFE, unlike bed_dyn_obj_assignment",
          "[ac4]") {
    // The direct-coded counterpart to the "nonstd flags exclude LFE" test
    // above: ac4_substream_info_obj()'s own std_bed_channel_assignment_flag
    // branch DOES add an LFE-flagged BED object at order 2, per
    // §6.3.2.10.5's Table 65 - a genuine semantic difference from
    // bed_dyn_obj_assignment()'s equivalent branch, not a typo either place
    // - this vector's first draft assumed they matched and its own
    // assertion caught the mistake.
    const auto frame = parse_wrapped_object_coded_group([](BitWriter& w) {
        w.put(0, 1);  // b_oamd_substream = 0
        w.put(0, 1);  // b_ajoc = 0 -> ac4_substream_info_obj()
        w.put(0, 3);  // n_objects_code (unused: b_dynamic_objects=0 below)
        w.put(0, 1);  // b_dynamic_objects
        w.put(1, 1);  // b_bed_objects
        w.put(1, 1);  // b_bed_start
        w.put(0, 1);  // b_ch_assign_code
        w.put(0, 1);  // b_nonstd_bed_channel_assignment_flags_present -> std path
        // Table 65: array position (9 - channel_order); orders 0 (L/R) and
        // 2 (LFE) -> positions 9 and 7 - the LAST and 3rd-to-last of the 10
        // bits written (position 0 is the first bit transmitted).
        w.put(0b101, 10);
        w.put(0, 1);  // b_bitrate_info
        w.put(0, 1);  // b_audio_ndot
        w.put(0, 2);  // substream_index = 0
    });

    REQUIRE(frame.toc.substream_groups.size() == 1);
    REQUIRE(frame.toc.substream_groups[0].substreams.size() == 1);
    REQUIRE(frame.toc.substream_groups[0].substreams[0].obj.has_value());
    const auto& obj = *frame.toc.substream_groups[0].substreams[0].obj;
    REQUIRE(obj.objects.size() == 3);  // L, R (order 0's 2-channel group), LFE (order 2)
    CHECK(obj.objects[0].kind == ac4::ObjectKind::kBed);
    CHECK_FALSE(obj.objects[0].lfe);
    CHECK(obj.objects[1].kind == ac4::ObjectKind::kBed);
    CHECK_FALSE(obj.objects[1].lfe);
    CHECK(obj.objects[2].kind == ac4::ObjectKind::kBed);
    CHECK(obj.objects[2].lfe);  // order 2 IS flagged lfe here
    CHECK_FALSE(obj.b_dynamic_objects);
}

// Regression: n_objects_code and both isf_config fields are 3 bits wide, and
// each indexed a six-entry count table directly, so codes 6 and 7 read past
// the end of it. Found by fuzz/fuzz_ac4_parse.cpp once ac4_objects was built
// with AddressSanitizer, as a stack-buffer-overflow on the committed
// ac4-substream-size-not-transmitted regression input; the uninstrumented runs
// before that read whatever followed the table and carried on. Each vector
// ends in substream_index = 2, which only comes back if the parse stayed in
// sync past the reserved code, and code 5 - the last entry each table has -
// pins the boundary.
namespace {

struct ReservedCountCase {
    std::uint32_t code;
    std::size_t objects;
};

}  // namespace

TEST_CASE("parse_substream_info_obj: a reserved n_objects_code names no objects", "[ac4]") {
    for (const ReservedCountCase tc : {ReservedCountCase{5, 7}, ReservedCountCase{6, 0},
                                       ReservedCountCase{7, 0}}) {
        CAPTURE(tc.code);
        const auto frame = parse_wrapped_object_coded_group([tc](BitWriter& w) {
            w.put(0, 1);        // b_oamd_substream = 0
            w.put(0, 1);        // b_ajoc = 0 -> ac4_substream_info_obj()
            w.put(tc.code, 3);  // n_objects_code
            w.put(1, 1);        // b_dynamic_objects
            w.put(1, 1);        // b_lfe
            w.put(0, 1);        // b_bitrate_info
            w.put(0, 1);        // b_audio_ndot
            w.put(2, 2);        // substream_index = 2
        });

        REQUIRE(frame.toc.substream_groups.size() == 1);
        REQUIRE(frame.toc.substream_groups[0].substreams.size() == 1);
        REQUIRE(frame.toc.substream_groups[0].substreams[0].obj.has_value());
        const auto& obj = *frame.toc.substream_groups[0].substreams[0].obj;
        CHECK(obj.b_dynamic_objects);
        CHECK(obj.objects.size() == tc.objects);
        REQUIRE(obj.substream_index.has_value());
        CHECK(*obj.substream_index == 2);
    }
}

TEST_CASE("parse_substream_info_obj: a reserved isf_config names no objects", "[ac4]") {
    for (const ReservedCountCase tc : {ReservedCountCase{5, 30}, ReservedCountCase{6, 0},
                                       ReservedCountCase{7, 0}}) {
        CAPTURE(tc.code);
        const auto frame = parse_wrapped_object_coded_group([tc](BitWriter& w) {
            w.put(0, 1);        // b_oamd_substream = 0
            w.put(0, 1);        // b_ajoc = 0 -> ac4_substream_info_obj()
            w.put(0, 3);        // n_objects_code (unused: b_dynamic_objects=0 below)
            w.put(0, 1);        // b_dynamic_objects
            w.put(0, 1);        // b_bed_objects
            w.put(1, 1);        // b_isf
            w.put(1, 1);        // b_isf_start
            w.put(tc.code, 3);  // isf_config
            w.put(0, 1);        // b_bitrate_info
            w.put(0, 1);        // b_audio_ndot
            w.put(2, 2);        // substream_index = 2
        });

        REQUIRE(frame.toc.substream_groups.size() == 1);
        REQUIRE(frame.toc.substream_groups[0].substreams.size() == 1);
        REQUIRE(frame.toc.substream_groups[0].substreams[0].obj.has_value());
        const auto& obj = *frame.toc.substream_groups[0].substreams[0].obj;
        CHECK(obj.objects.size() == tc.objects);
        for (const auto& object : obj.objects) {
            CHECK(object.kind == ac4::ObjectKind::kIsf);
        }
        REQUIRE(obj.substream_index.has_value());
        CHECK(*obj.substream_index == 2);
    }
}

TEST_CASE("parse_bed_dyn_obj_assignment: a reserved isf_config names no objects", "[ac4]") {
    for (const ReservedCountCase tc : {ReservedCountCase{5, 30}, ReservedCountCase{6, 0},
                                       ReservedCountCase{7, 0}}) {
        CAPTURE(tc.code);
        const auto frame = parse_wrapped_object_coded_group([tc](BitWriter& w) {
            w.put(0, 1);  // b_oamd_substream = 0
            w.put(1, 1);  // b_ajoc = 1
            w.put(0, 1);  // b_lfe
            w.put(0, 1);  // b_static_dmx = 0 -> dmx assignment is read
            w.put(0, 4);  // n_fullband_dmx_signals_minus1 = 0 -> n_signals = 1
            // bed_dyn_obj_assignment(1):
            w.put(0, 1);        // b_dyn_objects_only
            w.put(1, 1);        // b_isf
            w.put(tc.code, 3);  // isf_config
            w.put(0, 1);        // b_oamd_common_data_present
            w.put(0, 4);        // n_fullband_upmix_signals_minus1 = 0 -> 1 signal
            w.put(1, 1);        // bed_dyn_obj_assignment(1): b_dyn_objects_only = 1
            w.put(0, 1);        // b_bitrate_info
            w.put(0, 1);        // b_audio_ndot
            w.put(2, 2);        // substream_index = 2
        });

        REQUIRE(frame.toc.substream_groups.size() == 1);
        REQUIRE(frame.toc.substream_groups[0].substreams.size() == 1);
        REQUIRE(frame.toc.substream_groups[0].substreams[0].ajoc.has_value());
        const auto& ajoc = *frame.toc.substream_groups[0].substreams[0].ajoc;
        CHECK(ajoc.n_fullband_dmx_signals == 1);
        CHECK(ajoc.static_objects.size() == tc.objects);
        for (const auto& object : ajoc.static_objects) {
            CHECK(object.kind == ac4::ObjectKind::kIsf);
            CHECK(object.ajoc_coded);
        }
        CHECK(ajoc.n_fullband_upmix_signals == 1);
        CHECK(ajoc.upmix_objects.empty());
        REQUIRE(ajoc.substream_index.has_value());
        CHECK(*ajoc.substream_index == 2);
    }
}

TEST_CASE("parse_oamd_substream_info via ac4_substream_group_info's b_oamd_substream", "[ac4]") {
    const auto frame = parse_wrapped_object_coded_group([](BitWriter& w) {
        w.put(1, 1);  // b_oamd_substream = 1
        w.put(1, 1);  // b_oamd_ndot
        w.put(2, 2);  // substream_index = 2
        // The group's one substream still has to be parsed - simplest
        // ac4_substream_info_obj() shape: reserved-bytes branch, 0 bytes.
        w.put(0, 1);  // b_ajoc = 0
        w.put(0, 3);  // n_objects_code (unused)
        w.put(0, 1);  // b_dynamic_objects
        w.put(0, 1);  // b_bed_objects
        w.put(0, 1);  // b_isf
        w.put(0, 4);  // res_bytes = 0
        w.put(0, 1);  // b_bitrate_info
        w.put(0, 1);  // b_audio_ndot
        w.put(0, 2);  // substream_index = 0
    });

    REQUIRE(frame.toc.substream_groups.size() == 1);
    const auto& group = frame.toc.substream_groups[0];
    REQUIRE(group.oamd.has_value());
    CHECK(group.oamd->b_oamd_ndot);
    REQUIRE(group.oamd->substream_index.has_value());
    CHECK(*group.oamd->substream_index == 2);
    REQUIRE(group.substreams.size() == 1);
    REQUIRE(group.substreams[0].obj.has_value());
    CHECK(group.substreams[0].obj->objects.empty());
}

TEST_CASE("read_bitrate_indicator: terminal and extended codes resolve distinctly", "[ac4]") {
    // Table 90's own "Value of bitrate_indicator" bit-pattern column is
    // ambiguous as a plain integer - the 3-bit terminal code 0b100 (24
    // kbit/s) and the 5-bit extended code 0b00100 (32 kbit/s) are the same
    // int once leading zeros are dropped. A lookup table keyed by that raw
    // pattern (as this parser's first draft was) silently collapses both
    // to whichever value the table implementation happens to keep for a
    // duplicate key - proven here by checking that the two now resolve to
    // their correct, DISTINCT brate_ind-mapped kbit/s values rather than
    // both landing on the same one.
    auto build = [](std::uint32_t code, int width) {
        return [code, width](BitWriter& w) {
            w.put(0, 1);  // b_oamd_substream = 0
            w.put(0, 1);  // b_ajoc = 0 -> ac4_substream_info_obj()
            w.put(0, 3);  // n_objects_code (unused)
            w.put(0, 1);  // b_dynamic_objects
            w.put(0, 1);  // b_bed_objects
            w.put(0, 1);  // b_isf
            w.put(0, 4);  // res_bytes = 0
            w.put(1, 1);  // b_bitrate_info
            w.put(code, width);
            w.put(0, 1);  // b_audio_ndot
            w.put(0, 2);  // substream_index = 0
        };
    };

    const auto terminal = parse_wrapped_object_coded_group(build(0b100, 3));  // 24 kbit/s
    REQUIRE(terminal.toc.substream_groups[0].substreams[0].obj.has_value());
    REQUIRE(terminal.toc.substream_groups[0].substreams[0].obj->bitrate_kbps.has_value());
    CHECK(*terminal.toc.substream_groups[0].substreams[0].obj->bitrate_kbps == 24);

    const auto extended = parse_wrapped_object_coded_group(build(0b00100, 5));  // 32 kbit/s
    REQUIRE(extended.toc.substream_groups[0].substreams[0].obj.has_value());
    REQUIRE(extended.toc.substream_groups[0].substreams[0].obj->bitrate_kbps.has_value());
    CHECK(*extended.toc.substream_groups[0].substreams[0].obj->bitrate_kbps == 32);
}

TEST_CASE("parse_raw_frame refuses bitstream_version above 2", "[ac4]") {
    // §6.3.2.1.1: only bitstream_version 0-2 are decodable. The first byte's
    // top two bits are bitstream_version's raw 2-bit field; 0b11 (3) plus a
    // variable_bits(2) extension of 0 leaves it at 3, deliberately not the
    // 3 + 16*n a longer extension would produce - the smallest value that
    // exercises the refusal.
    const std::vector<std::byte> raw = {std::byte{0xC0}, std::byte{0x00}, std::byte{0x00},
                                        std::byte{0x00}};
    const auto result = ac4::parse_raw_frame(raw);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac4::Error::kUnsupportedBitstreamVersion);
}

// --- EMDF-only presentations (presentation_config 6) ------------------------
//
// TS 103 190-1 §4.2.3.2 and TS 103 190-2 §6.2.1.2/§6.2.1.3 set
// b_add_emdf_substreams for a presentation_config 6 presentation without
// transmitting it, and read the n_add_emdf_substreams loop after the
// if/else, so an EMDF-only presentation still carries a count and that many
// emdf_info() elements. No DEE encode writes one. Each frame below puts an
// EMDF-only presentation ahead of an ordinary one: the ordinary presentation,
// the substream groups and substream_index_table() come back as written only
// if that loop was read. The same frames were built with a separate Python
// bit writer and parsed by tools/references/ac4_parse.py before being
// written out here.
namespace {

// ac4_toc() for bitstream_version 2 up to its first presentation, announcing
// two: fs_index 0 (so no b_sf_multiplier is read anywhere), b_payload_base 0
// and b_program_id 0.
void write_v1_two_presentation_toc_preamble(BitWriter& w, std::uint32_t frame_rate_index) {
    w.put(2, 2);                 // bitstream_version = 2
    w.put(0, 10);                // sequence_counter
    w.put(0, 1);                 // b_wait_frames
    w.put(0, 1);                 // fs_index = 0 (44100 Hz)
    w.put(frame_rate_index, 4);  // frame_rate_index
    w.put(0, 1);                 // b_iframe_global
    w.put(0, 1);                 // b_single_presentation = 0
    w.put(1, 1);                 // b_more_presentations = 1
    w.put(0, 2);                 // variable_bits(2): chunk 0
    w.put(0, 1);                 //   no continuation -> n_presentations = 0 + 2
    w.put(0, 1);                 // b_payload_base = 0
    w.put(0, 1);                 // b_program_id = 0
}

// An EMDF-only ac4_presentation_v1_info(): presentation_config 6, then the
// n_add_emdf_substreams loop and nothing else. The count takes the
// variable_bits() escape to 4, and the four emdf_info() elements differ in
// length, so a parser that reads the wrong number of them, or none, loses
// its place before the next presentation.
void write_v1_emdf_only_presentation(BitWriter& w) {
    w.put(0, 1);  // b_single_substream_group = 0
    w.put(6, 3);  // presentation_config = 6
    w.put(1, 1);  // presentation_version: one 1 bit,
    w.put(0, 1);  //   then the terminator -> 1
    w.put(0, 2);  // n_add_emdf_substreams = 0 -> variable_bits(2) + 4
    w.put(0, 2);  //   variable_bits(2): chunk 0
    w.put(0, 1);  //   no continuation -> 4 emdf_info() elements
    // emdf_info() 1: escaped emdf_version and key_id, and an
    // emdf_payloads_substream() at substream_index 2.
    w.put(3, 2);  // emdf_version = 3 -> += variable_bits(2)
    w.put(0, 2);  //   chunk 0
    w.put(0, 1);  //   no continuation -> 3
    w.put(7, 3);  // key_id = 7 -> += variable_bits(3)
    w.put(1, 3);  //   chunk 1
    w.put(0, 1);  //   no continuation -> 8
    w.put(1, 1);  // b_emdf_payloads_substream_info
    w.put(2, 2);  // substream_index = 2
    w.put(0, 2);  // emdf_reserved: primary
    w.put(0, 2);  // emdf_reserved: secondary
    // emdf_info() 2: four bytes of emdf_reserved() data.
    w.put(0, 2);            // emdf_version
    w.put(0, 3);            // key_id
    w.put(0, 1);            // b_emdf_payloads_substream_info
    w.put(0, 2);            // emdf_reserved: primary
    w.put(2, 2);            // emdf_reserved: secondary = 2 -> 4 bytes
    w.put(0xDEADBEEF, 32);  // the reserved bytes
    // emdf_info() 3: every field 0.
    w.put(0, 2);
    w.put(0, 3);
    w.put(0, 1);
    w.put(0, 2);
    w.put(0, 2);
    // emdf_info() 4:
    w.put(1, 2);  // emdf_version = 1
    w.put(2, 3);  // key_id = 2
    w.put(0, 1);  // b_emdf_payloads_substream_info
    w.put(0, 2);  // emdf_reserved: primary
    w.put(0, 2);  // emdf_reserved: secondary
}

}  // namespace

TEST_CASE("parse_raw_frame: a v0 EMDF-only presentation followed by an ordinary one", "[ac4]") {
    BitWriter w;
    w.put(0, 2);   // bitstream_version = 0 (the v0 TOC path, <= 1)
    w.put(0, 10);  // sequence_counter
    w.put(0, 1);   // b_wait_frames
    w.put(0, 1);   // fs_index = 0 (44100 Hz)
    w.put(5, 4);   // frame_rate_index = 5 (frame_rate_multiply_info reads 0 bits)
    w.put(0, 1);   // b_iframe_global
    w.put(0, 1);   // b_single_presentation = 0
    w.put(1, 1);   // b_more_presentations = 1
    w.put(0, 2);   // variable_bits(2): chunk 0
    w.put(0, 1);   //   no continuation -> n_presentations = 0 + 2
    w.put(0, 1);   // b_payload_base = 0
    // Presentation 0, EMDF-only:
    w.put(0, 1);  // b_single_substream = 0
    w.put(6, 3);  // presentation_config = 6
    w.put(0, 1);  // presentation_version terminator (unary 0 -> version 0)
    w.put(2, 2);  // n_add_emdf_substreams = 2
    // emdf_info() 1, naming an emdf_payloads_substream() at substream_index 2:
    w.put(0, 2);  // emdf_version
    w.put(0, 3);  // key_id
    w.put(1, 1);  // b_emdf_payloads_substream_info
    w.put(2, 2);  // substream_index = 2
    w.put(0, 2);  // emdf_reserved: primary
    w.put(0, 2);  // emdf_reserved: secondary
    // emdf_info() 2, with one byte of emdf_reserved() data:
    w.put(1, 2);     // emdf_version = 1
    w.put(5, 3);     // key_id = 5
    w.put(0, 1);     // b_emdf_payloads_substream_info
    w.put(1, 2);     // emdf_reserved: primary = 1 -> 1 byte
    w.put(0, 2);     // emdf_reserved: secondary
    w.put(0xA5, 8);  // the reserved byte
    // Presentation 1, presentation_config 2 (Main + Associate):
    w.put(0, 1);  // b_single_substream = 0
    w.put(2, 3);  // presentation_config = 2
    w.put(0, 1);  // presentation_version terminator (unary 0 -> version 0)
    w.put(3, 3);  // md_compat = 3
    w.put(1, 1);  // b_belongs_to_presentation_id
    w.put(1, 2);  //   variable_bits(2): chunk 1
    w.put(0, 1);  //   no continuation -> presentation_id = 1
    // frame_rate_multiply_info(frame_rate_index=5): 0 bits.
    // emdf_info(): every field 0.
    w.put(0, 2);
    w.put(0, 3);
    w.put(0, 1);
    w.put(0, 2);
    w.put(0, 2);
    w.put(0, 1);  // b_hsf_ext
    // ac4_substream_info(), Main (fs_index 0, so no b_sf_multiplier):
    w.put(0b10, 2);  // channel_mode = Stereo
    w.put(0, 1);     // b_bitrate_info
    w.put(0, 1);     // b_content_type
    w.put(1, 1);     // b_iframe (frame_rate_factor 1)
    w.put(0, 2);     // substream_index = 0
    // ac4_substream_info(), Associate:
    w.put(0, 1);  // channel_mode = Mono
    w.put(0, 1);  // b_bitrate_info
    w.put(0, 1);  // b_content_type
    w.put(1, 1);  // b_iframe
    w.put(1, 2);  // substream_index = 1
    w.put(1, 1);  // b_pre_virtualized
    w.put(0, 1);  // b_add_emdf_substreams = 0
    // substream_index_table(): three substreams of 4, 2 and 1 bytes.
    w.put(3, 2);   // n_substreams = 3 (b_size_present is read only for 1)
    w.put(0, 1);   // b_more_bits
    w.put(4, 10);  // substream_size[0] = 4
    w.put(0, 1);   // b_more_bits
    w.put(2, 10);  // substream_size[1] = 2
    w.put(0, 1);   // b_more_bits
    w.put(1, 10);  // substream_size[2] = 1
    auto data = w.bytes();
    const std::size_t toc_bytes = data.size();
    // Substream 0: ac4_substream() with audio_size 0x123 (15 bits), b_more_bits 0.
    data.insert(data.end(), {std::byte{0x02}, std::byte{0x46}, std::byte{0}, std::byte{0}});
    data.insert(data.end(), 3, std::byte{0});  // substreams 1 and 2

    const auto result = ac4::parse_raw_frame(data);
    REQUIRE(result.has_value());
    const auto& toc = result->toc;
    CHECK(toc.bitstream_version == 0);
    CHECK(toc.n_presentations == 2);
    REQUIRE(toc.presentations_v0.size() == 2);

    const auto& emdf_only = toc.presentations_v0[0];
    REQUIRE(emdf_only.presentation_config.has_value());
    CHECK(*emdf_only.presentation_config == 6);
    CHECK(emdf_only.presentation_version == 0);
    CHECK_FALSE(emdf_only.md_compat.has_value());
    CHECK_FALSE(emdf_only.presentation_id.has_value());
    CHECK(emdf_only.substreams.empty());

    const auto& ordinary = toc.presentations_v0[1];
    REQUIRE(ordinary.presentation_config.has_value());
    CHECK(*ordinary.presentation_config == 2);
    CHECK(ordinary.presentation_version == 0);
    REQUIRE(ordinary.md_compat.has_value());
    CHECK(*ordinary.md_compat == 3);
    REQUIRE(ordinary.presentation_id.has_value());
    CHECK(*ordinary.presentation_id == 1);
    REQUIRE(ordinary.substreams.size() == 2);
    CHECK(ordinary.substreams[0].first == "Main");
    CHECK(ordinary.substreams[0].second.channel_mode_name == "Stereo");
    REQUIRE(ordinary.substreams[0].second.substream_index.has_value());
    CHECK(*ordinary.substreams[0].second.substream_index == 0);
    CHECK(ordinary.substreams[1].first == "Associate");
    CHECK(ordinary.substreams[1].second.channel_mode_name == "Mono");
    REQUIRE(ordinary.substreams[1].second.substream_index.has_value());
    CHECK(*ordinary.substreams[1].second.substream_index == 1);

    CHECK(toc.n_substreams == 3);
    CHECK(toc.substream_sizes == std::vector<int>{4, 2, 1});
    REQUIRE(result->substreams.size() == 3);
    CHECK(result->substreams[0].offset == toc_bytes);
    CHECK(result->substreams[0].is_audio);
    REQUIRE(result->substreams[0].audio_size.has_value());
    CHECK(*result->substreams[0].audio_size == 0x123);
    CHECK(result->substreams[1].is_audio);
    CHECK_FALSE(result->substreams[2].is_audio);  // the EMDF payloads substream
}

TEST_CASE("parse_raw_frame: a v1 EMDF-only presentation followed by an ordinary one", "[ac4]") {
    BitWriter w;
    write_v1_two_presentation_toc_preamble(w, 5);  // frame_rate_multiply_info reads 0 bits
    write_v1_emdf_only_presentation(w);
    // Presentation 1, a single substream group:
    w.put(1, 1);  // b_single_substream_group = 1
    w.put(1, 1);  // presentation_version: one 1 bit,
    w.put(0, 1);  //   then the terminator -> 1
    w.put(2, 3);  // md_compat = 2
    w.put(1, 1);  // b_presentation_id
    w.put(2, 2);  //   variable_bits(2): chunk 2
    w.put(0, 1);  //   no continuation -> presentation_id = 2
    // frame_rate_multiply_info(frame_rate_index=5): 0 bits.
    w.put(0, 1);  // frame_rate_fractions_info: b_frame_rate_fraction
    // emdf_info(): every field 0.
    w.put(0, 2);
    w.put(0, 3);
    w.put(0, 1);
    w.put(0, 2);
    w.put(0, 2);
    w.put(1, 1);  // b_presentation_filter
    w.put(1, 1);  // b_enable_presentation
    w.put(0, 3);  // ac4_sgi_specifier(): group_index = 0
    w.put(0, 1);  // b_pre_virtualized
    w.put(0, 1);  // b_add_emdf_substreams = 0
    w.put(0, 1);  // b_alternative
    w.put(0, 1);  // b_pres_ndot
    w.put(1, 2);  // ac4_presentation_substream_info(): substream_index = 1
    // ac4_substream_group_info() 0, one channel-coded substream:
    w.put(1, 1);       // b_substreams_present
    w.put(0, 1);       // b_hsf_ext
    w.put(1, 1);       // b_single_substream
    w.put(1, 1);       // b_channel_coded
    w.put(0b1110, 4);  // channel_mode = 5.1
    w.put(0, 1);       // b_bitrate_info
    w.put(0, 1);       // b_audio_ndot (frame_rate_factor 1)
    w.put(0, 2);       // substream_index = 0
    w.put(0, 1);       // b_content_type
    // substream_index_table(): three substreams of 4, 1 and 2 bytes.
    w.put(3, 2);   // n_substreams = 3
    w.put(0, 1);   // b_more_bits
    w.put(4, 10);  // substream_size[0] = 4
    w.put(0, 1);   // b_more_bits
    w.put(1, 10);  // substream_size[1] = 1
    w.put(0, 1);   // b_more_bits
    w.put(2, 10);  // substream_size[2] = 2
    auto data = w.bytes();
    const std::size_t toc_bytes = data.size();
    // Substream 0: ac4_substream() with audio_size 0x155 (15 bits), b_more_bits 0.
    data.insert(data.end(), {std::byte{0x02}, std::byte{0xAA}, std::byte{0}, std::byte{0}});
    data.insert(data.end(), 3, std::byte{0});  // substreams 1 and 2

    const auto result = ac4::parse_raw_frame(data);
    REQUIRE(result.has_value());
    const auto& toc = result->toc;
    CHECK(toc.bitstream_version == 2);
    CHECK(toc.n_presentations == 2);
    REQUIRE(toc.presentations_v1.size() == 2);

    const auto& emdf_only = toc.presentations_v1[0];
    REQUIRE(emdf_only.presentation_config.has_value());
    CHECK(*emdf_only.presentation_config == 6);
    CHECK(emdf_only.presentation_version == 1);
    CHECK_FALSE(emdf_only.md_compat.has_value());
    CHECK_FALSE(emdf_only.enable_presentation.has_value());
    CHECK(emdf_only.group_refs.empty());
    CHECK(emdf_only.frame_rate_factor == 1);

    const auto& ordinary = toc.presentations_v1[1];
    CHECK_FALSE(ordinary.presentation_config.has_value());
    CHECK(ordinary.presentation_version == 1);
    REQUIRE(ordinary.md_compat.has_value());
    CHECK(*ordinary.md_compat == 2);
    REQUIRE(ordinary.enable_presentation.has_value());
    CHECK(*ordinary.enable_presentation);
    CHECK(ordinary.group_refs == std::vector<int>{0});

    REQUIRE(toc.substream_groups.size() == 1);
    const auto& group = toc.substream_groups[0];
    CHECK(group.b_substreams_present);
    CHECK(group.b_channel_coded);
    REQUIRE(group.substreams.size() == 1);
    REQUIRE(group.substreams[0].chan.has_value());
    const auto& chan = *group.substreams[0].chan;
    CHECK(chan.channel_mode_name == "5.1");
    REQUIRE(chan.substream_index.has_value());
    CHECK(*chan.substream_index == 0);
    CHECK_FALSE(group.content_type.has_value());

    CHECK(toc.n_substreams == 3);
    CHECK(toc.substream_sizes == std::vector<int>{4, 1, 2});
    REQUIRE(result->substreams.size() == 3);
    CHECK(result->substreams[0].offset == toc_bytes);
    CHECK(result->substreams[0].is_audio);
    REQUIRE(result->substreams[0].audio_size.has_value());
    CHECK(*result->substreams[0].audio_size == 0x155);
    CHECK_FALSE(result->substreams[1].is_audio);  // the presentation substream
    CHECK_FALSE(result->substreams[2].is_audio);  // the EMDF payloads substream
}

TEST_CASE("parse_raw_frame: substream groups take frame_rate_factor past an EMDF-only presentation",
          "[ac4]") {
    // frame_rate_index 1 (24 fps) makes frame_rate_multiply_info() read a
    // b_multiplier bit. Presentation 1 sets it, so its frame_rate_factor is 2
    // and the group's ac4_substream_info_chan() reads two b_audio_ndot bits.
    // The EMDF-only presentation ahead of it sends no
    // frame_rate_multiply_info() and keeps the default of 1; a group that took
    // its factor from that presentation would read one b_audio_ndot bit and
    // substream_index 1 as 2.
    BitWriter w;
    write_v1_two_presentation_toc_preamble(w, 1);
    write_v1_emdf_only_presentation(w);
    // Presentation 1, a single substream group:
    w.put(1, 1);  // b_single_substream_group = 1
    w.put(1, 1);  // presentation_version: one 1 bit,
    w.put(0, 1);  //   then the terminator -> 1
    w.put(2, 3);  // md_compat = 2
    w.put(1, 1);  // b_presentation_id
    w.put(2, 2);  //   variable_bits(2): chunk 2
    w.put(0, 1);  //   no continuation -> presentation_id = 2
    w.put(1, 1);  // frame_rate_multiply_info(frame_rate_index=1): b_multiplier -> 2
    // frame_rate_fractions_info(frame_rate_index=1): 0 bits.
    // emdf_info(): every field 0.
    w.put(0, 2);
    w.put(0, 3);
    w.put(0, 1);
    w.put(0, 2);
    w.put(0, 2);
    w.put(1, 1);  // b_presentation_filter
    w.put(1, 1);  // b_enable_presentation
    w.put(0, 3);  // ac4_sgi_specifier(): group_index = 0
    w.put(0, 1);  // b_pre_virtualized
    w.put(0, 1);  // b_add_emdf_substreams = 0
    w.put(0, 1);  // b_alternative
    w.put(0, 1);  // b_pres_ndot
    w.put(0, 2);  // ac4_presentation_substream_info(): substream_index = 0
    // ac4_substream_group_info() 0, one channel-coded substream:
    w.put(1, 1);       // b_substreams_present
    w.put(0, 1);       // b_hsf_ext
    w.put(1, 1);       // b_single_substream
    w.put(1, 1);       // b_channel_coded
    w.put(0b1110, 4);  // channel_mode = 5.1
    w.put(0, 1);       // b_bitrate_info
    w.put(0, 1);       // b_audio_ndot 1 of frame_rate_factor 2
    w.put(1, 1);       // b_audio_ndot 2 of frame_rate_factor 2
    w.put(1, 2);       // substream_index = 1
    w.put(0, 1);       // b_content_type
    // substream_index_table(): three substreams of 1, 4 and 4 bytes.
    w.put(3, 2);   // n_substreams = 3
    w.put(0, 1);   // b_more_bits
    w.put(1, 10);  // substream_size[0] = 1
    w.put(0, 1);   // b_more_bits
    w.put(4, 10);  // substream_size[1] = 4
    w.put(0, 1);   // b_more_bits
    w.put(4, 10);  // substream_size[2] = 4
    auto data = w.bytes();
    data.insert(data.end(), 1, std::byte{0});  // substream 0
    // Substream 1: ac4_substream() with audio_size 0x15 (15 bits), b_more_bits 0.
    data.insert(data.end(), {std::byte{0x00}, std::byte{0x2A}, std::byte{0}, std::byte{0}});
    data.insert(data.end(), 4, std::byte{0});  // substream 2

    const auto result = ac4::parse_raw_frame(data);
    REQUIRE(result.has_value());
    const auto& toc = result->toc;
    REQUIRE(toc.presentations_v1.size() == 2);
    CHECK(toc.presentations_v1[0].frame_rate_factor == 1);
    CHECK(toc.presentations_v1[1].frame_rate_factor == 2);
    REQUIRE(toc.substream_groups.size() == 1);
    const auto& group = toc.substream_groups[0];
    REQUIRE(group.substreams.size() == 1);
    REQUIRE(group.substreams[0].chan.has_value());
    REQUIRE(group.substreams[0].chan->substream_index.has_value());
    CHECK(*group.substreams[0].chan->substream_index == 1);
    CHECK_FALSE(group.content_type.has_value());
    CHECK(toc.n_substreams == 3);
    CHECK(toc.substream_sizes == std::vector<int>{1, 4, 4});
    REQUIRE(result->substreams.size() == 3);
    CHECK(result->substreams[1].is_audio);
    REQUIRE(result->substreams[1].audio_size.has_value());
    CHECK(*result->substreams[1].audio_size == 0x15);
}

TEST_CASE("parse_raw_frame: a v0 presentation's runaway EMDF-substream count stops at truncation",
          "[ac4]") {
    // Regression vector for the n_add_emdf_substreams loop
    // (parse_add_emdf_substreams() in ac4.cpp), reached here through
    // parse_presentation_info_v0(): n escapes through variable_bits() with no
    // upper bound, and the comment beside the loop's `if (r.error()) break;`
    // describes what used to happen without it - "a 200-byte frame spends six
    // seconds walking a count no data backs". This and the v0 EMDF-only
    // presentation test above are the only frames in this suite that take
    // the bitstream_version 0/1 path.
    BitWriter w;
    w.put(0, 2);   // bitstream_version = 0 (the v0 TOC path, <= 1)
    w.put(0, 10);  // sequence_counter
    w.put(0, 1);   // b_wait_frames
    w.put(0, 1);   // fs_index = 0 (44100 Hz)
    w.put(5, 4);   // frame_rate_index = 5 (frame_rate_multiply_info reads 0 bits)
    w.put(0, 1);   // b_iframe_global
    w.put(1, 1);   // b_single_presentation -> n_presentations = 1
    w.put(0, 1);   // b_payload_base = 0
    // parse_presentation_info_v0(), b_single_substream branch:
    w.put(1, 1);  // b_single_substream = 1
    w.put(0, 1);  // presentation_version terminator (unary 0 -> version 0)
    w.put(0, 3);  // md_compat
    w.put(0, 1);  // b_belongs_to_presentation_id = 0
    // frame_rate_multiply_info(frame_rate_index=5): 0 bits (default case).
    // emdf_info(): version(2)=0, key_id(3)=0, b_payloads_substream_info(1)=0,
    // emdf_reserved: primary(2)=0, secondary(2)=0.
    w.put(0, 2);
    w.put(0, 3);
    w.put(0, 1);
    w.put(0, 2);
    w.put(0, 2);
    // parse_substream_info_v0(): channel_mode=0 (mono, 1 bit; fs_index != 1
    // so b_sf_multiplier is never read), b_bitrate_info=0, b_content_type=0,
    // one b_iframe bit (frame_rate_factor == 1), substream_index=0.
    w.put(0, 1);  // channel_mode = 0
    w.put(0, 1);  // b_bitrate_info
    w.put(0, 1);  // b_content_type
    w.put(0, 1);  // b_iframe
    w.put(0, 2);  // substream_index
    w.put(0, 1);  // b_pre_virtualized
    w.put(1, 1);  // b_add_emdf_substreams = 1
    w.put(0, 2);  // n = 0 -> escapes via variable_bits(2)
    // Escape n to a real (not phantom-zero) 22,369,623 via 12 rounds of
    // variable_bits(2): 11 continuations of the maximal 2-bit chunk (3), then
    // one terminating round - value = ((((...(3*4+4)...)*4+4)+3), landing on
    // 22,369,619 (+4 -> n; the "put_variable_bits round-trips" test below
    // checks that value against these exact bits). Large enough that actually
    // walking it - each iteration a full emdf_info() - takes many seconds; the
    // escape itself is 36 bits.
    for (int round = 0; round < 11; ++round) {
        w.put(0b11, 2);  // value chunk = 3
        w.put(1, 1);     // continuation
    }
    w.put(0b11, 2);  // final chunk = 3
    w.put(0, 1);     // terminate: n = 22,369,619 + 4 = 22,369,623
    // No further data at all: the loop's first parse_emdf_info() call runs
    // off the end immediately, and the guard has to notice on THIS iteration,
    // not the 22-millionth.

    const auto data = w.bytes();
    const auto start = std::chrono::steady_clock::now();
    const auto result = ac4::parse_raw_frame(data);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac4::Error::kTruncated);
    // The guard's whole job is to notice on the first iteration rather than
    // the 22-millionth - generous even against a loaded shared runner, since
    // the guarded path is a handful of reads, not a loop bound by the
    // escaped count.
    CHECK(elapsed < std::chrono::seconds(2));
}

TEST_CASE("parse_raw_frame: a v1 presentation's runaway EMDF-substream count stops at truncation",
          "[ac4]") {
    // The v1 counterpart of the v0 case above: parse_presentation_v1_info()
    // reaches the same guarded n_add_emdf_substreams loop. Follows
    // write_ac4_object_coded_preamble()'s own field values up to
    // b_add_emdf_substreams (not reused directly - that helper hard-codes the
    // bit clear, and every other test relies on that), then sets it instead
    // of clearing it and appends the same escape as the v0 test.
    BitWriter w;
    w.put(2, 2);   // bitstream_version = 2
    w.put(0, 10);  // sequence_counter
    w.put(0, 1);   // b_wait_frames
    w.put(0, 1);   // fs_index = 0
    w.put(5, 4);   // frame_rate_index = 5
    w.put(0, 1);   // b_iframe_global
    w.put(1, 1);   // b_single_presentation -> n_presentations = 1
    w.put(0, 1);   // b_payload_base = 0
    w.put(0, 1);   // b_program_id = 0
    // ac4_presentation_v1_info():
    w.put(1, 1);  // b_single_substream_group = 1
    w.put(0, 1);  // presentation_version terminator (unary 0 -> version 0)
    w.put(0, 3);  // md_compat
    w.put(0, 1);  // b_presentation_id = 0
    w.put(0, 1);  // frame_rate_fractions_info: frame_rate_factor==1 branch
    w.put(0, 2);  // emdf_info: version
    w.put(0, 3);  // emdf_info: key_id
    w.put(0, 1);  // emdf_info: b_payloads_substream_info
    w.put(0, 2);  // emdf_reserved: primary
    w.put(0, 2);  // emdf_reserved: secondary
    w.put(0, 1);  // b_presentation_filter = 0
    w.put(0, 3);  // ac4_sgi_specifier(): group_index = 0
    w.put(0, 1);  // b_pre_virtualized
    w.put(1, 1);  // b_add_emdf_substreams = 1 (the preamble helper leaves this 0)
    w.put(0, 1);  // b_alternative
    w.put(0, 1);  // b_pres_ndot
    w.put(0, 2);  // ac4_presentation_substream_info()'s substream_index_ref
    // Same escape as the v0 test: n = 0 -> variable_bits(2), 12 rounds
    // landing on 22,369,623, then no further data.
    w.put(0, 2);  // n = 0
    for (int round = 0; round < 11; ++round) {
        w.put(0b11, 2);
        w.put(1, 1);
    }
    w.put(0b11, 2);
    w.put(0, 1);

    const auto data = w.bytes();
    const auto start = std::chrono::steady_clock::now();
    const auto result = ac4::parse_raw_frame(data);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac4::Error::kTruncated);
    CHECK(elapsed < std::chrono::seconds(2));
}

TEST_CASE("put_variable_bits round-trips through the reader's own escapes", "[ac4]") {
    // The helper the two regression vectors below depend on, checked against
    // a value this suite already derives by hand: the v0 EMDF test's 11
    // continuations of the maximal 2-bit chunk, then a final one. Each round
    // is 3 bits, and the value comes back as the substream_index of the
    // simplest object-coded substream once the helper is swapped in for the
    // index's own escape.
    BitWriter by_hand;
    for (int round = 0; round < 11; ++round) {
        by_hand.put(0b11, 2);
        by_hand.put(1, 1);
    }
    by_hand.put(0b11, 2);
    by_hand.put(0, 1);
    BitWriter helper;
    put_variable_bits(helper, 2, 22'369'619);
    CHECK(helper.bytes() == by_hand.bytes());

    for (const std::uint32_t value : {0u, 3u, 4u, 19u, 20u, 1000u, 22'369'619u, 0xFFFF'FFFFu}) {
        CAPTURE(value);
        const auto frame = parse_wrapped_object_coded_group([value](BitWriter& w) {
            w.put(0, 1);  // b_oamd_substream = 0
            w.put(0, 1);  // b_ajoc = 0 -> ac4_substream_info_obj()
            w.put(0, 3);  // n_objects_code (unused)
            w.put(0, 1);  // b_dynamic_objects
            w.put(0, 1);  // b_bed_objects
            w.put(0, 1);  // b_isf
            w.put(0, 4);  // res_bytes = 0
            w.put(0, 1);  // b_bitrate_info
            w.put(0, 1);  // b_audio_ndot
            w.put(3, 2);  // substream_index = 3 -> += variable_bits(2)
            put_variable_bits(w, 2, value);
        });
        REQUIRE(frame.toc.substream_groups[0].substreams[0].obj.has_value());
        REQUIRE(frame.toc.substream_groups[0].substreams[0].obj->substream_index.has_value());
        CHECK(static_cast<std::uint32_t>(
                  *frame.toc.substream_groups[0].substreams[0].obj->substream_index) ==
              3u + value);
    }
}

// Regression: presentation_config_ext_info() skipped `8 * n_skip_bytes` bits
// with n_skip_bytes converted to int, and n_skip_bytes escapes through
// variable_bits() to 2^32. Found by fuzz/fuzz_ac4_parse.cpp as a UBSan
// signed-overflow report once ac4_objects was built with sanitizers. The
// count here is 2^29 + 1 bytes, whose 8x product (2^32 + 8) overflows int.
// One byte and every field the rest of a v0 TOC needs follow it, so the
// frame parses if that product is taken as the 8 bits it wraps to; read as
// the count that was sent, it runs 2^29 bytes past a frame of a few.
TEST_CASE("parse_raw_frame: presentation_config_ext_info's skip count runs past the frame",
          "[ac4]") {
    BitWriter w;
    w.put(0, 2);   // bitstream_version = 0 (the v0 TOC path)
    w.put(0, 10);  // sequence_counter
    w.put(0, 1);   // b_wait_frames
    w.put(0, 1);   // fs_index = 0
    w.put(5, 4);   // frame_rate_index = 5 (frame_rate_multiply_info reads 0 bits)
    w.put(0, 1);   // b_iframe_global
    w.put(1, 1);   // b_single_presentation -> n_presentations = 1
    w.put(0, 1);   // b_payload_base = 0
    // parse_presentation_info_v0():
    w.put(0, 1);  // b_single_substream = 0
    w.put(7, 3);  // presentation_config = 7 -> += variable_bits(2)
    put_variable_bits(w, 2, 0);
    w.put(0, 1);  // presentation_version terminator
    w.put(0, 3);  // md_compat
    w.put(0, 1);  // b_belongs_to_presentation_id = 0
    // emdf_info(): version(2), key_id(3), b_payloads_substream_info(1),
    // emdf_reserved primary(2)/secondary(2), all zero.
    w.put(0, 10);
    w.put(0, 1);  // b_hsf_ext
    // presentation_config 7 is not 0-5 -> presentation_config_ext_info():
    w.put(1, 5);  // n_skip_bytes = 1
    w.put(1, 1);  // b_more_skip_bytes -> += variable_bits(2) << 5
    put_variable_bits(w, 2, 1u << 24);  // n_skip_bytes = 2^29 + 1
    w.put(0, 8);  // the byte a wrapped count of 8 bits would skip
    w.put(0, 1);  // b_pre_virtualized
    w.put(0, 1);  // b_add_emdf_substreams = 0
    write_ac4_single_empty_substream_index_table(w);

    const auto data = w.bytes();
    const auto start = std::chrono::steady_clock::now();
    const auto result = ac4::parse_raw_frame(data);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac4::Error::kTruncated);
    // Skipping 2^32 bits one at a time takes seconds; moving the position
    // does not. Same generous bound as the runaway-count tests above.
    CHECK(elapsed < std::chrono::seconds(2));
}

// Regression: parse_raw_frame() bounded each substream with
// `offset + size > frame size` on size_t, reading payload_base and
// substream_size[] back from their int fields - so a size above INT_MAX
// sign-extended to nearly 2^64. A payload_base of 1032 bytes past a frame of
// a few, plus a size of 2^32 - 1032, wrapped that sum back to the TOC's own
// length: the check passed, and the audio substream's header was read from a
// subspan starting 1032 bytes past the end of the data. Found reading the
// code while fixing the overflow above.
TEST_CASE("parse_raw_frame: a payload_base and substream size that wrap are truncated", "[ac4]") {
    BitWriter w;
    w.put(2, 2);   // bitstream_version = 2
    w.put(0, 10);  // sequence_counter
    w.put(0, 1);   // b_wait_frames
    w.put(0, 1);   // fs_index = 0
    w.put(5, 4);   // frame_rate_index = 5
    w.put(0, 1);   // b_iframe_global
    w.put(1, 1);   // b_single_presentation -> n_presentations = 1
    w.put(1, 1);   // b_payload_base = 1
    w.put(31, 5);  // payload_base_minus1 = 31 -> 32 -> += variable_bits(3)
    put_variable_bits(w, 3, 1000);  // payload_base = 1032
    w.put(0, 1);  // b_program_id = 0
    // ac4_presentation_v1_info(), as write_ac4_object_coded_preamble() writes it:
    w.put(1, 1);   // b_single_substream_group = 1
    w.put(0, 1);   // presentation_version terminator
    w.put(0, 3);   // md_compat
    w.put(0, 1);   // b_presentation_id = 0
    w.put(0, 1);   // frame_rate_fractions_info
    w.put(0, 10);  // emdf_info(), all zero
    w.put(0, 1);   // b_presentation_filter = 0
    w.put(0, 3);   // ac4_sgi_specifier(): group_index = 0
    w.put(0, 1);   // b_pre_virtualized
    w.put(0, 1);   // b_add_emdf_substreams = 0
    w.put(0, 1);   // b_alternative
    w.put(0, 1);   // b_pres_ndot
    w.put(0, 2);   // ac4_presentation_substream_info()'s substream_index_ref
    write_ac4_object_coded_group_preamble(w);
    w.put(0, 1);  // b_oamd_substream = 0
    w.put(0, 1);  // b_ajoc = 0 -> ac4_substream_info_obj()
    w.put(0, 3);  // n_objects_code = 0
    w.put(1, 1);  // b_dynamic_objects
    w.put(0, 1);  // b_lfe
    w.put(0, 1);  // b_bitrate_info
    w.put(0, 1);  // b_audio_ndot
    w.put(0, 2);  // substream_index = 0 -> substream 0 is audio
    w.put(0, 1);  // b_content_type = 0
    // substream_index_table(): one substream of 2^32 - 1032 bytes.
    w.put(1, 2);                          // n_substreams = 1
    w.put(1, 1);                          // b_size_present
    w.put(1, 1);                          // b_more_bits
    w.put(0x3F8, 10);                     // substream_size low bits
    put_variable_bits(w, 2, 0x3F'FFFE);  // << 10 -> 0xFFFFF800 + 0x3F8 = 2^32 - 1032
    auto data = w.bytes();
    data.resize(data.size() + 4, std::byte{0});

    const auto result = ac4::parse_raw_frame(data);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac4::Error::kTruncated);
}

TEST_CASE("describe returns a distinct, non-empty string for every Error", "[ac4]") {
    for (const auto error :
         {ac4::Error::kTruncated, ac4::Error::kLostSync, ac4::Error::kUnsupportedBitstreamVersion}) {
        CAPTURE(static_cast<int>(error));
        CHECK_FALSE(ac4::describe(error).empty());
    }
}

// --------------------------------------------------------------------------
// Carriage helpers (AC-4 bitstream inspector): the 'dac4' box, per-frame timing and the
// RFC 6381 string, all against the real DEE fixture's own parsed TOC.

TEST_CASE("build_dac4 writes the dac4 DEE's MP4 muxer writes for DEE's streams", "[ac4][carriage]") {
    // What dee_mp4muxer (DEE 6.5.4) writes in the 'dac4' box when it muxes
    // each committed stream: ac4_dsi_v1 with the average bit rate mode
    // DEE's wait_frames imply, and one presentation of one channel-coded
    // substream, described in full, dialogue enhancement indicated.
    struct Leg {
        const char* name;
        const char* dac4;
    };
    const std::vector<Leg> legs = {
        {"ac4-stereo-64", "20ba01400000001fffffffe0010ff88000004200000250100000030080"},
        {"ac4-20-music-192", "20ba01400000001fffffffe0010ff88000004200000250100000030080"},
        {"ac4-20-tones-192", "20ba01400000001fffffffe0010ff88000004200000250100000030080"},
        {"ac4-51-music-384", "20ba01400000001fffffffe0010ff98000004800008e501000008f0080"},
        {"ac4-51-film-96", "20ba01400000001fffffffe0010ff98000004800008e501000008f0080"},
        {"ac4-51-drc-ltrt-192", "20ba01400000001fffffffe0010ff98000004800008e501000008f0080"},
    };
    const auto hex = [](const std::vector<std::byte>& bytes) {
        std::string out;
        for (const std::byte b : bytes) {
            constexpr std::string_view kDigits = "0123456789abcdef";
            out += kDigits[std::to_integer<unsigned>(b) >> 4U];
            out += kDigits[std::to_integer<unsigned>(b) & 15U];
        }
        return out;
    };
    for (const Leg& leg : legs) {
        CAPTURE(leg.name);
        const auto data =
            read_file(std::filesystem::path{AC3FORGE_GOLDEN_EXTERNAL_BASELINE_DIR} / leg.name / "dee.ac4");
        const auto scanned = ac4::scan(data);
        REQUIRE_FALSE(scanned.frames.empty());
        auto frame = ac4::parse_raw_frame(scanned.frames.front().raw_ac4_frame);
        REQUIRE(frame.has_value());
        REQUIRE(frame->toc.presentations_v1.size() == 1);

        // The indicators are no part of the table of contents: unset, the
        // DSI closes before them, one byte short of the muxer's.
        const std::string without = hex(ac4::build_dac4(frame->toc));
        const std::string expected = leg.dac4;
        CHECK(without.size() == expected.size() - 2);
        CHECK(without.substr(0, 26) == expected.substr(0, 26));
        CHECK(without.substr(26, 2) == "0e");  // pres_bytes, 15 less the byte
        CHECK(without.substr(28) == expected.substr(28, without.size() - 28));

        frame->toc.presentations_v1[0].de_indicator = true;
        frame->toc.presentations_v1[0].immersive_audio_indicator = false;
        CHECK(hex(ac4::build_dac4(frame->toc)) == expected);
    }
}

namespace {

// Part 2 Annex E read back for the tests of build_dac4() below: ac4_dsi_v1()
// (E.6), ac4_bitrate_dsi() (E.7), and for a presentation of one channel-coded
// substream group ac4_presentation_v1_dsi() (E.10) and its
// ac4_substream_group_dsi() (E.11), transcribed from the annex apart from the
// writer.
class DsiBits {
   public:
    explicit DsiBits(const std::vector<std::byte>& bytes) : bytes_(bytes) {}

    std::uint32_t read(int n) {
        std::uint32_t value = 0;
        for (int i = 0; i < n; ++i) {
            REQUIRE(pos_ / 8 < bytes_.size());
            const unsigned byte = std::to_integer<unsigned>(bytes_[pos_ / 8]);
            value = (value << 1U) | ((byte >> (7U - static_cast<unsigned>(pos_ % 8))) & 1U);
            ++pos_;
        }
        return value;
    }
    bool flag() { return read(1) != 0; }
    void align() { pos_ = (pos_ + 7) / 8 * 8; }
    [[nodiscard]] std::size_t bit() const { return pos_; }

   private:
    const std::vector<std::byte>& bytes_;
    std::size_t pos_ = 0;
};

struct BitrateDsi {
    std::uint32_t mode = 0;
    std::uint32_t rate = 0;
    std::uint32_t precision = 0;
};

BitrateDsi read_bitrate(DsiBits& r) {
    BitrateDsi out;
    out.mode = r.read(2);
    out.rate = r.read(32);
    out.precision = r.read(32);
    return out;
}

struct PresentationDsi {
    std::uint32_t md_compat = 0;
    std::optional<std::uint32_t> presentation_id;
    std::uint32_t multiply = 0;
    std::uint32_t fraction = 0;
    std::uint32_t emdf_version = 0;
    std::uint32_t key_id = 0;
    std::uint32_t ch_mode = 0;
    std::optional<bool> four_back;
    std::optional<std::uint32_t> top_pairs;
    std::uint32_t groups = 0;
    std::optional<std::uint32_t> core;
    std::optional<bool> enable;
    // ac4_substream_group_dsi()
    bool substreams_present = false;
    bool hsf_ext = false;
    std::uint32_t sf_multiplier = 0;
    std::optional<std::uint32_t> bitrate_indicator;
    std::uint32_t substream_groups = 0;
    std::optional<std::uint32_t> content_classifier;
    std::optional<std::vector<std::byte>> language;
    bool pre_virtualized = false;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> add_emdf;  // version, key_id
    std::optional<BitrateDsi> bitrate;
    std::optional<bool> de_indicator;
    std::optional<bool> immersive_audio;
    std::optional<std::uint32_t> extended_id;
};

// ac4_presentation_v1_dsi(pres_bytes) for presentation_config_v1 0x1f, one
// channel-coded substream group of one substream.
PresentationDsi read_presentation(DsiBits& r, std::size_t pres_bytes) {
    const std::size_t start = r.bit();
    PresentationDsi p;
    REQUIRE(r.read(5) == 0x1FU);  // presentation_config_v1
    p.md_compat = r.read(3);
    if (r.flag()) {  // b_presentation_id
        p.presentation_id = r.read(5);
    }
    p.multiply = r.read(2);
    p.fraction = r.read(2);
    p.emdf_version = r.read(5);
    p.key_id = r.read(10);
    REQUIRE(r.flag());  // b_presentation_channel_coded
    p.ch_mode = r.read(5);
    if (p.ch_mode >= 11 && p.ch_mode <= 14) {
        p.four_back = r.flag();
        p.top_pairs = r.read(2);
    }
    CHECK(r.read(6) == 0U);  // reserved_zero
    p.groups = r.read(18);
    if (r.flag() && r.flag()) {  // b_presentation_core_differs, b_presentation_core_channel_coded
        p.core = r.read(2);
    }
    if (r.flag()) {  // b_presentation_filter
        p.enable = r.flag();
        const std::uint32_t n_filter_bytes = r.read(8);
        for (std::uint32_t i = 0; i < n_filter_bytes; ++i) {
            r.read(8);
        }
    }
    p.substreams_present = r.flag();
    p.hsf_ext = r.flag();
    REQUIRE(r.flag());         // b_channel_coded
    REQUIRE(r.read(8) == 1U);  // n_substreams
    p.sf_multiplier = r.read(2);
    if (r.flag()) {  // b_substream_bitrate_indicator
        p.bitrate_indicator = r.read(5);
    }
    CHECK(r.read(6) == 0U);  // reserved_zero
    p.substream_groups = r.read(18);
    if (r.flag()) {  // b_content_type
        p.content_classifier = r.read(3);
        if (r.flag()) {  // b_language_indicator
            std::vector<std::byte> tag(r.read(6));
            for (std::byte& b : tag) {
                b = static_cast<std::byte>(r.read(8));
            }
            p.language = tag;
        }
    }
    p.pre_virtualized = r.flag();
    if (r.flag()) {  // b_add_emdf_substreams
        const std::uint32_t n = r.read(7);
        for (std::uint32_t j = 0; j < n; ++j) {
            const std::uint32_t version = r.read(5);
            p.add_emdf.emplace_back(version, r.read(10));
        }
    }
    if (r.flag()) {  // b_presentation_bitrate_info
        p.bitrate = read_bitrate(r);
    }
    REQUIRE_FALSE(r.flag());  // b_alternative
    r.align();
    if (r.bit() - start <= (pres_bytes - 1) * 8) {
        p.de_indicator = r.flag();
        p.immersive_audio = r.flag();
        r.read(4);       // reserved
        if (r.flag()) {  // b_extended_presentation_id
            p.extended_id = r.read(9);
        } else {
            r.read(1);  // reserved
        }
    }
    CHECK(r.bit() - start == pres_bytes * 8);
    return p;
}

struct Dac4 {
    std::uint32_t bitstream_version = 0;
    std::uint32_t fs_index = 0;
    std::uint32_t frame_rate_index = 0;
    BitrateDsi bitrate;
    std::vector<std::uint32_t> versions;
    std::vector<std::size_t> sizes;  // pres_bytes
    std::vector<std::optional<PresentationDsi>> presentations;
};

// ac4_dsi_v1(), with each presentation's body read where it has one.
Dac4 read_dac4(const std::vector<std::byte>& bytes) {
    DsiBits r(bytes);
    Dac4 out;
    REQUIRE(r.read(3) == 1U);  // ac4_dsi_version
    out.bitstream_version = r.read(7);
    out.fs_index = r.read(1);
    out.frame_rate_index = r.read(4);
    const std::uint32_t n_presentations = r.read(9);
    if (out.bitstream_version > 1) {
        REQUIRE_FALSE(r.flag());  // b_program_id
    }
    out.bitrate = read_bitrate(r);
    r.align();
    for (std::uint32_t i = 0; i < n_presentations; ++i) {
        out.versions.push_back(r.read(8));
        std::size_t pres_bytes = r.read(8);
        if (pres_bytes == 255) {
            pres_bytes += r.read(16);  // add_pres_bytes
        }
        out.sizes.push_back(pres_bytes);
        if (pres_bytes > 0) {
            out.presentations.emplace_back(read_presentation(r, pres_bytes));
        } else {
            out.presentations.emplace_back(std::nullopt);
        }
    }
    CHECK(r.bit() == bytes.size() * 8);
    return out;
}

// A table of contents with one presentation of one substream group of one
// channel-coded substream in `ch_mode`, at 48 kHz and frame_rate_index 13.
ac4::Toc one_substream_toc(int ch_mode) {
    ac4::Toc toc;
    toc.bitstream_version = 2;
    toc.sample_rate_hz = 48000;
    toc.frame_rate_index = 13;
    toc.wait_frames = 0;
    toc.n_presentations = 1;
    ac4::PresentationInfoV1 pres;
    pres.presentation_version = 1;
    pres.group_refs = {0};
    toc.presentations_v1.push_back(pres);
    ac4::ChannelSubstreamInfo chan;
    chan.ch_mode = ch_mode;
    ac4::GroupSubstream substream;
    substream.chan = chan;
    ac4::SubstreamGroupInfo group;
    group.substreams.push_back(substream);
    toc.substream_groups.push_back(group);
    return toc;
}

std::uint32_t groups_of(std::initializer_list<int> groups) {
    std::uint32_t mask = 0;
    for (const int g : groups) {
        mask |= 1U << static_cast<unsigned>(g);
    }
    return mask;
}

PresentationDsi presentation_of(const ac4::Toc& toc) {
    const Dac4 dac4 = read_dac4(ac4::build_dac4(toc));
    REQUIRE(dac4.presentations.size() == 1);
    REQUIRE(dac4.presentations.front().has_value());
    return *dac4.presentations.front();
}

}  // namespace

TEST_CASE("build_dac4 gives each channel mode the channel groups Table A.27 lists",
          "[ac4][carriage]") {
    // Table A.27's layouts by Table A.28's channel modes, in both channel
    // group arrays: L/R 0, C 1, Ls/Rs 2, Lb/Rb 3, Tfl/Tfr 4, the LFE 6, Lw/Rw 17.
    struct Mode {
        int ch_mode;
        std::uint32_t groups;
    };
    const std::vector<Mode> modes = {
        {0, groups_of({1})},
        {1, groups_of({0})},
        {2, groups_of({0, 1})},
        {3, groups_of({0, 1, 2})},
        {4, groups_of({0, 1, 2, 6})},
        {5, groups_of({0, 1, 2, 3})},
        {6, groups_of({0, 1, 2, 3, 6})},
        {7, groups_of({0, 1, 2, 17})},
        {8, groups_of({0, 1, 2, 6, 17})},
        {9, groups_of({0, 1, 2, 4})},
        {10, groups_of({0, 1, 2, 4, 6})},
        // 22.2: every group but the 9.X layouts' Lscr/Rscr (16) and the
        // reserved 8.
        {15, groups_of({0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 17})},
    };
    for (const Mode& m : modes) {
        CAPTURE(m.ch_mode);
        const PresentationDsi p = presentation_of(one_substream_toc(m.ch_mode));
        CHECK(p.ch_mode == static_cast<std::uint32_t>(m.ch_mode));
        CHECK(p.groups == m.groups);
        CHECK(p.substream_groups == m.groups);
        CHECK_FALSE(p.four_back.has_value());
        CHECK_FALSE(p.core.has_value());
    }

    // 5.X.x and 7.X.x (11 and 12) and 9.X.x (13 and 14): C where the source
    // has it, Lb/Rb with the four back channels, Tsl/Tsr (7) for one top pair
    // and Tfl/Tfr with Tbl/Tbr (4 and 5) for two, and the 9.X layouts'
    // Lscr/Rscr (16). The core is 5.0.2's or 5.1.2's, Table E.14's 2 and 3.
    for (int ch_mode = 11; ch_mode <= 14; ++ch_mode) {
        for (const bool centre : {false, true}) {
            for (const bool back : {false, true}) {
                for (int top = 0; top <= 3; ++top) {
                    CAPTURE(ch_mode, centre, back, top);
                    ac4::Toc toc = one_substream_toc(ch_mode);
                    toc.substream_groups[0].substreams[0].chan->original_content =
                        ac4::OriginalContent{.b_4_back_channels_present = back,
                                             .b_centre_present = centre,
                                             .top_channels_present = top};
                    const std::uint32_t pairs = top == 0 ? 0U : (top == 3 ? 2U : 1U);
                    std::uint32_t expected = groups_of({0, 2});
                    expected |= centre ? groups_of({1}) : 0U;
                    expected |= back ? groups_of({3}) : 0U;
                    expected |= pairs == 1 ? groups_of({7}) : (pairs == 2 ? groups_of({4, 5}) : 0U);
                    expected |= ch_mode >= 13 ? groups_of({16}) : 0U;
                    expected |= ch_mode % 2 == 0 ? groups_of({6}) : 0U;
                    const PresentationDsi p = presentation_of(toc);
                    CHECK(p.groups == expected);
                    CHECK(p.substream_groups == expected);
                    CHECK(p.four_back == back);
                    CHECK(p.top_pairs == pairs);
                    CHECK(p.core == (ch_mode % 2 == 0 ? 3U : 2U));
                }
            }
        }
    }
}

TEST_CASE(
    "build_dac4's bit rate DSI follows wait_frames, and carries the substream's rate indicator",
    "[ac4][carriage]") {
    // Table E.7: constant with wait_frames 0, average with 1 to 6, variable
    // otherwise; the rate itself unknown.
    struct Wait {
        std::optional<int> wait_frames;
        std::uint32_t mode;
    };
    for (const Wait w : {Wait{0, 1}, Wait{1, 2}, Wait{6, 2}, Wait{7, 3}, Wait{std::nullopt, 3}}) {
        CAPTURE(w.wait_frames.value_or(-1));
        ac4::Toc toc = one_substream_toc(1);
        toc.wait_frames = w.wait_frames;
        toc.substream_groups[0].substreams[0].chan->brate_ind = 13;
        const Dac4 dac4 = read_dac4(ac4::build_dac4(toc));
        CHECK(dac4.bitrate.mode == w.mode);
        CHECK(dac4.bitrate.rate == 0);
        CHECK(dac4.bitrate.precision == 0xFFFFFFFFU);
        REQUIRE(dac4.presentations.front().has_value());
        const PresentationDsi& p = *dac4.presentations.front();
        CHECK(p.bitrate_indicator == 13U);
        REQUIRE(p.bitrate.has_value());
        CHECK(p.bitrate->mode == w.mode);
    }
    // Without the substream's rate indicator, neither is sent.
    const PresentationDsi p = presentation_of(one_substream_toc(1));
    CHECK_FALSE(p.bitrate_indicator.has_value());
    CHECK_FALSE(p.bitrate.has_value());
}

TEST_CASE("build_dac4 carries a presentation's identity, filter, EMDF, content type and indicators",
          "[ac4][carriage]") {
    ac4::Toc toc = one_substream_toc(4);
    ac4::PresentationInfoV1& pres = toc.presentations_v1[0];
    pres.md_compat = 3;
    pres.presentation_id = 17;
    pres.emdf = {.emdf_version = 5, .key_id = 700};
    pres.enable_presentation = false;
    pres.b_pre_virtualized = true;
    pres.b_add_emdf_substreams = true;
    pres.add_emdf = {{.emdf_version = 1, .key_id = 2}, {.emdf_version = 31, .key_id = 1023}};
    pres.de_indicator = true;
    pres.immersive_audio_indicator = false;
    ac4::SubstreamGroupInfo& group = toc.substream_groups[0];
    group.b_substreams_present = true;
    const std::vector<std::byte> english = {std::byte{'e'}, std::byte{'n'}, std::byte{'g'}};
    group.content_type = ac4::ContentType{.content_classifier = 2, .language_tag = english};
    group.substreams[0].hsf_ext_substream_index = 3;
    group.substreams[0].chan->sf_multiplier = 1;

    PresentationDsi p = presentation_of(toc);
    CHECK(p.md_compat == 3U);
    CHECK(p.presentation_id == 17U);
    CHECK(p.emdf_version == 5U);
    CHECK(p.key_id == 700U);
    CHECK(p.enable == false);
    CHECK(p.pre_virtualized);
    CHECK(p.add_emdf == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{1, 2}, {31, 1023}});
    CHECK(p.substreams_present);
    CHECK(p.hsf_ext);
    CHECK(p.sf_multiplier == 2U);  // sf_multiplier 1: 192 kHz
    CHECK(p.content_classifier == 2U);
    CHECK(p.language == english);
    CHECK(p.de_indicator == true);
    CHECK(p.immersive_audio == false);
    CHECK_FALSE(p.extended_id.has_value());

    // A content type with no language, an enabled filter, and an id past
    // presentation_id's five bits, which the extended id carries whole.
    group.content_type = ac4::ContentType{.content_classifier = 7, .language_tag = std::nullopt};
    pres.enable_presentation = true;
    pres.presentation_id = 300;
    p = presentation_of(toc);
    CHECK(p.content_classifier == 7U);
    CHECK_FALSE(p.language.has_value());
    CHECK(p.enable == true);
    CHECK(p.presentation_id == 300U % 32U);
    CHECK(p.extended_id == 300U);
}

TEST_CASE(
    "build_dac4 codes the frame rate factor and fraction where Tables E.12 and E.13 have them",
    "[ac4][carriage]") {
    struct Rate {
        int index;
        int factor;
        int fraction;
        std::uint32_t multiply;
        std::uint32_t fraction_code;
    };
    const std::vector<Rate> rates = {
        {0, 2, 1, 1, 0}, {1, 4, 1, 2, 0}, {2, 1, 1, 0, 0},  {4, 4, 1, 2, 0},  {5, 1, 2, 0, 1},
        {7, 2, 4, 1, 2}, {9, 4, 2, 2, 1}, {12, 1, 4, 0, 2}, {13, 2, 2, 0, 0},
    };
    for (const Rate& rate : rates) {
        CAPTURE(rate.index, rate.factor, rate.fraction);
        ac4::Toc toc = one_substream_toc(1);
        toc.frame_rate_index = rate.index;
        toc.presentations_v1[0].frame_rate_factor = rate.factor;
        toc.presentations_v1[0].frame_rate_fraction = rate.fraction;
        const Dac4 dac4 = read_dac4(ac4::build_dac4(toc));
        CHECK(dac4.frame_rate_index == static_cast<std::uint32_t>(rate.index));
        REQUIRE(dac4.presentations.front().has_value());
        CHECK(dac4.presentations.front()->multiply == rate.multiply);
        CHECK(dac4.presentations.front()->fraction == rate.fraction_code);
    }
}

TEST_CASE("build_dac4 sends a presentation of 255 bytes or more with add_pres_bytes",
          "[ac4][carriage]") {
    ac4::Toc toc = one_substream_toc(1);
    ac4::PresentationInfoV1& pres = toc.presentations_v1[0];
    pres.b_add_emdf_substreams = true;
    for (int j = 0; j < 127; ++j) {
        pres.add_emdf.push_back({.emdf_version = j % 32, .key_id = j * 8});
    }
    toc.substream_groups[0].content_type = ac4::ContentType{
        .content_classifier = 1, .language_tag = std::vector<std::byte>(63, std::byte{'a'})};
    const Dac4 dac4 = read_dac4(ac4::build_dac4(toc));
    REQUIRE(dac4.sizes.size() == 1);
    CHECK(dac4.sizes.front() > 255U);
    REQUIRE(dac4.presentations.front().has_value());
    const PresentationDsi& p = *dac4.presentations.front();
    REQUIRE(p.add_emdf.size() == 127);
    CHECK(p.add_emdf.back() == std::pair<std::uint32_t, std::uint32_t>{126 % 32, 126 * 8});
    CHECK(p.language == std::vector<std::byte>(63, std::byte{'a'}));
}

TEST_CASE("build_dac4 leaves a presentation it cannot describe whole without a body",
          "[ac4][carriage]") {
    const auto size_of = [](const ac4::Toc& toc) {
        const Dac4 dac4 = read_dac4(ac4::build_dac4(toc));
        REQUIRE(dac4.sizes.size() == 1);
        return dac4.sizes.front();
    };
    REQUIRE(size_of(one_substream_toc(1)) > 0U);
    const std::vector<std::pair<const char*, std::function<void(ac4::Toc&)>>> cases = {
        {"a presentation_config",
         [](ac4::Toc& t) { t.presentations_v1[0].presentation_config = 1; }},
        {"two substream groups", [](ac4::Toc& t) { t.presentations_v1[0].group_refs = {0, 0}; }},
        {"an alternative", [](ac4::Toc& t) { t.presentations_v1[0].b_alternative = true; }},
        {"a group the TOC lacks", [](ac4::Toc& t) { t.presentations_v1[0].group_refs = {5}; }},
        {"an object-coded group",
         [](ac4::Toc& t) { t.substream_groups[0].b_channel_coded = false; }},
        {"two substreams",
         [](ac4::Toc& t) {
             t.substream_groups[0].substreams.push_back(t.substream_groups[0].substreams[0]);
         }},
        {"no channel substream",
         [](ac4::Toc& t) { t.substream_groups[0].substreams[0].chan.reset(); }},
        {"a reserved channel mode",
         [](ac4::Toc& t) { t.substream_groups[0].substreams[0].chan->ch_mode.reset(); }},
        {"an EMDF version past 5 bits",
         [](ac4::Toc& t) { t.presentations_v1[0].emdf.emdf_version = 32; }},
        {"a key_id past 10 bits", [](ac4::Toc& t) { t.presentations_v1[0].emdf.key_id = 1024; }},
        {"an id past 5 bits with no indicators",
         [](ac4::Toc& t) { t.presentations_v1[0].presentation_id = 40; }},
    };
    for (const auto& [name, change] : cases) {
        CAPTURE(name);
        ac4::Toc toc = one_substream_toc(1);
        change(toc);
        CHECK(size_of(toc) == 0U);
    }

    // A version 0 presentation, and one the TOC does not describe, take no
    // body either.
    ac4::Toc legacy;
    legacy.bitstream_version = 1;
    legacy.frame_rate_index = 13;
    legacy.n_presentations = 2;
    legacy.presentations_v0.push_back(ac4::PresentationInfoV0{});
    const Dac4 dac4 = read_dac4(ac4::build_dac4(legacy));
    CHECK(dac4.bitstream_version == 1U);
    CHECK(dac4.versions == std::vector<std::uint32_t>{0, 0});
    CHECK(dac4.sizes == std::vector<std::size_t>{0, 0});
}

TEST_CASE("samples_per_frame follows Table 84, refusing the alternating rates",
          "[ac4][carriage]") {
    ac4::Toc toc;
    toc.sample_rate_hz = 48000;
    const std::array<std::optional<std::uint32_t>, 14> expected{{
        2002, 2000, 1920, std::nullopt, 1600, 1001, 1000, 960,
        std::nullopt, 800, 480, std::nullopt, 400, 2048,
    }};
    for (int index = 0; index < static_cast<int>(expected.size()); ++index) {
        toc.frame_rate_index = index;
        CAPTURE(index);
        CHECK(ac4::samples_per_frame(toc) == expected[static_cast<std::size_t>(index)]);
    }
    // 44,1 kHz: Table 83 defines only the sample-rate-locked 2048 frame.
    toc.sample_rate_hz = 44100;
    toc.frame_rate_index = 13;
    CHECK(ac4::samples_per_frame(toc) == std::optional<std::uint32_t>{2048});
    toc.frame_rate_index = 0;
    CHECK_FALSE(ac4::samples_per_frame(toc).has_value());
}

TEST_CASE("rfc6381_codec_string renders Annex E.13's dotted hex fields", "[ac4][carriage]") {
    const auto data = read_file(fixture_path());
    const auto scanned = ac4::scan(data);
    REQUIRE_FALSE(scanned.frames.empty());
    const auto frame = ac4::parse_raw_frame(scanned.frames.front().raw_ac4_frame);
    REQUIRE(frame.has_value());
    // bitstream_version 2, presentation_version 1, md_compat 0 on this DEE
    // encode - cross-checked against the probe table docs/verification.md
    // records for the same fixture.
    CHECK(ac4::rfc6381_codec_string(frame->toc) == "ac-4.02.01.00");
}
