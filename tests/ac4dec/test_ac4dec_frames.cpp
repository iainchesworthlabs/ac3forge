// ac4::Decoder on hand-built frames: which syntax each substream of a frame
// is read with, for the table-of-contents shapes the committed DEE streams do
// not have - bitstream_version 0 and 1 presentations, a frame-rate-multiplied
// series, the efficient high frame rate mode, object and A-JOC groups, and
// the refusals for what the syntax cannot follow. Each frame's substreams
// are the smallest the syntax allows (see the builders below), so every one
// that is read must be read to its exact end.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4/ac4_toc_writer.hpp"
#include "ac4dec/decoder.hpp"
#include "tables/huffman_codes.hpp"

namespace {

using ac4::DecodeError;
using ac4::SubstreamReport;
using ac4_toc_test::BitWriter;
using ac4_toc_test::ChanInfo;
using ac4_toc_test::PresV1;
using ac4_toc_test::TocStart;

// A mono SIMPLE audio substream: audio_size (3 bytes), a
// single_channel_element() of one long-frame track with max_sfb 0, and
// metadata() with nothing optional. At frame_len_base 1024 and below the
// sf_info() reads a transf_length (3, the whole frame) instead of
// b_long_frame.
//
// sus_ver 1: basic_metadata() and extended_metadata() take 4 bits. sus_ver 0
// adds dialnorm_bits and a drc_frame() bit, and for an associated or
// dialogue substream the fields extended_metadata() reads for it.
struct MonoAudio {
    int sus_ver = 1;
    int frame_len_base = 2048;
    bool associated = false;
    bool dialog = false;
};

std::vector<std::byte> mono_audio(const MonoAudio& m) {
    BitWriter w;
    w.put(3, 15);      // audio_size_value: 3 bytes
    w.flag(false);     // b_more_bits
    const std::size_t audio_start = w.size();
    w.put(0, 1);       // mono_codec_mode: SIMPLE
    w.put(0, 1);       // spec_frontend: ASF
    if (m.frame_len_base >= 1536) {
        w.flag(true);  // b_long_frame
    } else {
        w.put(3, 2);   // transf_length: the whole frame
    }
    w.put(0, 6);       // max_sfb
    w.put(0, 8);       // reference_scale_factor
    w.flag(false);     // b_snf_data_exists
    while (w.size() < audio_start + 24) {
        w.flag(false);  // fill_bits
    }
    if (m.sus_ver == 0) {
        w.put(20, 7);  // dialnorm_bits
    }
    w.flag(false);     // b_more_basic_metadata
    if (m.sus_ver >= 1) {
        w.flag(false);  // b_dialog
    } else if (m.associated) {
        w.flag(false);  // b_scale_main
        w.flag(false);  // b_scale_main_centre
        w.flag(false);  // b_scale_main_front
        w.put(128, 8);  // pan_associated
    }
    if (m.sus_ver == 0 && m.dialog) {
        w.flag(false);  // b_dialog_max_gain
        w.flag(false);  // b_pan_dialog_present
    }
    w.flag(false);     // b_channels_classifier
    w.flag(false);     // b_event_probability
    const int tools = m.sus_ver == 0 ? 2 : 1;
    w.put(static_cast<std::uint64_t>(tools), 7);  // tools_metadata_size_value
    w.flag(false);     // b_more_bits
    if (m.sus_ver == 0) {
        w.flag(false);  // b_drc_present
    }
    w.flag(false);     // b_de_data_present
    w.flag(false);     // b_emdf_payloads_substream
    w.align();
    return w.bytes();
}

// ac4_presentation_substream() for a presentation of channel mode 0 or 1:
// 17 bits, and one more for sg gains' flag when it has several groups, and
// for an object presentation one more for b_obj_loud_corr.
// `extra_flags` more zero flags follow for the custom downmix and loudness
// correction fields an immersive presentation reads.
std::vector<std::byte> presentation(int n_substream_groups = 1, bool objects = false, int extra_flags = 0) {
    BitWriter w;
    w.flag(false);   // b_additional_data
    w.put(20, 7);    // dialnorm_bits
    w.flag(false);   // b_further_loudness_info
    w.put(1, 5);     // drc_metadata_size_value
    w.flag(false);   // b_more_bits
    w.flag(false);   // b_drc_present
    if (n_substream_groups > 1) {
        w.flag(false);  // b_substream_group_gains_present
    }
    w.flag(false);   // b_associated
    if (objects) {
        w.flag(false);  // b_obj_loud_corr
    }
    for (int i = 0; i < extra_flags; ++i) {
        w.flag(false);
    }
    w.align();
    return w.bytes();
}

// An emdf_payloads_substream() of one empty payload.
std::vector<std::byte> emdf_payloads() {
    BitWriter w;
    w.put(1, 5);     // emdf_payload_id
    w.put(0, 4);     // b_smpoffst, b_duration, b_groupid, b_codecdata
    w.flag(true);    // b_discard_unknown_payload
    w.variable_bits(0, 8);
    w.put(0, 5);     // end
    w.align();
    return w.bytes();
}

const SubstreamReport& find(const ac4::FrameReport& report, int index) {
    const auto it = std::find_if(report.substreams.begin(), report.substreams.end(),
                                 [index](const SubstreamReport& s) { return s.index == index; });
    REQUIRE(it != report.substreams.end());
    return *it;
}

// Read to its exact end, not refused.
void check_read(const SubstreamReport& s, SubstreamReport::Kind kind) {
    INFO("substream " << s.index << ": " << s.refused_reason);
    CHECK(s.kind == kind);
    CHECK_FALSE(s.refused.has_value());
    CHECK(s.bits_read == s.size_bits);
}

void check_refused(const SubstreamReport& s, DecodeError error) {
    INFO("substream " << s.index << ": " << s.refused_reason);
    REQUIRE(s.refused.has_value());
    CHECK(*s.refused == error);
    CHECK_FALSE(s.refused_reason.empty());
}

ac4::FrameReport decode(const std::vector<std::byte>& frame) {
    ac4::Decoder decoder;
    const auto report = decoder.parse(frame);
    REQUIRE(report.has_value());
    return *report;
}

// One presentation, one channel-coded group of `infos`, the presentation
// substream after `audio`.
std::vector<std::byte> single_group_frame(const TocStart& start, const PresV1& p, const std::vector<ChanInfo>& infos,
                                          std::vector<std::vector<std::byte>> substreams) {
    BitWriter toc;
    ac4_toc_test::toc_start(toc, start);
    ac4_toc_test::presentation_v1(toc, p);
    ac4_toc_test::chan_group(toc, infos, start.fs_index);
    ac4_toc_test::index_table(toc, ac4_toc_test::sizes_of(substreams));
    toc.align();
    return ac4_toc_test::assemble(toc, substreams);
}

}  // namespace

TEST_CASE("DecodeError describes every value", "[ac4dec][frames]") {
    std::set<std::string_view> seen;
    for (const DecodeError error : {DecodeError::kTruncated, DecodeError::kInvalidToc, DecodeError::kInvalidStream,
                                    DecodeError::kUnsupported, DecodeError::kMissingIFrame}) {
        const std::string_view text = ac4::describe(error);
        CHECK_FALSE(text.empty());
        seen.insert(text);
    }
    CHECK(seen.size() == 5);
    CHECK(ac4::describe(static_cast<DecodeError>(99)) == "unknown error");
}

TEST_CASE("a bitstream_version 0 presentation's audio and EMDF substreams are read", "[ac4dec][frames]") {
    const auto audio = mono_audio({.sus_ver = 0});
    const auto emdf = emdf_payloads();
    BitWriter toc;
    ac4_toc_test::toc_start(toc, {.bitstream_version = 0});
    // ac4_presentation_info(), a single substream.
    toc.flag(true);   // b_single_substream
    ac4_toc_test::presentation_version(toc, 0);
    toc.put(0, 3);    // md_compat
    toc.flag(false);  // b_belongs_to_presentation_id
    ac4_toc_test::emdf_info(toc, 1);
    // ac4_substream_info(): mono.
    toc.put(0, 1);    // channel_mode
    toc.flag(false);  // b_sf_multiplier
    toc.flag(false);  // b_bitrate_info
    toc.flag(false);  // b_content_type
    toc.flag(true);   // b_iframe
    ac4_toc_test::substream_index(toc, 0);
    toc.flag(false);  // b_pre_virtualized
    toc.flag(false);  // b_add_emdf_substreams
    ac4_toc_test::index_table(toc, {audio.size(), emdf.size()});
    toc.align();
    const auto report = decode(ac4_toc_test::assemble(toc, {audio, emdf}));
    REQUIRE(report.substreams.size() == 2);
    check_read(find(report, 0), SubstreamReport::Kind::kAudio);
    check_read(find(report, 1), SubstreamReport::Kind::kEmdfPayloads);
    CHECK(find(report, 0).size_bits == 64);
}

TEST_CASE("a bitstream_version 1 Main + Associate presentation reads each role's metadata", "[ac4dec][frames]") {
    // Main is a dialogue substream by its content classifier and carries an
    // HSF extension link to substream 2; at 48 kHz without sf_multiplier the
    // link names nothing to read, so the extension is refused.
    const auto main = mono_audio({.sus_ver = 0, .dialog = true});
    const auto associate = mono_audio({.sus_ver = 0, .associated = true});
    const std::vector<std::byte> extension(1, std::byte{0});
    BitWriter toc;
    ac4_toc_test::toc_start(toc, {.bitstream_version = 1});
    toc.flag(false);  // b_single_substream
    toc.put(2, 3);    // presentation_config 2: Main + Associate
    ac4_toc_test::presentation_version(toc, 0);
    toc.put(0, 3);
    toc.flag(false);
    ac4_toc_test::emdf_info(toc);
    toc.flag(true);   // b_hsf_ext
    for (int role = 0; role < 2; ++role) {
        toc.put(0, 1);    // channel_mode: mono
        toc.flag(false);  // b_sf_multiplier
        toc.flag(false);  // b_bitrate_info
        toc.flag(role == 0);  // b_content_type
        if (role == 0) {
            toc.put(0b100, 3);  // content_classifier: dialogue
            toc.flag(false);    // b_language_indicator
        }
        toc.flag(true);   // b_iframe
        ac4_toc_test::substream_index(toc, role);
        if (role == 0) {
            ac4_toc_test::substream_index(toc, 2);  // ac4_hsf_ext_substream_info()
        }
    }
    toc.flag(false);
    toc.flag(false);
    ac4_toc_test::index_table(toc, {main.size(), associate.size(), extension.size()});
    toc.align();
    const auto report = decode(ac4_toc_test::assemble(toc, {main, associate, extension}));
    check_read(find(report, 0), SubstreamReport::Kind::kAudio);
    check_read(find(report, 1), SubstreamReport::Kind::kAudio);
    CHECK(find(report, 1).size_bits == 72);
    CHECK(find(report, 2).kind == SubstreamReport::Kind::kHsfExt);
    check_refused(find(report, 2), DecodeError::kUnsupported);
}

TEST_CASE("each instance of a frame-rate-multiplied series is read at its share of the frame",
          "[ac4dec][frames]") {
    // frame_rate_index 2 (2048 samples) with frame_rate_factor 2: two
    // consecutive substreams of 1024 samples, the first an I-frame.
    const TocStart start{.frame_rate_index = 2};
    PresV1 p;
    p.presentation_substream = 2;
    p.frame_rate_bits = {true, false};  // b_multiplier, not 4
    ChanInfo info;
    info.ch_mode = 0;
    info.b_audio_ndot = {true, false};
    const auto audio = mono_audio({.frame_len_base = 1024});
    const auto report = decode(single_group_frame(start, p, {info}, {audio, audio, presentation()}));
    REQUIRE(report.substreams.size() == 3);
    check_read(find(report, 0), SubstreamReport::Kind::kAudio);
    check_read(find(report, 1), SubstreamReport::Kind::kAudio);
    check_read(find(report, 2), SubstreamReport::Kind::kPresentation);
}

TEST_CASE("a frame of the efficient high frame rate mode is refused by name", "[ac4dec][frames]") {
    // frame_rate_index 10 with b_frame_rate_fraction set: every substream is
    // a fragment.
    const TocStart start{.frame_rate_index = 10};
    PresV1 p;
    p.frame_rate_bits = {true, false};  // b_frame_rate_fraction, not 4
    ChanInfo info;
    info.ch_mode = 0;
    const auto report = decode(single_group_frame(start, p, {info}, {mono_audio({}), presentation()}));
    REQUIRE(report.substreams.size() == 2);
    for (const SubstreamReport& s : report.substreams) {
        check_refused(s, DecodeError::kUnsupported);
        CHECK(s.kind == SubstreamReport::Kind::kOther);
    }
}

TEST_CASE("a frame rate index 44.1 kHz does not define refuses audio and presentation alike", "[ac4dec][frames]") {
    const TocStart start{.fs_index = 0, .frame_rate_index = 3};
    PresV1 p;
    p.frame_rate_bits = {false};  // b_multiplier
    ChanInfo info;
    info.ch_mode = 0;
    const auto report = decode(single_group_frame(start, p, {info}, {mono_audio({}), presentation()}));
    REQUIRE(report.substreams.size() == 2);
    check_refused(find(report, 0), DecodeError::kInvalidStream);
    check_refused(find(report, 1), DecodeError::kInvalidStream);
}

TEST_CASE("the decoder refuses a reserved channel mode and an index past the table", "[ac4dec][frames]") {
    // Two presentations: group 0's substream has the reserved 9-bit
    // channel_mode escape; group 1's names substream 5 of three.
    BitWriter toc;
    ac4_toc_test::toc_start(toc, {.n_presentations = 2});
    PresV1 first;
    first.presentation_substream = 2;
    ac4_toc_test::presentation_v1(toc, first);
    PresV1 second;
    second.groups = {1};
    second.presentation_substream = 2;
    ac4_toc_test::presentation_v1(toc, second);
    // Group 0: channel_mode 0b111111111 plus variable_bits(2) of 0.
    toc.flag(true);
    toc.flag(false);
    toc.flag(true);
    toc.flag(true);
    toc.put(0b111111111, 9);
    toc.variable_bits(0, 2);
    toc.flag(false);  // b_sf_multiplier
    toc.flag(false);  // b_bitrate_info
    toc.flag(true);   // b_audio_ndot
    ac4_toc_test::substream_index(toc, 0);
    toc.flag(false);  // b_content_type
    ChanInfo past;
    past.ch_mode = 0;
    past.substream_index = 5;
    ac4_toc_test::chan_group(toc, {past});
    const std::vector<std::vector<std::byte>> substreams = {mono_audio({}), mono_audio({}), presentation()};
    ac4_toc_test::index_table(toc, ac4_toc_test::sizes_of(substreams));
    toc.align();
    const auto report = decode(ac4_toc_test::assemble(toc, substreams));
    check_refused(find(report, 0), DecodeError::kInvalidStream);
    CHECK(find(report, 0).kind == SubstreamReport::Kind::kOther);
    check_refused(find(report, 5), DecodeError::kInvalidStream);
    CHECK(find(report, 5).kind == SubstreamReport::Kind::kAudio);
    check_read(find(report, 2), SubstreamReport::Kind::kPresentation);
    // Substream 1 is named by nothing and so not reported at all.
    CHECK(std::none_of(report.substreams.begin(), report.substreams.end(),
                       [](const SubstreamReport& s) { return s.index == 1; }));
}

TEST_CASE("object, A-JOC and object metadata substreams are read by their own syntax",
          "[ac4dec][frames]") {
    BitWriter toc;
    ac4_toc_test::toc_start(toc, {});
    PresV1 p;
    p.presentation_substream = 3;
    ac4_toc_test::presentation_v1(toc, p);
    // An object-coded group: an OAMD substream, an A-JOC and an object
    // substream.
    toc.flag(true);    // b_substreams_present
    toc.flag(false);   // b_hsf_ext
    toc.flag(false);   // b_single_substream
    toc.put(0, 2);     // two substreams
    toc.flag(false);   // b_channel_coded
    toc.flag(true);    // b_oamd_substream
    toc.flag(true);    // b_oamd_ndot
    ac4_toc_test::substream_index(toc, 2);
    toc.flag(true);    // b_ajoc
    toc.flag(false);   // b_lfe
    toc.flag(true);    // b_static_dmx
    toc.flag(false);   // b_oamd_common_data_present
    toc.put(3, 4);     // n_fullband_upmix_signals_minus1
    toc.flag(true);    // b_dyn_objects_only
    toc.flag(false);   // b_sf_multiplier
    toc.flag(false);   // b_bitrate_info
    toc.flag(true);    // b_audio_ndot
    ac4_toc_test::substream_index(toc, 0);
    toc.flag(false);   // b_ajoc: an object substream
    toc.put(2, 3);     // n_objects_code
    toc.flag(true);    // b_dynamic_objects
    toc.flag(false);   // b_lfe
    toc.flag(false);
    toc.flag(false);
    toc.flag(true);
    ac4_toc_test::substream_index(toc, 1);
    toc.flag(false);   // b_content_type
    const std::vector<std::byte> blank(4, std::byte{0});
    const std::vector<std::vector<std::byte>> substreams = {blank, blank, blank, presentation(1, true)};
    ac4_toc_test::index_table(toc, ac4_toc_test::sizes_of(substreams));
    toc.align();
    const auto report = decode(ac4_toc_test::assemble(toc, substreams));
    REQUIRE(report.substreams.size() == 4);
    // Each is read by the syntax its element names (Part 2 Table 50): four zero
    // bytes end the audio substreams' elements early, and the OAMD substream,
    // which sends no oamd_timing_data() and has none from an earlier frame,
    // cannot read its oamd_dyndata_multi() (src/ac4dec/ERRATA.md, "Which
    // oamd_timing_data() applies").
    CHECK(find(report, 0).kind == SubstreamReport::Kind::kAudio);
    CHECK(find(report, 1).kind == SubstreamReport::Kind::kAudio);
    check_refused(find(report, 0), DecodeError::kTruncated);
    check_refused(find(report, 1), DecodeError::kTruncated);
    CHECK(find(report, 2).kind == SubstreamReport::Kind::kOamd);
    check_refused(find(report, 2), DecodeError::kMissingIFrame);
    check_read(find(report, 3), SubstreamReport::Kind::kPresentation);
}

TEST_CASE("a presentation of three groups reads each and a group without substreams reads none",
          "[ac4dec][frames]") {
    // presentation_config 3 (M+E, dialogue, associated) over groups 0 to 2,
    // then a second presentation naming group 3, which has no substreams
    // in this elementary stream.
    BitWriter toc;
    ac4_toc_test::toc_start(toc, {.n_presentations = 2});
    PresV1 first;
    first.presentation_config = 3;
    first.groups = {0, 1, 2};
    first.presentation_substream = 3;
    ac4_toc_test::presentation_v1(toc, first);
    PresV1 second;
    second.groups = {3};
    second.presentation_substream = 4;
    ac4_toc_test::presentation_v1(toc, second);
    for (int group = 0; group < 3; ++group) {
        ChanInfo info;
        info.ch_mode = group == 0 ? 1 : 0;
        info.substream_index = group;
        ac4_toc_test::chan_group(toc, {info});
    }
    ChanInfo absent;
    absent.ch_mode = 0;
    ac4_toc_test::chan_group(toc, {absent}, 1, false);
    // Group 0 is stereo, the others mono. The stereo element takes 30 bits:
    // stereo_codec_mode, b_enable_mdct_stereo_proc, one sf_info(), one
    // chparam_info() and two sf_data().
    BitWriter stereo;
    stereo.put(4, 15);   // audio_size_value: 4 bytes
    stereo.flag(false);
    stereo.put(0, 2);    // stereo_codec_mode: SIMPLE
    stereo.flag(true);   // b_enable_mdct_stereo_proc
    stereo.flag(true);   // b_long_frame
    stereo.put(0, 6);    // max_sfb
    stereo.put(0, 2);    // sap_mode
    stereo.put(0, 9);    // sf_data()
    stereo.put(0, 9);    // sf_data()
    stereo.put(0, 2);    // fill_bits
    for (int i = 0; i < 4; ++i) {
        stereo.flag(false);  // b_more_basic_metadata, b_dialog, classifier, event
    }
    stereo.put(1, 7);
    stereo.flag(false);
    stereo.flag(false);  // b_de_data_present
    stereo.flag(false);  // b_emdf_payloads_substream
    stereo.align();
    const std::vector<std::vector<std::byte>> substreams = {stereo.bytes(), mono_audio({}), mono_audio({}),
                                                            presentation(3), presentation()};
    ac4_toc_test::index_table(toc, ac4_toc_test::sizes_of(substreams));
    toc.align();
    const auto report = decode(ac4_toc_test::assemble(toc, substreams));
    REQUIRE(report.substreams.size() == 5);
    for (const int index : {0, 1, 2}) {
        check_read(find(report, index), SubstreamReport::Kind::kAudio);
    }
    check_read(find(report, 3), SubstreamReport::Kind::kPresentation);
    check_read(find(report, 4), SubstreamReport::Kind::kPresentation);
}

TEST_CASE("an HSF extension whose owner cannot be read is refused with it", "[ac4dec][frames]") {
    // A 96 kHz stereo substream in ASPX mode, not an I-frame: with no
    // configuration its element fails, and the extension it links is
    // refused as unreadable alongside it.
    BitWriter owner;
    owner.put(1, 15);
    owner.flag(false);
    owner.put(1, 2);     // stereo_codec_mode: ASPX
    owner.put(0, 6);
    const std::vector<std::byte> extension(2, std::byte{0});
    PresV1 p;
    p.presentation_substream = 2;
    p.b_pres_ndot = false;
    ChanInfo info;
    info.sf_multiplier = 0;
    info.b_audio_ndot = {false};
    info.hsf_ext_substream_index = 1;
    const auto report = decode(single_group_frame({.b_iframe_global = false}, p, {info},
                                                  {owner.bytes(), extension, presentation()}));
    check_refused(find(report, 0), DecodeError::kMissingIFrame);
    CHECK(find(report, 1).kind == SubstreamReport::Kind::kHsfExt);
    check_refused(find(report, 1), DecodeError::kUnsupported);
}

namespace {

// A 96 kHz mono SIMPLE I-frame of one long-frame track with `max_sfb`. With
// max_sfb 63 its sections are none for bands 0 to 62 and codebook 1 for
// band 63, which with an extension of one band is the extension's own.
std::vector<std::byte> hsf_owner(int max_sfb) {
    BitWriter w;
    w.put(6, 15);      // audio_size_value: 6 bytes
    w.flag(false);
    const std::size_t start = w.size();
    w.put(0, 1);       // mono_codec_mode
    w.put(0, 1);       // spec_frontend
    w.flag(true);      // b_long_frame
    w.put(static_cast<std::uint64_t>(max_sfb), 6);
    if (max_sfb == 63) {
        w.put(0, 4);   // sect_cb 0
        w.put(31, 5);
        w.put(31, 5);
        w.put(0, 5);   // 63 bands
        w.put(1, 4);   // sect_cb 1
        w.put(0, 5);   // one band: 63, the extension's
    }
    w.put(0, 8);       // reference_scale_factor
    w.flag(false);     // b_snf_data_exists
    while (w.size() < start + 48) {
        w.flag(false);
    }
    for (int i = 0; i < 4; ++i) {
        w.flag(false);
    }
    w.put(1, 7);
    w.flag(false);
    w.flag(false);
    w.flag(false);
    w.align();
    return w.bytes();
}

}  // namespace

TEST_CASE("an HSF extension is refused alone when its own data is malformed", "[ac4dec][frames]") {
    PresV1 p;
    p.presentation_substream = 2;
    ChanInfo info;
    info.ch_mode = 0;
    info.sf_multiplier = 0;
    info.hsf_ext_substream_index = 1;

    SECTION("its spectral data runs out") {
        BitWriter ext;
        ext.put(1, 6);  // max_sfb_ext_hsf[0]: one band past the 48 kHz ones
        ext.align();    // and two bits where band 63's codewords should be
        const auto report =
            decode(single_group_frame({}, p, {info}, {hsf_owner(63), ext.bytes(), presentation()}));
        check_read(find(report, 0), SubstreamReport::Kind::kAudio);
        CHECK(find(report, 1).kind == SubstreamReport::Kind::kHsfExt);
        check_refused(find(report, 1), DecodeError::kInvalidStream);
    }
    SECTION("it is longer than its data") {
        BitWriter ext;
        ext.put(0, 6);
        ext.align();
        std::vector<std::byte> padded = ext.bytes();
        padded.push_back(std::byte{0});
        const auto report = decode(single_group_frame({}, p, {info}, {hsf_owner(0), padded, presentation()}));
        check_read(find(report, 0), SubstreamReport::Kind::kAudio);
        const SubstreamReport& extension = find(report, 1);
        CHECK_FALSE(extension.refused.has_value());
        CHECK(extension.bits_read == 8);
        CHECK(extension.size_bits == 16);
    }
}

TEST_CASE("ac4::Decoder keeps its carried state when moved", "[ac4dec][frames]") {
    // A moved-to or moved-assigned decoder is the same decoder: the frame
    // after the last one it read continues the stream (sequence_counter 2
    // after 1) and is read as such.
    PresV1 p;
    ChanInfo info;
    info.ch_mode = 0;
    const auto frame = [&](int counter) {
        return single_group_frame({.sequence_counter = counter}, p, {info}, {mono_audio({}), presentation()});
    };
    ac4::Decoder first;
    REQUIRE(first.parse(frame(1)).has_value());
    ac4::Decoder moved(std::move(first));
    const auto second = moved.parse(frame(2));
    REQUIRE(second.has_value());
    CHECK(second->sequence_counter == 2);
    ac4::Decoder assigned;
    assigned = std::move(moved);
    const auto third = assigned.parse(frame(3));
    REQUIRE(third.has_value());
    check_read(find(*third, 0), SubstreamReport::Kind::kAudio);
}

TEST_CASE("a presentation_config 5 presentation takes each group's role from its content type",
          "[ac4dec][frames]") {
    // Four references to three groups - associated, dialogue and
    // unclassified - the first repeated: a group named twice is read once.
    BitWriter toc;
    ac4_toc_test::toc_start(toc, {});
    PresV1 p;
    p.presentation_config = 5;
    p.groups = {0, 1, 2, 0};
    p.presentation_substream = 3;
    ac4_toc_test::presentation_v1(toc, p);
    const std::vector<std::optional<int>> classifiers = {0b010, 0b100, std::nullopt};
    for (int group = 0; group < 3; ++group) {
        ChanInfo info;
        info.ch_mode = 0;
        info.substream_index = group;
        ac4_toc_test::chan_group(toc, {info}, 1, true, classifiers[static_cast<std::size_t>(group)]);
    }
    const std::vector<std::vector<std::byte>> substreams = {mono_audio({}), mono_audio({}), mono_audio({}),
                                                            presentation(4)};
    ac4_toc_test::index_table(toc, ac4_toc_test::sizes_of(substreams));
    toc.align();
    const auto report = decode(ac4_toc_test::assemble(toc, substreams));
    REQUIRE(report.substreams.size() == 4);
    for (const int index : {0, 1, 2}) {
        check_read(find(report, index), SubstreamReport::Kind::kAudio);
    }
    check_read(find(report, 3), SubstreamReport::Kind::kPresentation);
}

TEST_CASE("a 9.X.4 channel substream is refused as not decoded", "[ac4dec][frames]") {
    // 9.1.4 with four back channels and both top pairs: the presentation
    // substream reads bs_ch_config 0's b_cdmx_data_present, the stereo
    // downmix flag and seven loudness correction flags, as 7.1.4's does.
    PresV1 p;
    ChanInfo info;
    info.ch_mode = 14;
    const std::vector<std::byte> blank(4, std::byte{0});
    const auto report = decode(single_group_frame({}, p, {info}, {blank, presentation(1, false, 9)}));
    CHECK(find(report, 0).kind == SubstreamReport::Kind::kAudio);
    check_refused(find(report, 0), DecodeError::kUnsupported);
    CHECK(find(report, 0).refused_reason.find("9.X.4") != std::string_view::npos);
    check_read(find(report, 1), SubstreamReport::Kind::kPresentation);
}

TEST_CASE("two channels linking one HSF extension: the second is read without it", "[ac4dec][frames]") {
    PresV1 p;
    p.presentation_substream = 3;
    ChanInfo first;
    first.ch_mode = 0;
    first.sf_multiplier = 0;
    first.hsf_ext_substream_index = 2;
    ChanInfo second = first;
    second.substream_index = 1;
    BitWriter ext;
    ext.put(0, 6);
    ext.align();
    const auto report = decode(single_group_frame(
        {}, p, {first, second}, {hsf_owner(0), hsf_owner(0), ext.bytes(), presentation()}));
    check_read(find(report, 0), SubstreamReport::Kind::kAudio);
    check_read(find(report, 2), SubstreamReport::Kind::kHsfExt);
    // A 96 kHz substream whose extension could not be resolved to it.
    CHECK(find(report, 1).kind == SubstreamReport::Kind::kAudio);
    check_refused(find(report, 1), DecodeError::kUnsupported);
}

TEST_CASE("an HSF-linked channel outside the index table is refused with its extension", "[ac4dec][frames]") {
    PresV1 p;
    ChanInfo info;
    info.ch_mode = 0;
    info.sf_multiplier = 1;
    info.substream_index = 5;
    info.hsf_ext_substream_index = 0;
    const std::vector<std::byte> ext(1, std::byte{0});
    const auto report = decode(single_group_frame({}, p, {info}, {ext, presentation()}));
    check_refused(find(report, 5), DecodeError::kInvalidStream);
    CHECK(find(report, 0).kind == SubstreamReport::Kind::kHsfExt);
    check_refused(find(report, 0), DecodeError::kInvalidStream);
    check_read(find(report, 1), SubstreamReport::Kind::kPresentation);
}

TEST_CASE("a series whose first substream index is INT_MAX names nothing past it", "[ac4dec][frames]") {
    // Two instances from substream_index 2^31 - 1: the first is outside the
    // table, and the second, which would be 2^31, is not named at all.
    const TocStart start{.frame_rate_index = 2};
    PresV1 p;
    p.presentation_substream = 0;
    p.frame_rate_bits = {true, false};
    ChanInfo info;
    info.ch_mode = 0;
    info.b_audio_ndot = {true, true};
    info.substream_index = 2147483647;
    const auto report = decode(single_group_frame(start, p, {info}, {presentation()}));
    REQUIRE(report.substreams.size() == 2);
    check_read(find(report, 0), SubstreamReport::Kind::kPresentation);
    check_refused(find(report, 2147483647), DecodeError::kInvalidStream);
}

namespace {

// The element of a one-signal SIMPLE downmix or a one-object direct-coded
// substream after its codec mode: mono_data(0) of one long-frame track with
// max_sfb 0, as mono_audio() writes it.
void mono_track(BitWriter& w) {
    w.put(0, 1);    // spec_frontend: ASF
    w.flag(true);   // b_long_frame
    w.put(0, 6);    // max_sfb
    w.put(0, 8);    // reference_scale_factor
    w.flag(false);  // b_snf_data_exists
}

// metadata() of an object substream at sus_ver 1 with nothing optional: its
// channel_mode is negative, so basic_metadata() and extended_metadata() read
// only their presence flags.
void object_metadata(BitWriter& w) {
    w.flag(false);  // b_more_basic_metadata
    w.flag(false);  // b_dialog
    w.flag(false);  // b_channels_classifier
    w.flag(false);  // b_event_probability
    w.put(1, 7);    // tools_metadata_size_value
    w.flag(false);  // b_more_bits
    w.flag(false);  // b_de_data_present
    w.flag(false);  // b_emdf_payloads_substream
}

// One object_info_block() of a dynamic object with b_no_delta: its basic and
// render info new, at `x` (pos3D_X), with default zone and other properties.
void new_block(BitWriter& w, int x) {
    w.flag(false);                            // b_object_not_active
    w.flag(true);                             // b_default_basic_info_md
    w.put(static_cast<std::uint64_t>(x), 6);  // pos3D_X
    w.put(0, 6);                              // pos3D_Y
    w.flag(true);                             // pos3D_Z_sign
    w.put(0, 4);                              // pos3D_Z
    w.flag(true);                             // b_grouped_zone_defaults
    w.flag(true);                             // b_grouped_other_defaults
    w.flag(false);                            // b_add_table_data
}

// An A-JOC substream of one downmix signal and one object: a SIMPLE
// var_channel_element(), the timing and dynamic data of both OAMD portions,
// ajoc() with one data point of one parameter band at the dry matrix's centre
// (0), and ajoc_dmx_de_data() with no dialogue objects.
std::vector<std::byte> ajoc_audio() {
    BitWriter w;
    w.put(0, 15);  // audio_size_value, rewritten below
    w.flag(false);
    const std::size_t start = w.size();
    w.flag(false);  // b_some_signals_inactive
    w.put(0, 1);    // var_codec_mode: SIMPLE
    mono_track(w);  // n_dmx_signals 1: mono_data(0)
    w.flag(true);   // b_dmx_timing
    w.put(0, 1);    // oa_sample_offset_type 0b0
    w.put(1, 3);    // num_obj_info_blocks
    w.put(0, 6);    // block_offset_factor
    w.put(0, 2);    // ramp_duration_code
    new_block(w, 31);
    w.flag(false);  // b_oamd_extension_present
    w.put(0, 3);    // ajoc_num_decorr
    w.flag(true);   // ajoc_object_present
    w.put(1, 2);    // ajoc_num_dpoints
    w.put(0, 5);    // ajoc_start_pos
    w.put(0, 6);    // ajoc_ramp_len_minus1
    w.put(7, 3);    // ajoc_num_bands_code: one band
    w.put(1, 1);    // ajoc_quant_select: coarse
    w.put(0, 1);    // ajoc_sparse_select
    w.flag(true);   // ajoc_b_nodt: the first data point frequency-differential only
    const ac4::detail::HuffCode centre = ac4::detail::tables::kAjocHcbDryCoarseF0Codes[25];
    w.put(centre.code, centre.bits);  // ajoc_hcw
    w.flag(true);                     // b_dmx_de_cfg
    w.flag(false);                    // b_keep_dmx_de_coeffs
    w.put(0, 2);                      // de_max_gain
    w.put(0, 1);                      // de_main_dlg_flag[]: no dialogue objects
    w.flag(false);                    // b_umx_timing
    w.flag(true);                     // b_derive_timing_from_dmx
    new_block(w, 62);
    const std::size_t audio_bits = w.size() - start;
    while ((w.size() - start) % 8 != 0) {
        w.flag(false);  // fill_bits
    }
    object_metadata(w);
    w.align();
    std::vector<std::byte> bytes = w.bytes();
    const std::size_t audio_bytes = (audio_bits + 7) / 8;
    bytes[0] = static_cast<std::byte>(audio_bytes >> 7U);
    bytes[1] = static_cast<std::byte>((audio_bytes << 1U) & 0xFEU);
    return bytes;
}

// A direct-coded substream of one dynamic object: audio_data_objs(1, 0) is a
// SIMPLE single_channel_element().
std::vector<std::byte> object_audio() {
    BitWriter w;
    w.put(3, 15);  // audio_size_value: 3 bytes
    w.flag(false);
    w.put(0, 1);  // mono_codec_mode: SIMPLE
    mono_track(w);
    while (w.size() < 16 + 24) {
        w.flag(false);  // fill_bits
    }
    object_metadata(w);
    w.align();
    return w.bytes();
}

// The OAMD substream of that group: common data with headphone data in its
// add_data, the timing of two blocks, and the direct-coded object's two
// blocks, the second a partial reuse with a differential position.
std::vector<std::byte> oamd_substream() {
    BitWriter w;
    w.flag(true);     // b_oamd_common_data_present
    w.flag(false);    // b_default_screen_size_ratio
    w.put(9, 5);      // master_screen_size_ratio_code
    w.flag(true);     // b_bed_object_chan_distribute
    w.flag(true);     // b_additional_data
    w.put(0, 1);      // add_data_bytes_minus1: one byte
    w.flag(false);    // trim(): b_trim_present
    w.flag(false);    // bed_render_info(): b_bed_render_info
    w.flag(true);     // headphone(): b_headphone
    w.put(0b001, 3);  // hp_operation_mode
    w.flag(true);     // b_head_track_disable_all
    w.put(0, 1);      // add_data: the byte's last bit
    w.flag(true);     // b_oamd_timing_present
    w.put(0b11, 2);   // oa_sample_offset_type
    w.put(17, 5);     // oa_sample_offset
    w.put(2, 3);      // num_obj_info_blocks
    w.put(3, 6);      // block_offset_factor
    w.put(0b11, 2);   // ramp_duration_code
    w.flag(true);     // b_use_ramp_table
    w.put(10, 4);     // ramp_duration_table: 1 601
    w.put(40, 6);     // block_offset_factor
    w.put(0b01, 2);   // ramp_duration_code: 512
    // oamd_dyndata_multi(): the A-JOC object is passed over; the direct-coded
    // one's two blocks, the first with b_no_delta (b_oamd_ndot).
    w.flag(false);     // b_object_not_active
    w.flag(false);     // b_default_basic_info_md
    w.put(0b10, 2);    // basic_info_md: gain and priority
    w.put(0, 1);       // object_gain_code 0b0
    w.put(20, 6);      // object_gain_value
    w.put(31, 5);      // object_priority_code
    w.put(10, 6);      // pos3D_X
    w.put(20, 6);      // pos3D_Y
    w.flag(false);     // pos3D_Z_sign
    w.put(5, 4);       // pos3D_Z
    w.flag(false);     // b_grouped_zone_defaults
    w.put(0b101, 3);   // group_zone_flag[]: zone_mask sent, snap
    w.put(4, 3);       // zone_mask
    w.flag(false);     // b_grouped_other_defaults
    w.put(0b1111, 4);  // group_other_mask
    w.put(1, 1);       // object_width_mode
    w.put(1, 5);       // object_width_X_code
    w.put(2, 5);       // object_width_Y_code
    w.put(3, 5);       // object_width_Z_code
    w.put(2, 3);       // object_screen_factor_code
    w.put(1, 2);       // object_depth_factor
    w.flag(false);     // b_obj_at_infinity
    w.put(6, 4);       // obj_distance_factor_code
    w.put(0b10, 2);    // object_div_mode
    w.put(26, 6);      // object_div_code
    w.flag(true);      // b_add_table_data
    w.put(1, 4);       // add_table_data_size_minus1: two bytes
    w.flag(true);      // b_obj_trim_disable
    w.flag(true);      // b_ext_prec_pos
    w.put(0b110, 3);   // ext_prec_pos_presence[]: X and Y
    w.put(1, 2);       // ext_prec_pos3D_X
    w.put(3, 2);       // ext_prec_pos3D_Y
    w.flag(false);     // b_headphone
    w.put(0, 6);       // add_table_data: the two bytes' last 6 bits
    // The second block.
    w.flag(false);    // b_object_not_active
    w.flag(true);     // b_basic_info_reuse
    w.flag(false);    // b_render_info_reuse
    w.flag(true);     // b_render_info_partial_reuse
    w.flag(false);    // b_obj_render_otherprops_present
    w.flag(false);    // b_obj_render_zone_present
    w.flag(true);     // b_obj_render_position_present
    w.flag(true);     // b_diff_pos_coding
    w.put(0b011, 3);  // diff_pos3D_X: +3
    w.put(0b100, 3);  // diff_pos3D_Y: -4
    w.put(0b000, 3);  // diff_pos3D_Z
    w.flag(false);    // b_add_table_data
    w.align();
    return w.bytes();
}

}  // namespace

TEST_CASE("an object group of A-JOC and direct-coded substreams and OAMD reads each to its end",
          "[ac4dec][frames]") {
    BitWriter toc;
    ac4_toc_test::toc_start(toc, {});
    PresV1 p;
    p.presentation_substream = 3;
    ac4_toc_test::presentation_v1(toc, p);
    toc.flag(true);   // b_substreams_present
    toc.flag(false);  // b_hsf_ext
    toc.flag(false);  // b_single_substream
    toc.put(0, 2);    // two substreams
    toc.flag(false);  // b_channel_coded
    toc.flag(true);   // b_oamd_substream
    toc.flag(true);   // b_oamd_ndot
    ac4_toc_test::substream_index(toc, 2);
    toc.flag(true);   // b_ajoc
    toc.flag(false);  // b_lfe
    toc.flag(false);  // b_static_dmx
    toc.put(0, 4);    // n_fullband_dmx_signals_minus1: one signal
    toc.flag(true);   // bed_dyn_obj_assignment(): b_dyn_objects_only
    toc.flag(false);  // b_oamd_common_data_present
    toc.put(0, 4);    // n_fullband_upmix_signals_minus1: one object
    toc.flag(true);   // b_dyn_objects_only
    toc.flag(false);  // b_sf_multiplier
    toc.flag(false);  // b_bitrate_info
    toc.flag(true);   // b_audio_ndot
    ac4_toc_test::substream_index(toc, 0);
    toc.flag(false);  // b_ajoc: a direct-coded substream
    toc.put(1, 3);    // n_objects_code: one object
    toc.flag(true);   // b_dynamic_objects
    toc.flag(false);  // b_lfe
    toc.flag(false);  // b_sf_multiplier
    toc.flag(false);  // b_bitrate_info
    toc.flag(true);   // b_audio_ndot
    ac4_toc_test::substream_index(toc, 1);
    toc.flag(false);  // b_content_type
    const std::vector<std::vector<std::byte>> substreams = {
        ajoc_audio(), object_audio(), oamd_substream(), presentation(1, true)};
    ac4_toc_test::index_table(toc, ac4_toc_test::sizes_of(substreams));
    toc.align();

    std::vector<ac4::SyntaxRecord> records;
    // A named callable: the sink refers to it and does not own it.
    const auto keep = [&records](const ac4::SyntaxRecord& record) { records.push_back(record); };
    ac4::DecoderConfig config;
    config.syntax = keep;
    ac4::Decoder decoder(config);
    const auto report = decoder.parse(ac4_toc_test::assemble(toc, substreams));
    REQUIRE(report.has_value());
    REQUIRE(report->substreams.size() == 4);
    check_read(find(*report, 0), SubstreamReport::Kind::kAudio);
    check_read(find(*report, 1), SubstreamReport::Kind::kAudio);
    check_read(find(*report, 2), SubstreamReport::Kind::kOamd);
    check_read(find(*report, 3), SubstreamReport::Kind::kPresentation);

    const auto value_of = [&records](int substream, std::string_view name,
                                     int nth = 0) -> std::optional<std::uint64_t> {
        for (const ac4::SyntaxRecord& record : records) {
            if (record.substream == substream && record.name == name && nth-- == 0) {
                return record.value;
            }
        }
        return std::nullopt;
    };
    // The prefix codes, one record of the bits read (src/ac4dec/ERRATA.md,
    // "Prefix codes in the trace").
    CHECK(value_of(2, "oa_sample_offset_type") == 0b11);
    CHECK(value_of(2, "basic_info_md") == 0b10);
    CHECK(value_of(2, "object_gain_code") == 0b0);
    CHECK(value_of(2, "ramp_duration_table") == 10);
    CHECK(value_of(2, "zone_mask") == 4);
    CHECK(value_of(2, "ext_prec_pos3D_Y") == 3);
    CHECK(value_of(2, "diff_pos3D_Y") == 0b100);
    CHECK(value_of(2, "b_head_track_disable_all") == 1);
    // The A-JOC substream's two portions, the second on the first's timing.
    CHECK(value_of(0, "pos3D_X", 0) == 31);
    CHECK(value_of(0, "pos3D_X", 1) == 62);
    CHECK(value_of(0, "ajoc_hcw") == 25);
    CHECK(value_of(0, "b_derive_timing_from_dmx") == 1);
}

TEST_CASE("a substream named by two object elements is read as the first names it",
          "[ac4dec][frames]") {
    BitWriter toc;
    ac4_toc_test::toc_start(toc, {});
    PresV1 p;
    ac4_toc_test::presentation_v1(toc, p);
    toc.flag(true);    // b_substreams_present
    toc.flag(false);   // b_hsf_ext
    toc.flag(true);    // b_single_substream
    toc.flag(false);   // b_channel_coded
    toc.flag(true);    // b_oamd_substream
    toc.flag(false);   // b_oamd_ndot
    ac4_toc_test::substream_index(toc, 0);
    toc.flag(false);   // b_ajoc: an object substream, also substream 0
    toc.put(1, 3);
    toc.flag(true);    // b_dynamic_objects
    toc.flag(false);
    toc.flag(false);
    toc.flag(false);
    toc.flag(true);
    ac4_toc_test::substream_index(toc, 0);
    toc.flag(false);   // b_content_type
    const std::vector<std::vector<std::byte>> substreams = {std::vector<std::byte>(2, std::byte{0}),
                                                            presentation(1, true)};
    ac4_toc_test::index_table(toc, ac4_toc_test::sizes_of(substreams));
    toc.align();
    const auto report = decode(ac4_toc_test::assemble(toc, substreams));
    REQUIRE(report.substreams.size() == 2);
    // The OAMD substream's element comes first: substream 0 is read as one,
    // whose two zero bytes send no timing for its oamd_dyndata_multi().
    CHECK(find(report, 0).kind == SubstreamReport::Kind::kOamd);
    check_refused(find(report, 0), DecodeError::kMissingIFrame);
    CHECK(find(report, 0).refused_reason.find("oamd_timing_data") != std::string_view::npos);
}
