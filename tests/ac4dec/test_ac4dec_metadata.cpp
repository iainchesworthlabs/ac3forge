// The AC-4 decoder's metadata syntax (src/ac4dec/src/syntax/metadata.cpp) on
// hand-built bitstreams: metadata() with its basic and extended metadata at
// both substream versions, further_loudness_info(), drc_frame(),
// dialog_enhancement() and emdf_payloads_substream(). The committed DEE
// streams reach only the branches that encoder writes; each case here writes
// the fields of one branch and checks the values read, the records emitted
// and where the reader stopped, or the refusal a malformed field earns.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4dec_bits.hpp"
#include "bit_reader.hpp"
#include "syntax/context.hpp"
#include "syntax/metadata.hpp"
#include "tables/huffman_tables.hpp"

namespace {

using ac4::DecodeError;
using ac4::detail::BitReader;
using ac4::detail::DrcContext;
using ac4::detail::DrcFrame;
using ac4::detail::DrcState;
using ac4::detail::EmdfPayloads;
using ac4::detail::FurtherLoudnessInfo;
using ac4::detail::Metadata;
using ac4::detail::MetadataState;
using ac4::detail::ParseResult;
using ac4::detail::SubstreamContext;
using ac4dec_test::BitWriter;
using ac4dec_test::Recorder;
namespace ch_mode = ac4::detail::ch_mode;
namespace tables = ac4::detail::tables;

// Reads `w` with `parse(reader)`, recording into `rec`, and returns the result
// with the reader's final position.
template <typename F>
ParseResult read_with(const BitWriter& w, Recorder& rec, std::size_t& position, F&& parse) {
    const std::vector<std::byte> bytes = w.bytes();
    BitReader reader(bytes, 0, rec);
    const ParseResult result = parse(reader);
    position = reader.position();
    return result;
}

SubstreamContext context(int mode, int sus_ver, bool b_iframe) {
    SubstreamContext ctx;
    ctx.ch_mode = mode;
    ctx.sus_ver = sus_ver;
    ctx.b_iframe = b_iframe;
    return ctx;
}

// tools_metadata_size for `tools` bits, the tools themselves, and
// b_emdf_payloads_substream = 0.
void put_tools(BitWriter& w, const BitWriter& tools) {
    const std::uint64_t size = tools.size();
    w.put(size & 0x7FU, 7);
    w.flag(size >= 128);
    if (size >= 128) {
        w.variable_bits(size >> 7U, 3);
    }
    w.append(tools);
    w.flag(false);  // b_emdf_payloads_substream
}

// The drc_frame() and dialog_enhancement() of a frame that carries neither.
BitWriter no_tools(int sus_ver) {
    BitWriter tools;
    if (sus_ver == 0) {
        tools.flag(false);  // b_drc_present
    }
    tools.flag(false);  // b_de_data_present
    return tools;
}

// basic_metadata() at sus_ver 1 with b_more_basic_metadata 0, and
// extended_metadata() with nothing present, for a substream with no centre
// classifier bits to worry about.
void put_empty_basic_and_extended_sus1(BitWriter& w) {
    w.flag(false);  // b_more_basic_metadata
    w.flag(false);  // b_dialog
    w.flag(false);  // b_channels_classifier
    w.flag(false);  // b_event_probability
}

}  // namespace

// --- basic_metadata() -------------------------------------------------------

TEST_CASE("metadata reads a sus_ver 0 5.1 substream's dialnorm, loudness and stereo downmix block",
          "[ac4dec][metadata]") {
    BitWriter w;
    w.put(27, 7);       // dialnorm_bits
    w.flag(true);       // b_more_basic_metadata
    w.flag(true);       // b_further_loudness_info
    // further_loudness_info(sus_ver 0): the full form.
    w.put(1, 2);        // loudness_version
    w.put(0, 4);        // loud_prac_type 0: no dialgate or loudcorr_type
    for (int i = 0; i < 6; ++i) {
        w.flag(false);  // b_loudrelgat .. b_max_truepk
    }
    w.flag(false);      // b_prgmbndy
    w.flag(false);      // b_lra
    w.flag(false);      // b_loudmntry
    w.flag(false);      // b_max_loudmntry
    w.flag(false);      // b_extension
    // mode > stereo, sus_ver 0: b_stereo_dmx_coeff.
    w.flag(true);
    w.put(3, 3);        // loro_centre_mixgain
    w.put(4, 3);        // loro_surround_mixgain
    w.flag(true);       // b_loro_dmx_loud_corr
    w.put(17, 5);       // loro_dmx_loud_corr
    w.flag(true);       // b_ltrt_mixinfo
    w.put(5, 3);        // ltrt_centre_mixgain
    w.put(6, 3);        // ltrt_surround_mixgain
    w.flag(true);       // b_ltrt_dmx_loud_corr
    w.put(9, 5);        // ltrt_dmx_loud_corr
    w.flag(true);       // b_lfe_mixinfo: 5.1 has an LFE
    w.put(21, 5);       // lfe_mixgain
    w.put(2, 2);        // preferred_dmx_method
    w.flag(true);       // b_predmixtyp_5ch
    w.put(5, 3);        // pre_dmixtyp_5ch
    w.flag(true);       // b_preupmixtyp_5ch
    w.put(11, 4);       // pre_upmixtyp_5ch
    w.put(3, 2);        // phase90_info_mc
    w.flag(true);       // b_surround_attenuation_known
    w.flag(false);      // b_lfe_attenuation_known
    w.flag(true);       // b_dc_blocking
    w.flag(true);       // dc_block_on
    // extended_metadata(sus_ver 0), neither associated nor dialogue.
    w.flag(false);      // b_channels_classifier
    w.flag(false);      // b_event_probability
    put_tools(w, no_tools(0));

    Recorder rec;
    std::size_t end = 0;
    MetadataState state;
    Metadata out;
    const auto result = read_with(w, rec, end, [&](BitReader& r) {
        return ac4::detail::parse_metadata(r, context(ch_mode::k5_1, 0, true), state, out);
    });
    REQUIRE(result.has_value());
    CHECK(end == w.size());
    CHECK(out.basic.dialnorm_bits == 27);
    REQUIRE(out.basic.further_loudness_info.has_value());
    CHECK(out.basic.further_loudness_info->loudness_version == 1);
    CHECK(out.basic.further_loudness_info->loud_prac_type == 0);
    REQUIRE(out.basic.stereo_dmx_coeff.has_value());
    const auto& coeff = *out.basic.stereo_dmx_coeff;
    CHECK(coeff.loro_centre_mixgain == 3);
    CHECK(coeff.loro_surround_mixgain == 4);
    CHECK(coeff.loro_dmx_loud_corr == 17);
    CHECK(coeff.b_ltrt_mixinfo);
    CHECK(coeff.ltrt_centre_mixgain == 5);
    CHECK(coeff.ltrt_surround_mixgain == 6);
    CHECK(coeff.ltrt_dmx_loud_corr == 9);
    CHECK(coeff.lfe_mixgain == 21);
    CHECK(coeff.preferred_dmx_method == 2);
    CHECK(out.basic.pre_dmixtyp_5ch == 5);
    CHECK(out.basic.pre_upmixtyp_5ch == 11);
    CHECK(out.basic.phase90_info_mc == 3);
    CHECK(out.basic.b_surround_attenuation_known);
    CHECK_FALSE(out.basic.b_lfe_attenuation_known);
    CHECK(out.basic.dc_block_on == true);
    REQUIRE(out.drc.has_value());  // sus_ver 0 carries drc_frame() here
    CHECK_FALSE(out.drc->b_drc_present);
    CHECK(out.tools_metadata_size == 2);
    CHECK(rec.value("lfe_mixgain") == 21);
}

TEST_CASE("metadata reads a sus_ver 1 stereo substream's loudness and previous downmix info",
          "[ac4dec][metadata]") {
    BitWriter w;
    w.flag(true);    // b_more_basic_metadata
    w.flag(true);    // b_substream_loudness_info
    w.put(200, 8);   // substream_loudness_bits
    w.flag(true);    // b_further_substream_loudness_info
    // further_loudness_info(sus_ver 1, substream form).
    w.flag(true);    // b_loudcorr_dialgate
    w.flag(true);    // b_loudrelgat
    w.put(1234, 11);
    for (int i = 0; i < 5; ++i) {
        w.flag(false);  // b_loudspchgat .. b_max_truepk
    }
    w.flag(false);   // b_lra
    w.flag(false);   // b_loudmntry
    w.flag(false);   // b_max_loudmntry
    w.flag(true);    // b_rtllcomp
    w.put(99, 8);    // rtll_comp
    w.flag(true);    // b_extension
    w.put(4, 5);     // e_bits_size
    w.put(0b1011, 4);  // extensions_bits
    // stereo: b_prev_dmx_info.
    w.flag(true);
    w.put(6, 3);     // pre_dmixtyp_2ch
    w.put(2, 2);     // phase90_info_2ch
    w.flag(false);   // b_dc_blocking
    // extended_metadata(sus_ver 1): b_dialog with a stereo pan.
    w.flag(true);    // b_dialog
    w.flag(true);    // b_dialog_max_gain
    w.put(3, 2);     // dialog_max_gain
    w.flag(true);    // b_pan_dialog_present
    w.put(10, 8);    // pan_dialog[0]
    w.put(250, 8);   // pan_dialog[1]
    w.put(1, 2);     // pan_signal_selector
    w.flag(true);    // b_channels_classifier
    w.flag(true);    // b_l_active
    w.flag(true);    // b_l_has_dialog
    w.flag(false);   // b_r_active
    w.flag(true);    // b_event_probability
    w.put(12, 4);    // event_probability
    put_tools(w, no_tools(1));

    Recorder rec;
    std::size_t end = 0;
    MetadataState state;
    Metadata out;
    const auto result = read_with(w, rec, end, [&](BitReader& r) {
        return ac4::detail::parse_metadata(r, context(ch_mode::kStereo, 1, true), state, out);
    });
    REQUIRE(result.has_value());
    CHECK(end == w.size());
    CHECK_FALSE(out.basic.dialnorm_bits.has_value());
    CHECK(out.basic.substream_loudness_bits == 200);
    REQUIRE(out.basic.further_loudness_info.has_value());
    const auto& info = *out.basic.further_loudness_info;
    CHECK_FALSE(info.loud_prac_type.has_value());
    CHECK(info.b_loudcorr_dialgate);
    CHECK(info.loudrelgat == 1234);
    CHECK(info.rtll_comp == 99);
    CHECK(info.e_bits_size == 4U);
    CHECK(info.extensions_bits == 4U);
    CHECK(rec.value("extensions_bits") == 0b1011);
    CHECK(out.basic.pre_dmixtyp_2ch == 6);
    CHECK(out.basic.phase90_info_2ch == 2);
    CHECK_FALSE(out.basic.dc_block_on.has_value());
    CHECK(out.extended.b_dialog);
    CHECK(out.extended.dialog_max_gain == 3);
    CHECK(out.extended.pan_dialog[0] == 10);
    CHECK(out.extended.pan_dialog[1] == 250);
    CHECK(out.extended.pan_signal_selector == 1);
    CHECK(out.extended.b_l_has_dialog);
    CHECK_FALSE(out.extended.b_r_active);
    CHECK_FALSE(out.extended.b_c_active);  // stereo has no centre to classify
    CHECK(out.extended.event_probability == 12);
    CHECK_FALSE(out.drc.has_value());  // at sus_ver 1 the presentation substream has DRC
}

TEST_CASE("metadata reads the upmix type of each 7.X layout", "[ac4dec][metadata]") {
    struct Case {
        int mode;
        int upmix_bits;  // 2 for 3/4/0, 1 for 3/2/2, 0 for 5/2/0
    };
    for (const Case c : {Case{ch_mode::k7_0_340, 2}, Case{ch_mode::k7_1_340, 2}, Case{ch_mode::k7_0_322, 1},
                         Case{ch_mode::k7_1_322, 1}, Case{ch_mode::k7_0_520, 0}, Case{ch_mode::k7_1_520, 0}}) {
        INFO("ch_mode " << c.mode);
        BitWriter w;
        w.flag(true);    // b_more_basic_metadata
        w.flag(false);   // b_substream_loudness_info
        w.flag(true);    // b_upmixtyp_7ch
        if (c.upmix_bits > 0) {
            w.put(1, c.upmix_bits);
        }
        w.put(1, 2);     // phase90_info_mc
        w.flag(false);   // b_surround_attenuation_known
        w.flag(false);   // b_lfe_attenuation_known
        w.flag(false);   // b_dc_blocking
        w.flag(false);   // b_dialog
        w.flag(false);   // b_channels_classifier
        w.flag(false);   // b_event_probability
        put_tools(w, no_tools(1));

        Recorder rec;
        std::size_t end = 0;
        MetadataState state;
        Metadata out;
        const auto result = read_with(w, rec, end, [&](BitReader& r) {
            return ac4::detail::parse_metadata(r, context(c.mode, 1, true), state, out);
        });
        REQUIRE(result.has_value());
        CHECK(end == w.size());
        CHECK(out.basic.b_upmixtyp_7ch);
        CHECK(out.basic.pre_upmixtyp_3_4.has_value() == (c.upmix_bits == 2));
        CHECK(out.basic.pre_upmixtyp_3_2_2.has_value() == (c.upmix_bits == 1));
        CHECK(out.basic.phase90_info_mc == 1);
    }
}

// --- extended_metadata() ----------------------------------------------------

TEST_CASE("metadata reads a sus_ver 0 associated mono substream's scaling and pan", "[ac4dec][metadata]") {
    SubstreamContext ctx = context(ch_mode::kMono, 0, true);
    ctx.b_associated = true;
    ctx.b_dialog = true;
    BitWriter w;
    w.put(31, 7);    // dialnorm_bits
    w.flag(false);   // b_more_basic_metadata
    w.flag(true);    // b_scale_main
    w.put(100, 8);
    w.flag(true);    // b_scale_main_centre
    w.put(101, 8);
    w.flag(true);    // b_scale_main_front
    w.put(102, 8);
    w.put(77, 8);    // pan_associated
    w.flag(false);   // b_dialog_max_gain
    w.flag(true);    // b_pan_dialog_present
    w.put(128, 8);   // pan_dialog (mono: one value, no selector)
    w.flag(true);    // b_channels_classifier
    w.flag(true);    // b_c_active
    w.flag(true);    // b_c_has_dialog
    w.flag(false);   // b_event_probability
    put_tools(w, no_tools(0));

    Recorder rec;
    std::size_t end = 0;
    MetadataState state;
    Metadata out;
    const auto result =
        read_with(w, rec, end, [&](BitReader& r) { return ac4::detail::parse_metadata(r, ctx, state, out); });
    REQUIRE(result.has_value());
    CHECK(end == w.size());
    CHECK(out.basic.dialnorm_bits == 31);
    CHECK(out.extended.b_associated);
    CHECK(out.extended.b_dialog);
    CHECK(out.extended.scale_main == 100);
    CHECK(out.extended.scale_main_centre == 101);
    CHECK(out.extended.scale_main_front == 102);
    CHECK(out.extended.pan_associated == 77);
    CHECK(out.extended.pan_dialog[0] == 128);
    CHECK_FALSE(out.extended.pan_signal_selector.has_value());
    CHECK(out.extended.b_c_has_dialog);
    CHECK(rec.count("pan_dialog") == 1);
    CHECK(rec.count("b_dialog") == 0);  // a parameter at sus_ver 0, not a field
}

TEST_CASE("extended_metadata classifies every channel its channel mode holds", "[ac4dec][metadata]") {
    // Which classifier flags each mode reads, all written as 1 so that each
    // b_*_active is followed by its b_*_has_dialog where the syntax has one.
    struct Case {
        int mode;
        std::vector<const char*> flags;
    };
    const std::vector<Case> cases = {
        {ch_mode::k7_1_322,
         {"b_c_active", "b_l_active", "b_r_active", "b_ls_active", "b_rs_active", "b_tfl_active",
          "b_tfr_active", "b_lfe_active"}},
        {ch_mode::k7_0_520, {"b_c_active", "b_l_active", "b_r_active", "b_ls_active", "b_rs_active",
                             "b_lw_active", "b_rw_active"}},
        {ch_mode::k7_1_340, {"b_c_active", "b_l_active", "b_r_active", "b_ls_active", "b_rs_active",
                             "b_lb_active", "b_rb_active", "b_lfe_active"}},
        {ch_mode::k22_2, {"b_c_active", "b_l_active", "b_r_active", "b_ls_active", "b_rs_active",
                          "b_lb_active", "b_rb_active", "b_lw_active", "b_rw_active", "b_lfe_active"}},
    };
    for (const Case& c : cases) {
        INFO("ch_mode " << c.mode);
        BitWriter w;
        w.flag(false);  // b_more_basic_metadata
        w.flag(false);  // b_dialog
        w.flag(true);   // b_channels_classifier
        for (const char* name : c.flags) {
            const std::string_view flag{name};
            w.flag(true);
            if (flag == "b_c_active" || flag == "b_l_active" || flag == "b_r_active") {
                w.flag(true);  // b_*_has_dialog
            }
        }
        w.flag(false);  // b_event_probability
        put_tools(w, no_tools(1));

        Recorder rec;
        std::size_t end = 0;
        MetadataState state;
        Metadata out;
        const auto result = read_with(w, rec, end, [&](BitReader& r) {
            return ac4::detail::parse_metadata(r, context(c.mode, 1, true), state, out);
        });
        REQUIRE(result.has_value());
        CHECK(end == w.size());
        for (const char* name : c.flags) {
            CHECK(rec.count(name) == 1);
        }
        CHECK(out.extended.b_c_has_dialog);
        CHECK(out.extended.b_l_has_dialog);
        CHECK(out.extended.b_r_has_dialog);
        CHECK(out.extended.b_ls_active);
        CHECK(out.extended.b_lfe_active == (c.mode != ch_mode::k7_0_520));
        CHECK(out.extended.b_tfl_active == (c.mode == ch_mode::k7_1_322));
        CHECK(out.extended.b_lw_active == (c.mode == ch_mode::k7_0_520 || c.mode == ch_mode::k22_2));
        CHECK(out.extended.b_lb_active == (c.mode == ch_mode::k7_1_340 || c.mode == ch_mode::k22_2));
    }
}

TEST_CASE("metadata fails when tools_metadata_size disagrees with the tools read", "[ac4dec][metadata]") {
    BitWriter w;
    put_empty_basic_and_extended_sus1(w);
    w.put(5, 7);     // tools_metadata_size_value: 5, but dialog_enhancement() takes 1
    w.flag(false);   // b_more_bits
    w.flag(false);   // b_de_data_present
    w.flag(false);
    Recorder rec;
    std::size_t end = 0;
    MetadataState state;
    Metadata out;
    const auto result = read_with(w, rec, end, [&](BitReader& r) {
        return ac4::detail::parse_metadata(r, context(ch_mode::kStereo, 1, true), state, out);
    });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().error == DecodeError::kInvalidStream);
}

TEST_CASE("metadata reads an inline emdf_payloads_substream()", "[ac4dec][metadata]") {
    BitWriter w;
    put_empty_basic_and_extended_sus1(w);
    w.put(1, 7);     // tools_metadata_size_value
    w.flag(false);   // b_more_bits
    w.flag(false);   // b_de_data_present
    w.flag(true);    // b_emdf_payloads_substream
    w.put(7, 5);     // emdf_payload_id
    w.flag(false);   // b_smpoffst
    w.flag(false);   // b_duration
    w.flag(false);   // b_groupid
    w.flag(false);   // b_codecdata
    w.flag(true);    // b_discard_unknown_payload
    w.variable_bits(1, 8);  // emdf_payload_size
    w.put(0xA5, 8);
    w.put(0, 5);     // emdf_payload_id 0 ends the loop
    w.align();

    Recorder rec;
    std::size_t end = 0;
    MetadataState state;
    Metadata out;
    const auto result = read_with(w, rec, end, [&](BitReader& r) {
        return ac4::detail::parse_metadata(r, context(ch_mode::kStereo, 1, true), state, out);
    });
    REQUIRE(result.has_value());
    CHECK(end == w.size());
    CHECK(out.b_emdf_payloads_substream);
    REQUIRE(out.emdf_payloads.payloads.size() == 1);
    CHECK(out.emdf_payloads.payloads[0].emdf_payload_id == 7U);
    CHECK(out.emdf_payloads.payloads[0].bytes == std::vector<std::uint8_t>{0xA5});
}

TEST_CASE("metadata stops with kTruncated when the substream ends inside it", "[ac4dec][metadata]") {
    BitWriter w;
    w.flag(true);   // b_more_basic_metadata
    w.flag(true);   // b_substream_loudness_info, then nothing
    Recorder rec;
    std::size_t end = 0;
    MetadataState state;
    Metadata out;
    // Two bits of data in one byte: the loudness bits run past the byte.
    const auto result = read_with(w, rec, end, [&](BitReader& r) {
        return ac4::detail::parse_metadata(r, context(ch_mode::kStereo, 1, true), state, out);
    });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().error == DecodeError::kTruncated);
}

// --- further_loudness_info() ------------------------------------------------

TEST_CASE("further_loudness_info reads every optional field of the full form", "[ac4dec][metadata]") {
    BitWriter w;
    w.put(3, 2);       // loudness_version 3: extended follows
    w.put(2, 4);       // extended_loudness_version
    w.put(5, 4);       // loud_prac_type
    w.flag(true);      // b_loudcorr_dialgate
    w.put(6, 3);       // dialgate_prac_type
    w.flag(true);      // b_loudcorr_type
    w.flag(true);
    w.put(1, 11);      // loudrelgat
    w.flag(true);
    w.put(2, 11);      // loudspchgat
    w.put(7, 3);       // dialgate_prac_type
    w.flag(true);
    w.put(3, 11);      // loudstrm3s
    w.flag(true);
    w.put(4, 11);      // max_loudstrm3s
    w.flag(true);
    w.put(5, 11);      // truepk
    w.flag(true);
    w.put(6, 11);      // max_truepk
    w.flag(true);      // b_prgmbndy
    w.put(0b001, 3);   // prgmbndy_bit x3, the last a 1
    w.flag(true);      // b_end_or_start
    w.flag(true);      // b_prgmbndy_offset
    w.put(1000, 11);
    w.flag(true);      // b_lra
    w.put(513, 10);
    w.put(4, 3);       // lra_prac_type
    w.flag(true);
    w.put(8, 11);      // loudmntry
    w.flag(true);
    w.put(9, 11);      // max_loudmntry
    // sus_ver 0's extension: b_rtllcomp and rtll_comp count in e_bits_size,
    // here 31 + variable_bits(4) = 31 + 2 = 33: 9 carried, 24 extension bits.
    w.flag(true);      // b_extension
    w.put(31, 5);
    w.variable_bits(2, 4);
    w.flag(true);      // b_rtllcomp
    w.put(42, 8);
    w.put(0xABCDEF, 24);

    Recorder rec;
    std::size_t end = 0;
    FurtherLoudnessInfo info;
    const auto result = read_with(
        w, rec, end, [&](BitReader& r) { return ac4::detail::parse_further_loudness_info(r, 0, false, info); });
    REQUIRE(result.has_value());
    CHECK(end == w.size());
    CHECK(info.loudness_version == 5);
    CHECK(info.loud_prac_type == 5);
    CHECK(info.dialgate_prac_type == 6);
    CHECK(info.b_loudcorr_type);
    CHECK(info.loudrelgat == 1);
    CHECK(info.loudspchgat == 2);
    CHECK(info.loudspchgat_dialgate_prac_type == 7);
    CHECK(info.loudstrm3s == 3);
    CHECK(info.max_loudstrm3s == 4);
    CHECK(info.truepk == 5);
    CHECK(info.max_truepk == 6);
    CHECK(info.prgmbndy_bits == 3);
    CHECK(info.b_end_or_start);
    CHECK(info.prgmbndy_offset == 1000);
    CHECK(info.lra == 513);
    CHECK(info.lra_prac_type == 4);
    CHECK(info.loudmntry == 8);
    CHECK(info.max_loudmntry == 9);
    CHECK(info.rtll_comp == 42);
    CHECK(info.e_bits_size == 33U);
    CHECK(info.extensions_bits == 24U);
    CHECK(rec.value("extensions_bits") == 0xABCDEFU);
    CHECK(rec.count("prgmbndy_bit") == 3);
}

TEST_CASE("further_loudness_info refuses malformed extensions and unterminated boundaries",
          "[ac4dec][metadata]") {
    // Everything up to the extension, at the presentation form.
    const auto head = [](BitWriter& w) {
        w.put(0, 2);    // loudness_version
        w.put(0, 4);    // loud_prac_type
        for (int i = 0; i < 6; ++i) {
            w.flag(false);
        }
        w.flag(false);  // b_prgmbndy
        w.flag(false);  // b_lra
        w.flag(false);  // b_loudmntry
        w.flag(false);  // b_max_loudmntry
    };

    SECTION("a sus_ver 0 e_bits_size smaller than the rtll_comp it carries") {
        BitWriter w;
        head(w);
        w.flag(true);   // b_extension
        w.put(5, 5);    // e_bits_size 5 < 9
        w.flag(true);   // b_rtllcomp
        w.put(0, 8);
        w.put(0, 16);
        Recorder rec;
        std::size_t end = 0;
        FurtherLoudnessInfo info;
        const auto result = read_with(
            w, rec, end, [&](BitReader& r) { return ac4::detail::parse_further_loudness_info(r, 0, false, info); });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kInvalidStream);
    }
    SECTION("sus_ver 1 extension bits past the end of the data") {
        BitWriter w;
        head(w);
        w.flag(false);  // b_rtllcomp
        w.flag(true);   // b_extension
        w.put(31, 5);
        w.variable_bits(100, 4);  // far more than is left
        Recorder rec;
        std::size_t end = 0;
        FurtherLoudnessInfo info;
        const auto result = read_with(
            w, rec, end, [&](BitReader& r) { return ac4::detail::parse_further_loudness_info(r, 1, true, info); });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
        CHECK(rec.count("extensions_bits") == 0);
    }
    SECTION("a program boundary no 1 closes") {
        BitWriter w;
        w.put(0, 2);
        w.put(0, 4);
        for (int i = 0; i < 6; ++i) {
            w.flag(false);
        }
        w.flag(true);  // b_prgmbndy, then zeros to the end
        w.put(0, 13);
        Recorder rec;
        std::size_t end = 0;
        FurtherLoudnessInfo info;
        const auto result = read_with(
            w, rec, end, [&](BitReader& r) { return ac4::detail::parse_further_loudness_info(r, 1, true, info); });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
        // One record per zero the data holds, padding included: 13 bits
        // precede the loop.
        CHECK(rec.count("prgmbndy_bit") == static_cast<int>(w.bytes().size() * 8 - 13));
    }
}

// --- drc_frame() ------------------------------------------------------------

TEST_CASE("DRC helper tables give each channel mode and frame length its count", "[ac4dec][metadata]") {
    using ac4::detail::nr_drc_channels;
    using ac4::detail::nr_drc_subframes;
    const auto channels = [](int mode) { return nr_drc_channels(DrcContext{.ch_mode = mode}); };
    CHECK(channels(ch_mode::kMono) == 1);
    CHECK(channels(ch_mode::kStereo) == 1);
    CHECK(channels(ch_mode::k3_0) == -1);
    for (int mode = ch_mode::k5_0; mode <= ch_mode::k7_1_322; ++mode) {
        CHECK(channels(mode) == 3);
    }
    for (int mode = ch_mode::k7_0_4; mode <= ch_mode::k22_2; ++mode) {
        CHECK(channels(mode) == 4);
    }
    CHECK(channels(-1) == -1);
    CHECK(nr_drc_subframes(384) == 1);
    CHECK(nr_drc_subframes(512) == 2);
    CHECK(nr_drc_subframes(768) == 3);
    CHECK(nr_drc_subframes(960) == 3);
    CHECK(nr_drc_subframes(1024) == 4);
    CHECK(nr_drc_subframes(1536) == 6);
    CHECK(nr_drc_subframes(1920) == 6);
    CHECK(nr_drc_subframes(2048) == 8);
    CHECK(nr_drc_subframes(1000) == -1);
}

namespace {

// drc_gainset_size: 6 bits, b_more_bits and variable_bits(2) of the rest.
void put_gainset_size(BitWriter& w, std::uint64_t size) {
    w.put(size & 0x3FU, 6);
    w.flag(size >= 64);
    if (size >= 64) {
        w.variable_bits(size >> 6U, 2);
    }
}

// The drc_gain_code for a difference of `diff` dB.
void put_drc_diff(BitWriter& w, int diff) {
    w.code(tables::kDrcHcb, tables::kDrcHcb.cb_off + diff);
}

int drc_code_bits(int diff) {
    BitWriter w;
    put_drc_diff(w, diff);
    return static_cast<int>(w.size());
}

}  // namespace

TEST_CASE("drc_frame reads a configuration of three modes and their gains", "[ac4dec][metadata]") {
    // 5.1 at frame_len_base 512: three DRC channels and two subframes; mode 0
    // has drc_gains_config 2, two bands, so twelve gains, the first
    // drc_gain_val and eleven drc_gain_code differences of +1.
    const DrcContext ctx{.b_iframe = true, .ch_mode = ch_mode::k5_1, .frame_len_base = 512};
    const int used = 7 + 11 * drc_code_bits(1);

    BitWriter w;
    w.flag(true);   // b_drc_present
    // drc_config()
    w.put(2, 3);    // drc_decoder_nr_modes: three modes
    w.put(0, 3);    // drc_decoder_mode_id 0
    w.flag(false);  // drc_repeat_profile_flag
    w.flag(false);  // drc_default_profile_flag
    w.flag(false);  // drc_compression_curve_flag
    w.put(2, 2);    // drc_gains_config
    w.put(5, 3);    // drc_decoder_mode_id 5: output levels follow
    w.put(12, 5);   // drc_output_level_from
    w.put(20, 5);   // drc_output_level_to
    w.flag(true);   // drc_repeat_profile_flag
    w.put(0, 3);    // drc_repeat_id: mode 0
    w.put(2, 3);    // drc_decoder_mode_id 2
    w.flag(false);  // drc_repeat_profile_flag
    w.flag(true);   // drc_default_profile_flag: a curve
    w.put(4, 3);    // drc_eac3_profile
    // drc_data(): mode 0, drc_version 0.
    put_gainset_size(w, static_cast<std::uint64_t>(used));
    w.put(0, 2);    // drc_version
    w.put(60, 7);   // drc_gain_val: -4 dB
    for (int i = 0; i < 11; ++i) {
        put_drc_diff(w, 1);
    }
    // mode 5 repeats mode 0, drc_version 1 with 5 bits of drc2_bits.
    put_gainset_size(w, static_cast<std::uint64_t>(2 + used + 5));
    w.put(1, 2);
    w.put(64, 7);   // drc_gain_val: 0 dB
    for (int i = 0; i < 11; ++i) {
        put_drc_diff(w, 1);
    }
    w.put(0b10101, 5);  // drc2_bits
    // mode 2 has a curve: the frame ends with the reset flag.
    w.flag(true);   // drc_reset_flag
    w.put(3, 2);    // drc_reserved

    Recorder rec;
    std::size_t end = 0;
    DrcState state;
    DrcFrame out;
    const auto result =
        read_with(w, rec, end, [&](BitReader& r) { return ac4::detail::parse_drc_frame(r, ctx, state, out); });
    REQUIRE(result.has_value());
    CHECK(end == w.size());
    CHECK(out.drc_config_present);
    REQUIRE(state.config_valid);
    CHECK(state.config.drc_decoder_nr_modes == 2);
    CHECK(state.config.drc_decoder_mode[1] == 5);
    CHECK(state.config.mode[5].drc_output_level_from == 12);
    CHECK(state.config.mode[5].drc_output_level_to == 20);
    CHECK(state.config.mode[5].drc_repeat_profile_flag);
    CHECK(state.config.mode[5].drc_gains_config == 2);
    CHECK(state.config.mode[2].drc_compression_curve_flag);
    CHECK(state.config.drc_eac3_profile == 4);
    REQUIRE(out.n_gainsets == 2);
    const auto& first = out.gainsets[0];
    CHECK(first.nr_drc_channels == 3);
    CHECK(first.nr_drc_bands == 2);
    CHECK(first.nr_drc_subframes == 2);
    // Frequency-differential in the first subframe, time-differential after,
    // each channel from the one before's first gain.
    CHECK(first.gain(0, 0, 0) == -4);
    CHECK(first.gain(0, 1, 0) == -3);
    CHECK(first.gain(0, 0, 1) == -3);
    CHECK(first.gain(0, 1, 1) == -2);
    CHECK(first.gain(1, 0, 0) == -3);
    CHECK(first.gain(2, 1, 1) == 0);
    const auto& second = out.gainsets[1];
    CHECK(second.drc_decoder_mode_id == 5);
    CHECK(second.drc_version == 1);
    CHECK(second.drc2_bits == 5U);
    CHECK(second.gain(0, 0, 0) == 0);
    CHECK(rec.value("drc2_bits") == 0b10101U);
    CHECK(out.curve_present);
    CHECK(out.drc_reset_flag);
    CHECK(out.drc_reserved == 3);

    SECTION("a later frame reads its gains with the configuration kept") {
        BitWriter next;
        next.flag(true);  // b_drc_present, no drc_config() outside an I-frame
        put_gainset_size(next, static_cast<std::uint64_t>(used + 2));  // the size that counts drc_version
        next.put(0, 2);
        next.put(70, 7);
        for (int i = 0; i < 11; ++i) {
            put_drc_diff(next, -1);
        }
        put_gainset_size(next, 2);  // drc_version 2: nothing but drc2_bits, none of them
        next.put(2, 2);
        next.flag(false);  // drc_reset_flag
        next.put(0, 2);
        Recorder rec2;
        DrcFrame later;
        const DrcContext non_iframe{.b_iframe = false, .ch_mode = ch_mode::k5_1, .frame_len_base = 512};
        const auto again = read_with(next, rec2, end, [&](BitReader& r) {
            return ac4::detail::parse_drc_frame(r, non_iframe, state, later);
        });
        REQUIRE(again.has_value());
        CHECK(end == next.size());
        CHECK_FALSE(later.drc_config_present);
        CHECK(later.gainsets[0].gain(0, 0, 0) == 6);
        CHECK(later.gainsets[0].gain(0, 1, 0) == 5);
        CHECK_FALSE(later.gainsets[1].gains_present);
        CHECK(later.gainsets[1].drc2_bits == 0U);
    }
    SECTION("an I-frame without DRC forgets the configuration") {
        BitWriter off;
        off.flag(false);
        Recorder rec2;
        DrcFrame none;
        REQUIRE(read_with(off, rec2, end, [&](BitReader& r) {
                    return ac4::detail::parse_drc_frame(r, ctx, state, none);
                }).has_value());
        CHECK_FALSE(state.config_valid);
        BitWriter next;
        next.flag(true);
        const DrcContext non_iframe{.b_iframe = false, .ch_mode = ch_mode::k5_1, .frame_len_base = 512};
        const auto missing = read_with(next, rec2, end, [&](BitReader& r) {
            return ac4::detail::parse_drc_frame(r, non_iframe, state, none);
        });
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().error == DecodeError::kMissingIFrame);
    }
}

TEST_CASE("drc_frame reads a full compression curve", "[ac4dec][metadata]") {
    BitWriter w;
    w.flag(true);    // b_drc_present
    w.put(0, 3);     // one mode
    w.put(1, 3);     // drc_decoder_mode_id
    w.flag(false);   // drc_repeat_profile_flag
    w.flag(false);   // drc_default_profile_flag
    w.flag(true);    // drc_compression_curve_flag
    w.put(1, 4);     // drc_lev_nullband_low
    w.put(2, 4);     // drc_lev_nullband_high
    w.put(3, 4);     // drc_gain_max_boost
    w.put(4, 5);     // drc_lev_max_boost
    w.put(1, 1);     // drc_nr_boost_sections
    w.put(5, 4);     // drc_gain_section_boost
    w.put(6, 5);     // drc_lev_section_boost
    w.put(7, 5);     // drc_gain_max_cut
    w.put(8, 6);     // drc_lev_max_cut
    w.put(1, 1);     // drc_nr_cut_sections
    w.put(9, 5);     // drc_gain_section_cut
    w.put(10, 5);    // drc_lev_section_cut
    w.flag(false);   // drc_tc_default_flag
    w.put(11, 8);    // drc_tc_attack
    w.put(12, 8);    // drc_tc_release
    w.put(13, 8);    // drc_tc_attack_fast
    w.put(14, 8);    // drc_tc_release_fast
    w.flag(true);    // drc_adaptive_smoothing_flag
    w.put(15, 5);    // drc_attack_threshold
    w.put(16, 5);    // drc_release_threshold
    w.put(0, 3);     // drc_eac3_profile
    w.flag(false);   // drc_reset_flag
    w.put(1, 2);     // drc_reserved

    Recorder rec;
    std::size_t end = 0;
    DrcState state;
    DrcFrame out;
    const DrcContext ctx{.b_iframe = true, .ch_mode = ch_mode::kStereo, .frame_len_base = 2048};
    const auto result =
        read_with(w, rec, end, [&](BitReader& r) { return ac4::detail::parse_drc_frame(r, ctx, state, out); });
    REQUIRE(result.has_value());
    CHECK(end == w.size());
    REQUIRE(state.config.mode[1].curve.has_value());
    const auto& curve = *state.config.mode[1].curve;
    CHECK(curve.drc_lev_nullband_low == 1);
    CHECK(curve.drc_lev_nullband_high == 2);
    CHECK(curve.drc_gain_max_boost == 3);
    CHECK(curve.drc_lev_max_boost == 4);
    CHECK(curve.drc_gain_section_boost == 5);
    CHECK(curve.drc_lev_section_boost == 6);
    CHECK(curve.drc_gain_max_cut == 7);
    CHECK(curve.drc_lev_max_cut == 8);
    CHECK(curve.drc_gain_section_cut == 9);
    CHECK(curve.drc_lev_section_cut == 10);
    CHECK(curve.drc_tc_attack == 11);
    CHECK(curve.drc_tc_release_fast == 14);
    CHECK(curve.drc_attack_threshold == 15);
    CHECK(curve.drc_release_threshold == 16);
    CHECK(out.n_gainsets == 0);
    CHECK(out.curve_present);
    CHECK(out.drc_reserved == 1);
}

TEST_CASE("drc_frame refuses what its syntax cannot follow", "[ac4dec][metadata]") {
    // One mode, id 0, with the given drc_gains_config, then drc_eac3_profile.
    const auto one_mode = [](BitWriter& w, int gains_config) {
        w.flag(true);
        w.put(0, 3);
        w.put(0, 3);
        w.flag(false);
        w.flag(false);
        w.flag(false);
        w.put(static_cast<std::uint64_t>(gains_config), 2);
        w.put(0, 3);
    };
    const auto run = [](const BitWriter& w, const DrcContext& ctx) {
        Recorder rec;
        std::size_t end = 0;
        DrcState state;
        DrcFrame out;
        return read_with(w, rec, end, [&](BitReader& r) { return ac4::detail::parse_drc_frame(r, ctx, state, out); });
    };
    const DrcContext stereo{.b_iframe = true, .ch_mode = ch_mode::kStereo, .frame_len_base = 2048};

    SECTION("a repeat of a mode not yet configured") {
        BitWriter w;
        w.flag(true);
        w.put(0, 3);
        w.put(0, 3);
        w.flag(true);   // drc_repeat_profile_flag
        w.put(3, 3);    // drc_repeat_id 3: never configured
        w.put(0, 8);
        const auto result = run(w, stereo);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kInvalidStream);
    }
    SECTION("channel-dependent gains for 3.0, which neither table covers") {
        BitWriter w;
        one_mode(w, 1);
        put_gainset_size(w, 7);
        w.put(0, 2);
        w.put(64, 7);
        w.put(0, 16);
        const auto result = run(w, DrcContext{.b_iframe = true, .ch_mode = ch_mode::k3_0, .frame_len_base = 2048});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kUnsupported);
    }
    SECTION("channel-dependent gains at a frame length Table 169 does not list") {
        BitWriter w;
        one_mode(w, 1);
        put_gainset_size(w, 7);
        w.put(0, 2);
        w.put(64, 7);
        w.put(0, 16);
        const auto result = run(w, DrcContext{.b_iframe = true, .ch_mode = ch_mode::kStereo, .frame_len_base = 1000});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kInvalidStream);
    }
    SECTION("a drc_version 1 gainset smaller than its gains") {
        BitWriter w;
        one_mode(w, 0);
        put_gainset_size(w, 3);  // drc_version and drc_gain_val alone take 9
        w.put(1, 2);
        w.put(64, 7);
        w.put(0, 8);
        const auto result = run(w, stereo);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kInvalidStream);
    }
    SECTION("a drc_version 0 gainset of neither size reading") {
        BitWriter w;
        one_mode(w, 0);
        put_gainset_size(w, 20);
        w.put(0, 2);
        w.put(64, 7);
        w.put(0, 8);
        const auto result = run(w, stereo);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kInvalidStream);
    }
    SECTION("a configuration cut short") {
        BitWriter w;
        w.flag(true);
        w.put(7, 3);  // eight modes, and the data ends after the first id
        w.put(0, 3);
        const auto result = run(w, stereo);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
    }
}

TEST_CASE("a sus_ver 0 substream's metadata carries a long drc_frame behind escaped sizes",
          "[ac4dec][metadata]") {
    // drc_version 1 with 150 bits of drc2_bits: drc_gainset_size 159 needs
    // its b_more_bits, and tools_metadata_size (the whole drc_frame() plus
    // dialog_enhancement()) goes past 127 and needs its own.
    BitWriter tools;
    tools.flag(true);    // b_drc_present
    tools.put(0, 3);     // one mode
    tools.put(0, 3);
    tools.flag(false);
    tools.flag(false);
    tools.flag(false);
    tools.put(0, 2);     // drc_gains_config 0: one gain
    tools.put(0, 3);     // drc_eac3_profile
    put_gainset_size(tools, 2 + 7 + 150);
    tools.put(1, 2);     // drc_version
    tools.put(64, 7);    // drc_gain_val
    for (int i = 0; i < 150; ++i) {
        tools.flag(i % 3 == 0);
    }
    tools.flag(false);   // b_de_data_present
    REQUIRE(tools.size() >= 128);

    BitWriter w;
    w.put(20, 7);        // dialnorm_bits
    w.flag(false);       // b_more_basic_metadata
    w.flag(false);       // b_channels_classifier
    w.flag(false);       // b_event_probability
    put_tools(w, tools);

    Recorder rec;
    std::size_t end = 0;
    MetadataState state;
    Metadata out;
    const auto result = read_with(w, rec, end, [&](BitReader& r) {
        return ac4::detail::parse_metadata(r, context(ch_mode::kStereo, 0, true), state, out);
    });
    REQUIRE(result.has_value());
    CHECK(end == w.size());
    CHECK(out.tools_metadata_size == tools.size());
    REQUIRE(out.drc.has_value());
    CHECK(out.drc->gainsets[0].drc_gainset_size == 159U);
    CHECK(out.drc->gainsets[0].drc2_bits == 150U);
    CHECK(out.drc->gainsets[0].nr_drc_channels == 1);
    CHECK(rec.count("tools_metadata_size") == 1);
    CHECK(rec.count("drc_gainset_size") == 1);
}

// --- dialog_enhancement() ---------------------------------------------------

namespace {

// Metadata of a sus_ver 1 substream whose tools are `de`.
BitWriter with_de(const BitWriter& de) {
    BitWriter w;
    put_empty_basic_and_extended_sus1(w);
    put_tools(w, de);
    return w;
}

ParseResult read_metadata(const BitWriter& w, const SubstreamContext& ctx, MetadataState& state, Metadata& out,
                          Recorder& rec) {
    std::size_t end = 0;
    const auto result =
        read_with(w, rec, end, [&](BitReader& r) { return ac4::detail::parse_metadata(r, ctx, state, out); });
    if (result) {
        CHECK(end == w.size());
    }
    return result;
}

void put_de_config(BitWriter& w, int method, int max_gain, int channel_config) {
    w.put(static_cast<std::uint64_t>(method), 2);
    w.put(static_cast<std::uint64_t>(max_gain), 2);
    w.put(static_cast<std::uint64_t>(channel_config), 3);
}

}  // namespace

TEST_CASE("de_nr_channels follows Table 171", "[ac4dec][metadata]") {
    using ac4::detail::de_nr_channels;
    CHECK(de_nr_channels(0b000) == 0);
    CHECK(de_nr_channels(0b001) == 1);
    CHECK(de_nr_channels(0b010) == 1);
    CHECK(de_nr_channels(0b100) == 1);
    CHECK(de_nr_channels(0b011) == 2);
    CHECK(de_nr_channels(0b101) == 2);
    CHECK(de_nr_channels(0b110) == 2);
    CHECK(de_nr_channels(0b111) == 3);
}

TEST_CASE("dialog_enhancement decodes three channels' parameters and carries them between frames",
          "[ac4dec][metadata]") {
    const auto& abs1 = tables::kDeHcbAbs1;
    const auto& diff1 = tables::kDeHcbDiff1;
    MetadataState state;

    // I-frame: de_method 1 (table 1, panning sent), three channels.
    BitWriter de;
    de.flag(true);   // b_de_data_present
    put_de_config(de, 1, 2, 0b111);
    de.put(9, 5);    // de_mix_coef1_idx
    de.put(21, 5);   // de_mix_coef2_idx
    de.code(abs1, abs1.cb_off + 2);
    for (int band = 1; band < 8; ++band) {
        de.code(diff1, diff1.cb_off + 1);
    }
    for (int ch = 1; ch < 3; ++ch) {
        for (int band = 0; band < 8; ++band) {
            de.code(diff1, diff1.cb_off + 1);
        }
    }
    Metadata out;
    Recorder rec;
    REQUIRE(read_metadata(with_de(de), context(ch_mode::k5_1, 1, true), state, out, rec).has_value());
    const auto& enhancement = out.dialog_enhancement;
    CHECK(enhancement.de_config_present);
    CHECK(enhancement.config.de_method == 1);
    CHECK(enhancement.config.de_max_gain == 2);
    CHECK(enhancement.de_nr_channels == 3);
    CHECK(enhancement.data.de_mix_coef1_idx == 9);
    CHECK(enhancement.data.de_mix_coef2_idx == 21);
    for (int band = 0; band < 8; ++band) {
        INFO("band " << band);
        CHECK(enhancement.data.de_par[0][static_cast<std::size_t>(band)] == 2 + band);
        CHECK(enhancement.data.de_par[1][static_cast<std::size_t>(band)] == 3 + band);
        CHECK(enhancement.data.de_par[2][static_cast<std::size_t>(band)] == 4 + band);
    }
    CHECK(rec.count("de_par_code") == 24);

    // A later frame: panning kept, parameters time-differential (-1 each).
    BitWriter next;
    next.flag(true);   // b_de_data_present
    next.flag(false);  // b_de_config_flag: the I-frame's configuration
    next.flag(true);   // de_keep_pos_flag
    next.flag(false);  // de_keep_data_flag
    for (int i = 0; i < 24; ++i) {
        next.code(diff1, diff1.cb_off - 1);
    }
    Metadata later;
    Recorder rec2;
    REQUIRE(read_metadata(with_de(next), context(ch_mode::k5_1, 1, false), state, later, rec2).has_value());
    CHECK_FALSE(later.dialog_enhancement.de_config_present);
    CHECK(later.dialog_enhancement.data.de_keep_pos_flag);
    CHECK(later.dialog_enhancement.data.de_mix_coef1_idx == 9);
    CHECK(later.dialog_enhancement.data.de_mix_coef2_idx == 21);
    CHECK(later.dialog_enhancement.data.de_par[0][0] == 1);
    CHECK(later.dialog_enhancement.data.de_par[2][7] == 10);

    // And one that keeps the data too.
    BitWriter kept;
    kept.flag(true);
    kept.flag(false);  // b_de_config_flag
    kept.flag(true);   // de_keep_pos_flag
    kept.flag(true);   // de_keep_data_flag
    Metadata third;
    Recorder rec3;
    REQUIRE(read_metadata(with_de(kept), context(ch_mode::k5_1, 1, false), state, third, rec3).has_value());
    CHECK(third.dialog_enhancement.data.de_keep_data_flag);
    CHECK(third.dialog_enhancement.data.de_par == later.dialog_enhancement.data.de_par);
    CHECK(rec3.count("de_par_code") == 0);
}

TEST_CASE("dialog_enhancement reads M/S processing and the signal contribution", "[ac4dec][metadata]") {
    const auto& abs0 = tables::kDeHcbAbs0;
    const auto& diff0 = tables::kDeHcbDiff0;

    SECTION("de_method 0 with M/S codes one of two channels") {
        BitWriter de;
        de.flag(true);
        put_de_config(de, 0, 0, 0b110);
        de.flag(true);   // de_ms_proc_flag
        de.code(abs0, 5);
        for (int band = 1; band < 8; ++band) {
            de.code(diff0, diff0.cb_off);
        }
        MetadataState state;
        Metadata out;
        Recorder rec;
        REQUIRE(read_metadata(with_de(de), context(ch_mode::kStereo, 1, true), state, out, rec).has_value());
        CHECK(out.dialog_enhancement.data.de_ms_proc_flag);
        CHECK(out.dialog_enhancement.data.de_par[0][7] == 5);
        CHECK(out.dialog_enhancement.data.de_par[1][0] == 0);  // not coded this frame
        CHECK(rec.count("de_par_code") == 8);
        CHECK(rec.count("de_mix_coef1_idx") == 0);
    }
    SECTION("de_method 2 sends a contribution after two channels") {
        BitWriter de;
        de.flag(true);
        put_de_config(de, 2, 1, 0b101);
        de.flag(false);  // de_ms_proc_flag
        de.code(abs0, 3);
        for (int band = 1; band < 16; ++band) {
            de.code(diff0, diff0.cb_off);
        }
        de.put(17, 5);   // de_signal_contribution
        MetadataState state;
        Metadata out;
        Recorder rec;
        REQUIRE(read_metadata(with_de(de), context(ch_mode::k3_0, 1, true), state, out, rec).has_value());
        CHECK(out.dialog_enhancement.data.de_signal_contribution == 17);
        CHECK(out.dialog_enhancement.data.de_par[1][7] == 3);
    }
    SECTION("de_method 3 with two channels sends one mixing coefficient") {
        const auto& abs1 = tables::kDeHcbAbs1;
        const auto& diff1 = tables::kDeHcbDiff1;
        BitWriter de;
        de.flag(true);
        put_de_config(de, 3, 3, 0b011);
        de.put(4, 5);    // de_mix_coef1_idx, and no coef2 for two channels
        de.code(abs1, abs1.cb_off);
        for (int band = 1; band < 16; ++band) {
            de.code(diff1, diff1.cb_off);
        }
        de.put(2, 5);    // de_signal_contribution
        MetadataState state;
        Metadata out;
        Recorder rec;
        REQUIRE(read_metadata(with_de(de), context(ch_mode::k5_0, 1, true), state, out, rec).has_value());
        CHECK(out.dialog_enhancement.data.de_mix_coef1_idx == 4);
        CHECK(rec.count("de_mix_coef2_idx") == 0);
        CHECK(out.dialog_enhancement.data.de_signal_contribution == 2);
    }
    SECTION("a configuration of no channels reads no parameters") {
        BitWriter de;
        de.flag(true);
        put_de_config(de, 0, 0, 0b000);
        MetadataState state;
        Metadata out;
        Recorder rec;
        REQUIRE(read_metadata(with_de(de), context(ch_mode::kStereo, 1, true), state, out, rec).has_value());
        CHECK(out.dialog_enhancement.de_nr_channels == 0);
        CHECK(rec.count("de_par_code") == 0);
    }
}

TEST_CASE("dialog_enhancement's configuration follows the I-frame rule", "[ac4dec][metadata]") {
    const auto& abs0 = tables::kDeHcbAbs0;
    const auto& diff0 = tables::kDeHcbDiff0;
    // One centre channel, de_method 0.
    const auto one_channel_iframe_data = [&](BitWriter& de) {
        de.code(abs0, 1);
        for (int band = 1; band < 8; ++band) {
            de.code(diff0, diff0.cb_off);
        }
    };

    SECTION("a non-I-frame with no configuration before it") {
        BitWriter de;
        de.flag(true);
        de.flag(false);  // b_de_config_flag
        de.put(0, 16);
        MetadataState state;
        Metadata out;
        Recorder rec;
        const auto result = read_metadata(with_de(de), context(ch_mode::kMono, 1, false), state, out, rec);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kMissingIFrame);
    }
    SECTION("a non-I-frame that sends its own configuration") {
        BitWriter de;
        de.flag(true);
        de.flag(true);   // b_de_config_flag
        put_de_config(de, 0, 1, 0b001);
        de.flag(false);  // de_keep_data_flag
        for (int band = 0; band < 8; ++band) {
            de.code(diff0, diff0.cb_off + 2);  // time-differential from zero
        }
        MetadataState state;
        Metadata out;
        Recorder rec;
        REQUIRE(read_metadata(with_de(de), context(ch_mode::kMono, 1, false), state, out, rec).has_value());
        CHECK(out.dialog_enhancement.b_de_config_flag);
        CHECK(out.dialog_enhancement.de_config_present);
        CHECK(out.dialog_enhancement.data.de_par[0][3] == 2);
        CHECK(state.de.config_valid);
    }
    SECTION("an I-frame without dialogue enhancement forgets the configuration") {
        MetadataState state;
        BitWriter first;
        first.flag(true);
        put_de_config(first, 0, 0, 0b001);
        one_channel_iframe_data(first);
        Metadata out;
        Recorder rec;
        REQUIRE(read_metadata(with_de(first), context(ch_mode::kMono, 1, true), state, out, rec).has_value());
        REQUIRE(state.de.config_valid);

        BitWriter absent;
        absent.flag(false);
        REQUIRE(read_metadata(with_de(absent), context(ch_mode::kMono, 1, true), state, out, rec).has_value());
        CHECK_FALSE(state.de.config_valid);
        CHECK(state.de.data.de_par_prev[0][0] == 0);
    }
}

TEST_CASE("a 9.X.4 substream's dialog_enhancement carries a simulcast core set", "[ac4dec][metadata]") {
    const auto& abs0 = tables::kDeHcbAbs0;
    const auto& diff0 = tables::kDeHcbDiff0;
    BitWriter de;
    de.flag(true);
    put_de_config(de, 0, 0, 0b001);
    de.code(abs0, 6);
    for (int band = 1; band < 8; ++band) {
        de.code(diff0, diff0.cb_off);
    }
    de.flag(true);   // b_de_simulcast
    de.code(abs0, 2);
    for (int band = 1; band < 8; ++band) {
        de.code(diff0, diff0.cb_off + 1);
    }

    BitWriter w;
    w.flag(false);   // b_more_basic_metadata
    w.flag(false);   // b_dialog
    w.flag(false);   // b_channels_classifier
    w.flag(false);   // b_event_probability
    put_tools(w, de);
    MetadataState state;
    Metadata out;
    Recorder rec;
    REQUIRE(read_metadata(w, context(ch_mode::k9_1_4, 1, true), state, out, rec).has_value());
    CHECK(out.dialog_enhancement.b_de_simulcast);
    CHECK(out.dialog_enhancement.data.de_par[0][7] == 6);
    CHECK(out.dialog_enhancement.core_data.de_par[0][0] == 2);
    CHECK(out.dialog_enhancement.core_data.de_par[0][7] == 9);
    CHECK(rec.count("de_par_code") == 16);

    // A 9.X.4 frame without the simulcast set clears the core chain.
    BitWriter single;
    single.flag(true);
    single.flag(false);  // b_de_config_flag
    single.flag(true);   // de_keep_data_flag
    single.flag(false);  // b_de_simulcast
    BitWriter w2;
    w2.flag(false);
    w2.flag(false);
    w2.flag(false);
    w2.flag(false);
    put_tools(w2, single);
    Metadata later;
    REQUIRE(read_metadata(w2, context(ch_mode::k9_1_4, 1, false), state, later, rec).has_value());
    CHECK_FALSE(later.dialog_enhancement.b_de_simulcast);
    CHECK(state.de.core_data.de_par_prev[0][7] == 0);
    CHECK(later.dialog_enhancement.data.de_par[0][7] == 6);
}

TEST_CASE("dialog_enhancement fails on a codeword cut short by the end of the substream",
          "[ac4dec][metadata]") {
    // de_config() then one bit of a DE_HCB_ABS_0 codeword whose shortest
    // codeword is longer: the substream ends inside it.
    BitWriter w;
    put_empty_basic_and_extended_sus1(w);
    w.put(40, 7);    // tools_metadata_size_value
    w.flag(false);
    w.flag(true);    // b_de_data_present
    put_de_config(w, 0, 0, 0b001);
    MetadataState state;
    Metadata out;
    Recorder rec;
    std::size_t end = 0;
    const auto result = read_with(w, rec, end, [&](BitReader& r) {
        return ac4::detail::parse_metadata(r, context(ch_mode::kMono, 1, true), state, out);
    });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().error == DecodeError::kTruncated);
}

// --- emdf_payloads_substream() ----------------------------------------------

TEST_CASE("emdf_payloads_substream reads each payload configuration", "[ac4dec][metadata]") {
    BitWriter w;
    // Payload 1: the escaped id, a sample offset and every optional field.
    w.put(31, 5);
    w.variable_bits(40, 5);   // emdf_payload_id 31 + 40
    w.flag(true);             // b_smpoffst
    w.variable_bits(300, 11);
    w.flag(true);             // b_duration
    w.variable_bits(2000, 11);
    w.flag(true);             // b_groupid
    w.variable_bits(3, 2);
    w.flag(true);             // b_codecdata
    w.put(0x5A, 8);
    w.flag(false);            // b_discard_unknown_payload
    w.put(19, 5);             // priority (b_smpoffst: no frame alignment bit)
    w.put(2, 2);              // proc_allowed
    w.variable_bits(2, 8);    // emdf_payload_size
    w.put(0x01, 8);
    w.put(0xFE, 8);
    // Payload 2: frame aligned, with the duplicate flags.
    w.put(3, 5);
    w.flag(false);            // b_smpoffst
    w.flag(false);
    w.flag(false);
    w.flag(false);
    w.flag(false);            // b_discard_unknown_payload
    w.flag(true);             // b_payload_frame_aligned
    w.flag(true);             // b_create_duplicate
    w.flag(false);            // b_remove_duplicate
    w.put(1, 5);              // priority
    w.put(3, 2);              // proc_allowed
    w.variable_bits(0, 8);
    // Payload 3: neither aligned nor offset, so no priority.
    w.put(4, 5);
    w.put(0, 4);
    w.flag(false);            // b_discard_unknown_payload
    w.flag(false);            // b_payload_frame_aligned
    w.variable_bits(0, 8);
    w.put(0, 5);              // end
    w.align();

    Recorder rec;
    std::size_t end = 0;
    EmdfPayloads out;
    const auto result =
        read_with(w, rec, end, [&](BitReader& r) { return ac4::detail::parse_emdf_payloads_substream(r, out); });
    REQUIRE(result.has_value());
    CHECK(end == w.size());
    REQUIRE(out.payloads.size() == 3);
    const auto& first = out.payloads[0];
    CHECK(first.emdf_payload_id == 71U);
    CHECK(first.config.smpoffst == 300U);
    CHECK(first.config.duration == 2000U);
    CHECK(first.config.groupid == 3U);
    CHECK(first.config.codecdata == 0x5A);
    CHECK_FALSE(first.config.b_payload_frame_aligned);
    CHECK(first.config.priority == 19);
    CHECK(first.config.proc_allowed == 2);
    CHECK(first.bytes == std::vector<std::uint8_t>{0x01, 0xFE});
    const auto& second = out.payloads[1];
    CHECK(second.config.b_payload_frame_aligned);
    CHECK(second.config.b_create_duplicate);
    CHECK_FALSE(second.config.b_remove_duplicate);
    CHECK(second.config.priority == 1);
    CHECK(second.bytes.empty());
    const auto& third = out.payloads[2];
    CHECK_FALSE(third.config.priority.has_value());
    CHECK_FALSE(third.config.smpoffst.has_value());
}

TEST_CASE("emdf_payloads_substream refuses a payload size past the end", "[ac4dec][metadata]") {
    BitWriter w;
    w.put(2, 5);
    w.put(0, 4);
    w.flag(true);            // b_discard_unknown_payload
    w.variable_bits(50, 8);  // 50 bytes, and two follow
    w.put(0, 16);
    Recorder rec;
    std::size_t end = 0;
    EmdfPayloads out;
    const auto result =
        read_with(w, rec, end, [&](BitReader& r) { return ac4::detail::parse_emdf_payloads_substream(r, out); });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().error == DecodeError::kTruncated);
    CHECK(rec.count("emdf_payload_byte") == 0);
}
