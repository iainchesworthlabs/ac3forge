// The AC-4 decoder's presentation substream syntax
// (src/ac4dec/src/syntax/presentation.cpp): what presentation_context_v1()
// derives from a table of contents, and ac4_presentation_substream() with its
// targets, additional data, substream group gains, custom downmix data and
// loudness corrections, on hand-built bitstreams. The committed DEE streams
// all carry a single stereo, 5.1 or IMS group, so most of this syntax is
// reached nowhere else.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4/ac4.hpp"
#include "ac4dec_bits.hpp"
#include "bit_reader.hpp"
#include "syntax/context.hpp"
#include "syntax/presentation.hpp"

namespace {

using ac4::DecodeError;
using ac4::detail::BitReader;
using ac4::detail::ParseResult;
using ac4::detail::PresentationContext;
using ac4::detail::PresentationSubstream;
using ac4::detail::PresentationSubstreamState;
using ac4dec_test::BitWriter;
using ac4dec_test::Recorder;
namespace ch_mode = ac4::detail::ch_mode;

// Reads `w` as a presentation substream. On success the reader must stop at
// the end of `w`, which every case byte-aligns as the syntax does.
ParseResult read_presentation(const BitWriter& w, const PresentationContext& ctx, PresentationSubstreamState& state,
                              PresentationSubstream& out, Recorder& rec) {
    const std::vector<std::byte> bytes = w.bytes();
    BitReader reader(bytes, 0, rec);
    const ParseResult result = ac4::detail::parse_presentation_substream(reader, ctx, state, out);
    if (result) {
        CHECK(reader.position() == w.size());
    }
    return result;
}

PresentationContext stereo_context() {
    PresentationContext ctx;
    ctx.pres_ch_mode = ch_mode::kStereo;
    ctx.n_substream_groups = 1;
    ctx.n_substreams_in_presentation = 1;
    ctx.frame_len_base = 2048;
    return ctx;
}

// dialnorm_bits, no further loudness, and a drc_frame() without DRC.
void put_loudness_and_no_drc(BitWriter& w, int dialnorm = 20) {
    w.put(static_cast<std::uint64_t>(dialnorm), 7);
    w.flag(false);  // b_further_loudness_info
    w.put(1, 5);    // drc_metadata_size_value: the one bit below
    w.flag(false);  // b_more_bits
    w.flag(false);  // b_drc_present
}

ac4::ChannelSubstreamInfo chan(int mode) {
    ac4::ChannelSubstreamInfo info;
    info.ch_mode = mode;
    return info;
}

ac4::GroupSubstream chan_substream(int mode) {
    ac4::GroupSubstream sub;
    sub.kind = ac4::GroupSubstream::Kind::kChan;
    sub.chan = chan(mode);
    return sub;
}

ac4::SubstreamGroupInfo group_of(std::vector<ac4::GroupSubstream> substreams) {
    ac4::SubstreamGroupInfo group;
    group.b_substreams_present = true;
    group.substreams = std::move(substreams);
    return group;
}

}  // namespace

// --- superset() and presentation_context_v1() ----------------------------

TEST_CASE("superset_ch_mode gives the lowest mode holding both", "[ac4dec][presentation]") {
    using ac4::detail::superset_ch_mode;
    using ac4::detail::superset_ch_mode_core;
    CHECK(superset_ch_mode(-1, 4) == 4);
    CHECK(superset_ch_mode(3, -1) == 3);
    CHECK(superset_ch_mode(0, 1) == 1);
    CHECK(superset_ch_mode(1, 0) == 1);
    CHECK(superset_ch_mode(0, 2) == 2);
    CHECK(superset_ch_mode(3, 4) == 4);
    CHECK(superset_ch_mode(ch_mode::k7_0_340, ch_mode::k7_0_322) == ch_mode::k7_0_4);
    CHECK(superset_ch_mode(ch_mode::k7_0_340, ch_mode::k7_0_520) == ch_mode::k22_2);
    CHECK(superset_ch_mode(ch_mode::k9_0_4, ch_mode::k22_2) == -1);  // no mode holds Lscr and 22.2
    CHECK(superset_ch_mode(2, 16) == -1);
    CHECK(superset_ch_mode_core(3, 4) == 4);
    CHECK(superset_ch_mode_core(4, 5) == 6);
    CHECK(superset_ch_mode_core(-1, 5) == 5);
    CHECK(superset_ch_mode_core(7, 3) == -1);
}

TEST_CASE("presentation_context_v1 counts substream groups by presentation_config", "[ac4dec][presentation]") {
    ac4::Toc toc;
    toc.frame_rate_index = 13;
    toc.substream_groups = {group_of({chan_substream(ch_mode::kStereo)}),
                            group_of({chan_substream(ch_mode::kMono)}),
                            group_of({chan_substream(ch_mode::kMono)})};
    ac4::PresentationInfoV1 p;
    p.group_refs = {0, 1, 2};

    struct Case {
        std::optional<int> config;
        int groups;
    };
    for (const Case c : {Case{std::nullopt, 1}, Case{0, 2}, Case{1, 1}, Case{2, 2}, Case{3, 3}, Case{4, 2},
                         Case{5, 3}, Case{7, 0}}) {
        p.presentation_config = c.config;
        const PresentationContext ctx = ac4::detail::presentation_context_v1(toc, p);
        INFO("presentation_config " << c.config.value_or(-1));
        CHECK(ctx.n_substream_groups == c.groups);
        // The helpers run over every group named, whatever the count.
        CHECK(ctx.n_substreams_in_presentation == 3);
        CHECK(ctx.pres_ch_mode == ch_mode::kStereo);
        CHECK(ctx.frame_len_base == 2048);
    }
}

TEST_CASE("presentation_context_v1 derives the channel helpers of Part 2 6.3.3.1", "[ac4dec][presentation]") {
    ac4::Toc toc;
    toc.frame_rate_index = 3;  // 1536 at 48 kHz

    SECTION("7.0.4 and 7.1.4 substreams, with their content flags") {
        auto a = chan_substream(ch_mode::k7_0_4);
        a.chan->original_content = ac4::OriginalContent{true, true, 1};
        auto b = chan_substream(ch_mode::k7_1_4);
        b.chan->original_content = ac4::OriginalContent{false, true, 3};
        auto reserved = chan_substream(0);
        reserved.chan->ch_mode.reset();  // a reserved channel_mode adds nothing
        toc.substream_groups = {group_of({a}), group_of({b, reserved})};
        ac4::PresentationInfoV1 p;
        p.group_refs = {0, 1, 1, 7, -1};  // a repeat and two groups the TOC does not hold
        p.b_alternative = true;
        p.b_pres_ndot = true;
        const PresentationContext ctx = ac4::detail::presentation_context_v1(toc, p);
        CHECK(ctx.b_alternative);
        CHECK(ctx.b_pres_ndot);
        CHECK(ctx.n_substreams_in_presentation == 3);
        CHECK(ctx.pres_ch_mode == ch_mode::k7_1_4);
        CHECK(ctx.pres_ch_mode_core == 6);
        CHECK(ctx.b_pres_4_back_channels_present);
        CHECK(ctx.pres_top_channel_pairs == 2);
        CHECK(ctx.b_pres_has_lfe);
        CHECK(ctx.frame_len_base == 1536);
    }
    SECTION("9.0.4 alone takes the 5.0.2 core") {
        auto a = chan_substream(ch_mode::k9_0_4);
        a.chan->original_content = ac4::OriginalContent{false, false, 2};
        toc.substream_groups = {group_of({a})};
        ac4::PresentationInfoV1 p;
        p.group_refs = {0};
        const PresentationContext ctx = ac4::detail::presentation_context_v1(toc, p);
        CHECK(ctx.pres_ch_mode == ch_mode::k9_0_4);
        CHECK(ctx.pres_ch_mode_core == 5);
        CHECK(ctx.pres_top_channel_pairs == 1);
        CHECK_FALSE(ctx.b_pres_4_back_channels_present);
        CHECK_FALSE(ctx.b_pres_has_lfe);
    }
    SECTION("an A-JOC substream with a static downmix leaves only a core") {
        ac4::GroupSubstream ajoc;
        ajoc.kind = ac4::GroupSubstream::Kind::kAjoc;
        ajoc.ajoc = ac4::AjocSubstreamInfo{};
        ajoc.ajoc->b_static_dmx = true;
        ajoc.ajoc->b_lfe = true;
        toc.substream_groups = {group_of({chan_substream(ch_mode::kStereo), ajoc})};
        ac4::PresentationInfoV1 p;
        p.group_refs = {0};
        const PresentationContext ctx = ac4::detail::presentation_context_v1(toc, p);
        CHECK(ctx.pres_ch_mode == -1);
        CHECK(ctx.pres_ch_mode_core == 4);
        CHECK(ctx.b_pres_has_lfe);
    }
    SECTION("an adaptive A-JOC or an object substream leaves neither") {
        ac4::GroupSubstream ajoc;
        ajoc.kind = ac4::GroupSubstream::Kind::kAjoc;
        ajoc.ajoc = ac4::AjocSubstreamInfo{};
        ac4::GroupSubstream obj;
        obj.kind = ac4::GroupSubstream::Kind::kObj;
        auto static_core = ajoc;
        static_core.ajoc->b_static_dmx = true;
        toc.substream_groups = {group_of({ajoc}), group_of({obj}), group_of({static_core})};
        for (const int group : {0, 1}) {
            ac4::PresentationInfoV1 p;
            p.group_refs = {2, group};
            const PresentationContext ctx = ac4::detail::presentation_context_v1(toc, p);
            INFO("group " << group);
            CHECK(ctx.pres_ch_mode == -1);
            CHECK(ctx.pres_ch_mode_core == -1);
            CHECK_FALSE(ctx.b_pres_has_lfe);
        }
    }
    SECTION("a reserved frame rate index leaves no frame length") {
        toc.sample_rate_hz = 44100;
        toc.substream_groups = {group_of({chan_substream(ch_mode::kStereo)})};
        ac4::PresentationInfoV1 p;
        p.group_refs = {0};
        CHECK(ac4::detail::presentation_context_v1(toc, p).frame_len_base == 0);
    }
}

// --- ac4_presentation_substream() -----------------------------------------

TEST_CASE("a stereo presentation substream with nothing optional reads 17 bits", "[ac4dec][presentation]") {
    BitWriter w;
    w.flag(false);  // b_additional_data
    put_loudness_and_no_drc(w, 31);
    w.flag(false);  // b_associated
    w.align();
    PresentationSubstreamState state;
    PresentationSubstream out;
    Recorder rec;
    REQUIRE(read_presentation(w, stereo_context(), state, out, rec).has_value());
    CHECK(out.dialnorm_bits == 31);
    CHECK(out.drc_metadata_size == 1U);
    CHECK_FALSE(out.drc.b_drc_present);
    CHECK(rec.end_bit() == 17);
}

TEST_CASE("an alternative presentation substream reads its name and targets", "[ac4dec][presentation]") {
    PresentationContext ctx = stereo_context();
    ctx.b_alternative = true;
    ctx.n_substreams_in_presentation = 3;

    BitWriter w;
    w.flag(true);   // b_name_present
    w.flag(true);   // b_length
    w.put(2, 5);    // name_len
    w.put('H', 8);
    w.put('i', 8);
    w.put(3, 2);    // n_targets_minus1 3: escaped
    w.variable_bits(1, 2);  // five targets
    for (int t = 0; t < 5; ++t) {
        w.put(static_cast<std::uint64_t>(t), 3);   // target_level
        w.put(0b1000 | static_cast<std::uint64_t>(t), 4);  // target_device_category
        w.flag(t == 0);                            // b_tdc_extension
        if (t == 0) {
            w.put(9, 4);                           // reserved_bits
        }
        w.flag(t == 1);                            // b_ducking_depth_present
        if (t == 1) {
            w.put(33, 6);
        }
        w.flag(t == 2);                            // b_loud_corr_target
        if (t == 2) {
            w.put(17, 5);
        }
        // Three substreams: inactive, alt_data_set_index 0, and 1 + 2.
        w.flag(false);
        w.flag(true);
        w.put(0, 1);
        w.flag(true);
        w.put(1, 1);
        w.variable_bits(2, 2);
    }
    w.flag(false);  // b_additional_data
    put_loudness_and_no_drc(w);
    w.flag(false);  // b_associated
    w.align();

    PresentationSubstreamState state;
    PresentationSubstream out;
    Recorder rec;
    REQUIRE(read_presentation(w, ctx, state, out, rec).has_value());
    CHECK(out.b_name_present);
    CHECK(out.presentation_name == std::vector<std::uint8_t>{'H', 'i'});
    REQUIRE(out.targets.size() == 5);
    CHECK(out.targets[0].reserved_bits == 9);
    CHECK(out.targets[1].max_ducking_depth == 33);
    CHECK(out.targets[2].loud_corr_target == 17);
    CHECK(out.targets[4].target_level == 4);
    CHECK(out.targets[4].target_device_category == 0b1100);
    for (const auto& target : out.targets) {
        REQUIRE(target.alt_data_set_index.size() == 3);
        CHECK_FALSE(target.alt_data_set_index[0].has_value());
        CHECK(target.alt_data_set_index[1] == 0U);
        CHECK(target.alt_data_set_index[2] == 3U);
    }
}

TEST_CASE("an alternative presentation's default-length name and a target count no data backs",
          "[ac4dec][presentation]") {
    PresentationContext ctx = stereo_context();
    ctx.b_alternative = true;

    SECTION("without b_length the name is 32 bytes") {
        BitWriter w;
        w.flag(true);   // b_name_present
        w.flag(false);  // b_length
        for (int i = 0; i < 32; ++i) {
            w.put(static_cast<std::uint64_t>('a' + i % 26), 8);
        }
        w.put(0, 2);    // one target
        w.put(0, 3);
        w.put(0, 4);
        w.flag(false);
        w.flag(false);
        w.flag(false);
        w.flag(false);  // b_active
        w.flag(false);  // b_additional_data
        put_loudness_and_no_drc(w);
        w.flag(false);
        w.align();
        PresentationSubstreamState state;
        PresentationSubstream out;
        Recorder rec;
        REQUIRE(read_presentation(w, ctx, state, out, rec).has_value());
        REQUIRE(out.presentation_name.size() == 32);
        CHECK(out.presentation_name[31] == 'a' + 5);
        CHECK(rec.count("presentation_name") == 32);
    }
    SECTION("a huge escaped target count runs out of data") {
        BitWriter w;
        w.flag(false);  // b_name_present
        w.put(3, 2);
        w.variable_bits(1'000'000, 2);
        w.put(0, 24);
        PresentationSubstreamState state;
        PresentationSubstream out;
        Recorder rec;
        const auto result = read_presentation(w, ctx, state, out, rec);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
        CHECK(out.targets.size() < 10);
    }
}

TEST_CASE("the presentation substream's additional data carries advanced dialogue enhancement",
          "[ac4dec][presentation]") {
    // add_data_bytes_minus1 then the byte-aligned fields inside it: the
    // add_data run fills what they leave.
    const auto additional = [](BitWriter& w, bool object_mode, bool advanced, bool config, int thresh) {
        w.flag(true);   // b_additional_data
        w.put(1, 4);    // add_data_bytes_minus1: two bytes
        w.align();
        const std::size_t start = w.size();
        w.flag(true);   // immersive_audio_indicator
        if (object_mode) {
            w.flag(true);  // b_oamd_common_timing
        }
        w.flag(advanced);  // b_advanced_de_data_present
        if (advanced) {
            w.flag(config);  // b_advanced_de_config_present
            if (config) {
                w.put(10, 6);
                w.put(20, 6);
                w.put(7, 4);
            }
            w.put(static_cast<std::uint64_t>(thresh & 0x3F), 6);
            w.put(19, 5);  // advanced_de_compr_gain
        }
        while (w.size() < start + 16) {
            w.flag(true);  // add_data
        }
    };

    SECTION("a configuration in an I-frame, kept by a later frame, dropped by the next I-frame") {
        PresentationContext ctx = stereo_context();
        ctx.b_pres_ndot = true;
        // The configuration alone runs past two bytes, so this one takes four.
        BitWriter w;
        w.flag(true);
        w.put(3, 4);    // add_data_bytes_minus1: four bytes
        w.align();
        const std::size_t start = w.size();
        w.flag(false);  // immersive_audio_indicator
        w.flag(true);   // b_advanced_de_data_present
        w.flag(true);   // b_advanced_de_config_present
        w.put(10, 6);
        w.put(20, 6);
        w.put(7, 4);
        w.put(0b111110, 6);  // advanced_de_compr_thresh: -2
        w.put(19, 5);
        const std::size_t fields = w.size() - start;
        for (std::size_t i = fields; i < 32; ++i) {
            w.flag(i % 2 == 0);
        }
        put_loudness_and_no_drc(w);
        w.flag(false);
        w.align();
        PresentationSubstreamState state;
        PresentationSubstream out;
        Recorder rec;
        REQUIRE(read_presentation(w, ctx, state, out, rec).has_value());
        CHECK(out.add_data_bytes == 4U);
        CHECK(out.add_data_bits == 32 - fields);
        REQUIRE(out.advanced_de_data.has_value());
        REQUIRE(out.advanced_de_data->config.has_value());
        CHECK(out.advanced_de_data->config->advanced_de_compr_tc_attack == 10);
        CHECK(out.advanced_de_data->config->advanced_de_compr_tc_release == 20);
        CHECK(out.advanced_de_data->config->advanced_de_compr_ratio == 7);
        CHECK(out.advanced_de_data->advanced_de_compr_thresh == -2);
        CHECK(out.advanced_de_data->advanced_de_compr_gain == 19);
        REQUIRE(state.advanced_de_config.has_value());

        // Not an I-frame: the configuration is kept.
        PresentationContext later = stereo_context();
        BitWriter next;
        additional(next, false, true, false, 5);
        put_loudness_and_no_drc(next);
        next.flag(false);
        next.align();
        PresentationSubstream kept;
        REQUIRE(read_presentation(next, later, state, kept, rec).has_value());
        REQUIRE(kept.advanced_de_data.has_value());
        REQUIRE(kept.advanced_de_data->config.has_value());
        CHECK(kept.advanced_de_data->config->advanced_de_compr_ratio == 7);
        CHECK(kept.advanced_de_data->advanced_de_compr_thresh == 5);
        CHECK(kept.immersive_audio_indicator);

        // An I-frame with advanced DE but no configuration resets it.
        BitWriter reset;
        additional(reset, false, true, false, 0);
        put_loudness_and_no_drc(reset);
        reset.flag(false);
        reset.align();
        PresentationSubstream cleared;
        REQUIRE(read_presentation(reset, ctx, state, cleared, rec).has_value());
        REQUIRE(cleared.advanced_de_data.has_value());
        CHECK_FALSE(cleared.advanced_de_data->config.has_value());
        CHECK_FALSE(state.advanced_de_config.has_value());
    }
    SECTION("an I-frame without advanced DE disables it") {
        PresentationSubstreamState state;
        state.advanced_de_config = ac4::detail::AdvancedDeConfig{1, 2, 3};
        PresentationContext ctx = stereo_context();
        ctx.b_pres_ndot = true;
        BitWriter w;
        w.flag(false);
        put_loudness_and_no_drc(w);
        w.flag(false);
        w.align();
        PresentationSubstream out;
        Recorder rec;
        REQUIRE(read_presentation(w, ctx, state, out, rec).has_value());
        CHECK_FALSE(state.advanced_de_config.has_value());
    }
    SECTION("an object presentation reads b_oamd_common_timing") {
        PresentationContext ctx = stereo_context();
        ctx.pres_ch_mode = -1;
        BitWriter w;
        additional(w, true, false, false, 0);
        put_loudness_and_no_drc(w);
        w.flag(false);  // b_associated
        // loud_corr(): b_obj_loud_corr 0 and nothing after it.
        w.flag(false);
        w.align();
        PresentationSubstreamState state;
        PresentationSubstream out;
        Recorder rec;
        REQUIRE(read_presentation(w, ctx, state, out, rec).has_value());
        CHECK(out.b_oamd_common_timing == true);
        CHECK(out.add_data_bits == 13U);
    }
    SECTION("escaped add_data_bytes") {
        PresentationContext ctx = stereo_context();
        BitWriter w;
        w.flag(true);
        w.put(15, 4);           // add_data_bytes_minus1 15: escaped
        w.variable_bits(1, 2);  // 17 bytes
        w.align();
        for (int i = 0; i < 17 * 8; ++i) {
            w.flag(false);      // immersive_audio_indicator, b_advanced_de_data_present, add_data
        }
        put_loudness_and_no_drc(w);
        w.flag(false);
        w.align();
        PresentationSubstreamState state;
        PresentationSubstream out;
        Recorder rec;
        REQUIRE(read_presentation(w, ctx, state, out, rec).has_value());
        CHECK(out.add_data_bytes == 17U);
        CHECK(out.add_data_bits == 17U * 8 - 2);
    }
    SECTION("add_data_bytes too small for the fields inside it") {
        PresentationContext ctx = stereo_context();
        BitWriter w;
        w.flag(true);
        w.put(0, 4);     // one byte
        w.align();
        w.flag(false);
        w.flag(true);    // b_advanced_de_data_present
        w.flag(true);    // with a configuration: 29 bits
        w.put(0, 32);
        PresentationSubstreamState state;
        PresentationSubstream out;
        Recorder rec;
        const auto result = read_presentation(w, ctx, state, out, rec);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kInvalidStream);
    }
    SECTION("add_data running past the end of the substream") {
        PresentationContext ctx = stereo_context();
        BitWriter w;
        w.flag(true);
        w.put(14, 4);    // fifteen bytes, and two follow
        w.align();
        w.put(0, 16);
        PresentationSubstreamState state;
        PresentationSubstream out;
        Recorder rec;
        const auto result = read_presentation(w, ctx, state, out, rec);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
        CHECK(rec.count("add_data") == 0);
    }
}

TEST_CASE("the presentation substream reads its further loudness info and a DRC frame of its own",
          "[ac4dec][presentation]") {
    PresentationContext ctx = stereo_context();
    ctx.b_pres_ndot = true;
    // A drc_frame() of one compression-curve mode, long enough (34 bits) that
    // drc_metadata_size needs its b_more_bits.
    BitWriter curve;
    curve.flag(true);
    curve.put(0, 3);
    curve.put(0, 3);
    curve.flag(false);
    curve.flag(false);
    curve.flag(true);   // drc_compression_curve_flag
    curve.put(0, 4);
    curve.put(0, 4);
    curve.put(0, 4);    // drc_gain_max_boost 0
    curve.put(0, 5);    // drc_gain_max_cut 0
    curve.flag(true);   // drc_tc_default_flag
    curve.put(0, 3);
    curve.flag(true);   // drc_reset_flag
    curve.put(0, 2);
    REQUIRE(curve.size() >= 32);

    BitWriter w;
    w.flag(false);   // b_additional_data
    w.put(40, 7);    // dialnorm_bits
    w.flag(true);    // b_further_loudness_info: the presentation form
    w.put(0, 2);
    w.put(1, 4);     // loud_prac_type 1
    w.flag(false);   // b_loudcorr_dialgate
    w.flag(false);   // b_loudcorr_type
    for (int i = 0; i < 6; ++i) {
        w.flag(false);
    }
    w.flag(false);   // b_prgmbndy
    w.flag(false);   // b_lra
    w.flag(false);
    w.flag(false);
    w.flag(false);   // b_rtllcomp
    w.flag(false);   // b_extension
    const std::uint64_t size = curve.size();
    w.put(size & 0x1FU, 5);  // drc_metadata_size_value
    w.flag(true);            // b_more_bits
    w.variable_bits(size >> 5U, 3);
    w.append(curve);
    w.flag(false);   // b_associated
    w.align();

    PresentationSubstreamState state;
    PresentationSubstream out;
    Recorder rec;
    REQUIRE(read_presentation(w, ctx, state, out, rec).has_value());
    CHECK(out.dialnorm_bits == 40);
    REQUIRE(out.further_loudness_info.has_value());
    CHECK(out.further_loudness_info->loud_prac_type == 1);
    CHECK(out.drc_metadata_size == size);
    CHECK(out.drc.b_drc_present);
    CHECK(out.drc.drc_reset_flag);
    CHECK(state.drc.config_valid);

    SECTION("a drc_metadata_size that disagrees fails") {
        BitWriter bad;
        bad.flag(false);
        bad.put(40, 7);
        bad.flag(false);
        bad.put(2, 5);    // two bits, and drc_frame() takes one
        bad.flag(false);
        bad.flag(false);  // b_drc_present
        bad.put(0, 8);
        PresentationSubstream again;
        const auto result = read_presentation(bad, ctx, state, again, rec);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kInvalidStream);
    }
    SECTION("a DRC frame this presentation's context cannot read fails") {
        BitWriter bad;
        bad.flag(false);
        bad.put(40, 7);
        bad.flag(false);
        bad.put(1, 5);
        bad.flag(false);
        bad.flag(true);   // b_drc_present, not an I-frame, no configuration
        bad.put(0, 8);
        PresentationContext later = stereo_context();
        PresentationSubstreamState fresh;
        PresentationSubstream again;
        const auto result = read_presentation(bad, later, fresh, again, rec);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kMissingIFrame);
    }
}

TEST_CASE("the presentation substream reads substream group gains and keeps them", "[ac4dec][presentation]") {
    PresentationContext ctx = stereo_context();
    ctx.n_substream_groups = 3;
    PresentationSubstreamState state;
    Recorder rec;

    BitWriter first;
    first.flag(false);
    put_loudness_and_no_drc(first);
    first.flag(true);    // b_substream_group_gains_present
    first.flag(false);   // b_keep
    first.put(10, 6);
    first.put(20, 6);
    first.put(63, 6);
    first.flag(false);   // b_associated
    first.align();
    PresentationSubstream out;
    REQUIRE(read_presentation(first, ctx, state, out, rec).has_value());
    CHECK(out.sg_gain == std::vector<int>{10, 20, 63});

    BitWriter kept;
    kept.flag(false);
    put_loudness_and_no_drc(kept);
    kept.flag(true);
    kept.flag(true);     // b_keep
    kept.flag(false);
    kept.align();
    PresentationSubstream again;
    REQUIRE(read_presentation(kept, ctx, state, again, rec).has_value());
    CHECK(again.b_keep);
    CHECK(again.sg_gain == std::vector<int>{10, 20, 63});

    // Kept for a different number of groups: nothing to repeat, so 0 dB.
    PresentationContext two = ctx;
    two.n_substream_groups = 2;
    PresentationSubstream fewer;
    REQUIRE(read_presentation(kept, two, state, fewer, rec).has_value());
    CHECK(fewer.sg_gain == std::vector<int>{0, 0});

    BitWriter absent;
    absent.flag(false);
    put_loudness_and_no_drc(absent);
    absent.flag(false);  // b_substream_group_gains_present
    absent.flag(false);
    absent.align();
    PresentationSubstream none;
    REQUIRE(read_presentation(absent, ctx, state, none, rec).has_value());
    CHECK(none.sg_gain.empty());

    SECTION("gains cut off by the end of the substream") {
        BitWriter cut;
        cut.flag(false);
        put_loudness_and_no_drc(cut);
        cut.flag(true);
        cut.flag(false);
        cut.put(1, 6);   // one of three, then the data ends
        PresentationContext many = ctx;
        many.n_substream_groups = 40;
        PresentationSubstream truncated;
        const auto result = read_presentation(cut, many, state, truncated, rec);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
    }
}

TEST_CASE("the presentation substream reads an associated presentation's scaling", "[ac4dec][presentation]") {
    BitWriter w;
    w.flag(false);
    put_loudness_and_no_drc(w);
    w.flag(true);    // b_associated
    w.flag(true);
    w.put(1, 8);     // scale_main
    w.flag(true);
    w.put(2, 8);     // scale_main_centre
    w.flag(true);
    w.put(3, 8);     // scale_main_front
    w.flag(true);    // b_associate_is_mono
    w.put(4, 8);     // pan_associated
    w.align();
    PresentationSubstreamState state;
    PresentationSubstream out;
    Recorder rec;
    REQUIRE(read_presentation(w, stereo_context(), state, out, rec).has_value());
    CHECK(out.b_associated);
    CHECK(out.scale_main == 1);
    CHECK(out.scale_main_centre == 2);
    CHECK(out.scale_main_front == 3);
    CHECK(out.b_associate_is_mono);
    CHECK(out.pan_associated == 4);
}

// --- custom_dmx_data() and loud_corr() -----------------------------------

namespace {

PresentationContext immersive_context(int mode, int top_pairs, bool back) {
    PresentationContext ctx = stereo_context();
    ctx.pres_ch_mode = mode;
    ctx.pres_top_channel_pairs = top_pairs;
    ctx.b_pres_4_back_channels_present = back;
    ctx.b_pres_has_lfe = mode == ch_mode::k7_1_4 || mode == ch_mode::k9_1_4;
    return ctx;
}

// Everything before custom_dmx_data().
void put_head(BitWriter& w) {
    w.flag(false);
    put_loudness_and_no_drc(w);
    w.flag(false);  // b_associated
}

// loud_corr() for a mode above 10 with no corrections, and pres_ch_mode_core
// below 3.
void put_empty_immersive_loud_corr(BitWriter& w) {
    w.flag(false);  // b_corr_for_immersive_out
    w.flag(false);  // b_loro_loud_comp
    w.flag(false);  // b_ltrt_loud_comp
    w.flag(false);  // loud_corr_5_X's b_loud_comp
}

PresentationSubstream read_ok(const BitWriter& w, const PresentationContext& ctx, Recorder& rec) {
    PresentationSubstreamState state;
    PresentationSubstream out;
    const auto result = read_presentation(w, ctx, state, out, rec);
    REQUIRE(result.has_value());
    return out;
}

}  // namespace

TEST_CASE("custom_dmx_data derives bs_ch_config from the presentation's channels", "[ac4dec][presentation]") {
    struct Case {
        int mode;
        int top_pairs;
        bool back;
        int bs_ch_config;
    };
    for (const Case c : {Case{ch_mode::k9_0_4, 2, true, 0}, Case{ch_mode::k7_1_4, 2, true, 1},
                         Case{ch_mode::k7_0_4, 2, false, 2}, Case{ch_mode::k9_1_4, 1, true, 3},
                         Case{ch_mode::k7_0_4, 1, true, 4}, Case{ch_mode::k7_1_4, 1, false, 5},
                         Case{ch_mode::k9_0_4, 2, false, -1}, Case{ch_mode::k7_0_4, 0, true, -1}}) {
        INFO("mode " << c.mode << ", top pairs " << c.top_pairs << ", back " << c.back);
        BitWriter w;
        put_head(w);
        if (c.bs_ch_config >= 0) {
            w.flag(false);  // b_cdmx_data_present
        }
        w.flag(false);      // b_stereo_dmx_coeff
        put_empty_immersive_loud_corr(w);
        w.align();
        Recorder rec;
        const auto out = read_ok(w, immersive_context(c.mode, c.top_pairs, c.back), rec);
        CHECK(out.custom_dmx_data.bs_ch_config == c.bs_ch_config);
        CHECK(rec.count("b_cdmx_data_present") == (c.bs_ch_config >= 0 ? 1 : 0));
    }
}

TEST_CASE("cdmx_parameters reads the tools of bs_ch_config 0 for each output configuration",
          "[ac4dec][presentation]") {
    BitWriter w;
    put_head(w);
    w.flag(true);    // b_cdmx_data_present
    w.put(3, 2);     // four configurations
    // 0: out 0 - tool_scr_to_c_l (to C), tool_t4_to_f_s (both to front),
    // tool_b4_to_b2.
    w.put(0, 3);
    w.flag(true);  w.put(1, 3);           // b_put_screen_to_c, gain_f1_code
    w.flag(true);  w.put(2, 3);           // b_top_front_to_front, gain_t2a_code
    w.flag(true);  w.put(3, 3);           // b_top_back_to_front, gain_t2d_code
    w.put(4, 3);                          // gain_b_code
    // 1: out 1 - screen to L/R, tool_t4_to_t2, tool_b4_to_b2.
    w.put(1, 3);
    w.flag(false); w.put(5, 3);           // gain_f2_code
    w.put(6, 3);                          // gain_t1_code
    w.put(7, 3);                          // gain_b_code
    // 2: out 2 - tool_b4_to_b2 alone.
    w.put(2, 3);
    w.flag(false); w.put(0, 3);
    w.put(1, 3);
    // 3: out 0 again, the tops to the sides this time.
    w.put(0, 3);
    w.flag(false); w.put(0, 3);
    w.flag(false); w.put(2, 3);           // gain_t2b_code
    w.flag(false); w.put(3, 3);           // gain_t2e_code
    w.put(4, 3);
    w.flag(false);   // b_stereo_dmx_coeff
    put_empty_immersive_loud_corr(w);
    w.align();

    Recorder rec;
    const auto out = read_ok(w, immersive_context(ch_mode::k9_0_4, 2, true), rec);
    const auto& cdmx = out.custom_dmx_data;
    CHECK(cdmx.bs_ch_config == 0);
    REQUIRE(cdmx.n_cdmx_configs == 4);
    CHECK(cdmx.cdmx[0].b_put_screen_to_c == true);
    CHECK(cdmx.cdmx[0].gain_f1_code == 1);
    CHECK(cdmx.cdmx[0].gain_t2a_code == 2);
    CHECK(cdmx.cdmx[0].gain_t2b_code == 7);  // assigned, not sent
    CHECK(cdmx.cdmx[0].gain_t2d_code == 3);
    CHECK(cdmx.cdmx[0].gain_t2e_code == 7);
    CHECK(cdmx.cdmx[0].gain_b_code == 4);
    CHECK(cdmx.cdmx[1].gain_f2_code == 5);
    CHECK(cdmx.cdmx[1].gain_t1_code == 6);
    CHECK(cdmx.cdmx[1].gain_b_code == 7);
    CHECK(cdmx.cdmx[2].out_ch_config == 2);
    CHECK(cdmx.cdmx[2].gain_b_code == 1);
    CHECK_FALSE(cdmx.cdmx[2].gain_t1_code.has_value());
    CHECK(cdmx.cdmx[3].gain_t2b_code == 2);
    CHECK_FALSE(cdmx.cdmx[3].gain_t2a_code.has_value());
    CHECK(cdmx.cdmx[3].gain_t2e_code == 3);
}

TEST_CASE("cdmx_parameters reads tool_t4_to_f_s_b and the output configurations needing one tool",
          "[ac4dec][presentation]") {
    BitWriter w;
    put_head(w);
    w.flag(true);    // b_cdmx_data_present
    w.put(3, 2);     // four configurations, bs_ch_config 1: no screen tool
    // out 3: tool_t4_to_f_s_b, fronts to front and backs to front.
    w.put(3, 3);
    w.flag(true);  w.put(1, 3);           // gain_t2a_code
    w.flag(true);  w.put(2, 3);           // gain_t2d_code
    // out 3: to the sides.
    w.put(3, 3);
    w.flag(false); w.flag(true);  w.put(3, 3);   // gain_t2b_code
    w.flag(false); w.flag(true);  w.put(4, 3);   // gain_t2e_code
    // out 3: to the backs.
    w.put(3, 3);
    w.flag(false); w.flag(false); w.put(5, 3);   // gain_t2c_code
    w.flag(false); w.flag(false); w.put(6, 3);   // gain_t2f_code
    // out 4: tool_t4_to_t2.
    w.put(4, 3);
    w.put(2, 3);
    w.flag(false);   // b_stereo_dmx_coeff
    put_empty_immersive_loud_corr(w);
    w.align();

    Recorder rec;
    const auto out = read_ok(w, immersive_context(ch_mode::k7_1_4, 2, true), rec);
    const auto& cdmx = out.custom_dmx_data;
    CHECK(cdmx.bs_ch_config == 1);
    CHECK_FALSE(cdmx.cdmx[0].b_put_screen_to_c.has_value());
    CHECK(cdmx.cdmx[0].gain_t2a_code == 1);
    CHECK(cdmx.cdmx[0].gain_t2b_code == 7);
    CHECK(cdmx.cdmx[0].gain_t2d_code == 2);
    CHECK(cdmx.cdmx[0].gain_t2e_code == 7);
    CHECK(cdmx.cdmx[1].b_top_front_to_side == true);
    CHECK(cdmx.cdmx[1].gain_t2b_code == 3);
    CHECK(cdmx.cdmx[1].gain_t2e_code == 4);
    CHECK(cdmx.cdmx[2].gain_t2c_code == 5);
    CHECK(cdmx.cdmx[2].gain_t2b_code == 7);
    CHECK(cdmx.cdmx[2].gain_t2f_code == 6);
    CHECK(cdmx.cdmx[2].gain_t2e_code == 7);
    CHECK(cdmx.cdmx[3].gain_t1_code == 2);

    SECTION("an output configuration no tool serves reads nothing") {
        BitWriter unused;
        put_head(unused);
        unused.flag(true);
        unused.put(0, 2);    // one configuration
        unused.put(6, 3);    // out 6: none of Table 127's
        unused.flag(false);
        put_empty_immersive_loud_corr(unused);
        unused.align();
        Recorder rec2;
        const auto none = read_ok(unused, immersive_context(ch_mode::k7_1_4, 2, true), rec2);
        CHECK(none.custom_dmx_data.cdmx[0].out_ch_config == 6);
        CHECK_FALSE(none.custom_dmx_data.cdmx[0].gain_b_code.has_value());
    }
}

TEST_CASE("cdmx_parameters reads the two-top and one-bit configurations", "[ac4dec][presentation]") {
    SECTION("bs_ch_config 2: a one-bit out_ch_config") {
        BitWriter w;
        put_head(w);
        w.flag(true);
        w.put(1, 2);    // two configurations
        w.put(0, 1);    // out 0: tool_t4_to_f_s, to the sides
        w.flag(false); w.put(1, 3);   // gain_t2b_code
        w.flag(false); w.put(2, 3);   // gain_t2e_code
        w.put(1, 1);    // out 1: tool_t4_to_t2
        w.put(3, 3);
        w.flag(false);
        put_empty_immersive_loud_corr(w);
        w.align();
        Recorder rec;
        const auto out = read_ok(w, immersive_context(ch_mode::k7_0_4, 2, false), rec);
        CHECK(out.custom_dmx_data.cdmx[0].gain_t2b_code == 1);
        CHECK(out.custom_dmx_data.cdmx[0].gain_t2e_code == 2);
        CHECK(out.custom_dmx_data.cdmx[1].gain_t1_code == 3);
    }
    SECTION("bs_ch_config 3: the screen tool and the two-top tools") {
        BitWriter w;
        put_head(w);
        w.flag(true);
        w.put(3, 2);    // four configurations
        w.put(0, 3);    // out 0: screen, tool_t2_to_f_s (to front), tool_b4_to_b2
        w.flag(true); w.put(1, 3);
        w.flag(true); w.put(2, 3);    // b_top_to_front, gain_t2a_code
        w.put(3, 3);                  // gain_b_code
        w.put(0, 3);    // out 0 again, tops to the side
        w.flag(true); w.put(1, 3);
        w.flag(false); w.put(4, 3);   // gain_t2b_code
        w.put(5, 3);
        w.put(1, 3);    // out 1: tool_b4_to_b2
        w.flag(true); w.put(1, 3);
        w.put(6, 3);
        w.put(3, 3);    // out 3: tool_t2_to_f_s_b, to front
        w.flag(true); w.put(1, 3);
        w.flag(true); w.put(7, 3);
        w.flag(false);
        put_empty_immersive_loud_corr(w);
        w.align();
        Recorder rec;
        const auto out = read_ok(w, immersive_context(ch_mode::k9_1_4, 1, true), rec);
        const auto& cdmx = out.custom_dmx_data;
        CHECK(cdmx.bs_ch_config == 3);
        CHECK(cdmx.cdmx[0].b_top_to_front == true);
        CHECK(cdmx.cdmx[0].gain_t2a_code == 2);
        CHECK(cdmx.cdmx[0].gain_t2b_code == 7);
        CHECK(cdmx.cdmx[0].gain_b_code == 3);
        CHECK(cdmx.cdmx[1].gain_t2b_code == 4);
        CHECK(cdmx.cdmx[2].gain_b_code == 6);
        CHECK(cdmx.cdmx[3].gain_t2a_code == 7);
    }
    SECTION("bs_ch_config 4: tool_t2_to_f_s_b to the sides and to the backs") {
        BitWriter w;
        put_head(w);
        w.flag(true);
        w.put(2, 2);    // three configurations
        w.put(3, 3);
        w.flag(false); w.flag(true); w.put(2, 3);    // b_top_to_side, gain_t2b_code
        w.put(3, 3);
        w.flag(false); w.flag(false); w.put(3, 3);   // gain_t2c_code
        w.put(2, 3);    // out 2: tool_b4_to_b2
        w.put(5, 3);
        w.flag(false);
        put_empty_immersive_loud_corr(w);
        w.align();
        Recorder rec;
        const auto out = read_ok(w, immersive_context(ch_mode::k7_0_4, 1, true), rec);
        const auto& cdmx = out.custom_dmx_data;
        CHECK(cdmx.bs_ch_config == 4);
        CHECK(cdmx.cdmx[0].b_top_to_side == true);
        CHECK(cdmx.cdmx[0].gain_t2b_code == 2);
        CHECK(cdmx.cdmx[1].gain_t2c_code == 3);
        CHECK(cdmx.cdmx[1].gain_t2b_code == 7);
        CHECK(cdmx.cdmx[2].gain_b_code == 5);
    }
    SECTION("bs_ch_config 5: tool_t2_to_f_s for out 0 only") {
        BitWriter w;
        put_head(w);
        w.flag(true);
        w.put(1, 2);
        w.put(0, 1);
        w.flag(false); w.put(6, 3);
        w.put(1, 1);
        w.flag(false);
        put_empty_immersive_loud_corr(w);
        w.align();
        Recorder rec;
        const auto out = read_ok(w, immersive_context(ch_mode::k7_1_4, 1, false), rec);
        CHECK(out.custom_dmx_data.bs_ch_config == 5);
        CHECK(out.custom_dmx_data.cdmx[0].gain_t2b_code == 6);
        CHECK(out.custom_dmx_data.cdmx[1].out_ch_config == 1);
        CHECK_FALSE(out.custom_dmx_data.cdmx[1].gain_t2b_code.has_value());
    }
}

TEST_CASE("custom_dmx_data reads the stereo downmix coefficients", "[ac4dec][presentation]") {
    PresentationContext ctx = stereo_context();
    ctx.pres_ch_mode = ch_mode::k5_1;
    ctx.b_pres_has_lfe = true;
    BitWriter w;
    put_head(w);
    w.flag(true);    // b_stereo_dmx_coeff
    w.put(1, 3);
    w.put(2, 3);
    w.flag(true);    // b_ltrt_mixinfo
    w.put(3, 3);
    w.put(4, 3);
    w.flag(true);    // b_lfe_mixinfo
    w.put(25, 5);
    w.put(3, 2);     // preferred_dmx_method
    // loud_corr() for 5.1: no immersive flag, the two downmix corrections.
    w.flag(true);
    w.put(11, 5);    // loro_dmx_loud_corr
    w.flag(true);
    w.put(12, 5);    // ltrt_dmx_loud_corr
    w.align();
    Recorder rec;
    const auto out = read_ok(w, ctx, rec);
    REQUIRE(out.custom_dmx_data.stereo_dmx_coeff.has_value());
    const auto& coeff = *out.custom_dmx_data.stereo_dmx_coeff;
    CHECK(coeff.loro_centre_mixgain == 1);
    CHECK(coeff.loro_surround_mixgain == 2);
    CHECK(coeff.ltrt_centre_mixgain == 3);
    CHECK(coeff.ltrt_surround_mixgain == 4);
    CHECK(coeff.lfe_mixgain == 25);
    CHECK(coeff.preferred_dmx_method == 3);
    CHECK_FALSE(coeff.loro_dmx_loud_corr.has_value());  // not carried here
    CHECK(out.loud_corr.loro_dmx_loud_corr == 11);
    CHECK(out.loud_corr.ltrt_dmx_loud_corr == 12);
    CHECK(rec.count("b_corr_for_immersive_out") == 0);
}

TEST_CASE("loud_corr reads every correction an immersive presentation can carry", "[ac4dec][presentation]") {
    PresentationContext ctx = immersive_context(ch_mode::k7_0_4, 0, false);
    ctx.pres_ch_mode_core = 5;
    BitWriter w;
    put_head(w);
    w.flag(false);   // b_stereo_dmx_coeff (bs_ch_config -1: no cdmx data)
    w.flag(true);    // b_corr_for_immersive_out
    w.flag(false);   // b_loro_loud_comp
    w.flag(false);   // b_ltrt_loud_comp
    for (int value = 1; value <= 5; ++value) {
        w.flag(true);  // b_loud_comp: 5_X, 5_X_2, 7_X, 7_X_4, 7_X_2
        w.put(static_cast<std::uint64_t>(value), 5);
    }
    w.flag(false);   // loud_corr_5_X_4 absent
    w.flag(true);
    w.put(6, 5);     // loud_corr_core_5_X_2
    w.flag(true);
    w.put(7, 5);     // loud_corr_core_5_X
    w.flag(true);
    w.put(8, 5);     // loud_corr_core_loro
    w.put(9, 5);     // loud_corr_core_ltrt
    w.align();
    Recorder rec;
    const auto out = read_ok(w, ctx, rec);
    const auto& corr = out.loud_corr;
    CHECK(corr.b_corr_for_immersive_out);
    CHECK_FALSE(corr.loro_dmx_loud_corr.has_value());
    CHECK(corr.loud_corr_5_X == 1);
    CHECK(corr.loud_corr_5_X_2 == 2);
    CHECK(corr.loud_corr_7_X == 3);
    CHECK(corr.loud_corr_7_X_4 == 4);
    CHECK(corr.loud_corr_7_X_2 == 5);
    CHECK_FALSE(corr.loud_corr_5_X_4.has_value());
    CHECK(corr.loud_corr_core_5_X_2 == 6);
    CHECK(corr.loud_corr_core_5_X == 7);
    CHECK(corr.loud_corr_core_loro == 8);
    CHECK(corr.loud_corr_core_ltrt == 9);
    CHECK_FALSE(corr.loud_corr_9_X_4.has_value());
}

TEST_CASE("loud_corr reads an object presentation's corrections", "[ac4dec][presentation]") {
    PresentationContext ctx = stereo_context();
    ctx.pres_ch_mode = -1;
    ctx.pres_ch_mode_core = 4;  // a static A-JOC core: 5.1
    BitWriter w;
    put_head(w);
    w.flag(false);   // b_stereo_dmx_coeff: the core is 5.X
    w.flag(true);    // b_obj_loud_corr
    w.flag(false);   // b_corr_for_immersive_out
    w.flag(true);
    w.put(3, 5);     // loro_dmx_loud_corr
    w.flag(false);   // b_ltrt_loud_comp
    w.flag(true);
    w.put(4, 5);     // loud_corr_5_X
    w.flag(false);   // loud_corr_core_5_X absent
    w.flag(false);   // no core loro/ltrt pair
    w.flag(true);
    w.put(30, 5);    // loud_corr_9_X_4
    w.align();
    Recorder rec;
    const auto out = read_ok(w, ctx, rec);
    CHECK(out.loud_corr.b_obj_loud_corr);
    CHECK(out.loud_corr.loro_dmx_loud_corr == 3);
    CHECK_FALSE(out.loud_corr.ltrt_dmx_loud_corr.has_value());
    CHECK(out.loud_corr.loud_corr_5_X == 4);
    CHECK_FALSE(out.loud_corr.loud_corr_7_X_4.has_value());
    CHECK_FALSE(out.loud_corr.loud_corr_core_5_X.has_value());
    CHECK(out.loud_corr.loud_corr_9_X_4 == 30);
}
