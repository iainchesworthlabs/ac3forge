#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
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
    void put(std::uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            bits_.push_back(((value >> i) & 1u) != 0);
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

}  // namespace

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

TEST_CASE("parse_substream_info_ajoc refuses b_oamd_common_data_present", "[ac4]") {
    // §6.2.8.1's oamd_common_data() is out of scope (see ac4.hpp's module
    // docs) - proves the refusal is reached cleanly, mid-substream-group,
    // rather than silently misparsing the bits that follow.
    BitWriter w;
    write_ac4_object_coded_preamble(w);
    write_ac4_object_coded_group_preamble(w);
    w.put(0, 1);  // b_oamd_substream = 0
    w.put(1, 1);  // b_ajoc = 1
    w.put(0, 1);  // b_lfe
    w.put(1, 1);  // b_static_dmx (skip dmx assignment to keep this short)
    w.put(1, 1);  // b_oamd_common_data_present -> refusal happens right here
    const auto data = w.bytes();

    const auto result = ac4::parse_raw_frame(data);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac4::Error::kOamdCommonDataPresent);
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

TEST_CASE("describe returns a distinct, non-empty string for every Error", "[ac4]") {
    for (const auto error :
         {ac4::Error::kTruncated, ac4::Error::kLostSync, ac4::Error::kUnsupportedBitstreamVersion,
          ac4::Error::kOamdCommonDataPresent}) {
        CAPTURE(static_cast<int>(error));
        CHECK_FALSE(ac4::describe(error).empty());
    }
}
