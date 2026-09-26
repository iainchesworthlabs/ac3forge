#include "syntax/channel_elements.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>

#include "tables/sfb_tables.hpp"

namespace ac4::detail {

namespace {

using std::size_t;

class ElementParser {
   public:
    ElementParser(BitReader& r, const SubstreamContext& ctx, ChannelElementState& state, ChannelElement& out,
                  BitReader* hsf_reader)
        : r_(r), ctx_(ctx), state_(state), out_(out), hsf_reader_(hsf_reader) {}

    ParseResult single_channel_element();
    ParseResult channel_pair_element();
    ParseResult element_3_0();
    ParseResult element_5_x(bool b_has_lfe);
    ParseResult element_7_x();
    ParseResult immersive_element(bool b_lfe);
    ParseResult var_element(int n_dmx_signals, bool b_has_lfe);
    // audio_data_objs()'s mono_data(1), before its element.
    ParseResult objects_lfe() {
        out_.objs_lfe = true;
        return mono_data(true);
    }

   private:
    // --- configuration from I-frames ---
    ParseResult begin(ElementKind kind, int mode, bool needs_aspx, std::optional<AcplConfigKind> acpl_1ch,
                      bool acpl_2ch);

    // --- data elements ---
    ParseResult companding_control(int num_chan);
    ParseResult mono_data(bool b_lfe);
    ParseResult stereo_data();
    ParseResult two_channel_data();
    ParseResult three_channel_data();
    ParseResult four_channel_data();
    ParseResult five_channel_data();
    ParseResult residual_tracks(const std::array<int, 2>& bases);
    ParseResult track_info(size_t mark, int position, int& info) const;
    ParseResult chparam_infos(int count, int info);
    ParseResult aspx_1ch();
    ParseResult aspx_2ch();
    ParseResult acpl_1ch();
    ParseResult acpl_2ch();

    ParseResult add_info(int spec_frontend, bool dual_maxsfb, bool side_limited, int& index);
    ParseResult add_track(int info, bool side_channel, bool lfe);

    BitReader& r_;
    const SubstreamContext& ctx_;
    ChannelElementState& state_;
    ChannelElement& out_;
    int aspx_position_ = 0;
    BitReader* hsf_reader_ = nullptr;
    HsfExtHeader hsf_header_{};
    bool hsf_peeked_ = false;
};

ParseResult ElementParser::begin(ElementKind kind, int mode, bool needs_aspx,
                                 std::optional<AcplConfigKind> acpl_1ch, bool acpl_2ch) {
    out_.kind = kind;
    out_.codec_mode = mode;
    const bool needs_config = needs_aspx || acpl_1ch.has_value() || acpl_2ch;
    if (ctx_.b_iframe) {
        const bool changed = state_.configured_codec_mode != mode || state_.configured_kind != kind;
        state_.configured_codec_mode = mode;
        state_.configured_kind = kind;
        state_.aspx_config.reset();
        state_.acpl_config_1ch.reset();
        state_.acpl_config_2ch.reset();
        if (changed) {
            state_.aspx = {};
        }
        if (needs_aspx) {
            AspxConfig config{};
            if (auto ok = parse_aspx_config(r_, config); !ok) {
                return ok;
            }
            state_.aspx_config = config;
            out_.aspx_config = config;
        }
        if (acpl_1ch) {
            AcplConfig1ch config{};
            if (auto ok = parse_acpl_config_1ch(r_, *acpl_1ch, config); !ok) {
                return ok;
            }
            state_.acpl_config_1ch = config;
        }
        if (acpl_2ch) {
            AcplConfig2ch config{};
            if (auto ok = parse_acpl_config_2ch(r_, config); !ok) {
                return ok;
            }
            state_.acpl_config_2ch = config;
        }
        return check(r_);
    }
    if (needs_config && (state_.configured_codec_mode != mode || state_.configured_kind != kind)) {
        return fail(DecodeError::kMissingIFrame,
                    "this codec mode needs configuration that no I-frame has sent");
    }
    if (needs_aspx) {
        out_.aspx_config = state_.aspx_config;
    }
    return {};
}

ParseResult ElementParser::companding_control(int num_chan) {
    CompandingControl cc;
    cc.num_chan = num_chan;
    if (num_chan > 1) {
        cc.sync_flag = r_.read_flag("sync_flag");
    }
    bool b_need_avg = false;
    const int nc = cc.sync_flag ? 1 : num_chan;
    for (int ch = 0; ch < nc; ++ch) {
        cc.b_compand_on[static_cast<size_t>(ch)] = r_.read_flag("b_compand_on");
        if (!cc.b_compand_on[static_cast<size_t>(ch)]) {
            b_need_avg = true;
        }
    }
    if (b_need_avg) {
        cc.b_compand_avg = r_.read_flag("b_compand_avg");
    }
    out_.companding = cc;
    return check(r_);
}

ParseResult ElementParser::add_info(int spec_frontend, bool dual_maxsfb, bool side_limited, int& index) {
    SfInfo info;
    if (auto ok = parse_sf_info(r_, ctx_, spec_frontend, dual_maxsfb, side_limited, info); !ok) {
        return ok;
    }
    index = static_cast<int>(out_.infos.size());
    out_.infos.push_back(std::move(info));
    return {};
}

ParseResult ElementParser::add_track(int info, bool side_channel, bool lfe) {
    Track track;
    track.info = info;
    track.side_channel = side_channel;
    track.lfe = lfe;
    if (hsf_reader_ != nullptr && !hsf_peeked_) {
        // Table 17's max_sfb_ext_hsf[]: read once, before this element's
        // first track's asf_section_data() needs it, using that track's own
        // b_different_framing - the only one known this early (see
        // parse_audio_data_chan's doc comment).
        const SfInfo& first = out_.infos[static_cast<size_t>(info)];
        if (auto ok = parse_hsf_ext_header(*hsf_reader_, first.psy.b_different_framing, hsf_header_); !ok) {
            return ok;
        }
        hsf_peeked_ = true;
    }
    const HsfExtHeader* hsf = hsf_reader_ != nullptr ? &hsf_header_ : nullptr;
    if (auto ok = parse_sf_data(r_, ctx_, out_.infos[static_cast<size_t>(info)], side_channel, hsf, track.data,
                                track.hsf);
        !ok) {
        return ok;
    }
    out_.tracks.push_back(std::move(track));
    return {};
}

ParseResult ElementParser::chparam_infos(int count, int info) {
    for (int i = 0; i < count; ++i) {
        ChparamInfo chparam;
        if (auto ok = parse_chparam_info(r_, ctx_, out_.infos[static_cast<size_t>(info)], chparam); !ok) {
            return ok;
        }
        out_.chparams.push_back(std::move(chparam));
    }
    return {};
}

// 4.2.6.2 mono_data(b_lfe)
ParseResult ElementParser::mono_data(bool b_lfe) {
    if (b_lfe) {
        SfInfo info;
        if (auto ok = parse_sf_info_lfe(r_, ctx_, info); !ok) {
            return ok;
        }
        const int index = static_cast<int>(out_.infos.size());
        out_.infos.push_back(std::move(info));
        return add_track(index, false, true);
    }
    const int spec_frontend = static_cast<int>(r_.read(1, "spec_frontend"));
    int index = 0;
    if (auto ok = add_info(spec_frontend, false, false, index); !ok) {
        return ok;
    }
    return add_track(index, false, false);
}

// 4.2.6.4 stereo_data()
ParseResult ElementParser::stereo_data() {
    const bool mdct = r_.read_flag("b_enable_mdct_stereo_proc");
    out_.b_enable_mdct_stereo_proc.push_back(mdct);
    if (mdct) {
        int index = 0;
        if (auto ok = add_info(0, false, false, index); !ok) {
            return ok;
        }
        if (auto ok = chparam_infos(1, index); !ok) {
            return ok;
        }
        if (auto ok = add_track(index, false, false); !ok) {
            return ok;
        }
        return add_track(index, false, false);
    }
    const int spec_frontend_l = static_cast<int>(r_.read(1, "spec_frontend_l"));
    int left = 0;
    if (auto ok = add_info(spec_frontend_l, false, false, left); !ok) {
        return ok;
    }
    const int spec_frontend_r = static_cast<int>(r_.read(1, "spec_frontend_r"));
    int right = 0;
    if (auto ok = add_info(spec_frontend_r, false, false, right); !ok) {
        return ok;
    }
    if (auto ok = add_track(left, false, false); !ok) {
        return ok;
    }
    return add_track(right, false, false);
}

// 4.2.6.7 two_channel_data()
ParseResult ElementParser::two_channel_data() {
    const bool mdct = r_.read_flag("b_enable_mdct_stereo_proc");
    out_.b_enable_mdct_stereo_proc.push_back(mdct);
    int info0 = 0;
    int info1 = 0;
    if (auto ok = add_info(0, false, false, info0); !ok) {
        return ok;
    }
    if (mdct) {
        if (auto ok = chparam_infos(1, info0); !ok) {
            return ok;
        }
        info1 = info0;
    } else if (auto ok = add_info(0, false, false, info1); !ok) {
        return ok;
    }
    if (auto ok = add_track(info0, false, false); !ok) {
        return ok;
    }
    return add_track(info1, false, false);
}

// 4.2.6.8 three_channel_data() with 4.2.6.11 three_channel_info()
ParseResult ElementParser::three_channel_data() {
    int info = 0;
    if (auto ok = add_info(0, false, false, info); !ok) {
        return ok;
    }
    out_.chel_matsel.push_back(static_cast<int>(r_.read(4, "chel_matsel")));
    if (auto ok = chparam_infos(2, info); !ok) {
        return ok;
    }
    for (int i = 0; i < 3; ++i) {
        if (auto ok = add_track(info, false, false); !ok) {
            return ok;
        }
    }
    return {};
}

// 4.2.6.9 four_channel_data() with 4.2.6.12 four_channel_info()
ParseResult ElementParser::four_channel_data() {
    int info = 0;
    if (auto ok = add_info(0, false, false, info); !ok) {
        return ok;
    }
    if (auto ok = chparam_infos(4, info); !ok) {
        return ok;
    }
    for (int i = 0; i < 4; ++i) {
        if (auto ok = add_track(info, false, false); !ok) {
            return ok;
        }
    }
    return {};
}

// 4.2.6.10 five_channel_data() with 4.2.6.13 five_channel_info()
ParseResult ElementParser::five_channel_data() {
    int info = 0;
    if (auto ok = add_info(0, false, false, info); !ok) {
        return ok;
    }
    out_.chel_matsel.push_back(static_cast<int>(r_.read(4, "chel_matsel")));
    if (auto ok = chparam_infos(5, info); !ok) {
        return ok;
    }
    for (int i = 0; i < 5; ++i) {
        if (auto ok = add_track(info, false, false); !ok) {
            return ok;
        }
    }
    return {};
}

// The ASPX_ACPL_1 residuals: max_sfb_master, two chparam_info() and two
// sf_data(ASF) (Part 1 Tables 25 and 33). The syntax names no sf_info for
// them. The reading taken, recorded in the errata register: residual i, and
// the chparam_info() for it, have the framing of the track its A-CPL module
// pairs it with (clause 5.3.4.3.2 and Pseudocodes 117 and 120), whose sf_info
// is bases[i]. The largest transform length those two sf_info()s signal sets
// max_sfb_master's width (n_side_bits, Table 106); a window group of that
// length takes max_sfb_master as its max_sfb, and a shorter one the value
// Tables B.8 to B.19 map it to.
ParseResult ElementParser::residual_tracks(const std::array<int, 2>& bases) {
    int master_index = -1;
    int master_length = 0;
    for (const int base : bases) {
        const AsfPsyInfo& psy = out_.infos[static_cast<size_t>(base)].psy;
        const int count = psy.b_long_frame ? 1 : 2;
        for (int k = 0; k < count; ++k) {
            const int index = psy.b_long_frame ? kLongFrameIndex : psy.transf_length[static_cast<size_t>(k)];
            const int length = transform_length_samples(ctx_, index);
            if (length > master_length) {
                master_length = length;
                master_index = index;
            }
        }
    }
    const int bits = n_side_bits(ctx_, master_index);
    if (bits == 0) {
        return fail(DecodeError::kInvalidStream, "no max_sfb_master width for this transform length");
    }
    const int max_sfb_master = static_cast<int>(r_.read(bits, "max_sfb_master"));
    out_.max_sfb_master = max_sfb_master;

    std::array<int, 2> residuals{};
    for (size_t i = 0; i < bases.size(); ++i) {
        SfInfo residual = out_.infos[static_cast<size_t>(bases[i])];
        residual.psy.b_dual_maxsfb = false;
        residual.psy.b_side_limited = false;
        const int halves = residual.psy.b_different_framing ? 2 : 1;
        for (int k = 0; k < halves; ++k) {
            const int index = residual.psy.b_long_frame ? kLongFrameIndex
                                                        : residual.psy.transf_length[static_cast<size_t>(k)];
            const int length = transform_length_samples(ctx_, index);
            const int max_sfb = length == master_length
                                    ? max_sfb_master
                                    : tables::max_sfb_from_master(master_length, max_sfb_master, length);
            if (max_sfb < 0) {
                return fail(DecodeError::kInvalidStream, "max_sfb_master has no mapping to this transform length");
            }
            residual.psy.max_sfb[static_cast<size_t>(k)] = max_sfb;
        }
        residuals[i] = static_cast<int>(out_.infos.size());
        out_.infos.push_back(std::move(residual));
    }
    for (const int residual : residuals) {
        if (auto ok = chparam_infos(1, residual); !ok) {
            return ok;
        }
    }
    for (const int residual : residuals) {
        if (auto ok = add_track(residual, false, false); !ok) {
            return ok;
        }
    }
    return {};
}

// The sf_info of the track at `position` among the tracks read since
// `mark`, in syntax order.
ParseResult ElementParser::track_info(size_t mark, int position, int& info) const {
    const size_t at = mark + static_cast<size_t>(position);
    if (at >= out_.tracks.size()) {
        return fail(DecodeError::kInvalidStream, "the channel data holds fewer tracks than its mapping names");
    }
    info = out_.tracks[at].info;
    return {};
}

ParseResult ElementParser::aspx_1ch() {
    if (!state_.aspx_config) {
        return fail(DecodeError::kMissingIFrame, "aspx_data_1ch() before any aspx_config()");
    }
    if (aspx_position_ >= static_cast<int>(state_.aspx.size())) {
        return fail(DecodeError::kInvalidStream, "more A-SPX data elements than any channel element has");
    }
    AspxData1ch data{};
    auto& position = state_.aspx[static_cast<size_t>(aspx_position_++)];
    if (auto ok = parse_aspx_data_1ch(r_, ctx_, *state_.aspx_config, position, data); !ok) {
        return ok;
    }
    out_.aspx_1ch.push_back(std::move(data));
    return {};
}

ParseResult ElementParser::aspx_2ch() {
    if (!state_.aspx_config) {
        return fail(DecodeError::kMissingIFrame, "aspx_data_2ch() before any aspx_config()");
    }
    if (aspx_position_ >= static_cast<int>(state_.aspx.size())) {
        return fail(DecodeError::kInvalidStream, "more A-SPX data elements than any channel element has");
    }
    AspxData2ch data{};
    auto& position = state_.aspx[static_cast<size_t>(aspx_position_++)];
    if (auto ok = parse_aspx_data_2ch(r_, ctx_, *state_.aspx_config, position, data); !ok) {
        return ok;
    }
    out_.aspx_2ch.push_back(std::move(data));
    return {};
}

ParseResult ElementParser::acpl_1ch() {
    if (!state_.acpl_config_1ch) {
        return fail(DecodeError::kMissingIFrame, "acpl_data_1ch() before any acpl_config_1ch()");
    }
    AcplData1ch data{};
    if (auto ok = parse_acpl_data_1ch(r_, ctx_, *state_.acpl_config_1ch, data); !ok) {
        return ok;
    }
    out_.acpl_1ch.push_back(std::move(data));
    return {};
}

ParseResult ElementParser::acpl_2ch() {
    if (!state_.acpl_config_2ch) {
        return fail(DecodeError::kMissingIFrame, "acpl_data_2ch() before any acpl_config_2ch()");
    }
    AcplData2ch data{};
    if (auto ok = parse_acpl_data_2ch(r_, ctx_, *state_.acpl_config_2ch, data); !ok) {
        return ok;
    }
    out_.acpl_2ch = std::move(data);
    return {};
}

// 4.2.6.1 single_channel_element(b_iframe)
ParseResult ElementParser::single_channel_element() {
    const int mode = static_cast<int>(r_.read(1, "mono_codec_mode"));
    if (auto ok = begin(ElementKind::kSingle, mode, mode == codec_mode::kAspx, std::nullopt, false); !ok) {
        return ok;
    }
    if (mode == codec_mode::kSimple) {
        return mono_data(false);
    }
    if (auto ok = companding_control(1); !ok) {
        return ok;
    }
    if (auto ok = mono_data(false); !ok) {
        return ok;
    }
    return aspx_1ch();
}

// 4.2.6.3 channel_pair_element(b_iframe)
ParseResult ElementParser::channel_pair_element() {
    const int mode = static_cast<int>(r_.read(2, "stereo_codec_mode"));
    std::optional<AcplConfigKind> acpl;
    if (mode == codec_mode::kAspxAcpl1) {
        acpl = AcplConfigKind::kPartial;
    } else if (mode == codec_mode::kAspxAcpl2) {
        acpl = AcplConfigKind::kFull;
    }
    if (auto ok = begin(ElementKind::kPair, mode, mode != codec_mode::kSimple, acpl, false); !ok) {
        return ok;
    }
    switch (mode) {
        case codec_mode::kSimple:
            return stereo_data();
        case codec_mode::kAspx:
            if (auto ok = companding_control(2); !ok) {
                return ok;
            }
            if (auto ok = stereo_data(); !ok) {
                return ok;
            }
            return aspx_2ch();
        case codec_mode::kAspxAcpl1: {
            if (auto ok = companding_control(1); !ok) {
                return ok;
            }
            const bool mdct = r_.read_flag("b_enable_mdct_stereo_proc");
            out_.b_enable_mdct_stereo_proc.push_back(mdct);
            int info_m = 0;
            int info_s = 0;
            if (mdct) {
                if (auto ok = add_info(0, true, false, info_m); !ok) {
                    return ok;
                }
                if (auto ok = chparam_infos(1, info_m); !ok) {
                    return ok;
                }
                info_s = info_m;
            } else {
                const int spec_frontend_m = static_cast<int>(r_.read(1, "spec_frontend_m"));
                if (auto ok = add_info(spec_frontend_m, false, false, info_m); !ok) {
                    return ok;
                }
                const int spec_frontend_s = static_cast<int>(r_.read(1, "spec_frontend_s"));
                if (auto ok = add_info(spec_frontend_s, false, true, info_s); !ok) {
                    return ok;
                }
            }
            if (auto ok = add_track(info_m, false, false); !ok) {
                return ok;
            }
            if (auto ok = add_track(info_s, true, false); !ok) {
                return ok;
            }
            if (auto ok = aspx_1ch(); !ok) {
                return ok;
            }
            return acpl_1ch();
        }
        case codec_mode::kAspxAcpl2: {
            if (auto ok = companding_control(1); !ok) {
                return ok;
            }
            const int spec_frontend = static_cast<int>(r_.read(1, "spec_frontend"));
            int info = 0;
            if (auto ok = add_info(spec_frontend, false, false, info); !ok) {
                return ok;
            }
            if (auto ok = add_track(info, false, false); !ok) {
                return ok;
            }
            if (auto ok = aspx_1ch(); !ok) {
                return ok;
            }
            return acpl_1ch();
        }
        default:
            return fail(DecodeError::kInvalidStream, "stereo_codec_mode out of range");
    }
}

// 4.2.6.5 3_0_channel_element(b_iframe)
ParseResult ElementParser::element_3_0() {
    const int mode = static_cast<int>(r_.read(1, "3_0_codec_mode"));
    if (auto ok = begin(ElementKind::k3_0, mode, mode == codec_mode::kAspx, std::nullopt, false); !ok) {
        return ok;
    }
    if (mode == codec_mode::kAspx) {
        if (auto ok = companding_control(3); !ok) {
            return ok;
        }
    }
    const int config = static_cast<int>(r_.read(1, "3_0_coding_config"));
    out_.coding_config = config;
    if (config == 0) {
        if (auto ok = stereo_data(); !ok) {
            return ok;
        }
        if (auto ok = mono_data(false); !ok) {
            return ok;
        }
    } else if (auto ok = three_channel_data(); !ok) {
        return ok;
    }
    if (mode == codec_mode::kAspx) {
        if (auto ok = aspx_2ch(); !ok) {
            return ok;
        }
        return aspx_1ch();
    }
    return {};
}

// 4.2.6.6 5_X_channel_element(b_has_lfe, b_iframe)
ParseResult ElementParser::element_5_x(bool b_has_lfe) {
    const int mode = static_cast<int>(r_.read(3, "5_X_codec_mode"));
    if (mode > codec_mode::kAspxAcpl3) {
        return fail(DecodeError::kInvalidStream, "5_X_codec_mode 5 to 7 are reserved");
    }
    std::optional<AcplConfigKind> acpl;
    if (mode == codec_mode::kAspxAcpl1) {
        acpl = AcplConfigKind::kPartial;
    } else if (mode == codec_mode::kAspxAcpl2) {
        acpl = AcplConfigKind::kFull;
    }
    if (auto ok = begin(ElementKind::k5X, mode, mode != codec_mode::kSimple, acpl,
                        mode == codec_mode::kAspxAcpl3);
        !ok) {
        return ok;
    }
    if (b_has_lfe) {
        if (auto ok = mono_data(true); !ok) {
            return ok;
        }
    }
    switch (mode) {
        case codec_mode::kSimple:
        case codec_mode::kAspx: {
            if (mode == codec_mode::kAspx) {
                if (auto ok = companding_control(5); !ok) {
                    return ok;
                }
            }
            const int config = static_cast<int>(r_.read(2, "coding_config"));
            out_.coding_config = config;
            ParseResult ok;
            switch (config) {
                case 0:
                    out_.two_ch_mode = r_.read_flag("2ch_mode");
                    ok = two_channel_data();
                    if (ok) {
                        ok = two_channel_data();
                    }
                    if (ok) {
                        ok = mono_data(false);
                    }
                    break;
                case 1:
                    ok = three_channel_data();
                    if (ok) {
                        ok = two_channel_data();
                    }
                    break;
                case 2:
                    ok = four_channel_data();
                    if (ok) {
                        ok = mono_data(false);
                    }
                    break;
                default:
                    ok = five_channel_data();
                    break;
            }
            if (!ok) {
                return ok;
            }
            if (mode == codec_mode::kAspx) {
                if (auto next = aspx_2ch(); !next) {
                    return next;
                }
                if (auto next = aspx_2ch(); !next) {
                    return next;
                }
                return aspx_1ch();
            }
            return {};
        }
        case codec_mode::kAspxAcpl1:
        case codec_mode::kAspxAcpl2: {
            if (auto ok = companding_control(3); !ok) {
                return ok;
            }
            const int config = static_cast<int>(r_.read(1, "coding_config"));
            out_.coding_config = config;
            const size_t track_mark = out_.tracks.size();
            if (config != 0) {
                if (auto ok = three_channel_data(); !ok) {
                    return ok;
                }
            } else if (auto ok = two_channel_data(); !ok) {
                return ok;
            }
            if (mode == codec_mode::kAspxAcpl1) {
                // Table 181: the channel data's first two tracks are A and B,
                // which the two A-CPL modules pair with the residuals.
                std::array<int, 2> bases{};
                for (int i = 0; i < 2; ++i) {
                    if (auto ok = track_info(track_mark, i, bases[static_cast<size_t>(i)]); !ok) {
                        return ok;
                    }
                }
                if (auto ok = residual_tracks(bases); !ok) {
                    return ok;
                }
            }
            if (config == 0) {
                if (auto ok = mono_data(false); !ok) {
                    return ok;
                }
            }
            if (auto ok = aspx_2ch(); !ok) {
                return ok;
            }
            if (auto ok = aspx_1ch(); !ok) {
                return ok;
            }
            if (auto ok = acpl_1ch(); !ok) {
                return ok;
            }
            return acpl_1ch();
        }
        default: {  // ASPX_ACPL_3
            if (auto ok = companding_control(2); !ok) {
                return ok;
            }
            if (auto ok = stereo_data(); !ok) {
                return ok;
            }
            if (auto ok = aspx_2ch(); !ok) {
                return ok;
            }
            return acpl_2ch();
        }
    }
}

// 4.2.6.14 7_X_channel_element(channel_mode, b_iframe)
ParseResult ElementParser::element_7_x() {
    const int mode = static_cast<int>(r_.read(2, "7_X_codec_mode"));
    std::optional<AcplConfigKind> acpl;
    if (mode == codec_mode::kAspxAcpl1) {
        acpl = AcplConfigKind::kPartial;
    } else if (mode == codec_mode::kAspxAcpl2) {
        acpl = AcplConfigKind::kFull;
    }
    if (auto ok = begin(ElementKind::k7X, mode, mode != codec_mode::kSimple, acpl, false); !ok) {
        return ok;
    }
    if (ctx_.has_lfe()) {
        if (auto ok = mono_data(true); !ok) {
            return ok;
        }
    }
    const bool acpl_mode = mode == codec_mode::kAspxAcpl1 || mode == codec_mode::kAspxAcpl2;
    if (acpl_mode) {
        if (auto ok = companding_control(5); !ok) {
            return ok;
        }
    }
    const int config = static_cast<int>(r_.read(2, "coding_config"));
    out_.coding_config = config;
    const size_t track_mark = out_.tracks.size();
    ParseResult ok;
    switch (config) {
        case 0:
            out_.two_ch_mode = r_.read_flag("2ch_mode");
            ok = two_channel_data();
            if (ok) {
                ok = two_channel_data();
            }
            break;
        case 1:
            ok = three_channel_data();
            if (ok) {
                ok = two_channel_data();
            }
            break;
        case 2:
            ok = four_channel_data();
            break;
        default:
            ok = five_channel_data();
            break;
    }
    if (!ok) {
        return ok;
    }
    // Where A, B, D and E (L, R, Ls and Rs, Table 183) sit among the tracks
    // the switch read: Table 182's input mapping, which Table 184 repeats.
    int pos_a = 0;
    int pos_b = 1;
    int pos_d = 2;
    int pos_e = 3;
    if (config == 0 && out_.two_ch_mode.value_or(false)) {
        pos_b = 2;
        pos_d = 1;
    } else if (config == 1 || config == 3) {
        pos_d = 3;
        pos_e = 4;
    }
    const bool back_pair = ctx_.ch_mode == ch_mode::k7_0_340 || ctx_.ch_mode == ch_mode::k7_1_340;
    const auto bases_at = [&](int first, int second, std::array<int, 2>& bases) -> ParseResult {
        if (auto next = track_info(track_mark, first, bases[0]); !next) {
            return next;
        }
        return track_info(track_mark, second, bases[1]);
    };
    if (mode == codec_mode::kSimple || mode == codec_mode::kAspx) {
        const bool use_sap = r_.read_flag("b_use_sap_add_ch");
        out_.b_use_sap_add_ch = use_sap;
        if (use_sap) {
            // The two chparam_info() come before the additional channels'
            // own sf_info. The reading taken, recorded in the errata
            // register: each has the framing of the track Table 183 codes
            // its additional channel against - D and E for 3/4/0, A and B
            // for 5/2/0 and 3/2/2.
            std::array<int, 2> bases{};
            if (auto next = back_pair ? bases_at(pos_d, pos_e, bases) : bases_at(pos_a, pos_b, bases); !next) {
                return next;
            }
            for (const int base : bases) {
                if (auto next = chparam_infos(1, base); !next) {
                    return next;
                }
            }
        }
        if (auto next = two_channel_data(); !next) {
            return next;
        }
    }
    if (mode == codec_mode::kAspxAcpl1) {
        // Table 202: the A-CPL modules pair the residuals with Ls and Rs (D
        // and E) for 3/4/0 or when add_ch_base is set, and with L and R (A
        // and B) otherwise.
        std::array<int, 2> bases{};
        const bool surround = back_pair || ctx_.add_ch_base;
        if (auto next = surround ? bases_at(pos_d, pos_e, bases) : bases_at(pos_a, pos_b, bases); !next) {
            return next;
        }
        if (auto next = residual_tracks(bases); !next) {
            return next;
        }
    }
    if (config == 0 || config == 2) {
        if (auto next = mono_data(false); !next) {
            return next;
        }
    }
    if (mode != codec_mode::kSimple) {
        if (auto next = aspx_2ch(); !next) {
            return next;
        }
        if (auto next = aspx_2ch(); !next) {
            return next;
        }
        if (auto next = aspx_1ch(); !next) {
            return next;
        }
    }
    if (mode == codec_mode::kAspx) {
        if (auto next = aspx_2ch(); !next) {
            return next;
        }
    }
    if (acpl_mode) {
        if (auto next = acpl_1ch(); !next) {
            return next;
        }
        return acpl_1ch();
    }
    return {};
}

// Part 2 6.2.4.1 immersive_channel_element(b_lfe, b_5fronts, b_iframe) with
// b_5fronts 0, the 7.X.4 channel modes, and immers_cfg() (6.2.4.2).
// core_channel_config is 7CH_STATIC in every codec mode but ASPX_AJCC's
// 5CH_DYNAMIC (Table 74).
//
// The syntax names no framing for its chparam_info() elements, which need one
// (Part 1 Table 47). The reading taken, recorded in the errata register: each
// has the framing of the track the step it parameterises codes the other
// against, as the 7_X element's do. The two b_use_sap_add_ch sends are Part 2
// 5.2.3.2 step 4's, which codes F and G against D and E; the four after the
// tracks H to K are Table 20's a'_0 to a'_3, which predict H, I, J and K from
// D, E, F and G.
ParseResult ElementParser::immersive_element(bool b_lfe) {
    // immersive_codec_mode_code (6.3.5.1, Table 73): a 1 is ASPX_AJCC, and
    // after a 0 two more bits give SCPL to ASPX_ACPL_2. One record, of one or
    // three bits, valued at the bits read.
    const std::size_t start = r_.position();
    int mode = immersive_mode::kAspxAjcc;
    if (r_.peek_raw(1) != 0) {
        r_.consume(1);
        r_.emit(start, 1, 1, "immersive_codec_mode_code");
    } else {
        mode = static_cast<int>(r_.peek_raw(3));
        r_.consume(3);
        r_.emit(start, 3, static_cast<std::uint64_t>(mode), "immersive_codec_mode_code");
    }
    if (auto ok = check(r_); !ok) {
        return ok;
    }
    std::optional<AcplConfigKind> acpl;
    if (mode == immersive_mode::kAspxAcpl1) {
        acpl = AcplConfigKind::kPartial;
    } else if (mode == immersive_mode::kAspxAcpl2) {
        acpl = AcplConfigKind::kFull;
    }
    if (auto ok = begin(ElementKind::kImmersive, mode, mode != immersive_mode::kScpl, acpl, false); !ok) {
        return ok;
    }
    if (b_lfe) {
        if (auto ok = mono_data(true); !ok) {
            return ok;
        }
    }
    if (mode == immersive_mode::kAspxAjcc) {
        if (auto ok = companding_control(5); !ok) {
            return ok;
        }
    }
    const int grouping = static_cast<int>(r_.read(2, "core_5ch_grouping"));
    out_.core_5ch_grouping = grouping;
    const size_t track_mark = out_.tracks.size();
    ParseResult ok;
    switch (grouping) {
        case 0:
            out_.two_ch_mode = r_.read_flag("2ch_mode");
            ok = two_channel_data();
            if (ok) {
                ok = two_channel_data();
            }
            if (ok) {
                ok = mono_data(false);
            }
            break;
        case 1:
            ok = three_channel_data();
            if (ok) {
                ok = two_channel_data();
            }
            break;
        case 2:
            ok = four_channel_data();
            if (ok) {
                ok = mono_data(false);
            }
            break;
        default:
            ok = five_channel_data();
            break;
    }
    if (!ok) {
        return ok;
    }
    // Part 2 Table 19: the tracks D and E among the core's, counted from the
    // first after the LFE's; F and G follow them in the 7CH_STATIC
    // two_channel_data(), the fifth and sixth.
    int pos_d = 2;
    int pos_e = 3;
    if (grouping == 0 && out_.two_ch_mode.value_or(false)) {
        pos_d = 1;
    } else if (grouping == 1 || grouping == 3) {
        pos_d = 3;
        pos_e = 4;
    }
    constexpr int kPosF = 5;
    constexpr int kPosG = 6;
    const auto chparams_after = [&](std::initializer_list<int> positions) -> ParseResult {
        for (const int position : positions) {
            int info = 0;
            if (auto next = track_info(track_mark, position, info); !next) {
                return next;
            }
            if (auto next = chparam_infos(1, info); !next) {
                return next;
            }
        }
        return {};
    };
    const bool seven_static = mode != immersive_mode::kAspxAjcc;
    if (seven_static) {
        const bool use_sap = r_.read_flag("b_use_sap_add_ch");
        out_.b_use_sap_add_ch = use_sap;
        if (use_sap) {
            if (auto next = chparams_after({pos_d, pos_e}); !next) {
                return next;
            }
        }
        if (auto next = two_channel_data(); !next) {
            return next;
        }
    }
    if (mode == immersive_mode::kAspxScpl) {
        // Table 8: (Ls, Lb), (Rs, Rb), C, (L, R), (Tfl, Tbl) and (Tfr, Tbr).
        for (const bool pair : {true, true, false, true, true, true}) {
            if (auto next = pair ? aspx_2ch() : aspx_1ch(); !next) {
                return next;
            }
        }
    } else if (mode != immersive_mode::kScpl) {
        const int pairs = seven_static ? 3 : 2;
        for (int k = 0; k < pairs; ++k) {
            if (auto next = aspx_2ch(); !next) {
                return next;
            }
        }
        if (auto next = aspx_1ch(); !next) {
            return next;
        }
    }
    if (mode == immersive_mode::kAspxAjcc) {
        AjccData data;
        if (auto next = parse_ajcc_data(r_, data); !next) {
            return next;
        }
        out_.ajcc = data;
    }
    if (mode == immersive_mode::kScpl || mode == immersive_mode::kAspxScpl || mode == immersive_mode::kAspxAcpl1) {
        for (int k = 0; k < 2; ++k) {
            if (auto next = two_channel_data(); !next) {
                return next;
            }
        }
        if (auto next = chparams_after({pos_d, pos_e, kPosF, kPosG}); !next) {
            return next;
        }
    }
    if (mode == immersive_mode::kAspxAcpl1 || mode == immersive_mode::kAspxAcpl2) {
        for (int k = 0; k < 4; ++k) {
            if (auto next = acpl_1ch(); !next) {
                return next;
            }
        }
    }
    return check(r_);
}

// Part 2 6.2.4.4 var_channel_element(b_iframe, n_dmx_signals, b_has_lfe). Its
// A-SPX data are an aspx_data_2ch() for each pair of the fullband tracks in
// syntax order and an aspx_data_1ch() for an odd last one, and its
// companding_control() lists the fullband tracks in syntax order
// (src/ac4dec/ERRATA.md, "var_channel_element()'s A-SPX and companding"):
// what that means is the reconstruction's, the syntax reads the same either
// way.
ParseResult ElementParser::var_element(int n_dmx_signals, bool b_has_lfe) {
    const int mode = static_cast<int>(r_.read(1, "var_codec_mode"));
    out_.var_signals = n_dmx_signals;
    out_.var_lfe = b_has_lfe;
    if (auto ok = begin(ElementKind::kVar, mode, mode == codec_mode::kAspx, std::nullopt, false);
        !ok) {
        return ok;
    }
    const bool odd = n_dmx_signals % 2 != 0;
    const int n_pairs = n_dmx_signals / 2;
    if (mode == codec_mode::kAspx && n_dmx_signals <= 5) {
        if (auto ok = companding_control(n_dmx_signals); !ok) {
            return ok;
        }
    }
    if (b_has_lfe) {
        if (auto ok = mono_data(true); !ok) {
            return ok;
        }
    }
    if (odd) {
        if (n_dmx_signals == 1) {
            if (auto ok = mono_data(false); !ok) {
                return ok;
            }
        } else {
            for (int p = 0; p < n_pairs - 1; ++p) {
                if (auto ok = two_channel_data(); !ok) {
                    return ok;
                }
            }
            const int config = static_cast<int>(r_.read(1, "var_coding_config"));
            out_.coding_config = config;
            if (config == 0) {
                if (auto ok = two_channel_data(); !ok) {
                    return ok;
                }
                if (auto ok = mono_data(false); !ok) {
                    return ok;
                }
            } else if (auto ok = three_channel_data(); !ok) {
                return ok;
            }
        }
    } else {
        for (int p = 0; p < n_pairs; ++p) {
            if (auto ok = two_channel_data(); !ok) {
                return ok;
            }
        }
    }
    if (mode == codec_mode::kAspx) {
        for (int p = 0; p < n_pairs; ++p) {
            if (auto ok = aspx_2ch(); !ok) {
                return ok;
            }
        }
        if (odd) {
            if (auto ok = aspx_1ch(); !ok) {
                return ok;
            }
        }
    }
    return check(r_);
}

}  // namespace

std::optional<int> objs_to_channel_mode(int n_objects) noexcept {
    switch (n_objects) {
        case 1:
            return ch_mode::kMono;
        case 2:
            return ch_mode::kStereo;
        case 3:
            return ch_mode::k3_0;
        case 5:
            return ch_mode::k5_0;
        default:
            return std::nullopt;
    }
}

ParseResult parse_audio_data_objs(BitReader& r, const SubstreamContext& ctx, int n_objects,
                                  bool b_lfe, ChannelElementState& state, ChannelElement& out) {
    out = ChannelElement{};
    ElementParser parser(r, ctx, state, out, nullptr);
    if (b_lfe) {
        if (auto ok = parser.objects_lfe(); !ok) {
            return ok;
        }
    }
    if (n_objects == 0) {
        return check(r);
    }
    const std::optional<int> mode = objs_to_channel_mode(n_objects);
    if (!mode) {
        return fail(DecodeError::kInvalidStream, "an object count no channel element carries");
    }
    out.objs_channel_mode = mode;
    switch (*mode) {
        case ch_mode::kMono:
            return parser.single_channel_element();
        case ch_mode::kStereo:
            return parser.channel_pair_element();
        case ch_mode::k3_0:
            return parser.element_3_0();
        default:
            return parser.element_5_x(false);
    }
}

ParseResult parse_var_channel_element(BitReader& r, const SubstreamContext& ctx, int n_dmx_signals,
                                      bool b_has_lfe, ChannelElementState& state,
                                      ChannelElement& out) {
    out = ChannelElement{};
    if (n_dmx_signals < 1) {
        return fail(DecodeError::kInvalidStream, "a var_channel_element() of no signals");
    }
    ElementParser parser(r, ctx, state, out, nullptr);
    return parser.var_element(n_dmx_signals, b_has_lfe);
}

ParseResult parse_audio_data_chan(BitReader& r, const SubstreamContext& ctx, ChannelElementState& state,
                                  ChannelElement& out, BitReader* hsf_reader) {
    out = ChannelElement{};
    ElementParser parser(r, ctx, state, out, hsf_reader);
    switch (ctx.ch_mode) {
        case ch_mode::kMono:
            return parser.single_channel_element();
        case ch_mode::kStereo:
            return parser.channel_pair_element();
        case ch_mode::k3_0:
            return parser.element_3_0();
        case ch_mode::k5_0:
            return parser.element_5_x(false);
        case ch_mode::k5_1:
            return parser.element_5_x(true);
        case ch_mode::k7_0_340:
        case ch_mode::k7_1_340:
        case ch_mode::k7_0_520:
        case ch_mode::k7_1_520:
        case ch_mode::k7_0_322:
        case ch_mode::k7_1_322:
            return parser.element_7_x();
        case ch_mode::k7_0_4:
            return parser.immersive_element(false);
        case ch_mode::k7_1_4:
            return parser.immersive_element(true);
        case ch_mode::k9_0_4:
        case ch_mode::k9_1_4:
            return fail(DecodeError::kUnsupported,
                        "9.X.4, the immersive_channel_element() with b_5fronts, is not decoded");
        case ch_mode::k22_2:
            return fail(DecodeError::kUnsupported, "22_2_channel_element() is not decoded");
        default:
            return fail(DecodeError::kInvalidStream, "a reserved channel_mode");
    }
}

}  // namespace ac4::detail
