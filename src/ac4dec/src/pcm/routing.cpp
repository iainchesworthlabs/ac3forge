#include "pcm/routing.hpp"

#include <algorithm>
#include <cstddef>

namespace ac4::detail {
namespace {

using S = Speaker;

constexpr std::array kMono = {S::kCentre};
constexpr std::array kStereo = {S::kLeft, S::kRight};
constexpr std::array k30 = {S::kLeft, S::kRight, S::kCentre};
constexpr std::array k50 = {S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround};
constexpr std::array k51 = {S::kLeft, S::kRight, S::kCentre, S::kLfe, S::kLeftSurround, S::kRightSurround};
constexpr std::array k70_340 = {S::kLeft,          S::kRight,         S::kCentre,   S::kLeftSurround,
                                S::kRightSurround, S::kLeftBack,      S::kRightBack};
constexpr std::array k71_340 = {S::kLeft,          S::kRight,        S::kCentre,   S::kLfe,
                                S::kLeftSurround,  S::kRightSurround, S::kLeftBack, S::kRightBack};
constexpr std::array k70_520 = {S::kLeft,          S::kRight,    S::kCentre,   S::kLeftSurround,
                                S::kRightSurround, S::kLeftWide, S::kRightWide};
constexpr std::array k71_520 = {S::kLeft,         S::kRight,         S::kCentre,   S::kLfe,
                                S::kLeftSurround, S::kRightSurround, S::kLeftWide, S::kRightWide};
constexpr std::array k70_322 = {S::kLeft,          S::kRight,        S::kCentre,   S::kLeftSurround,
                                S::kRightSurround, S::kTopFrontLeft, S::kTopFrontRight};
constexpr std::array k71_322 = {S::kLeft,         S::kRight,         S::kCentre,       S::kLfe,
                                S::kLeftSurround, S::kRightSurround, S::kTopFrontLeft, S::kTopFrontRight};
// The 7.X.4 modes, in full decoding and in core decoding (5.X.2).
constexpr std::array k704 = {S::kLeft,          S::kRight,       S::kCentre,      S::kLeftSurround,
                             S::kRightSurround, S::kLeftBack,    S::kRightBack,   S::kTopFrontLeft,
                             S::kTopFrontRight, S::kTopBackLeft, S::kTopBackRight};
constexpr std::array k714 = {S::kLeft,          S::kRight,        S::kCentre,
                             S::kLfe,           S::kLeftSurround, S::kRightSurround,
                             S::kLeftBack,      S::kRightBack,    S::kTopFrontLeft,
                             S::kTopFrontRight, S::kTopBackLeft,  S::kTopBackRight};
constexpr std::array k704_core = {S::kLeft,         S::kRight,         S::kCentre,
                                  S::kLeftSurround, S::kRightSurround, S::kTopSideLeft,
                                  S::kTopSideRight};
constexpr std::array k714_core = {S::kLeft,        S::kRight,        S::kCentre,
                                  S::kLfe,         S::kLeftSurround, S::kRightSurround,
                                  S::kTopSideLeft, S::kTopSideRight};

// The labels a var_channel_element()'s fullband outputs are held under, in
// order; they name no loudspeaker (object_layout's comment).
constexpr std::array<Speaker, 16> kVarSlots = {
    S::kLeft,          S::kRight,        S::kCentre,        S::kLeftSurround,
    S::kRightSurround, S::kLeftBack,     S::kRightBack,     S::kLeftWide,
    S::kRightWide,     S::kTopFrontLeft, S::kTopFrontRight, S::kTopBackLeft,
    S::kTopBackRight,  S::kTopSideLeft,  S::kTopSideRight,  S::kLfe2};

// Each var_channel_element() layout's channels: the LFE first where it has
// one, then the slots.
struct VarLayouts {
    std::array<std::array<Speaker, 17>, 32> speakers{};
    std::array<std::size_t, 32> count{};
};
constexpr VarLayouts kVarLayouts = [] {
    VarLayouts v;
    for (std::size_t lfe = 0; lfe < 2; ++lfe) {
        for (std::size_t n = 1; n <= 16; ++n) {
            const std::size_t index = (n - 1) + lfe * 16;
            std::size_t k = 0;
            if (lfe != 0) {
                v.speakers[index][k++] = S::kLfe;
            }
            for (std::size_t s = 0; s < n; ++s) {
                v.speakers[index][k++] = kVarSlots[s];
            }
            v.count[index] = k;
        }
    }
    return v;
}();

// A direct-coded substream's LFE with an element of 0 to 3 objects.
constexpr std::array kLfeOnly = {S::kLfe};
constexpr std::array kLfeMono = {S::kLfe, S::kCentre};
constexpr std::array kLfeStereo = {S::kLfe, S::kLeft, S::kRight};
constexpr std::array kLfe30 = {S::kLfe, S::kLeft, S::kRight, S::kCentre};

[[nodiscard]] bool is_objects_with_lfe(int layout) noexcept {
    return layout >= object_layout::kObjectsWithLfeBase &&
           layout <= object_layout::kObjectsWithLfeBase + 3;
}

// Whether a layout's channels include the LFE.
[[nodiscard]] bool layout_has_lfe(int layout, DecodingMode decoding) noexcept {
    return std::ranges::find(speakers_of(layout, decoding), S::kLfe) !=
           speakers_of(layout, decoding).end();
}

// A 7.X mode's last pair, which Table 182 calls F and G.
[[nodiscard]] std::array<Speaker, 2> last_pair(int ch_mode) noexcept {
    switch (ch_mode) {
        case ch_mode::k7_0_340:
        case ch_mode::k7_1_340:
            return {S::kLeftBack, S::kRightBack};
        case ch_mode::k7_0_520:
        case ch_mode::k7_1_520:
            return {S::kLeftWide, S::kRightWide};
        default:
            return {S::kTopFrontLeft, S::kTopFrontRight};
    }
}

[[nodiscard]] bool is_7x(int ch_mode) noexcept {
    return ch_mode >= ch_mode::k7_0_340 && ch_mode <= ch_mode::k7_1_322;
}

// Walks an element's channel data in the order parse_audio_data_chan() read
// it, taking each part's tracks, stereo flags, chel_matsel values and
// chparam_info()s in turn.
class Walker {
   public:
    Walker(const ChannelElement& element, ElementRoute& out) : element_(element), out_(out) {}

    void lfe() { add(1, {S::kLfe}, false, 0, 0); }
    void mono(Speaker speaker) { add(1, {speaker}, false, 0, 0); }

    // `discarded`: tracks read and not decoded (DataElementRoute::discarded).
    void pair(Speaker o0, Speaker o1, bool discarded = false) {
        bool processed = false;
        if (pair_ < element_.b_enable_mdct_stereo_proc.size()) {
            processed = element_.b_enable_mdct_stereo_proc[pair_];
        } else {
            short_ = true;
        }
        ++pair_;
        add(2, {o0, o1}, processed, processed ? 1 : 0, 0);
        out_.data.back().discarded = discarded;
    }

    void three(Speaker o0, Speaker o1, Speaker o2) { add(3, {o0, o1, o2}, true, 2, next_matsel()); }

    void four(Speaker o0, Speaker o1, Speaker o2, Speaker o3) { add(4, {o0, o1, o2, o3}, true, 4, 0); }

    void five(Speaker o0, Speaker o1, Speaker o2, Speaker o3, Speaker o4) {
        add(5, {o0, o1, o2, o3, o4}, true, 5, next_matsel());
    }

    // The two chparam_info() b_use_sap_add_ch sends, for Table 183's steps.
    int take_chparams(int count) {
        const int first = chparam_;
        chparam_ += count;
        return first;
    }

    // ASPX_ACPL_1's residuals: two chparam_info(), then two sf_data(), the
    // tracks of `outputs`, each coded against its `bases` channel under its
    // own sf_info().
    void residuals(std::array<Speaker, 2> outputs, std::array<Speaker, 2> bases) {
        const int first = take_chparams(2);
        for (std::size_t i = 0; i < 2; ++i) {
            mono(outputs[i]);
            out_.steps.push_back({.first = bases[i],
                                  .second = outputs[i],
                                  .chparam = first + static_cast<int>(i),
                                  .framing = outputs[i]});
        }
    }

    // Whether everything the walk named is what the element holds.
    [[nodiscard]] bool complete() const noexcept {
        return !short_ && static_cast<std::size_t>(track_) == element_.tracks.size() &&
               static_cast<std::size_t>(chparam_) == element_.chparams.size() &&
               pair_ == element_.b_enable_mdct_stereo_proc.size() && matsel_ == element_.chel_matsel.size();
    }

   private:
    int next_matsel() {
        int value = 0;
        if (matsel_ < element_.chel_matsel.size()) {
            value = element_.chel_matsel[matsel_];
        } else {
            short_ = true;
        }
        ++matsel_;
        return value;
    }

    void add(int count, std::initializer_list<Speaker> outputs, bool processed, int chparams, int matsel) {
        DataElementRoute route;
        route.count = count;
        route.first_track = track_;
        std::size_t k = 0;
        for (const Speaker speaker : outputs) {
            route.outputs[k++] = speaker;
        }
        route.processed = processed;
        route.first_chparam = chparam_;
        route.chel_matsel = matsel;
        out_.data.push_back(route);
        track_ += count;
        chparam_ += chparams;
    }

    const ChannelElement& element_;
    ElementRoute& out_;
    int track_ = 0;
    int chparam_ = 0;
    std::size_t pair_ = 0;
    std::size_t matsel_ = 0;
    bool short_ = false;
};

// Part 2 clauses 5.2.3.2 to 5.2.3.4: the immersive element's tracks to the
// intermediate signals by Table 19, held in the channels routing.hpp's header
// comment gives them, with step 4's and Table 20's steps.
ParseResult route_immersive(const SubstreamContext& ctx, const ChannelElement& element,
                            DecodingMode decoding, ElementRoute& out) {
    Walker walk(element, out);
    const bool core = decoding == DecodingMode::kCore;
    const int mode = element.codec_mode;
    const bool lfe = !element.tracks.empty() && element.tracks.front().lfe;
    const bool two_ch_mode = element.two_ch_mode.value_or(false);
    const int grouping = element.core_5ch_grouping.value_or(-1);
    // A'' to G''. H'' to K'' are Lb, Rb, Tbl and Tbr.
    const Speaker a = S::kLeft;
    const Speaker b = S::kRight;
    const Speaker c = S::kCentre;
    const Speaker d = S::kLeftSurround;
    const Speaker e = S::kRightSurround;
    const Speaker f = core ? S::kTopSideLeft : S::kTopFrontLeft;
    const Speaker g = core ? S::kTopSideRight : S::kTopFrontRight;
    if (lfe) {
        walk.lfe();
    }
    // Table 19, element by element in the order the syntax reads them.
    switch (grouping) {
        case 0:
            walk.pair(a, two_ch_mode ? d : b);
            walk.pair(two_ch_mode ? b : d, e);
            walk.mono(c);
            break;
        case 1:
            walk.three(a, b, c);
            walk.pair(d, e);
            break;
        case 2:
            walk.four(a, b, d, e);
            walk.mono(c);
            break;
        default:
            walk.five(a, b, c, d, e);
            break;
    }
    // Table 74: every mode but ASPX_AJCC codes the seven channels of
    // 7CH_STATIC, F and G in a two_channel_data() after step 4's parameters.
    const bool seven = mode != immersive_mode::kAspxAjcc;
    const bool sap_add = seven && element.b_use_sap_add_ch.value_or(false);
    const int sap_first = sap_add ? walk.take_chparams(2) : 0;
    if (seven) {
        walk.pair(f, g);
    }
    // H to K, and Table 20's four chparam_info() after them.
    const bool coupled = mode == immersive_mode::kScpl || mode == immersive_mode::kAspxScpl ||
                         mode == immersive_mode::kAspxAcpl1;
    int prediction_first = 0;
    if (coupled) {
        walk.pair(S::kLeftBack, S::kRightBack, core);
        walk.pair(S::kTopBackLeft, S::kTopBackRight, core);
        prediction_first = walk.take_chparams(4);
    }
    // Step 4, in every 7CH_STATIC mode, ASPX_ACPL_2's included (ERRATA.md,
    // "ASPX_ACPL_2 and step 4").
    if (sap_add) {
        out.steps.push_back(
            {.first = d, .second = f, .chparam = sap_first, .framing = d, .prediction = false});
        out.steps.push_back(
            {.first = e, .second = g, .chparam = sap_first + 1, .framing = e, .prediction = false});
    }
    // Table 20: H'' = H' + a'0 D', I'' = I' + a'1 E', J'' = J' + a'2 F' and
    // K'' = K' + a'3 G', which core decoding has no use for.
    if (coupled && !core) {
        const std::array<std::array<Speaker, 2>, 4> predicted = {
            {{d, S::kLeftBack}, {e, S::kRightBack}, {f, S::kTopBackLeft}, {g, S::kTopBackRight}}};
        for (std::size_t j = 0; j < predicted.size(); ++j) {
            out.steps.push_back({.first = predicted[j][0],
                                 .second = predicted[j][1],
                                 .chparam = prediction_first + static_cast<int>(j),
                                 .framing = predicted[j][0],
                                 .prediction = true});
        }
    }
    // 5.2.3.3 and 5.2.3.4: what the mode leaves silent until A-CPL or A-JCC
    // makes it in the QMF domain.
    if (mode == immersive_mode::kAspxAjcc) {
        out.silent =
            core ? std::vector<Speaker>{S::kTopSideLeft, S::kTopSideRight}
                 : std::vector<Speaker>{S::kLeftBack,      S::kRightBack,   S::kTopFrontLeft,
                                        S::kTopFrontRight, S::kTopBackLeft, S::kTopBackRight};
    } else if (mode == immersive_mode::kAspxAcpl2 && !core) {
        out.silent = {S::kLeftBack, S::kRightBack, S::kTopBackLeft, S::kTopBackRight};
    }
    if (grouping < 0 || (grouping == 0 && !element.two_ch_mode.has_value()) ||
        lfe != ctx.has_lfe() || seven != element.b_use_sap_add_ch.has_value() || !walk.complete()) {
        return fail(DecodeError::kInvalidStream,
                    "an immersive element whose parts do not match its core_5ch_grouping");
    }
    return {};
}

// Part 2 clause 6.2.4.4: a var_channel_element()'s data elements in syntax
// order, the LFE's first; each fullband output goes to the next slot.
ParseResult route_var(const ChannelElement& element, ElementRoute& out) {
    Walker walk(element, out);
    const int n = element.var_signals;
    if (n < 1 || n > 16) {
        return fail(DecodeError::kInvalidStream,
                    "a var_channel_element() of no signals or more than 16");
    }
    if (element.var_lfe) {
        walk.lfe();
    }
    const auto slot = [](int k) { return kVarSlots[static_cast<std::size_t>(k)]; };
    const int pairs = n / 2;
    if (n % 2 != 0) {
        if (n == 1) {
            walk.mono(slot(0));
        } else {
            for (int p = 0; p < pairs - 1; ++p) {
                walk.pair(slot(2 * p), slot(2 * p + 1));
            }
            const int k = 2 * (pairs - 1);
            if (element.coding_config.value_or(-1) == 0) {
                walk.pair(slot(k), slot(k + 1));
                walk.mono(slot(k + 2));
            } else if (element.coding_config.value_or(-1) == 1) {
                walk.three(slot(k), slot(k + 1), slot(k + 2));
            } else {
                return fail(DecodeError::kInvalidStream,
                            "a var_channel_element() without its var_coding_config");
            }
        }
    } else {
        for (int p = 0; p < pairs; ++p) {
            walk.pair(slot(2 * p), slot(2 * p + 1));
        }
    }
    if (!walk.complete()) {
        return fail(DecodeError::kInvalidStream,
                    "a var_channel_element() whose parts do not match its signals");
    }
    return {};
}

}  // namespace

int var_signals(int layout) noexcept {
    if (layout < object_layout::kVarBase || layout >= object_layout::kVarBase + 32) {
        return 0;
    }
    return (layout - object_layout::kVarBase) % 16 + 1;
}

std::optional<int> pcm_layout(const SubstreamContext& ctx) noexcept {
    switch (ctx.coding) {
        case AudioCoding::kChannel:
            return ctx.ch_mode;
        case AudioCoding::kAjoc:
            if (ctx.b_static_dmx) {
                return ctx.b_lfe ? ch_mode::k5_1 : ch_mode::k5_0;
            }
            if (ctx.n_fullband_dmx < 1 || ctx.n_fullband_dmx > 16) {
                return std::nullopt;
            }
            return object_layout::var(ctx.n_fullband_dmx, ctx.b_lfe);
        case AudioCoding::kObjects:
            switch (ctx.n_objects) {
                case 0:
                    return ctx.b_lfe ? std::optional<int>{object_layout::objects_with_lfe(0)}
                                     : std::nullopt;
                case 1:
                    return ctx.b_lfe ? object_layout::objects_with_lfe(1) : ch_mode::kMono;
                case 2:
                    return ctx.b_lfe ? object_layout::objects_with_lfe(2) : ch_mode::kStereo;
                case 3:
                    return ctx.b_lfe ? object_layout::objects_with_lfe(3) : ch_mode::k3_0;
                case 5:
                    return ctx.b_lfe ? ch_mode::k5_1 : ch_mode::k5_0;
                default:
                    return std::nullopt;
            }
    }
    return std::nullopt;
}

int ajoc_input_channel(int i, int n_fb, bool lfe) noexcept {
    const int m = n_fb + (lfe ? 1 : 0);
    const int skip = lfe ? 1 : 0;
    if (n_fb > 3) {
        const int n_offset = 2 + n_fb % 2;
        if (i < n_offset) {
            return m - n_offset + i;
        }
        return i - n_offset + skip;
    }
    return i + skip;
}

bool is_immersive(int ch_mode) noexcept {
    return ch_mode == ch_mode::k7_0_4 || ch_mode == ch_mode::k7_1_4;
}

std::span<const Speaker> speakers_of(int ch_mode, DecodingMode decoding) noexcept {
    const bool core = decoding == DecodingMode::kCore;
    switch (ch_mode) {
        case ch_mode::kMono:
            return kMono;
        case ch_mode::kStereo:
            return kStereo;
        case ch_mode::k3_0:
            return k30;
        case ch_mode::k5_0:
            return k50;
        case ch_mode::k5_1:
            return k51;
        case ch_mode::k7_0_340:
            return k70_340;
        case ch_mode::k7_1_340:
            return k71_340;
        case ch_mode::k7_0_520:
            return k70_520;
        case ch_mode::k7_1_520:
            return k71_520;
        case ch_mode::k7_0_322:
            return k70_322;
        case ch_mode::k7_1_322:
            return k71_322;
        case ch_mode::k7_0_4:
            return core ? std::span<const Speaker>(k704_core) : std::span<const Speaker>(k704);
        case ch_mode::k7_1_4:
            return core ? std::span<const Speaker>(k714_core) : std::span<const Speaker>(k714);
        case object_layout::objects_with_lfe(0):
            return kLfeOnly;
        case object_layout::objects_with_lfe(1):
            return kLfeMono;
        case object_layout::objects_with_lfe(2):
            return kLfeStereo;
        case object_layout::objects_with_lfe(3):
            return kLfe30;
        default:
            break;
    }
    if (var_signals(ch_mode) > 0) {
        const auto index = static_cast<std::size_t>(ch_mode - object_layout::kVarBase);
        return std::span<const Speaker>(kVarLayouts.speakers[index])
            .first(kVarLayouts.count[index]);
    }
    return {};
}

ParseResult route_element(const SubstreamContext& ctx, const ChannelElement& element,
                          ElementRoute& out, DecodingMode decoding) {
    out = ElementRoute{};
    if (element.kind == ElementKind::kImmersive) {
        return route_immersive(ctx, element, decoding, out);
    }
    if (element.kind == ElementKind::kVar) {
        return route_var(element, out);
    }
    Walker walk(element, out);
    const bool lfe = !element.tracks.empty() && element.tracks.front().lfe;
    // A direct-coded substream's LFE comes before its element of one to
    // three objects (audio_data_objs(), Part 2 clause 6.2.3.2).
    if (lfe && is_objects_with_lfe(ctx.ch_mode)) {
        walk.lfe();
    }
    const auto config = element.coding_config.value_or(-1);
    const bool two_ch_mode = element.two_ch_mode.value_or(false);
    const int mode = element.codec_mode;
    const bool acpl =
        mode == codec_mode::kAspxAcpl1 || mode == codec_mode::kAspxAcpl2 || mode == codec_mode::kAspxAcpl3;
    switch (element.kind) {
        case ElementKind::kSingle:
            walk.mono(S::kCentre);
            break;
        case ElementKind::kPair:
            if (mode == codec_mode::kAspxAcpl2) {
                walk.mono(S::kLeft);
                out.silent = {S::kRight};
            } else {
                walk.pair(S::kLeft, S::kRight);
            }
            break;
        case ElementKind::k3_0:
            // Clause 5.3.4.2.
            if (config == 0) {
                walk.pair(S::kLeft, S::kRight);
                walk.mono(S::kCentre);
            } else {
                walk.three(S::kLeft, S::kRight, S::kCentre);
            }
            break;
        case ElementKind::k5X:
            if (lfe) {
                walk.lfe();
            }
            if (mode == codec_mode::kAspxAcpl3) {
                // 5.3.4.3.3: stereo_data() makes L and R.
                walk.pair(S::kLeft, S::kRight);
                out.silent = {S::kCentre, S::kLeftSurround, S::kRightSurround};
                break;
            }
            if (acpl) {
                // Table 181: A and B are L and R, which 5.3.4.3.2's matrix
                // pairs with the residuals.
                if (config == 0) {
                    walk.pair(S::kLeft, S::kRight);
                } else {
                    walk.three(S::kLeft, S::kRight, S::kCentre);
                }
                if (mode == codec_mode::kAspxAcpl1) {
                    walk.residuals({S::kLeftSurround, S::kRightSurround}, {S::kLeft, S::kRight});
                } else {
                    out.silent = {S::kLeftSurround, S::kRightSurround};
                }
                if (config == 0) {
                    walk.mono(S::kCentre);
                }
                break;
            }
            // Table 180.
            switch (config) {
                case 0:
                    walk.pair(S::kLeft, two_ch_mode ? S::kLeftSurround : S::kRight);
                    walk.pair(two_ch_mode ? S::kRight : S::kLeftSurround, S::kRightSurround);
                    walk.mono(S::kCentre);
                    break;
                case 1:
                    walk.three(S::kLeft, S::kRight, S::kCentre);
                    walk.pair(S::kLeftSurround, S::kRightSurround);
                    break;
                case 2:
                    walk.four(S::kLeft, S::kRight, S::kLeftSurround, S::kRightSurround);
                    walk.mono(S::kCentre);
                    break;
                default:
                    walk.five(S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround);
                    break;
            }
            break;
        case ElementKind::k7X: {
            // Table 182, its outputs A to G named by the channels they are at
            // identity (routing.hpp's header comment).
            if (lfe) {
                walk.lfe();
            }
            const auto [f, g] = last_pair(ctx.ch_mode);
            switch (config) {
                case 0:
                    walk.pair(S::kLeft, two_ch_mode ? S::kLeftSurround : S::kRight);
                    walk.pair(two_ch_mode ? S::kRight : S::kLeftSurround, S::kRightSurround);
                    break;
                case 1:
                    walk.three(S::kLeft, S::kRight, S::kCentre);
                    walk.pair(S::kLeftSurround, S::kRightSurround);
                    break;
                case 2:
                    walk.four(S::kLeft, S::kRight, S::kLeftSurround, S::kRightSurround);
                    break;
                default:
                    walk.five(S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround);
                    break;
            }
            const bool back = ctx.ch_mode == ch_mode::k7_0_340 || ctx.ch_mode == ch_mode::k7_1_340;
            if (acpl) {
                // Tables 184 and 185, and 5.3.4.4.2: ASPX_ACPL_1 codes F and
                // G as residuals against the pair Table 202 couples them with.
                if (mode == codec_mode::kAspxAcpl1) {
                    const bool surround_base = back || ctx.add_ch_base;
                    walk.residuals({f, g}, surround_base ? std::array{S::kLeftSurround, S::kRightSurround}
                                                         : std::array{S::kLeft, S::kRight});
                } else {
                    out.silent = {f, g};
                }
            } else if (element.b_use_sap_add_ch.value_or(false)) {
                const int first = walk.take_chparams(2);
                const Speaker left = back ? S::kLeftSurround : S::kLeft;
                const Speaker right = back ? S::kRightSurround : S::kRight;
                out.steps.push_back({.first = left, .second = f, .chparam = first, .framing = left});
                out.steps.push_back({.first = right, .second = g, .chparam = first + 1, .framing = right});
            }
            if (!acpl) {
                walk.pair(f, g);
            }
            if (config == 0 || config == 2) {
                walk.mono(S::kCentre);
            }
            break;
        }
        case ElementKind::kImmersive:
        case ElementKind::kVar:
            break;  // route_immersive() and route_var()
    }
    const bool five_x_acpl = element.kind == ElementKind::k5X && acpl;
    // ASPX_ACPL_3 sends no coding_config: its channel data is stereo_data().
    const bool has_config = element.kind == ElementKind::kSingle || element.kind == ElementKind::kPair ||
                            element.coding_config.has_value() ||
                            (five_x_acpl && mode == codec_mode::kAspxAcpl3);
    const bool needs_two_ch_mode =
        (element.kind == ElementKind::k5X || element.kind == ElementKind::k7X) && config == 0 && !five_x_acpl;
    const bool lfe_expected =
        (element.kind == ElementKind::k5X || element.kind == ElementKind::k7X ||
         is_objects_with_lfe(ctx.ch_mode)) &&
        layout_has_lfe(ctx.ch_mode, decoding);
    const bool needs_sap_add_ch = is_7x(ctx.ch_mode) && !acpl;
    if (!has_config || (needs_two_ch_mode && !element.two_ch_mode.has_value()) || lfe != lfe_expected ||
        (needs_sap_add_ch && !element.b_use_sap_add_ch.has_value()) || !walk.complete()) {
        return fail(DecodeError::kInvalidStream, "a channel element whose parts do not match its coding_config");
    }
    return {};
}

std::vector<AspxUnit> aspx_units(int ch_mode, int codec_mode, DecodingMode decoding) {
    if (const int n = var_signals(ch_mode); n > 0) {
        // Part 2 clause 6.2.4.4: an aspx_data_2ch() per pair of fullband
        // signals in syntax order, then an aspx_data_1ch() for an odd last one
        // (src/ac4dec/ERRATA.md, "var_channel_element()'s A-SPX and
        // companding").
        if (codec_mode != codec_mode::kAspx) {
            return {};
        }
        std::vector<AspxUnit> units;
        for (int p = 0; p < n / 2; ++p) {
            units.push_back({.pair = true,
                             .index = p,
                             .speakers = {kVarSlots[static_cast<std::size_t>(2 * p)],
                                          kVarSlots[static_cast<std::size_t>(2 * p + 1)]},
                             .first_only = false});
        }
        if (n % 2 != 0) {
            const Speaker last = kVarSlots[static_cast<std::size_t>(n - 1)];
            units.push_back(
                {.pair = false, .index = 0, .speakers = {last, last}, .first_only = false});
        }
        return units;
    }
    if (is_objects_with_lfe(ch_mode)) {
        // The element's own, its LFE carrying none.
        const int n = ch_mode - object_layout::kObjectsWithLfeBase;
        return n == 0 ? std::vector<AspxUnit>{}
                      : aspx_units(
                            n == 1 ? ch_mode::kMono : (n == 2 ? ch_mode::kStereo : ch_mode::k3_0),
                            codec_mode, decoding);
    }
    if (is_immersive(ch_mode)) {
        // Part 2 Table 8, in the order the syntax reads the elements.
        const bool core = decoding == DecodingMode::kCore;
        const Speaker tl = core ? S::kTopSideLeft : S::kTopFrontLeft;
        const Speaker tr = core ? S::kTopSideRight : S::kTopFrontRight;
        const AspxUnit front = {
            .pair = true, .index = 0, .speakers = {S::kLeft, S::kRight}, .first_only = false};
        const AspxUnit surround = {.pair = true,
                                   .index = 1,
                                   .speakers = {S::kLeftSurround, S::kRightSurround},
                                   .first_only = false};
        const AspxUnit centre = {
            .pair = false, .index = 0, .speakers = {S::kCentre, S::kCentre}, .first_only = false};
        switch (codec_mode) {
            case immersive_mode::kAspxScpl:
                // (Ls, Lb), (Rs, Rb), C, (L, R), (Tfl, Tbl), (Tfr, Tbr); in core
                // decoding the first channel of each coupled pair alone.
                return {{.pair = true,
                         .index = 0,
                         .speakers = {S::kLeftSurround, S::kLeftBack},
                         .first_only = core},
                        {.pair = true,
                         .index = 1,
                         .speakers = {S::kRightSurround, S::kRightBack},
                         .first_only = core},
                        centre,
                        {.pair = true,
                         .index = 2,
                         .speakers = {S::kLeft, S::kRight},
                         .first_only = false},
                        {.pair = true,
                         .index = 3,
                         .speakers = {tl, S::kTopBackLeft},
                         .first_only = core},
                        {.pair = true,
                         .index = 4,
                         .speakers = {tr, S::kTopBackRight},
                         .first_only = core}};
            case immersive_mode::kAspxAcpl1:
            case immersive_mode::kAspxAcpl2:
                // (A'', B''), (D'', E''), (F'', G''), C''.
                return {front,
                        surround,
                        {.pair = true, .index = 2, .speakers = {tl, tr}, .first_only = false},
                        centre};
            case immersive_mode::kAspxAjcc:
                return {front, surround, centre};
            default:
                return {};  // SCPL
        }
    }
    if (codec_mode == codec_mode::kSimple) {
        return {};
    }
    const AspxUnit front_pair = {.pair = true, .index = 0, .speakers = {S::kLeft, S::kRight}};
    const AspxUnit centre_single = {.pair = false, .index = 0, .speakers = {S::kCentre, S::kCentre}};
    const AspxUnit surround_pair = {.pair = true, .index = 1, .speakers = {S::kLeftSurround, S::kRightSurround}};
    const bool acpl_1_2 = codec_mode == codec_mode::kAspxAcpl1 || codec_mode == codec_mode::kAspxAcpl2;
    switch (ch_mode) {
        case ch_mode::kMono:
            return {centre_single};
        case ch_mode::kStereo:
            if (codec_mode != codec_mode::kAspx) {
                return {{.pair = false, .index = 0, .speakers = {S::kLeft, S::kLeft}}};
            }
            return {front_pair};
        case ch_mode::k3_0:
            return {front_pair, centre_single};
        case ch_mode::k5_0:
        case ch_mode::k5_1:
            if (codec_mode == codec_mode::kAspxAcpl3) {
                return {front_pair};
            }
            if (acpl_1_2) {
                return {front_pair, centre_single};
            }
            return {front_pair, surround_pair, centre_single};
        default:
            break;
    }
    if (!is_7x(ch_mode)) {
        return {};
    }
    if (acpl_1_2) {
        return {front_pair, surround_pair, centre_single};
    }
    // Table 213: 5/2/0 sends the wide pair second and the surrounds last; the
    // others the surrounds second and their last pair last.
    const auto last = last_pair(ch_mode);
    const bool wide = ch_mode == ch_mode::k7_0_520 || ch_mode == ch_mode::k7_1_520;
    const std::array<Speaker, 2> surround = {S::kLeftSurround, S::kRightSurround};
    return {{.pair = true, .index = 0, .speakers = {S::kLeft, S::kRight}},
            {.pair = true, .index = 1, .speakers = wide ? last : surround},
            {.pair = false, .index = 0, .speakers = {S::kCentre, S::kCentre}},
            {.pair = true, .index = 2, .speakers = wide ? surround : last}};
}

std::vector<Speaker> companded_speakers(int ch_mode, int codec_mode) {
    if (const int n = var_signals(ch_mode); n > 0) {
        // companding_control(n_dmx_signals) up to five signals, the fullband
        // outputs in syntax order.
        if (codec_mode != codec_mode::kAspx || n > 5) {
            return {};
        }
        return {kVarSlots.begin(), kVarSlots.begin() + n};
    }
    if (is_objects_with_lfe(ch_mode)) {
        const int n = ch_mode - object_layout::kObjectsWithLfeBase;
        return n == 0 ? std::vector<Speaker>{}
                      : companded_speakers(
                            n == 1 ? ch_mode::kMono : (n == 2 ? ch_mode::kStereo : ch_mode::k3_0),
                            codec_mode);
    }
    if (is_immersive(ch_mode)) {
        if (codec_mode == immersive_mode::kAspxAjcc) {
            return {S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround};
        }
        return {};
    }
    if (codec_mode == codec_mode::kSimple) {
        return {};
    }
    const bool aspx = codec_mode == codec_mode::kAspx;
    switch (ch_mode) {
        case ch_mode::kMono:
            return {S::kCentre};
        case ch_mode::kStereo:
            if (!aspx) {
                return {S::kLeft};
            }
            return {S::kLeft, S::kRight};
        case ch_mode::k3_0:
            return {S::kLeft, S::kRight, S::kCentre};
        case ch_mode::k5_0:
        case ch_mode::k5_1:
            if (codec_mode == codec_mode::kAspxAcpl3) {
                return {S::kLeft, S::kRight};
            }
            if (!aspx) {
                return {S::kLeft, S::kRight, S::kCentre};
            }
            return {S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround};
        default:
            break;
    }
    if (is_7x(ch_mode) && !aspx) {
        return {S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround};
    }
    return {};
}

}  // namespace ac4::detail
