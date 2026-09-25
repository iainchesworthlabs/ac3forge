// The inspector's table of contents (src/ac4/src/ac4.cpp) on hand-built
// frames, for the syntax branches no committed stream and no other case in
// tests/ac4 reaches: every channel_mode code of both parts with its optional
// fields, the TOC-level escapes (wait frames, payload base, program id,
// presentation counts and configurations, EMDF info), frame rate multiply and
// fraction info, content types with language tags, bitstream_version 0 and 1
// presentations of every shape, object and A-JOC substream infos with each
// bed assignment, oamd_common_data()'s nested elements, the sync frame's
// extended size, and the carriage helpers on a version 0 table of contents.
// The frames come from tests/ac4/ac4_toc_writer.hpp, which shares no code
// with src/ac4.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4/ac4_toc_writer.hpp"

namespace {

using ac4_toc_test::BitWriter;
using ac4_toc_test::ChanInfo;
using ac4_toc_test::PresV1;
using ac4_toc_test::TocStart;

// The table of contents in `w`, a substream_index_table() of `n` substreams
// of `size` bytes, and those substreams as zeros.
std::vector<std::byte> with_substreams(BitWriter w, int n, std::size_t size = 3, std::size_t payload_base = 0) {
    ac4_toc_test::index_table(w, std::vector<std::size_t>(static_cast<std::size_t>(n), size));
    w.align();
    std::vector<std::byte> frame = w.bytes();
    frame.resize(frame.size() + payload_base + static_cast<std::size_t>(n) * size, std::byte{0});
    return frame;
}

ac4::RawFrame parse(const std::vector<std::byte>& frame) {
    auto result = ac4::parse_raw_frame(frame);
    REQUIRE(result.has_value());
    return std::move(*result);
}

// A content_type() with a language tag: serialized (one chunk) or as bytes.
void content_type_with_language(BitWriter& w, int classifier, bool serialized) {
    w.put(static_cast<std::uint64_t>(classifier), 3);
    w.flag(true);    // b_language_indicator
    w.flag(serialized);
    if (serialized) {
        w.flag(true);           // b_start_tag
        w.put(0x656E, 16);      // language_tag_chunk
    } else {
        w.put(2, 6);            // n_language_tag_bytes
        w.put('e', 8);
        w.put('n', 8);
    }
}

}  // namespace

TEST_CASE("describe names an Error value outside the enumeration as unknown", "[ac4][toc]") {
    CHECK(ac4::describe(static_cast<ac4::Error>(42)) == "unknown ac4::Error");
}

TEST_CASE("scan reads a sync frame's extended frame_size and refuses short ones", "[ac4][toc]") {
    SECTION("frame_size 0xFFFF then 24 bits") {
        std::vector<std::byte> data = {std::byte{0xAC}, std::byte{0x40}, std::byte{0xFF}, std::byte{0xFF},
                                       std::byte{0x00}, std::byte{0x00}, std::byte{0x05}};
        data.resize(data.size() + 5, std::byte{0x11});
        const auto result = ac4::scan(data);
        CHECK_FALSE(result.stopped_at.has_value());
        REQUIRE(result.frames.size() == 1);
        CHECK(result.frames[0].raw_ac4_frame.size() == 5);
        CHECK(result.frames[0].raw_ac4_frame.data() == data.data() + 7);
        CHECK_FALSE(result.frames[0].crc_ok.has_value());
    }
    SECTION("an extended header cut short") {
        const std::vector<std::byte> data = {std::byte{0xAC}, std::byte{0x40}, std::byte{0xFF}, std::byte{0xFF},
                                             std::byte{0x00}};
        const auto result = ac4::scan(data);
        CHECK(result.stopped_at == ac4::Error::kTruncated);
        CHECK(result.stopped_at_offset == 0);
    }
    SECTION("a frame longer than the data") {
        std::vector<std::byte> data = {std::byte{0xAC}, std::byte{0x41}, std::byte{0x00}, std::byte{0x10}};
        data.resize(12, std::byte{0});
        const auto result = ac4::scan(data);
        CHECK(result.stopped_at == ac4::Error::kTruncated);
        CHECK(result.frames.empty());
    }
}

TEST_CASE("the table of contents reads every TOC-level escape and presentation shape", "[ac4][toc]") {
    BitWriter w;
    w.put(2, 2);       // bitstream_version
    w.put(7, 10);      // sequence_counter
    w.flag(true);      // b_wait_frames
    w.put(3, 3);       // wait_frames
    w.put(1, 2);       // br_code
    w.put(1, 1);       // fs_index
    w.put(3, 4);       // frame_rate_index 3: 29.97 fps
    w.flag(false);     // b_iframe_global
    w.flag(false);     // b_single_presentation
    w.flag(true);      // b_more_presentations
    w.variable_bits(1, 2);  // three presentations
    w.flag(true);      // b_payload_base
    w.put(31, 5);      // payload_base_minus1 31: escaped
    w.variable_bits(2, 3);  // payload base 34
    w.flag(true);      // b_program_id
    w.put(0xBEEF, 16);
    w.flag(true);      // b_program_uuid_present
    for (int i = 0; i < 4; ++i) {
        w.put(0x12345678, 32);
    }
    // Presentation 0: configuration 5 over five groups, the last escaped.
    w.flag(false);     // b_single_substream_group
    w.put(5, 3);
    ac4_toc_test::presentation_version(w, 1);
    w.put(5, 3);       // md_compat
    w.flag(true);      // b_presentation_id
    w.variable_bits(6, 2);
    w.flag(true);      // b_multiplier
    w.flag(true);      // b_multiplier_4: frame_rate_factor 4
    // emdf_info() with both escapes and reserved bytes.
    w.put(3, 2);
    w.variable_bits(1, 2);  // emdf_version 4
    w.put(7, 3);
    w.variable_bits(2, 3);  // key_id 9
    w.flag(true);
    ac4_toc_test::substream_index(w, 1);
    w.put(1, 2);       // primary: one byte
    w.put(2, 2);       // secondary: four bytes
    w.put(0, 40);
    w.flag(true);      // b_presentation_filter
    w.flag(true);      // b_enable_presentation
    w.flag(true);      // b_multi_pid
    w.put(3, 2);       // n_substream_groups_minus2 3: escaped
    w.variable_bits(0, 2);
    for (const int group : {0, 1, 2, 3, 7}) {
        ac4_toc_test::group_index(w, group);
    }
    w.flag(true);      // b_pre_virtualized
    w.flag(true);      // b_add_emdf_substreams
    w.flag(true);      // b_alternative
    w.flag(false);     // b_pres_ndot
    ac4_toc_test::substream_index(w, 4);
    w.put(0, 2);       // n_add_emdf_substreams 0: escaped
    w.variable_bits(0, 2);  // four
    for (int i = 0; i < 4; ++i) {
        ac4_toc_test::emdf_info(w, i == 2 ? std::optional<int>{2} : std::nullopt);
    }
    // Presentation 1: configuration 8, skipped through
    // presentation_config_ext_info().
    w.flag(false);
    w.put(7, 3);
    w.variable_bits(1, 2);
    ac4_toc_test::presentation_version(w, 1);
    w.put(0, 3);
    w.flag(false);
    w.flag(false);     // b_multiplier
    ac4_toc_test::emdf_info(w);
    w.flag(false);     // b_presentation_filter
    w.flag(false);     // b_multi_pid
    w.put(2, 5);       // n_skip_bytes
    w.flag(true);      // b_more_skip_bytes
    w.variable_bits(0, 2);
    w.put(0xABCD, 16);  // the two skipped bytes
    w.flag(false);
    w.flag(false);
    w.flag(false);
    w.flag(true);
    ac4_toc_test::substream_index(w, 4);
    // Presentation 2: EMDF only.
    w.flag(false);
    w.put(6, 3);
    ac4_toc_test::presentation_version(w, 0);
    w.put(1, 2);       // n_add_emdf_substreams
    ac4_toc_test::emdf_info(w, 0);
    // Eight substream groups, each a mono channel with four b_audio_ndot:
    // group 0 with a serialized language tag, group 1 with no substreams in
    // this stream (and an HSF extension flag), group 2 with a language tag
    // as bytes.
    for (int group = 0; group < 8; ++group) {
        const bool present = group != 1;
        w.flag(present);
        w.flag(group == 1);  // b_hsf_ext
        w.flag(true);        // b_single_substream
        w.flag(true);        // b_channel_coded
        w.put(0, 1);         // channel_mode: mono
        w.flag(false);       // b_sf_multiplier
        w.flag(false);       // b_bitrate_info
        for (int i = 0; i < 4; ++i) {
            w.flag(i == 0);  // b_audio_ndot
        }
        if (present) {
            ac4_toc_test::substream_index(w, 0);
        }
        w.flag(group == 0 || group == 2);  // b_content_type
        if (group == 0 || group == 2) {
            content_type_with_language(w, group == 0 ? 0b001 : 0b100, group == 0);
        }
    }
    const auto frame = parse(with_substreams(w, 5, 3, 34));
    const ac4::Toc& toc = frame.toc;
    CHECK(toc.sequence_counter == 7);
    CHECK(toc.wait_frames == 3);
    CHECK(toc.payload_base == 34);
    CHECK_FALSE(toc.b_iframe_global);
    CHECK(toc.short_program_id == 0xBEEF);
    REQUIRE(toc.program_uuid.has_value());
    for (std::size_t i = 0; i < 16; ++i) {
        CAPTURE(i);
        constexpr std::array<std::byte, 4> kWord = {std::byte{0x12}, std::byte{0x34},
                                                    std::byte{0x56}, std::byte{0x78}};
        CHECK((*toc.program_uuid)[i] == kWord[i % 4]);
    }
    REQUIRE(toc.n_presentations == 3);
    REQUIRE(toc.presentations_v1.size() == 3);
    const auto& first = toc.presentations_v1[0];
    CHECK(first.presentation_config == 5);
    CHECK(first.presentation_version == 1);
    CHECK(first.md_compat == 5);
    CHECK(first.presentation_id == 6);
    CHECK(first.frame_rate_factor == 4);
    CHECK(first.frame_rate_fraction == 1);
    CHECK(first.enable_presentation == true);
    CHECK(first.b_multi_pid);
    CHECK(first.group_refs == std::vector<int>{0, 1, 2, 3, 7});
    CHECK(first.b_pre_virtualized);
    CHECK(first.b_alternative);
    CHECK_FALSE(first.b_pres_ndot);
    CHECK(first.presentation_substream_index == 4);
    CHECK(first.emdf_payloads_substream_indices == std::vector<int>{1, 2});
    const auto& second = toc.presentations_v1[1];
    CHECK(second.presentation_config == 8);
    CHECK(second.group_refs.empty());
    CHECK(second.b_pres_ndot);
    const auto& third = toc.presentations_v1[2];
    CHECK(third.presentation_config == 6);
    CHECK(third.emdf_payloads_substream_indices == std::vector<int>{0});
    REQUIRE(toc.substream_groups.size() == 8);
    const auto& group0 = toc.substream_groups[0];
    REQUIRE(group0.content_type.has_value());
    CHECK(group0.content_type->content_classifier == 1);
    CHECK_FALSE(group0.content_type->language_tag.has_value());
    CHECK(group0.content_type->serialized_language_tag);
    CHECK_FALSE(group0.b_hsf_ext);
    const auto& group1 = toc.substream_groups[1];
    CHECK_FALSE(group1.b_substreams_present);
    CHECK(group1.b_hsf_ext);  // with no index to name, the group's flag says so
    CHECK_FALSE(group1.substreams[0].chan->substream_index.has_value());
    CHECK_FALSE(group1.substreams[0].hsf_ext_substream_index.has_value());
    const auto& group2 = toc.substream_groups[2];
    CHECK_FALSE(group2.content_type->serialized_language_tag);
    REQUIRE(group2.content_type->language_tag.has_value());
    CHECK(*group2.content_type->language_tag == std::vector<std::byte>{std::byte{'e'}, std::byte{'n'}});
    CHECK(toc.substream_groups[7].substreams[0].chan->b_iframe == std::vector<bool>{true, false, false, false});
    REQUIRE(frame.substreams.size() == 5);
    CHECK(frame.substreams[1].offset == frame.substreams[0].offset + 3);
    CHECK(frame.substreams[0].is_audio);
    CHECK_FALSE(frame.substreams[4].is_audio);
    CHECK(ac4::samples_per_frame(toc) == std::nullopt);  // 29.97 alternates
}

TEST_CASE("ac4_substream_info_chan reads every channel_mode code and its fields", "[ac4][toc]") {
    // One group of seventeen substreams: ch_mode 0 to 15 and the reserved
    // escape. The first has sf_multiplier 1, the second to fourth a terminal,
    // an extended and an unmapped bitrate code; the 7.X modes of 5/2/0 and
    // 3/2/2 carry add_ch_base (set for 5/2/0 without LFE) and the immersive
    // ones their original content.
    BitWriter w;
    ac4_toc_test::toc_start(w, {});
    ac4_toc_test::presentation_v1(w, PresV1{});
    w.flag(true);      // b_substreams_present
    w.flag(true);      // b_hsf_ext
    w.flag(false);     // b_single_substream
    w.put(3, 2);
    w.variable_bits(12, 2);  // 5 + 12 = 17 substreams
    w.flag(true);      // b_channel_coded
    for (int i = 0; i < 17; ++i) {
        if (i < 16) {
            ac4_toc_test::channel_mode(w, i);
        } else {
            w.put(0b111111111, 9);
            w.variable_bits(2, 2);
        }
        if (i >= 11 && i <= 14) {
            w.flag(i % 2 == 0);  // b_4_back_channels_present
            w.flag(true);        // b_centre_present
            w.put(static_cast<std::uint64_t>(i - 11), 2);
        }
        w.flag(i == 0);    // b_sf_multiplier
        if (i == 0) {
            w.put(1, 1);
        }
        w.flag(i >= 1 && i <= 3);  // b_bitrate_info
        if (i == 1) {
            w.put(0b010, 3);        // brate_ind 1: 20 kbit/s
        } else if (i == 2) {
            w.put(0b011, 3);
            w.put(1, 2);            // brate_ind 9: 80 kbit/s
        } else if (i == 3) {
            w.put(0b111, 3);
            w.put(3, 2);            // brate_ind 19: unlimited
        }
        if (i >= 7 && i <= 10) {
            w.flag(i == 7);         // add_ch_base
        }
        w.flag(true);               // b_audio_ndot
        ac4_toc_test::substream_index(w, i % 2);
        ac4_toc_test::substream_index(w, 2);  // hsf_ext_substream_index
    }
    w.flag(false);     // b_content_type
    const auto frame = parse(with_substreams(w, 3));
    const auto& subs = frame.toc.substream_groups.at(0).substreams;
    REQUIRE(subs.size() == 17);
    const std::vector<std::string> names = {"Mono", "Stereo", "3.0", "5.0", "5.1", "7.0: 3/4/0",
                                            "7.1: 3/4/0.1", "7.0: 5/2/0", "7.1: 5/2/0.1", "7.0: 3/2/2",
                                            "7.1: 3/2/2.1", "7.0.4", "7.1.4", "9.0.4", "9.1.4", "22.2"};
    for (int i = 0; i < 17; ++i) {
        INFO("substream " << i);
        const auto& chan = *subs[static_cast<std::size_t>(i)].chan;
        if (i < 16) {
            CHECK(chan.ch_mode == i);
            CHECK(chan.channel_mode_name == names[static_cast<std::size_t>(i)]);
        } else {
            CHECK(chan.channel_mode == 0b111111111 + 2);
            CHECK_FALSE(chan.ch_mode.has_value());
            CHECK(chan.channel_mode_name == "reserved");
        }
        CHECK(chan.original_content.has_value() == (i >= 11 && i <= 14));
        CHECK(chan.add_ch_base.has_value() == (i >= 7 && i <= 10));
        CHECK(subs[static_cast<std::size_t>(i)].hsf_ext_substream_index == 2);
        CHECK(chan.substream_index == i % 2);
    }
    CHECK(subs[0].chan->sf_multiplier == 1);
    CHECK(subs[1].chan->bitrate_kbps == 20);
    CHECK(subs[2].chan->bitrate_kbps == 80);
    CHECK_FALSE(subs[3].chan->bitrate_kbps.has_value());
    CHECK(subs[7].chan->add_ch_base == true);
    CHECK(subs[8].chan->add_ch_base == false);
    CHECK(subs[12].chan->original_content->b_4_back_channels_present);
    CHECK(subs[13].chan->original_content->top_channels_present == 2);
}

TEST_CASE("a frame cut short inside a channel-coded group is truncated", "[ac4][toc]") {
    BitWriter w;
    ac4_toc_test::toc_start(w, {});
    ac4_toc_test::presentation_v1(w, PresV1{});
    w.flag(true);
    w.flag(false);
    w.flag(false);
    w.put(3, 2);
    w.variable_bits(1000, 2);  // a thousand substreams the data does not hold
    w.flag(true);
    ac4_toc_test::channel_mode(w, 1);
    w.align();
    const auto result = ac4::parse_raw_frame(w.bytes());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac4::Error::kTruncated);
}

TEST_CASE("frame_rate_fractions_info reads the fraction for each frame rate family", "[ac4][toc]") {
    struct Case {
        int frame_rate_index;
        std::vector<bool> bits;
        int factor;
        int fraction;
    };
    for (const Case& c : {Case{7, {false, true}, 1, 2},        // 50 fps: b_multiplier 0, then a fraction
                          Case{7, {true}, 2, 1},               // b_multiplier 1: no fraction bit
                          Case{11, {true, true}, 1, 4},        // 119.88 fps: fraction 4
                          Case{12, {false}, 1, 1},
                          Case{0, {true}, 2, 1},
                          Case{2, {true, false}, 2, 1}}) {
        INFO("frame_rate_index " << c.frame_rate_index);
        BitWriter w;
        ac4_toc_test::toc_start(w, {.frame_rate_index = c.frame_rate_index});
        PresV1 p;
        p.frame_rate_bits = c.bits;
        ac4_toc_test::presentation_v1(w, p);
        ChanInfo info;
        info.b_audio_ndot = std::vector<bool>(static_cast<std::size_t>(c.factor), true);
        ac4_toc_test::chan_group(w, {info});
        const auto frame = parse(with_substreams(w, 2));
        const auto& presentation = frame.toc.presentations_v1.at(0);
        CHECK(presentation.frame_rate_factor == c.factor);
        CHECK(presentation.frame_rate_fraction == c.fraction);
        CHECK(frame.toc.substream_groups.at(0).substreams.at(0).chan->b_iframe.size() ==
              static_cast<std::size_t>(c.factor));
    }
}

TEST_CASE("object and A-JOC substream infos read each bed and object assignment", "[ac4][toc]") {
    BitWriter w;
    ac4_toc_test::toc_start(w, {});
    ac4_toc_test::presentation_v1(w, PresV1{});
    w.flag(true);      // b_substreams_present
    w.flag(true);      // b_hsf_ext
    w.flag(false);     // b_single_substream
    w.put(3, 2);
    w.variable_bits(6, 2);  // eleven substreams
    w.flag(false);     // b_channel_coded
    w.flag(false);     // b_oamd_substream
    const auto tail = [&](bool sf_multiplier, int index) {
        w.flag(sf_multiplier);
        if (sf_multiplier) {
            w.put(1, 1);
        }
        w.flag(index == 0);  // b_bitrate_info
        if (index == 0) {
            w.put(0b000, 3);  // 16 kbit/s
        }
        w.flag(true);        // b_audio_ndot
        ac4_toc_test::substream_index(w, index % 3);
        ac4_toc_test::substream_index(w, 2);  // hsf_ext_substream_index
    };
    // 0: A-JOC, dmx by ISF, upmix by channel assignment code.
    w.flag(true);      // b_ajoc
    w.flag(true);      // b_lfe
    w.flag(false);     // b_static_dmx
    w.put(3, 4);       // n_fullband_dmx_signals_minus1
    w.flag(false);     // b_dyn_objects_only
    w.flag(true);      // b_isf
    w.put(2, 3);       // isf_config: 10 objects
    w.flag(false);     // b_oamd_common_data_present
    w.put(0, 4);       // one upmix signal
    w.flag(false);
    w.flag(false);
    w.flag(true);      // b_ch_assign_code
    w.put(2, 3);       // five beds
    tail(true, 0);
    // 1: A-JOC, dmx by nonstd assignment per signal, upmix by nonstd flags.
    w.flag(true);
    w.flag(false);
    w.flag(false);
    w.put(2, 4);       // three dmx signals
    w.flag(false);
    w.flag(false);
    w.flag(false);     // b_ch_assign_code
    w.flag(false);     // b_channel_assignment_flags_present
    w.put(1, 2);       // n_bed_signals_minus1: bed_ch_bits of 3 signals is 2
    w.put(3, 4);       // nonstd_bed_channel_assignment 3: LFE, not counted
    w.put(5, 4);
    w.flag(false);     // b_oamd_common_data_present
    w.put(0, 4);
    w.flag(false);
    w.flag(false);
    w.flag(false);
    w.flag(true);      // b_channel_assignment_flags_present
    w.flag(true);      // b_nonstd_bed_channel_assignment_flags_present
    w.put((1U << 16) | (1U << 5) | (1U << 3) | 1U, 17);  // bits 0, 3, 5 and 16; 3 and 16 are LFE
    tail(false, 1);
    // 2: A-JOC, dmx by standard flags, upmix of 16 + escape signals, all
    // dynamic.
    w.flag(true);
    w.flag(false);
    w.flag(false);
    w.put(0, 4);
    w.flag(false);
    w.flag(false);
    w.flag(false);
    w.flag(true);
    w.flag(false);     // standard flags
    w.put((1U << 9) | (1U << 4) | (1U << 2) | 1U, 10);  // bits 0, 2, 4 and 9; 2 and 9 are LFE
    w.flag(false);
    w.put(15, 4);      // n_fullband_upmix_signals 16: escaped
    w.variable_bits(3, 3);
    w.flag(true);      // b_dyn_objects_only
    tail(false, 2);
    // 3: objects, dynamic with an LFE.
    w.flag(false);     // b_ajoc
    w.put(3, 3);       // n_objects_code: 3
    w.flag(true);      // b_dynamic_objects
    w.flag(true);      // b_lfe
    tail(true, 3);
    // 4: bed objects by channel assignment code.
    w.flag(false);
    w.put(7, 3);       // reserved: no count
    w.flag(false);
    w.flag(true);      // b_bed_objects
    w.flag(true);      // b_bed_start
    w.flag(true);      // b_ch_assign_code
    w.put(3, 3);       // eight beds, LFE the fourth
    tail(false, 4);
    // 5: bed objects by nonstd flags.
    w.flag(false);
    w.put(0, 3);
    w.flag(false);
    w.flag(true);
    w.flag(true);
    w.flag(false);
    w.flag(true);      // b_nonstd_bed_channel_assignment_flags_present
    w.put((1U << 3) | (1U << 16) | (1U << 1), 17);
    tail(false, 5);
    // 6: bed objects by standard flags.
    w.flag(false);
    w.put(0, 3);
    w.flag(false);
    w.flag(true);
    w.flag(true);
    w.flag(false);
    w.flag(false);
    w.put((1U << 2) | (1U << 0), 10);
    tail(false, 6);
    // 7: bed objects not starting here.
    w.flag(false);
    w.put(0, 3);
    w.flag(false);
    w.flag(true);
    w.flag(false);     // b_bed_start
    tail(false, 7);
    // 8: ISF objects starting here.
    w.flag(false);
    w.put(0, 3);
    w.flag(false);
    w.flag(false);     // b_bed_objects
    w.flag(true);      // b_isf
    w.flag(true);      // b_isf_start
    w.put(5, 3);       // 30 objects
    tail(false, 8);
    // 9: ISF not starting here.
    w.flag(false);
    w.put(0, 3);
    w.flag(false);
    w.flag(false);
    w.flag(true);
    w.flag(false);
    tail(false, 9);
    // 10: reserved bytes.
    w.flag(false);
    w.put(0, 3);
    w.flag(false);
    w.flag(false);
    w.flag(false);
    w.put(2, 4);       // res_bytes
    w.put(0xFFFF, 16);
    tail(false, 10);
    w.flag(false);     // b_content_type
    const auto frame = parse(with_substreams(w, 3));
    const auto& subs = frame.toc.substream_groups.at(0).substreams;
    REQUIRE(subs.size() == 11);
    for (const auto& sub : subs) {
        CHECK(sub.hsf_ext_substream_index == 2);
    }
    const auto& ajoc0 = *subs[0].ajoc;
    CHECK(ajoc0.b_lfe);
    CHECK(ajoc0.n_fullband_dmx_signals == 4);
    CHECK(ajoc0.static_objects.size() == 10);
    CHECK(ajoc0.static_objects[0].kind == ac4::ObjectKind::kIsf);
    CHECK(ajoc0.upmix_objects.size() == 5);
    CHECK(ajoc0.sf_multiplier == 1);
    CHECK(ajoc0.bitrate_kbps == 16);
    CHECK(ajoc0.brate_ind == 0);
    CHECK_FALSE(subs[1].ajoc->brate_ind.has_value());
    // What each object substream sends it holds: 3 dynamic objects, 4 to 7 bed
    // objects (7 continuing a bed), 8 and 9 ISF objects, 10 reserved.
    for (std::size_t i = 3; i < subs.size(); ++i) {
        CAPTURE(i);
        REQUIRE(subs[i].obj.has_value());
        CHECK(subs[i].obj->b_dynamic_objects == (i == 3));
        CHECK(subs[i].obj->b_bed_objects == (i >= 4 && i <= 7));
        CHECK(subs[i].obj->b_isf == (i == 8 || i == 9));
        CHECK_FALSE(subs[i].obj->brate_ind.has_value());
    }
    CHECK(subs[1].ajoc->static_objects.size() == 1);
    CHECK(subs[1].ajoc->upmix_objects.size() == 2);  // bits 0 and 5
    CHECK(subs[2].ajoc->static_objects.size() == 4);  // bits 0 and 4, two channels each
    CHECK(subs[2].ajoc->n_fullband_upmix_signals == 19);
    CHECK(subs[2].ajoc->upmix_objects.empty());
    const auto& obj3 = *subs[3].obj;
    REQUIRE(obj3.objects.size() == 3);
    CHECK(obj3.objects[0].lfe);
    CHECK(obj3.objects[1].kind == ac4::ObjectKind::kDyn);
    CHECK(obj3.sf_multiplier == 1);
    REQUIRE(subs[4].obj->objects.size() == 8);
    CHECK(subs[4].obj->objects[3].lfe);
    REQUIRE(subs[5].obj->objects.size() == 3);
    CHECK(subs[5].obj->objects[1].lfe);  // flag bit 3
    CHECK(subs[5].obj->objects[2].lfe);  // flag bit 16
    REQUIRE(subs[6].obj->objects.size() == 3);  // group 0 of two, and the LFE group
    CHECK(subs[6].obj->objects[2].lfe);
    CHECK(subs[7].obj->objects.empty());
    CHECK(subs[8].obj->objects.size() == 30);
    CHECK(subs[9].obj->objects.empty());
    CHECK(subs[10].obj->objects.empty());
    CHECK(subs[10].obj->substream_index == 1);
}

TEST_CASE("an A-JOC bed assignment counting past the data stops at truncation", "[ac4][toc]") {
    BitWriter w;
    ac4_toc_test::toc_start(w, {});
    ac4_toc_test::presentation_v1(w, PresV1{});
    w.flag(true);
    w.flag(false);
    w.flag(true);
    w.flag(false);     // b_channel_coded
    w.flag(false);     // b_oamd_substream
    w.flag(true);      // b_ajoc
    w.flag(false);
    w.flag(true);      // b_static_dmx
    w.flag(false);
    w.put(15, 4);
    w.variable_bits(4000, 3);  // 4016 upmix signals
    w.flag(false);
    w.flag(false);
    w.flag(false);
    w.flag(false);     // nonstd assignment per signal
    w.put(4000, 12);   // 4001 bed signals, and the data ends
    w.align();
    const auto result = ac4::parse_raw_frame(w.bytes());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac4::Error::kTruncated);
}

namespace {

// A frame of one object group of one A-JOC substream with static downmix
// whose oamd_common_data() is `oamd`.
std::vector<std::byte> ajoc_with_oamd(const BitWriter& oamd, bool pad = true) {
    BitWriter w;
    ac4_toc_test::toc_start(w, {});
    ac4_toc_test::presentation_v1(w, PresV1{});
    w.flag(true);
    w.flag(false);
    w.flag(true);
    w.flag(false);     // b_channel_coded
    w.flag(false);     // b_oamd_substream
    w.flag(true);      // b_ajoc
    w.flag(false);     // b_lfe
    w.flag(true);      // b_static_dmx
    w.flag(true);      // b_oamd_common_data_present
    w.append(oamd);
    w.put(0, 4);       // one upmix signal
    w.flag(true);      // b_dyn_objects_only
    w.flag(false);
    w.flag(false);
    w.flag(true);
    ac4_toc_test::substream_index(w, 0);
    w.flag(false);     // b_content_type
    if (!pad) {
        w.align();
        return w.bytes();
    }
    return with_substreams(w, 2);
}

}  // namespace

TEST_CASE("oamd_common_data reads trim, bed render info and headphone in its budget", "[ac4][toc]") {
    BitWriter oamd;
    oamd.flag(true);   // b_default_screen_size_ratio
    oamd.flag(false);  // b_bed_object_chan_distribute
    oamd.flag(true);   // b_additional_data
    oamd.put(1, 1);    // add_data_bytes_minus1 1: escaped
    oamd.variable_bits(30, 2);  // 32 bytes
    const std::size_t start = oamd.size();
    // trim(): nine configurations - default, disabled, every balance, then
    // defaults.
    oamd.flag(true);   // b_trim_present
    oamd.put(1, 2);    // warp_mode
    oamd.put(0, 2);
    oamd.put(0b10, 2);  // global_trim_mode
    oamd.flag(true);   // b_default_trim
    oamd.flag(false);
    oamd.flag(true);   // b_disable_trim
    oamd.flag(false);
    oamd.flag(false);
    oamd.put(0b11111, 5);
    oamd.put(1, 4);    // trim_centre
    oamd.put(2, 4);    // trim_surround
    oamd.put(3, 4);    // trim_height
    oamd.flag(true);
    oamd.put(4, 4);    // bal3d_y_tb
    oamd.flag(false);
    oamd.put(5, 4);    // bal3d_y_lis
    for (int i = 3; i < 9; ++i) {
        oamd.flag(true);
    }
    // bed_render_info(): every tool, each gain tool in another branch.
    oamd.flag(true);   // b_bed_render_info
    oamd.flag(true);   // b_stereo_dmx_coeff
    oamd.put(1, 3);
    oamd.put(2, 3);
    oamd.flag(true);   // b_ltrt_mixinfo
    oamd.put(3, 3);
    oamd.put(4, 3);
    oamd.flag(true);   // b_lfe_mixinfo
    oamd.put(20, 5);
    oamd.put(1, 2);
    oamd.flag(true);   // b_cdmx_data_present
    oamd.flag(true);
    oamd.put(5, 3);    // gain_w_to_f_code
    oamd.flag(true);
    oamd.put(6, 3);    // gain_b4_to_b2_code
    oamd.flag(true);   // b_tm_ch_present
    oamd.flag(true);   // t2_to_f_s_b: neither to front nor to side
    oamd.flag(false);
    oamd.flag(false);
    oamd.put(1, 3);
    oamd.flag(true);   // t2_to_f_s: not to front
    oamd.flag(false);
    oamd.put(2, 3);
    oamd.flag(true);   // b_tb_ch_present
    oamd.flag(true);   // tb_to_f_s_b: to side
    oamd.flag(false);
    oamd.flag(true);
    oamd.put(3, 3);
    oamd.flag(true);   // tb_to_f_s: to front
    oamd.flag(true);
    oamd.put(4, 3);
    oamd.flag(true);   // b_tf_ch_present
    oamd.flag(true);   // tf_to_f_s_b: to front
    oamd.flag(true);
    oamd.put(5, 3);
    oamd.flag(true);   // tf_to_f_s: not to front
    oamd.flag(false);
    oamd.put(7, 3);
    oamd.flag(true);   // b_cdmx_tfb_to_tm
    oamd.put(6, 3);
    // headphone()
    oamd.flag(true);
    oamd.put(0b010, 3);
    oamd.flag(true);   // b_head_track_disable_all
    while (oamd.size() < start + 32 * 8) {
        oamd.flag(false);  // add_data
    }
    const auto frame = parse(ajoc_with_oamd(oamd));
    const auto& ajoc = *frame.toc.substream_groups.at(0).substreams.at(0).ajoc;
    REQUIRE(ajoc.oamd_common_data.has_value());
    const auto& data = *ajoc.oamd_common_data;
    REQUIRE(data.trim.has_value());
    CHECK(data.trim->warp_mode == 1);
    REQUIRE(data.trim->configs.size() == 9);
    CHECK_FALSE(data.trim->configs[0].has_value());
    CHECK(data.trim->configs[1]->disabled);
    const auto& full = *data.trim->configs[2];
    CHECK(full.trim_centre == 1);
    CHECK(full.trim_surround == 2);
    CHECK(full.trim_height == 3);
    CHECK(full.bal3d_y_tb == std::pair<int, int>{1, 4});
    CHECK(full.bal3d_y_lis == std::pair<int, int>{0, 5});
    REQUIRE(data.bed_render_info.has_value());
    const auto& bed = *data.bed_render_info;
    REQUIRE(bed.stereo_dmx_coeff.has_value());
    CHECK(bed.stereo_dmx_coeff->ltrt_surround_mixgain == 4);
    CHECK(bed.stereo_dmx_coeff->lfe_mixgain == 20);
    CHECK(bed.gain_w_to_f_code == 5);
    CHECK(bed.gain_b4_to_b2_code == 6);
    CHECK_FALSE(bed.t2_to_f_s_b->code_a.has_value());
    CHECK(bed.t2_to_f_s_b->code_c == 1);
    CHECK(bed.t2_to_f_s_b->code_b == 7);
    CHECK(bed.t2_to_f_s->code_b == 2);
    CHECK(bed.tb_to_f_s_b->code_b == 3);
    CHECK(bed.tb_to_f_s->code_a == 4);
    CHECK(bed.tf_to_f_s_b->code_a == 5);
    CHECK(bed.tf_to_f_s->code_b == 7);
    CHECK_FALSE(bed.tf_to_f_s->code_a.has_value());
    CHECK(bed.gain_tfb_to_tm_code == 6);
    REQUIRE(data.headphone.has_value());
    CHECK(data.headphone->hp_operation_mode == 2);
    CHECK(data.headphone->b_head_track_disable_all == true);
    CHECK(ajoc.substream_index == 0);
}

TEST_CASE("oamd_common_data reads a bare bed render info and a trim past its budget", "[ac4][toc]") {
    SECTION("bed_render_info with neither downmix coefficients nor custom downmix") {
        BitWriter oamd;
        oamd.flag(true);
        oamd.flag(true);   // b_bed_object_chan_distribute
        oamd.flag(true);   // b_additional_data
        oamd.put(0, 1);    // one byte
        oamd.flag(false);  // b_trim_present
        oamd.flag(true);   // b_bed_render_info
        oamd.flag(false);  // b_stereo_dmx_coeff
        oamd.flag(false);  // b_cdmx_data_present
        oamd.flag(true);   // b_headphone
        oamd.put(0, 3);    // hp_operation_mode 0: no head tracking flag, and the
                           // byte is spent: no add_data follows
        const auto frame = parse(ajoc_with_oamd(oamd));
        const auto& data = *frame.toc.substream_groups.at(0).substreams.at(0).ajoc->oamd_common_data;
        CHECK(data.b_bed_object_chan_distribute);
        CHECK_FALSE(data.trim.has_value());
        REQUIRE(data.bed_render_info.has_value());
        CHECK_FALSE(data.bed_render_info->stereo_dmx_coeff.has_value());
        CHECK_FALSE(data.bed_render_info->gain_w_to_f_code.has_value());
        REQUIRE(data.headphone.has_value());
        CHECK_FALSE(data.headphone->b_head_track_disable_all.has_value());
    }
    SECTION("a trim longer than add_data_bytes") {
        BitWriter oamd;
        oamd.flag(true);
        oamd.flag(false);
        oamd.flag(true);
        oamd.put(0, 1);    // one byte, and trim() takes 16 bits
        oamd.flag(true);
        oamd.put(0, 2);
        oamd.put(0, 2);
        oamd.put(0b10, 2);
        for (int i = 0; i < 9; ++i) {
            oamd.flag(true);
        }
        const auto result = ac4::parse_raw_frame(ajoc_with_oamd(oamd));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ac4::Error::kTruncated);
    }
}

TEST_CASE("oamd_common_data's add_data running past the frame is truncated", "[ac4][toc]") {
    BitWriter oamd;
    oamd.flag(true);
    oamd.flag(false);
    oamd.flag(true);   // b_additional_data
    oamd.put(1, 1);
    oamd.variable_bits(100000, 2);  // far more bytes than the frame holds
    oamd.flag(false);  // b_trim_present
    oamd.flag(false);  // b_bed_render_info
    oamd.flag(false);  // b_headphone
    const auto result = ac4::parse_raw_frame(ajoc_with_oamd(oamd, false));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ac4::Error::kTruncated);
}

TEST_CASE("bitstream_version 1 presentations of every shape", "[ac4][toc]") {
    // frame_rate_index 0 (23.976 fps) with b_multiplier: two b_iframe per
    // substream.
    BitWriter w;
    ac4_toc_test::toc_start(w, {.bitstream_version = 1, .frame_rate_index = 0, .n_presentations = 5});
    // 0: a single 3.0 substream with every optional field.
    w.flag(true);      // b_single_substream
    ac4_toc_test::presentation_version(w, 0);
    w.put(4, 3);       // md_compat
    w.flag(true);      // b_belongs_to_presentation_id
    w.variable_bits(9, 2);
    w.flag(true);      // b_multiplier: frame_rate_factor 2
    ac4_toc_test::emdf_info(w, 3);
    w.put(0b1100, 4);  // channel_mode: 3.0
    w.flag(true);
    w.put(1, 1);       // sf_multiplier
    w.flag(true);
    w.put(0b100, 3);   // brate_ind 2: 24 kbit/s
    w.flag(true);      // b_content_type
    content_type_with_language(w, 0b010, false);
    w.flag(true);
    w.flag(false);     // b_iframe x2
    ac4_toc_test::substream_index(w, 0);
    w.flag(true);      // b_pre_virtualized
    w.flag(false);
    // 1: M+E, Dialog, Associate, with an HSF extension on the first.
    w.flag(false);
    w.put(3, 3);
    ac4_toc_test::presentation_version(w, 0);
    w.put(0, 3);
    w.flag(false);
    w.flag(false);     // b_multiplier
    ac4_toc_test::emdf_info(w);
    w.flag(true);      // b_hsf_ext
    const std::vector<unsigned> codes = {0b1111010, 0b1111101, 0b0};
    const std::vector<int> widths = {7, 7, 1};
    for (std::size_t i = 0; i < codes.size(); ++i) {
        w.put(codes[i], widths[i]);
        w.flag(false);
        w.flag(false);
        if (i < 2) {
            w.flag(i == 0);  // add_ch_base
        }
        w.flag(false);       // b_content_type
        w.flag(true);        // b_iframe (factor 1 here)
        ac4_toc_test::substream_index(w, static_cast<int>(i));
        if (i == 0) {
            ac4_toc_test::substream_index(w, 4);
        }
    }
    w.flag(false);
    w.flag(true);      // b_add_emdf_substreams
    w.put(1, 2);
    ac4_toc_test::emdf_info(w, 5);
    // 2: EMDF only.
    w.flag(false);
    w.put(6, 3);
    ac4_toc_test::presentation_version(w, 0);
    w.put(2, 2);
    ac4_toc_test::emdf_info(w, 1);
    ac4_toc_test::emdf_info(w);
    // 3: configuration 8, skipped.
    w.flag(false);
    w.put(7, 3);
    w.variable_bits(1, 2);
    ac4_toc_test::presentation_version(w, 0);
    w.put(0, 3);
    w.flag(false);
    w.flag(false);
    ac4_toc_test::emdf_info(w);
    w.flag(false);     // b_hsf_ext
    w.put(1, 5);       // n_skip_bytes
    w.flag(false);
    w.put(0x5A, 8);
    w.flag(false);
    w.flag(false);
    // 4: Main only, a reserved channel_mode.
    w.flag(false);
    w.put(5, 3);
    ac4_toc_test::presentation_version(w, 0);
    w.put(0, 3);
    w.flag(false);
    w.flag(false);
    ac4_toc_test::emdf_info(w);
    w.flag(false);     // b_hsf_ext
    w.put(0b1111111, 7);
    w.variable_bits(1, 2);
    w.flag(false);
    w.flag(false);
    w.flag(false);
    w.flag(true);
    ac4_toc_test::substream_index(w, 2);
    w.flag(false);
    w.flag(false);
    // Substream 0's audio_size escapes past 15 bits.
    ac4_toc_test::index_table(w, std::vector<std::size_t>(6, 4));
    w.align();
    std::vector<std::byte> bytes = w.bytes();
    BitWriter audio;
    audio.put(5, 15);
    audio.flag(true);
    audio.variable_bits(1, 7);
    audio.align();
    const auto header = audio.bytes();
    bytes.insert(bytes.end(), header.begin(), header.end());
    bytes.resize(bytes.size() + 6 * 4 - header.size(), std::byte{0});
    const auto frame = parse(bytes);
    const ac4::Toc& toc = frame.toc;
    REQUIRE(toc.presentations_v0.size() == 5);
    CHECK(toc.presentations_v1.empty());
    const auto& p0 = toc.presentations_v0[0];
    CHECK(p0.presentation_id == 9);
    CHECK(p0.md_compat == 4);
    CHECK(p0.b_pre_virtualized);
    CHECK(p0.emdf_payloads_substream_indices == std::vector<int>{3});
    REQUIRE(p0.substreams.size() == 1);
    CHECK(p0.substreams[0].first == "main");
    const auto& chan = p0.substreams[0].second;
    CHECK(chan.ch_mode == 2);
    CHECK(chan.sf_multiplier == 1);
    CHECK(chan.bitrate_kbps == 24);
    REQUIRE(chan.content_type.has_value());
    CHECK(chan.content_type->content_classifier == 0b010);
    CHECK(chan.b_iframe == std::vector<bool>{true, false});
    const auto& p1 = toc.presentations_v0[1];
    REQUIRE(p1.substreams.size() == 3);
    CHECK(p1.substreams[0].first == "M+E");
    CHECK(p1.substreams[1].first == "Dialog");
    CHECK(p1.substreams[2].first == "Associate");
    CHECK(p1.substreams[0].second.add_ch_base == true);
    CHECK(p1.substreams[1].second.add_ch_base == false);
    CHECK(p1.substreams[1].second.ch_mode == 10);
    CHECK(p1.substreams[0].second.hsf_ext_substream_index == 4);
    CHECK(p1.emdf_payloads_substream_indices == std::vector<int>{5});
    CHECK(toc.presentations_v0[2].presentation_config == 6);
    CHECK(toc.presentations_v0[2].emdf_payloads_substream_indices == std::vector<int>{1});
    CHECK(toc.presentations_v0[3].presentation_config == 8);
    CHECK(toc.presentations_v0[3].substreams.empty());
    const auto& reserved = toc.presentations_v0[4].substreams.at(0).second;
    CHECK(reserved.channel_mode == 0b1111111 + 1);
    CHECK_FALSE(reserved.ch_mode.has_value());
    REQUIRE(frame.substreams.size() == 6);
    CHECK(frame.substreams[0].is_audio);
    CHECK(frame.substreams[0].audio_size == 5 + (1 << 15));

    // The carriage helpers read a version 0 table of contents too; its dac4
    // would need Part 1 Annex E.4a's ac4_presentation_v0_dsi(), which
    // build_dac4() does not write.
    CHECK(ac4::build_dac4(toc).empty());
    CHECK_FALSE(ac4::dac4_refusal(toc).empty());
    CHECK(ac4::rfc6381_codec_string(toc) == "ac-4.01.00.04");
    CHECK(ac4::samples_per_frame(toc) == 2002U);
}

TEST_CASE("samples_per_frame covers every frame rate index", "[ac4][toc]") {
    ac4::Toc toc;
    const std::vector<std::optional<std::uint32_t>> expected = {
        2002, 2000, 1920, std::nullopt, 1600, 1001, 1000, 960, std::nullopt, 800, 480, std::nullopt, 400, 2048,
        std::nullopt, std::nullopt};
    for (int index = 0; index < 16; ++index) {
        toc.frame_rate_index = index;
        INFO("frame_rate_index " << index);
        CHECK(ac4::samples_per_frame(toc) == expected[static_cast<std::size_t>(index)]);
    }
    toc.sample_rate_hz = 44100;
    toc.frame_rate_index = 13;
    CHECK(ac4::samples_per_frame(toc) == 2048U);
    toc.frame_rate_index = 2;
    CHECK_FALSE(ac4::samples_per_frame(toc).has_value());
    // No presentations at all: version and md_compat read as 0.
    CHECK(ac4::rfc6381_codec_string(ac4::Toc{}) == "ac-4.00.00.00");
}
