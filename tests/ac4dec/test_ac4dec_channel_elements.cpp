// The AC-4 decoder's channel elements (src/ac4dec/src/syntax/
// channel_elements.cpp) and the A-SPX and A-CPL data they carry (aspx.cpp,
// acpl.cpp), on hand-built bitstreams. The committed DEE streams are stereo
// ASPX, 5.1 ASPX_ACPL and IMS; every other codec mode and coding
// configuration is reached only here.
//
// The element bitstreams are kept as small as the syntax allows, so that a
// case can be read against the tables: frame_len_base 2048, long frames, and
// max_sfb 0, so that every sf_info() is 7 bits (b_long_frame, max_sfb), every
// sf_data() 9 (reference_scale_factor, b_snf_data_exists) and every
// chparam_info() 2 (sap_mode 0). A-SPX runs over one signal and one noise
// subband group (aspx_start_freq 7, aspx_xover_subband_offset 5), and A-CPL
// over 7 parameter bands.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4dec_bits.hpp"
#include "bit_reader.hpp"
#include "syntax/acpl.hpp"
#include "syntax/aspx.hpp"
#include "syntax/channel_elements.hpp"
#include "syntax/context.hpp"
#include "syntax/substream.hpp"

namespace {

using ac4::DecodeError;
using ac4::detail::AcplConfig1ch;
using ac4::detail::AcplConfig2ch;
using ac4::detail::AcplConfigKind;
using ac4::detail::AcplData1ch;
using ac4::detail::AcplData2ch;
using ac4::detail::AcplDataType;
using ac4::detail::AcplHcbType;
using ac4::detail::AspxConfig;
using ac4::detail::AspxData1ch;
using ac4::detail::AspxData2ch;
using ac4::detail::AspxDataType;
using ac4::detail::AspxElementState;
using ac4::detail::AspxHcbType;
using ac4::detail::AspxIntClass;
using ac4::detail::AspxStereoMode;
using ac4::detail::BitReader;
using ac4::detail::ChannelElement;
using ac4::detail::ChannelElementState;
using ac4::detail::Codebook;
using ac4::detail::ElementKind;
using ac4::detail::ParseResult;
using ac4::detail::SubstreamContext;
using ac4dec_test::BitWriter;
using ac4dec_test::Recorder;
namespace ch_mode = ac4::detail::ch_mode;
namespace codec_mode = ac4::detail::codec_mode;
namespace immersive_mode = ac4::detail::immersive_mode;

// The shortest codeword of a codebook, and its index.
void put_shortest(BitWriter& w, const Codebook& codebook) {
    w.put(codebook.sorted[0].code, codebook.sorted[0].bits);
}

int shortest_index(const Codebook& codebook) { return codebook.sorted[0].index; }

SubstreamContext context(int mode, bool b_iframe) {
    SubstreamContext ctx;
    ctx.ch_mode = mode;
    ctx.b_iframe = b_iframe;
    ctx.frame_len_base = 2048;
    return ctx;
}

// Writes channel elements field by field, in the shape the file comment
// describes.
struct ElementWriter {
    BitWriter w;
    bool iframe = true;

    void sf_info() {
        w.flag(true);   // b_long_frame
        w.put(0, 6);    // max_sfb
    }
    void sf_info_dual() {
        sf_info();
        w.put(0, 6);    // max_sfb_side
    }
    void sf_info_side_limited() {
        w.flag(true);
        w.put(0, 5);    // max_sfb_side: n_side_bits at 2048
    }
    void sf_info_lfe() { w.put(0, 3); }  // max_sfb: n_msfbl_bits at 2048
    void sf_data() {
        w.put(0, 8);    // reference_scale_factor
        w.flag(false);  // b_snf_data_exists
    }
    void chparam() { w.put(0, 2); }  // sap_mode 0

    void mono(bool lfe = false) {
        if (lfe) {
            sf_info_lfe();
        } else {
            w.flag(false);  // spec_frontend: ASF
            sf_info();
        }
        sf_data();
    }
    void stereo(bool mdct) {
        w.flag(mdct);   // b_enable_mdct_stereo_proc
        if (mdct) {
            sf_info();
            chparam();
        } else {
            w.flag(false);
            sf_info();
            w.flag(false);
            sf_info();
        }
        sf_data();
        sf_data();
    }
    void two_channel(bool mdct) {
        w.flag(mdct);
        sf_info();
        if (mdct) {
            chparam();
        } else {
            sf_info();
        }
        sf_data();
        sf_data();
    }
    void three_channel() {
        sf_info();
        w.put(5, 4);    // chel_matsel
        chparam();
        chparam();
        for (int i = 0; i < 3; ++i) {
            sf_data();
        }
    }
    void four_channel() {
        sf_info();
        for (int i = 0; i < 4; ++i) {
            chparam();
        }
        for (int i = 0; i < 4; ++i) {
            sf_data();
        }
    }
    void five_channel() {
        sf_info();
        w.put(9, 4);    // chel_matsel
        for (int i = 0; i < 5; ++i) {
            chparam();
        }
        for (int i = 0; i < 5; ++i) {
            sf_data();
        }
    }
    // max_sfb_master 0, then two chparam_info() and two sf_data().
    void residuals() {
        w.put(0, 5);    // max_sfb_master: n_side_bits of a 2048 transform
        chparam();
        chparam();
        sf_data();
        sf_data();
    }
    // num_chan > 1: sync_flag 1, one b_compand_on 1.
    void companding(int num_chan) {
        if (num_chan > 1) {
            w.flag(true);
        }
        w.flag(true);
    }

    void aspx_config() {
        w.put(0, 1);    // aspx_quant_mode_env
        w.put(7, 3);    // aspx_start_freq: six master subband groups
        w.put(0, 2);    // aspx_stop_freq
        w.put(0, 1);    // aspx_master_freq_scale: low resolution
        w.flag(false);  // aspx_interpolation
        w.flag(false);  // aspx_preflat
        w.flag(false);  // aspx_limiter
        w.put(0, 2);    // aspx_noise_sbg: one noise group
        w.put(0, 1);    // aspx_num_env_bits_fixfix
        w.put(0, 2);    // aspx_freq_res_mode 0: signalled
    }
    // FIXFIX, one envelope, low resolution.
    void aspx_framing() {
        w.put(0, 1);    // aspx_int_class FIXFIX
        w.put(0, 1);    // tmp_num_env
        w.put(0, 1);    // aspx_freq_res
    }
    void aspx_envelopes(AspxStereoMode mode) {
        put_shortest(w, ac4::detail::aspx_codebook(AspxDataType::kSignal, 0, mode, AspxHcbType::kF0));
    }
    void aspx_noise(AspxStereoMode mode) {
        put_shortest(w, ac4::detail::aspx_codebook(AspxDataType::kNoise, 0, mode, AspxHcbType::kF0));
    }
    void aspx_1ch() {
        if (iframe) {
            w.put(5, 3);  // aspx_xover_subband_offset
        }
        aspx_framing();
        w.flag(false);  // aspx_sig_delta_dir
        w.flag(false);  // aspx_noise_delta_dir
        w.put(1, 2);    // aspx_tna_mode
        w.flag(false);  // aspx_ah_present
        w.flag(false);  // aspx_fic_present
        w.flag(false);  // aspx_tic_present
        aspx_envelopes(AspxStereoMode::kLevel);
        aspx_noise(AspxStereoMode::kLevel);
    }
    void aspx_2ch(bool balance = false) {
        if (iframe) {
            w.put(5, 3);
        }
        aspx_framing();
        w.flag(balance);  // aspx_balance
        if (!balance) {
            aspx_framing();
        }
        for (int ch = 0; ch < 2; ++ch) {
            w.flag(false);  // aspx_sig_delta_dir
            w.flag(false);  // aspx_noise_delta_dir
        }
        w.put(1, 2);        // aspx_tna_mode, left
        if (!balance) {
            w.put(2, 2);    // right
        }
        w.flag(false);      // aspx_ah_left
        w.flag(false);      // aspx_ah_right
        w.flag(false);      // aspx_fic_present
        w.flag(false);      // aspx_tic_present
        const AspxStereoMode right = balance ? AspxStereoMode::kBalance : AspxStereoMode::kLevel;
        aspx_envelopes(AspxStereoMode::kLevel);
        aspx_envelopes(right);
        aspx_noise(AspxStereoMode::kLevel);
        aspx_noise(right);
    }

    void acpl_config_1ch(bool partial) {
        w.put(3, 2);    // acpl_num_param_bands_id: 7 bands
        w.put(0, 1);    // acpl_quant_mode: fine
        if (partial) {
            w.put(7, 3);  // acpl_qmf_band_minus1: band 8, parameter band 4
        }
    }
    void acpl_config_2ch() {
        w.put(3, 2);
        w.put(0, 1);    // acpl_quant_mode_0
        w.put(1, 1);    // acpl_quant_mode_1
    }
    void acpl_params(AcplDataType type, int quant_mode, int start, int bands) {
        w.put(0, 1);    // diff_type: DIFF_FREQ
        put_shortest(w, ac4::detail::acpl_codebook(type, quant_mode, AcplHcbType::kF0));
        for (int band = start + 1; band < bands; ++band) {
            put_shortest(w, ac4::detail::acpl_codebook(type, quant_mode, AcplHcbType::kDf));
        }
    }
    void acpl_1ch(bool partial) {
        w.put(0, 1);    // acpl_interpolation_type: smooth
        w.put(0, 1);    // acpl_num_param_sets_cod: one set
        const int start = partial ? 4 : 0;
        acpl_params(AcplDataType::kAlpha, 0, start, 7);
        acpl_params(AcplDataType::kBeta, 0, start, 7);
    }
    void acpl_2ch() {
        w.put(0, 1);
        w.put(0, 1);
        for (int i = 0; i < 2; ++i) {
            acpl_params(AcplDataType::kAlpha, 0, 0, 7);
        }
        for (int i = 0; i < 2; ++i) {
            acpl_params(AcplDataType::kBeta, 0, 0, 7);
        }
        acpl_params(AcplDataType::kBeta3, 0, 0, 7);
        for (int i = 0; i < 6; ++i) {
            acpl_params(AcplDataType::kGamma, 1, 0, 7);
        }
    }
};

ParseResult read_element(const BitWriter& w, const SubstreamContext& ctx, ChannelElementState& state,
                         ChannelElement& out, Recorder* rec = nullptr) {
    const std::vector<std::byte> bytes = w.bytes();
    Recorder local;
    BitReader reader(bytes, 0, rec != nullptr ? *rec : local);
    const ParseResult result = ac4::detail::parse_audio_data_chan(reader, ctx, state, out);
    if (result) {
        CHECK(reader.position() == w.size());
    }
    return result;
}

struct Shape {
    std::size_t tracks = 0;
    std::size_t infos = 0;
    std::size_t chparams = 0;
    std::size_t aspx_1ch = 0;
    std::size_t aspx_2ch = 0;
    std::size_t acpl_1ch = 0;
    bool acpl_2ch = false;
};

void check_shape(const ChannelElement& out, const Shape& shape) {
    CHECK(out.tracks.size() == shape.tracks);
    CHECK(out.infos.size() == shape.infos);
    CHECK(out.chparams.size() == shape.chparams);
    CHECK(out.aspx_1ch.size() == shape.aspx_1ch);
    CHECK(out.aspx_2ch.size() == shape.aspx_2ch);
    CHECK(out.acpl_1ch.size() == shape.acpl_1ch);
    CHECK(out.acpl_2ch.has_value() == shape.acpl_2ch);
}

}  // namespace

// --- single_channel_element() and channel_pair_element() ------------------

TEST_CASE("single_channel_element reads SIMPLE and ASPX mono", "[ac4dec][channel_elements]") {
    SECTION("SIMPLE") {
        ElementWriter e;
        e.w.put(0, 1);  // mono_codec_mode
        e.mono();
        ChannelElementState state;
        ChannelElement out;
        REQUIRE(read_element(e.w, context(ch_mode::kMono, true), state, out).has_value());
        CHECK(out.kind == ElementKind::kSingle);
        CHECK(out.codec_mode == codec_mode::kSimple);
        CHECK_FALSE(out.companding.has_value());
        check_shape(out, {.tracks = 1, .infos = 1});
    }
    SECTION("ASPX, with companding off and averaged") {
        ElementWriter e;
        e.w.put(1, 1);
        e.aspx_config();
        e.w.flag(false);  // b_compand_on
        e.w.flag(true);   // b_compand_avg
        e.mono();
        e.aspx_1ch();
        ChannelElementState state;
        ChannelElement out;
        REQUIRE(read_element(e.w, context(ch_mode::kMono, true), state, out).has_value());
        CHECK(out.codec_mode == codec_mode::kAspx);
        REQUIRE(out.companding.has_value());
        CHECK(out.companding->num_chan == 1);
        CHECK_FALSE(out.companding->b_compand_on[0]);
        CHECK(out.companding->b_compand_avg);
        check_shape(out, {.tracks = 1, .infos = 1, .aspx_1ch = 1});
        const AspxData1ch& aspx = out.aspx_1ch[0];
        CHECK(aspx.xover_subband_offset == 5);
        CHECK(aspx.num_aspx_timeslots == 16);
        CHECK(aspx.groups.num_sbg_master == 6);
        CHECK(aspx.groups.num_sbg_sig_highres == 1);
        CHECK(aspx.groups.num_sbg_noise == 1);
        CHECK(aspx.channel.tna_mode[0] == 1);
        CHECK(aspx.channel.sig[0].huff_index[0] ==
              shortest_index(ac4::detail::aspx_codebook(AspxDataType::kSignal, 0, AspxStereoMode::kLevel,
                                                        AspxHcbType::kF0)));
        REQUIRE(state.aspx_config.has_value());
        CHECK(state.aspx_config->start_freq == 7);

        // The next frame, not an I-frame, uses the configuration and the
        // crossover the I-frame sent.
        ElementWriter next;
        next.iframe = false;
        next.w.put(1, 1);
        next.w.flag(true);  // b_compand_on
        next.mono();
        next.aspx_1ch();
        ChannelElement later;
        REQUIRE(read_element(next.w, context(ch_mode::kMono, false), state, later).has_value());
        REQUIRE(later.aspx_1ch.size() == 1);
        CHECK(later.aspx_1ch[0].xover_subband_offset == 5);
        CHECK_FALSE(later.companding->b_compand_avg);
    }
}

TEST_CASE("channel_pair_element reads every stereo codec mode", "[ac4dec][channel_elements]") {
    SECTION("SIMPLE, with and without MDCT stereo processing") {
        for (const bool mdct : {true, false}) {
            ElementWriter e;
            e.w.put(0, 2);  // stereo_codec_mode
            e.stereo(mdct);
            ChannelElementState state;
            ChannelElement out;
            REQUIRE(read_element(e.w, context(ch_mode::kStereo, true), state, out).has_value());
            CHECK(out.kind == ElementKind::kPair);
            REQUIRE(out.b_enable_mdct_stereo_proc.size() == 1);
            CHECK(out.b_enable_mdct_stereo_proc[0] == mdct);
            check_shape(out, {.tracks = 2, .infos = mdct ? 1U : 2U, .chparams = mdct ? 1U : 0U});
        }
    }
    SECTION("ASPX, with and without a balanced pair") {
        for (const bool balance : {false, true}) {
            ElementWriter e;
            e.w.put(1, 2);
            e.aspx_config();
            e.companding(2);
            e.stereo(true);
            e.aspx_2ch(balance);
            ChannelElementState state;
            ChannelElement out;
            REQUIRE(read_element(e.w, context(ch_mode::kStereo, true), state, out).has_value());
            REQUIRE(out.companding.has_value());
            CHECK(out.companding->sync_flag);
            check_shape(out, {.tracks = 2, .infos = 1, .chparams = 1, .aspx_2ch = 1});
            const AspxData2ch& aspx = out.aspx_2ch[0];
            CHECK(aspx.balance == balance);
            CHECK(aspx.channels[1].tna_mode[0] == (balance ? 1 : 2));
            CHECK(aspx.channels[1].stereo_mode == (balance ? AspxStereoMode::kBalance : AspxStereoMode::kLevel));
        }
    }
    SECTION("ASPX_ACPL_1, with MDCT stereo processing: a dual max_sfb") {
        ElementWriter e;
        e.w.put(2, 2);
        e.aspx_config();
        e.acpl_config_1ch(true);
        e.companding(1);
        e.w.flag(true);   // b_enable_mdct_stereo_proc
        e.sf_info_dual();
        e.chparam();
        e.sf_data();
        e.sf_data();
        e.aspx_1ch();
        e.acpl_1ch(true);
        ChannelElementState state;
        ChannelElement out;
        REQUIRE(read_element(e.w, context(ch_mode::kStereo, true), state, out).has_value());
        check_shape(out, {.tracks = 2, .infos = 1, .chparams = 1, .aspx_1ch = 1, .acpl_1ch = 1});
        CHECK(out.infos[0].psy.b_dual_maxsfb);
        CHECK(out.tracks[1].side_channel);
        REQUIRE(state.acpl_config_1ch.has_value());
        CHECK(state.acpl_config_1ch->kind == AcplConfigKind::kPartial);
        CHECK(state.acpl_config_1ch->qmf_band == 8);
        CHECK(state.acpl_config_1ch->param_band == 4);
        const AcplData1ch& acpl = out.acpl_1ch[0];
        CHECK(acpl.num_bands == 7);
        CHECK(acpl.start_band == 4);
        CHECK(acpl.alpha1.sets[0].huff_index[4] ==
              shortest_index(ac4::detail::acpl_codebook(AcplDataType::kAlpha, 0, AcplHcbType::kF0)));
    }
    SECTION("ASPX_ACPL_1, without: a side-limited side channel") {
        ElementWriter e;
        e.w.put(2, 2);
        e.aspx_config();
        e.acpl_config_1ch(true);
        e.companding(1);
        e.w.flag(false);  // b_enable_mdct_stereo_proc
        e.w.flag(false);  // spec_frontend_m
        e.sf_info();
        e.w.flag(false);  // spec_frontend_s
        e.sf_info_side_limited();
        e.sf_data();
        e.sf_data();
        e.aspx_1ch();
        e.acpl_1ch(true);
        ChannelElementState state;
        ChannelElement out;
        REQUIRE(read_element(e.w, context(ch_mode::kStereo, true), state, out).has_value());
        check_shape(out, {.tracks = 2, .infos = 2, .aspx_1ch = 1, .acpl_1ch = 1});
        CHECK(out.infos[1].psy.b_side_limited);
    }
    SECTION("ASPX_ACPL_2") {
        ElementWriter e;
        e.w.put(3, 2);
        e.aspx_config();
        e.acpl_config_1ch(false);
        e.companding(1);
        e.mono();
        e.aspx_1ch();
        e.acpl_1ch(false);
        ChannelElementState state;
        ChannelElement out;
        REQUIRE(read_element(e.w, context(ch_mode::kStereo, true), state, out).has_value());
        CHECK(out.codec_mode == codec_mode::kAspxAcpl2);
        check_shape(out, {.tracks = 1, .infos = 1, .aspx_1ch = 1, .acpl_1ch = 1});
        CHECK(state.acpl_config_1ch->kind == AcplConfigKind::kFull);
        CHECK(out.acpl_1ch[0].start_band == 0);
    }
}

TEST_CASE("a channel element that needs I-frame configuration refuses a frame without it",
          "[ac4dec][channel_elements]") {
    SECTION("no I-frame at all") {
        ElementWriter e;
        e.iframe = false;
        e.w.put(1, 2);  // ASPX
        e.companding(2);
        e.stereo(true);
        e.aspx_2ch();
        ChannelElementState state;
        ChannelElement out;
        const auto result = read_element(e.w, context(ch_mode::kStereo, false), state, out);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kMissingIFrame);
    }
    SECTION("an I-frame in another codec mode") {
        ElementWriter first;
        first.w.put(0, 2);
        first.stereo(true);
        ChannelElementState state;
        ChannelElement out;
        REQUIRE(read_element(first.w, context(ch_mode::kStereo, true), state, out).has_value());
        ElementWriter next;
        next.iframe = false;
        next.w.put(1, 2);
        next.companding(2);
        next.stereo(true);
        next.aspx_2ch();
        const auto result = read_element(next.w, context(ch_mode::kStereo, false), state, out);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kMissingIFrame);
    }
    SECTION("carried state that names the mode but holds no configuration") {
        // Only a damaged state gets here; each data element checks for its
        // own configuration all the same.
        struct Case {
            int mode;
            int stereo_codec_mode;
        };
        for (const Case c : {Case{ch_mode::kMono, -1}, Case{ch_mode::kStereo, codec_mode::kAspx},
                             Case{ch_mode::kStereo, codec_mode::kAspxAcpl2}}) {
            ChannelElementState state;
            ElementWriter e;
            e.iframe = false;
            if (c.mode == ch_mode::kMono) {
                state.configured_codec_mode = codec_mode::kAspx;
                state.configured_kind = ElementKind::kSingle;
                e.w.put(1, 1);
                e.companding(1);
                e.mono();
            } else {
                state.configured_codec_mode = c.stereo_codec_mode;
                state.configured_kind = ElementKind::kPair;
                state.aspx_config = AspxConfig{.valid = true, .start_freq = 7};
                state.aspx[0].have_xover_subband_offset = true;
                state.aspx[0].xover_subband_offset = 5;
                if (c.stereo_codec_mode == codec_mode::kAspx) {
                    state.aspx_config.reset();
                    e.w.put(1, 2);
                    e.companding(2);
                    e.stereo(true);
                } else {
                    e.w.put(3, 2);
                    e.companding(1);
                    e.mono();
                    e.aspx_1ch();
                }
            }
            e.w.put(0, 64);
            ChannelElement out;
            const auto result = read_element(e.w, context(c.mode, false), state, out);
            INFO("ch_mode " << c.mode << ", codec mode " << c.stereo_codec_mode);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().error == DecodeError::kMissingIFrame);
        }
    }
}

TEST_CASE("channel elements refuse the speech frontend and the modes not decoded", "[ac4dec][channel_elements]") {
    SECTION("spec_frontend SSF") {
        ElementWriter e;
        e.w.put(0, 1);
        e.w.flag(true);  // spec_frontend: SSF
        e.w.put(0, 16);
        ChannelElementState state;
        ChannelElement out;
        const auto result = read_element(e.w, context(ch_mode::kMono, true), state, out);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kUnsupported);
    }
    SECTION("9.X.4, 22.2 and reserved channel modes") {
        BitWriter w;
        w.put(0, 32);
        for (const int mode : {ch_mode::k9_0_4, ch_mode::k9_1_4, ch_mode::k22_2}) {
            ChannelElementState state;
            ChannelElement out;
            const auto result = read_element(w, context(mode, true), state, out);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().error == DecodeError::kUnsupported);
            CHECK(result.error().reason.find(mode == ch_mode::k22_2 ? "22_2" : "9.X.4") != std::string_view::npos);
        }
        // The 7.X.4 modes read their immersive element, which 32 zero bits
        // cannot hold: SCPL with its LFE, grouping 0 and the first sf_info()s.
        for (const int mode : {ch_mode::k7_0_4, ch_mode::k7_1_4}) {
            ChannelElementState state;
            ChannelElement out;
            const auto result = read_element(w, context(mode, true), state, out);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().error == DecodeError::kTruncated);
            CHECK(out.kind == ElementKind::kImmersive);
            CHECK(out.codec_mode == immersive_mode::kScpl);
        }
        ChannelElementState state;
        ChannelElement out;
        const auto reserved = read_element(w, context(16, true), state, out);
        REQUIRE_FALSE(reserved.has_value());
        CHECK(reserved.error().error == DecodeError::kInvalidStream);
    }
}

// --- 3_0_channel_element() -------------------------------------------------

TEST_CASE("3_0_channel_element reads both coding configurations, SIMPLE and ASPX", "[ac4dec][channel_elements]") {
    for (const int mode : {codec_mode::kSimple, codec_mode::kAspx}) {
        for (const int config : {0, 1}) {
            INFO("3_0_codec_mode " << mode << ", 3_0_coding_config " << config);
            ElementWriter e;
            e.w.put(static_cast<std::uint64_t>(mode), 1);
            if (mode == codec_mode::kAspx) {
                e.aspx_config();
                // companding_control(3) unsynchronised: three flags, one off.
                e.w.flag(false);  // sync_flag
                e.w.flag(true);
                e.w.flag(false);
                e.w.flag(true);
                e.w.flag(false);  // b_compand_avg
            }
            e.w.put(static_cast<std::uint64_t>(config), 1);
            if (config == 0) {
                e.stereo(false);
                e.mono();
            } else {
                e.three_channel();
            }
            if (mode == codec_mode::kAspx) {
                e.aspx_2ch();
                e.aspx_1ch();
            }
            ChannelElementState state;
            ChannelElement out;
            REQUIRE(read_element(e.w, context(ch_mode::k3_0, true), state, out).has_value());
            CHECK(out.kind == ElementKind::k3_0);
            CHECK(out.coding_config == config);
            CHECK(out.tracks.size() == 3);
            if (config == 1) {
                CHECK(out.chel_matsel == std::vector<int>{5});
                CHECK(out.chparams.size() == 2);
            }
            if (mode == codec_mode::kAspx) {
                REQUIRE(out.companding.has_value());
                CHECK_FALSE(out.companding->sync_flag);
                CHECK(out.companding->b_compand_on[0]);
                CHECK_FALSE(out.companding->b_compand_on[1]);
                CHECK(out.aspx_2ch.size() == 1);
                CHECK(out.aspx_1ch.size() == 1);
            }
        }
    }
}

// --- 5_X_channel_element() -------------------------------------------------

TEST_CASE("5_X_channel_element reads the four SIMPLE and ASPX coding configurations", "[ac4dec][channel_elements]") {
    for (const int mode : {codec_mode::kSimple, codec_mode::kAspx}) {
        for (const int config : {0, 1, 2, 3}) {
            for (const bool lfe : {false, true}) {
                INFO("5_X_codec_mode " << mode << ", coding_config " << config << ", lfe " << lfe);
                ElementWriter e;
                e.w.put(static_cast<std::uint64_t>(mode), 3);
                if (mode == codec_mode::kAspx) {
                    e.aspx_config();
                }
                if (lfe) {
                    e.mono(true);
                }
                if (mode == codec_mode::kAspx) {
                    e.companding(5);
                }
                e.w.put(static_cast<std::uint64_t>(config), 2);
                std::size_t tracks = 0;
                switch (config) {
                    case 0:
                        e.w.flag(true);  // 2ch_mode
                        e.two_channel(true);
                        e.two_channel(false);
                        e.mono();
                        tracks = 5;
                        break;
                    case 1:
                        e.three_channel();
                        e.two_channel(true);
                        tracks = 5;
                        break;
                    case 2:
                        e.four_channel();
                        e.mono();
                        tracks = 5;
                        break;
                    default:
                        e.five_channel();
                        tracks = 5;
                        break;
                }
                if (mode == codec_mode::kAspx) {
                    e.aspx_2ch();
                    e.aspx_2ch(true);
                    e.aspx_1ch();
                }
                ChannelElementState state;
                ChannelElement out;
                const int ch = lfe ? ch_mode::k5_1 : ch_mode::k5_0;
                REQUIRE(read_element(e.w, context(ch, true), state, out).has_value());
                CHECK(out.kind == ElementKind::k5X);
                CHECK(out.coding_config == config);
                CHECK(out.tracks.size() == tracks + (lfe ? 1U : 0U));
                CHECK(out.two_ch_mode.has_value() == (config == 0));
                if (lfe) {
                    CHECK(out.tracks[0].lfe);
                    CHECK(out.infos[0].is_lfe);
                }
                if (mode == codec_mode::kAspx) {
                    CHECK(out.aspx_2ch.size() == 2);
                    CHECK(out.aspx_1ch.size() == 1);
                    CHECK(out.companding->num_chan == 5);
                }
            }
        }
    }
}

TEST_CASE("5_X_channel_element reads the three A-CPL codec modes", "[ac4dec][channel_elements]") {
    SECTION("ASPX_ACPL_1 and ASPX_ACPL_2, both coding configurations") {
        for (const int mode : {codec_mode::kAspxAcpl1, codec_mode::kAspxAcpl2}) {
            for (const int config : {0, 1}) {
                INFO("5_X_codec_mode " << mode << ", coding_config " << config);
                const bool partial = mode == codec_mode::kAspxAcpl1;
                ElementWriter e;
                e.w.put(static_cast<std::uint64_t>(mode), 3);
                e.aspx_config();
                e.acpl_config_1ch(partial);
                e.mono(true);   // 5.1's LFE
                e.companding(3);
                e.w.put(static_cast<std::uint64_t>(config), 1);
                if (config != 0) {
                    e.three_channel();
                } else {
                    e.two_channel(false);
                }
                if (partial) {
                    e.residuals();
                }
                if (config == 0) {
                    e.mono();
                }
                e.aspx_2ch();
                e.aspx_1ch();
                e.acpl_1ch(partial);
                e.acpl_1ch(partial);
                ChannelElementState state;
                ChannelElement out;
                REQUIRE(read_element(e.w, context(ch_mode::k5_1, true), state, out).has_value());
                CHECK(out.codec_mode == mode);
                CHECK(out.acpl_1ch.size() == 2);
                CHECK(out.max_sfb_master.has_value() == partial);
                // LFE, three channel tracks (or two and a mono), and two
                // residuals for ASPX_ACPL_1.
                CHECK(out.tracks.size() == (partial ? 6U : 4U));
            }
        }
    }
    SECTION("ASPX_ACPL_3") {
        ElementWriter e;
        e.w.put(4, 3);
        e.aspx_config();
        e.acpl_config_2ch();
        e.companding(2);
        e.stereo(true);
        e.aspx_2ch();
        e.acpl_2ch();
        ChannelElementState state;
        ChannelElement out;
        REQUIRE(read_element(e.w, context(ch_mode::k5_0, true), state, out).has_value());
        CHECK(out.codec_mode == codec_mode::kAspxAcpl3);
        check_shape(out, {.tracks = 2, .infos = 1, .chparams = 1, .aspx_2ch = 1, .acpl_2ch = true});
        REQUIRE(state.acpl_config_2ch.has_value());
        CHECK(state.acpl_config_2ch->num_param_bands == 7);
        CHECK(state.acpl_config_2ch->quant_mode_1 == 1);
        CHECK(out.acpl_2ch->gamma[5].quant_mode == 1);
        CHECK(out.acpl_2ch->beta3.data_type == AcplDataType::kBeta3);
    }
    SECTION("reserved codec modes") {
        for (const int mode : {5, 6, 7}) {
            BitWriter w;
            w.put(static_cast<std::uint64_t>(mode), 3);
            w.put(0, 13);
            ChannelElementState state;
            ChannelElement out;
            const auto result = read_element(w, context(ch_mode::k5_1, true), state, out);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().error == DecodeError::kInvalidStream);
        }
    }
}

// --- 7_X_channel_element() -------------------------------------------------

TEST_CASE("7_X_channel_element reads SIMPLE and ASPX with and without SAP for the additional channels",
          "[ac4dec][channel_elements]") {
    for (const int mode : {codec_mode::kSimple, codec_mode::kAspx}) {
        for (const int config : {0, 1, 2, 3}) {
            for (const int ch : {ch_mode::k7_1_340, ch_mode::k7_0_520, ch_mode::k7_1_322}) {
                const bool use_sap = config % 2 == 0;
                INFO("7_X_codec_mode " << mode << ", coding_config " << config << ", ch_mode " << ch);
                ElementWriter e;
                e.w.put(static_cast<std::uint64_t>(mode), 2);
                if (mode == codec_mode::kAspx) {
                    e.aspx_config();
                }
                const bool lfe = ch == ch_mode::k7_1_340 || ch == ch_mode::k7_1_322;
                if (lfe) {
                    e.mono(true);
                }
                e.w.put(static_cast<std::uint64_t>(config), 2);
                switch (config) {
                    case 0:
                        e.w.flag(ch == ch_mode::k7_1_340);  // 2ch_mode
                        e.two_channel(false);
                        e.two_channel(true);
                        break;
                    case 1:
                        e.three_channel();
                        e.two_channel(false);
                        break;
                    case 2:
                        e.four_channel();
                        break;
                    default:
                        e.five_channel();
                        break;
                }
                e.w.flag(use_sap);  // b_use_sap_add_ch
                if (use_sap) {
                    e.chparam();
                    e.chparam();
                }
                e.two_channel(true);
                if (config == 0 || config == 2) {
                    e.mono();
                }
                if (mode == codec_mode::kAspx) {
                    e.aspx_2ch();
                    e.aspx_2ch();
                    e.aspx_1ch();
                    e.aspx_2ch();
                }
                ChannelElementState state;
                ChannelElement out;
                REQUIRE(read_element(e.w, context(ch, true), state, out).has_value());
                CHECK(out.kind == ElementKind::k7X);
                CHECK(out.b_use_sap_add_ch == use_sap);
                // Five main channels, whichever configuration carries them,
                // and the two additional ones.
                CHECK(out.tracks.size() == 5U + 2U + (lfe ? 1U : 0U));
                if (mode == codec_mode::kAspx) {
                    CHECK(out.aspx_2ch.size() == 3);
                    CHECK(out.aspx_1ch.size() == 1);
                }
            }
        }
    }
}

TEST_CASE("7_X_channel_element reads the A-CPL codec modes", "[ac4dec][channel_elements]") {
    for (const int mode : {codec_mode::kAspxAcpl1, codec_mode::kAspxAcpl2}) {
        for (const int config : {0, 3}) {
            for (const bool add_ch_base : {false, true}) {
                const int ch = add_ch_base ? ch_mode::k7_0_322 : ch_mode::k7_0_340;
                const bool partial = mode == codec_mode::kAspxAcpl1;
                INFO("7_X_codec_mode " << mode << ", coding_config " << config << ", add_ch_base " << add_ch_base);
                ElementWriter e;
                e.w.put(static_cast<std::uint64_t>(mode), 2);
                e.aspx_config();
                e.acpl_config_1ch(partial);
                e.companding(5);
                e.w.put(static_cast<std::uint64_t>(config), 2);
                if (config == 0) {
                    e.w.flag(false);  // 2ch_mode
                    e.two_channel(true);
                    e.two_channel(true);
                } else {
                    e.five_channel();
                }
                if (partial) {
                    e.residuals();
                }
                if (config == 0) {
                    e.mono();
                }
                e.aspx_2ch();
                e.aspx_2ch();
                e.aspx_1ch();
                e.acpl_1ch(partial);
                e.acpl_1ch(partial);
                SubstreamContext ctx = context(ch, true);
                ctx.add_ch_base = add_ch_base;
                ChannelElementState state;
                ChannelElement out;
                REQUIRE(read_element(e.w, ctx, state, out).has_value());
                CHECK(out.codec_mode == mode);
                CHECK_FALSE(out.b_use_sap_add_ch.has_value());
                CHECK(out.acpl_1ch.size() == 2);
                CHECK(out.aspx_2ch.size() == 2);
                CHECK(out.tracks.size() == 5U + (partial ? 2U : 0U));
            }
        }
    }
}

// --- aspx_data_1ch() and aspx_data_2ch() -----------------------------------

namespace {

AspxConfig aspx_config(int freq_res_mode, int num_env_bits_fixfix = 1, int quant_mode_env = 0) {
    AspxConfig config;
    config.valid = true;
    config.quant_mode_env = static_cast<std::uint8_t>(quant_mode_env);
    config.start_freq = 7;
    config.num_env_bits_fixfix = static_cast<std::uint8_t>(num_env_bits_fixfix);
    config.freq_res_mode = static_cast<std::uint8_t>(freq_res_mode);
    return config;
}

ParseResult read_aspx_1ch(const BitWriter& w, const SubstreamContext& ctx, const AspxConfig& config,
                          AspxElementState& state, AspxData1ch& out, bool exact = true) {
    const std::vector<std::byte> bytes = w.bytes();
    Recorder rec;
    BitReader reader(bytes, 0, rec);
    const ParseResult result = ac4::detail::parse_aspx_data_1ch(reader, ctx, config, state, out);
    if (result && exact) {
        CHECK(reader.position() == w.size());
    }
    return result;
}

// One channel's envelopes: `sig` signal envelopes of `sig_bands` codewords
// each and `noise` noise envelopes of one, all frequency-differential.
void put_envelopes(BitWriter& w, int sig, int sig_bands, int noise, int qmode, AspxStereoMode mode) {
    for (int env = 0; env < sig; ++env) {
        put_shortest(w, ac4::detail::aspx_codebook(AspxDataType::kSignal, qmode, mode, AspxHcbType::kF0));
        for (int band = 1; band < sig_bands; ++band) {
            put_shortest(w, ac4::detail::aspx_codebook(AspxDataType::kSignal, qmode, mode, AspxHcbType::kDf));
        }
    }
    for (int env = 0; env < noise; ++env) {
        put_shortest(w, ac4::detail::aspx_codebook(AspxDataType::kNoise, 0, mode, AspxHcbType::kF0));
    }
}

}  // namespace

TEST_CASE("aspx_data_1ch reads each interval class", "[ac4dec][channel_elements]") {
    // aspx_start_freq 7 and crossover 3: three high-resolution signal groups,
    // two low-resolution ones, one noise group; 16 time slots, so the
    // relative borders and counts take 2 bits.
    SECTION("FIXFIX with four envelopes, the resolution signalled") {
        const AspxConfig config = aspx_config(0);
        BitWriter w;
        w.put(3, 3);      // aspx_xover_subband_offset
        w.put(0, 1);      // FIXFIX
        w.put(2, 2);      // tmp_num_env: four envelopes
        w.put(1, 1);      // aspx_freq_res: high
        for (int i = 0; i < 4 + 2; ++i) {
            w.flag(i == 1);  // delta_dir: envelope 1 time-differential
        }
        w.put(0, 2);      // aspx_tna_mode
        w.flag(true);     // aspx_ah_present
        w.put(0b101, 3);  // aspx_add_harmonic x3
        w.flag(true);     // aspx_fic_present
        w.put(0b011, 3);  // aspx_fic_used_in_sfb x3
        w.flag(true);     // aspx_tic_present
        w.put(0xF0F0, 16);  // aspx_tic_used_in_slot x16
        // Signal: four high-resolution envelopes of 3 bands, the second
        // time-differential; two noise envelopes.
        const auto sig = [](AspxHcbType type) {
            return ac4::detail::aspx_codebook(AspxDataType::kSignal, 0, AspxStereoMode::kLevel, type);
        };
        const auto noise = [](AspxHcbType type) {
            return ac4::detail::aspx_codebook(AspxDataType::kNoise, 0, AspxStereoMode::kLevel, type);
        };
        for (int env = 0; env < 4; ++env) {
            if (env == 1) {
                for (int band = 0; band < 3; ++band) {
                    put_shortest(w, sig(AspxHcbType::kDt));
                }
            } else {
                put_shortest(w, sig(AspxHcbType::kF0));
                put_shortest(w, sig(AspxHcbType::kDf));
                put_shortest(w, sig(AspxHcbType::kDf));
            }
        }
        put_shortest(w, noise(AspxHcbType::kF0));
        put_shortest(w, noise(AspxHcbType::kF0));
        AspxElementState state;
        AspxData1ch out;
        REQUIRE(read_aspx_1ch(w, context(ch_mode::kMono, true), config, state, out).has_value());
        const auto& f = out.channel.framing;
        CHECK(f.int_class == AspxIntClass::kFixFix);
        CHECK(f.num_env == 4);
        CHECK(f.num_noise == 2);
        CHECK(f.atsg_sig[2] == 8);
        CHECK(f.atsg_noise[1] == 8);
        CHECK(f.atsg_freqres[3] == 1);
        CHECK(out.channel.qmode_env == 0);
        CHECK(out.channel.add_harmonic[0]);
        CHECK_FALSE(out.channel.add_harmonic[1]);
        CHECK(out.channel.fic_used_in_sfb[2]);
        CHECK(out.channel.tic_used_in_slot[0]);
        CHECK_FALSE(out.channel.tic_used_in_slot[4]);
        CHECK(out.channel.sig[1].delta_dir == 1);
        CHECK(out.channel.sig[1].num_sbg == 3);
        CHECK(state.previous_stop_offset[0] == 0);
    }
    SECTION("FIXVAR, then a VARFIX that starts where it stopped") {
        BitWriter w;
        w.put(3, 3);
        w.put(0b10, 2);   // FIXVAR
        w.put(2, 2);      // aspx_var_bord_right
        w.put(1, 2);      // aspx_num_rel_right
        w.put(1, 2);      // aspx_rel_bord_right: 4
        w.put(0, 2);      // aspx_tsg_ptr: ptr_bits for three: none set
        for (int i = 0; i < 2 + 2; ++i) {
            w.flag(false);
        }
        w.put(0, 2);
        w.flag(false);
        w.flag(false);
        w.flag(false);
        put_envelopes(w, 2, 2, 2, 1, AspxStereoMode::kLevel);  // FIXVAR keeps the configured quant mode
        AspxElementState state;
        AspxData1ch out;
        const AspxConfig coarse = aspx_config(1, 1, 1);  // resolution always low, coarse envelopes
        REQUIRE(read_aspx_1ch(w, context(ch_mode::kMono, true), coarse, state, out).has_value());
        const auto& f = out.channel.framing;
        CHECK(f.int_class == AspxIntClass::kFixVar);
        CHECK(f.num_env == 2);
        CHECK(f.atsg_sig[0] == 0);
        CHECK(f.atsg_sig[2] == 18);
        CHECK(f.atsg_sig[1] == 14);
        CHECK(f.tsg_ptr == -1);
        CHECK(f.atsg_noise[1] == 14);  // FIXVAR, no pointer: the last inner border
        CHECK(out.channel.qmode_env == 1);
        CHECK(state.previous_stop_offset[0] == 2);

        // A non-I-frame VARFIX begins at the previous stop, 18 - 16 = 2.
        BitWriter next;
        next.put(0b110, 3);  // VARFIX
        next.put(1, 2);      // aspx_num_rel_left
        next.put(2, 2);      // aspx_rel_bord_left: 6
        next.put(2, 2);      // aspx_tsg_ptr 2: pointer 1
        for (int i = 0; i < 4; ++i) {
            next.flag(false);
        }
        next.put(0, 2);
        next.flag(false);
        next.flag(false);
        next.flag(false);
        put_envelopes(next, 2, 2, 2, 1, AspxStereoMode::kLevel);
        AspxData1ch later;
        REQUIRE(read_aspx_1ch(next, context(ch_mode::kMono, false), coarse, state, later).has_value());
        const auto& g = later.channel.framing;
        CHECK(g.int_class == AspxIntClass::kVarFix);
        CHECK(g.atsg_sig[0] == 2);
        CHECK(g.atsg_sig[1] == 8);
        CHECK(g.atsg_sig[2] == 16);
        CHECK(g.tsg_ptr == 1);
        CHECK(g.atsg_noise[1] == 8);  // VARFIX with a pointer: num_env - 1
        CHECK(later.xover_subband_offset == 3);
    }
    SECTION("VARVAR in an I-frame, with the resolution derived from the length") {
        const AspxConfig config = aspx_config(2);
        BitWriter w;
        w.put(3, 3);
        w.put(0b111, 3);  // VARVAR
        w.put(1, 2);      // aspx_var_bord_left
        w.put(1, 2);      // aspx_num_rel_left
        w.put(0, 2);      // aspx_rel_bord_left: 2
        w.put(0, 2);      // aspx_var_bord_right
        w.put(1, 2);      // aspx_num_rel_right
        w.put(3, 2);      // aspx_rel_bord_right: 8
        w.put(3, 3);      // aspx_tsg_ptr 3 (three bits for three envelopes): pointer 2
        for (int i = 0; i < 3 + 2; ++i) {
            w.flag(false);
        }
        w.put(0, 2);
        w.flag(false);
        w.flag(false);
        w.flag(false);
        // Borders 1, 3, 8, 16: lengths 2, 5, 8. Mode 2 makes an envelope high
        // resolution before the pointer (16 > 8 slots) or when 12 * length
        // exceeds 2 * 16 + 39 = 71: envelopes 0 and 1 and 2 are all high.
        put_envelopes(w, 3, 3, 2, 0, AspxStereoMode::kLevel);
        AspxElementState state;
        AspxData1ch out;
        REQUIRE(read_aspx_1ch(w, context(ch_mode::kMono, true), config, state, out).has_value());
        const auto& f = out.channel.framing;
        CHECK(f.int_class == AspxIntClass::kVarVar);
        CHECK(f.atsg_sig[0] == 1);
        CHECK(f.atsg_sig[1] == 3);
        CHECK(f.atsg_sig[2] == 8);
        CHECK(f.atsg_sig[3] == 16);
        CHECK(f.atsg_noise[1] == 8);  // VARVAR with pointer 2
        CHECK(f.atsg_freqres[0] == 1);
        CHECK(f.atsg_freqres[1] == 1);
        CHECK(f.atsg_freqres[2] == 1);
    }
    SECTION("FIXFIX with the resolution always high") {
        const AspxConfig config = aspx_config(3, 0);
        BitWriter w;
        w.put(3, 3);
        w.put(0, 1);
        w.put(0, 1);      // tmp_num_env: one bit, one envelope
        w.flag(false);
        w.flag(false);
        w.put(0, 2);
        w.flag(false);
        w.flag(false);
        w.flag(false);
        put_envelopes(w, 1, 3, 1, 0, AspxStereoMode::kLevel);
        AspxElementState state;
        AspxData1ch out;
        REQUIRE(read_aspx_1ch(w, context(ch_mode::kMono, true), config, state, out).has_value());
        CHECK(out.channel.framing.atsg_freqres[0] == 1);
        CHECK(out.channel.sig[0].num_sbg == 3);
    }
}

TEST_CASE("aspx_data_1ch refuses what its syntax cannot follow", "[ac4dec][channel_elements]") {
    const auto refused = [](const BitWriter& w, const SubstreamContext& ctx, const AspxConfig& config,
                            AspxElementState& state) {
        AspxData1ch out;
        const auto result = read_aspx_1ch(w, ctx, config, state, out);
        REQUIRE_FALSE(result.has_value());
        return result.error().error;
    };
    BitWriter zeros;
    zeros.put(0, 64);

    SECTION("no configuration") {
        AspxElementState state;
        CHECK(refused(zeros, context(ch_mode::kMono, true), AspxConfig{}, state) == DecodeError::kMissingIFrame);
    }
    SECTION("no crossover from an I-frame") {
        AspxElementState state;
        CHECK(refused(zeros, context(ch_mode::kMono, false), aspx_config(0), state) == DecodeError::kMissingIFrame);
    }
    SECTION("a frame length with no time slot count") {
        AspxElementState state;
        SubstreamContext ctx = context(ch_mode::kMono, true);
        ctx.frame_len_base = 1000;
        CHECK(refused(zeros, ctx, aspx_config(0), state) == DecodeError::kInvalidStream);
    }
    SECTION("a crossover past the master subband groups") {
        BitWriter w;
        w.put(7, 3);
        w.put(0, 61);
        AspxElementState state;
        CHECK(refused(w, context(ch_mode::kMono, true), aspx_config(0), state) == DecodeError::kInvalidStream);
    }
    SECTION("eight FIXFIX envelopes") {
        BitWriter w;
        w.put(3, 3);
        w.put(0, 1);
        w.put(3, 2);  // tmp_num_env 3
        w.put(0, 58);
        AspxElementState state;
        CHECK(refused(w, context(ch_mode::kMono, true), aspx_config(0), state) == DecodeError::kInvalidStream);
    }
    SECTION("more than five envelopes") {
        BitWriter w;
        w.put(3, 3);
        w.put(0b111, 3);  // VARVAR
        w.put(0, 2);
        w.put(3, 2);      // three left borders
        w.put(0, 6);
        w.put(0, 2);
        w.put(3, 2);      // three right borders: seven envelopes
        w.put(0, 44);
        AspxElementState state;
        CHECK(refused(w, context(ch_mode::kMono, true), aspx_config(0), state) == DecodeError::kInvalidStream);
    }
    SECTION("an envelope codeword cut short") {
        BitWriter w;
        w.put(3, 3);
        w.put(0, 1);
        w.put(0, 2);
        w.put(0, 1);
        w.flag(false);
        w.flag(false);
        w.put(0, 2);
        w.flag(false);
        w.flag(false);
        w.flag(false);
        AspxElementState state;
        AspxData1ch out;
        const auto result = read_aspx_1ch(w, context(ch_mode::kMono, true), aspx_config(0), state, out, false);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
    }
}

TEST_CASE("derive_aspx_subband_groups counts the noise groups and the timeslots", "[ac4dec][channel_elements]") {
    AspxConfig config = aspx_config(0);
    config.start_freq = 0;
    config.master_freq_scale = 1;
    config.noise_sbg = 3;
    ac4::detail::AspxSubbandGroups groups;
    REQUIRE(ac4::detail::derive_aspx_subband_groups(config, 0, groups).has_value());
    CHECK(groups.num_sbg_master == 22);
    CHECK(groups.sba == 18);
    CHECK(groups.sbz == 62);
    CHECK(groups.num_sbg_sig_highres == 22);
    CHECK(groups.num_sbg_sig_lowres == 11);
    // 3 * log2(62 / 18) + 0.5 = 5.85: five noise groups.
    CHECK(groups.num_sbg_noise == 5);
    CHECK_FALSE(ac4::detail::derive_aspx_subband_groups(config, -1, groups).has_value());

    // A lower crossover with a wider noise band: more than five groups.
    AspxConfig wide = aspx_config(0);
    wide.start_freq = 0;
    wide.noise_sbg = 3;
    const auto refused = ac4::detail::derive_aspx_subband_groups(wide, 0, groups);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().error == DecodeError::kInvalidStream);

    using ac4::detail::aspx_num_timeslots;
    CHECK(aspx_num_timeslots(2048) == 16);
    CHECK(aspx_num_timeslots(1920) == 15);
    CHECK(aspx_num_timeslots(1536) == 12);
    CHECK(aspx_num_timeslots(1024) == 16);
    CHECK(aspx_num_timeslots(960) == 15);
    CHECK(aspx_num_timeslots(768) == 12);
    CHECK(aspx_num_timeslots(512) == 8);
    CHECK(aspx_num_timeslots(384) == 6);
    CHECK(aspx_num_timeslots(100) == 0);
}

TEST_CASE("aspx_data_2ch reads the interleaved-waveform flags of both channels", "[ac4dec][channel_elements]") {
    const AspxConfig config = aspx_config(0, 0);
    for (const int variant : {0, 1, 2}) {
        INFO("variant " << variant);
        BitWriter w;
        w.put(5, 3);       // crossover 5: one group of each kind
        w.put(0, 1);       // FIXFIX, one envelope
        w.put(0, 1);
        w.put(0, 1);
        w.flag(false);     // aspx_balance
        w.put(0, 1);       // right channel's own framing
        w.put(0, 1);
        w.put(0, 1);
        for (int i = 0; i < 4; ++i) {
            w.flag(false);
        }
        w.put(1, 2);       // tna left
        w.put(3, 2);       // tna right
        w.flag(true);      // aspx_ah_left
        w.flag(true);
        w.flag(true);      // aspx_ah_right
        w.flag(false);
        w.flag(true);      // aspx_fic_present
        w.flag(variant != 1);  // aspx_fic_left
        if (variant != 1) {
            w.flag(true);
        }
        w.flag(variant != 0);  // aspx_fic_right
        if (variant != 0) {
            w.flag(true);
        }
        w.flag(true);      // aspx_tic_present
        w.flag(variant == 2);  // aspx_tic_copy
        if (variant != 2) {
            w.flag(variant == 0);  // aspx_tic_left
            w.flag(variant == 1);  // aspx_tic_right
        }
        w.put(0xAAAA, 16);  // the one channel's aspx_tic_used_in_slot
        for (int ch = 0; ch < 2; ++ch) {
            put_shortest(w, ac4::detail::aspx_codebook(AspxDataType::kSignal, 0, AspxStereoMode::kLevel,
                                                       AspxHcbType::kF0));
        }
        for (int ch = 0; ch < 2; ++ch) {
            put_shortest(w, ac4::detail::aspx_codebook(AspxDataType::kNoise, 0, AspxStereoMode::kLevel,
                                                       AspxHcbType::kF0));
        }
        const std::vector<std::byte> bytes = w.bytes();
        Recorder rec;
        BitReader reader(bytes, 0, rec);
        AspxElementState state;
        AspxData2ch out;
        REQUIRE(ac4::detail::parse_aspx_data_2ch(reader, context(ch_mode::kStereo, true), config, state, out)
                    .has_value());
        CHECK(reader.position() == w.size());
        CHECK(out.channels[1].tna_mode[0] == 3);
        CHECK(out.channels[0].add_harmonic[0]);
        CHECK_FALSE(out.channels[1].add_harmonic[0]);
        CHECK(out.fic_left == (variant != 1));
        CHECK(out.fic_right == (variant != 0));
        CHECK(out.tic_copy == (variant == 2));
        CHECK(out.channels[0].tic_used_in_slot[0] == (variant != 1));
        CHECK(out.channels[1].tic_used_in_slot[0] == (variant != 0));
    }
}

// --- acpl_data_1ch() and acpl_data_2ch() ---------------------------------

TEST_CASE("acpl_data_1ch reads steep interpolation with two parameter sets", "[ac4dec][channel_elements]") {
    BitWriter cw;
    cw.put(0, 2);    // 15 bands
    cw.put(1, 1);    // coarse
    const std::vector<std::byte> config_bytes = cw.bytes();
    Recorder rec;
    BitReader config_reader(config_bytes, 0, rec);
    AcplConfig1ch config;
    REQUIRE(ac4::detail::parse_acpl_config_1ch(config_reader, AcplConfigKind::kFull, config).has_value());
    CHECK(config.num_param_bands == 15);
    CHECK(config.quant_mode == 1);

    BitWriter w;
    w.put(1, 1);     // acpl_interpolation_type: steep
    w.put(1, 1);     // two parameter sets
    w.put(3, 5);     // acpl_param_timeslot
    w.put(9, 5);
    for (const AcplDataType type : {AcplDataType::kAlpha, AcplDataType::kBeta}) {
        // Set 0 frequency-differential, set 1 time-differential.
        w.put(0, 1);
        put_shortest(w, ac4::detail::acpl_codebook(type, 1, AcplHcbType::kF0));
        for (int band = 1; band < 15; ++band) {
            put_shortest(w, ac4::detail::acpl_codebook(type, 1, AcplHcbType::kDf));
        }
        w.put(1, 1);
        for (int band = 0; band < 15; ++band) {
            put_shortest(w, ac4::detail::acpl_codebook(type, 1, AcplHcbType::kDt));
        }
    }
    const std::vector<std::byte> bytes = w.bytes();
    BitReader reader(bytes, 0, rec);
    AcplData1ch out;
    REQUIRE(ac4::detail::parse_acpl_data_1ch(reader, context(ch_mode::kStereo, true), config, out).has_value());
    CHECK(reader.position() == w.size());
    CHECK(out.framing.interpolation_type == 1);
    CHECK(out.framing.num_param_sets == 2);
    CHECK(out.framing.param_timeslot[0] == 3);
    CHECK(out.framing.param_timeslot[1] == 9);
    CHECK(out.beta1.sets[1].diff_type == 1);
    CHECK(out.beta1.sets[1].huff_index[14] ==
          shortest_index(ac4::detail::acpl_codebook(AcplDataType::kBeta, 1, AcplHcbType::kDt)));
}

TEST_CASE("A-CPL data refuses a missing configuration and a truncated codeword", "[ac4dec][channel_elements]") {
    BitWriter zeros;
    zeros.put(0, 16);
    const std::vector<std::byte> bytes = zeros.bytes();
    Recorder rec;
    {
        BitReader reader(bytes, 0, rec);
        AcplData1ch out;
        const auto result = ac4::detail::parse_acpl_data_1ch(reader, context(ch_mode::kStereo, true), AcplConfig1ch{},
                                                             out);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kMissingIFrame);
    }
    {
        BitReader reader(bytes, 0, rec);
        AcplData2ch out;
        const auto result = ac4::detail::parse_acpl_data_2ch(reader, context(ch_mode::k5_0, true), AcplConfig2ch{},
                                                             out);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kMissingIFrame);
    }
    {
        // A valid configuration of 15 bands and two bytes of data: the
        // codewords run out.
        AcplConfig2ch config;
        config.valid = true;
        BitReader reader(bytes, 0, rec);
        AcplData2ch out;
        const auto result = ac4::detail::parse_acpl_data_2ch(reader, context(ch_mode::k5_0, true), config, out);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
    }
}

// --- ac4_substream() ---------------------------------------------------------

TEST_CASE("ac4_substream checks audio_size against the substream and the element", "[ac4dec][channel_elements]") {
    const auto run = [](const BitWriter& w, const SubstreamContext& ctx) {
        const std::vector<std::byte> bytes = w.bytes();
        Recorder rec;
        BitReader reader(bytes, 0, rec);
        ac4::detail::AudioSubstreamState state;
        ac4::detail::AudioSubstream out;
        return ac4::detail::parse_audio_substream(reader, ctx, state, out);
    };
    SECTION("an escaped audio_size past the end") {
        BitWriter w;
        w.put(100, 15);
        w.flag(true);           // b_more_bits
        w.variable_bits(1, 7);  // 100 + (1 << 15) bytes
        w.put(0, 32);
        const auto result = run(w, context(ch_mode::kMono, true));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kInvalidStream);
    }
    SECTION("an element longer than audio_size") {
        ElementWriter e;
        e.w.put(1, 15);         // one byte, and the element takes 18 bits
        e.w.flag(false);
        e.w.put(0, 1);
        e.mono();
        e.w.put(0, 32);
        const auto result = run(e.w, context(ch_mode::kMono, true));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kInvalidStream);
    }
    SECTION("a 96 kHz substream read without its extension") {
        BitWriter w;
        w.put(3, 15);
        w.flag(false);
        w.put(0, 40);
        SubstreamContext ctx = context(ch_mode::kMono, true);
        ctx.sf_multiplier = 0;
        const auto result = run(w, ctx);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kUnsupported);
    }
    SECTION("a header cut short") {
        BitWriter w;
        w.put(0, 8);
        const auto result = run(w, context(ch_mode::kMono, true));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == DecodeError::kTruncated);
    }
}
