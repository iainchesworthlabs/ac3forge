#include "pcm/routing.hpp"

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

    void pair(Speaker o0, Speaker o1) {
        bool processed = false;
        if (pair_ < element_.b_enable_mdct_stereo_proc.size()) {
            processed = element_.b_enable_mdct_stereo_proc[pair_];
        } else {
            short_ = true;
        }
        ++pair_;
        add(2, {o0, o1}, processed, processed ? 1 : 0, 0);
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

}  // namespace

std::span<const Speaker> speakers_of(int ch_mode) noexcept {
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
        default:
            return {};
    }
}

ParseResult route_element(const SubstreamContext& ctx, const ChannelElement& element, ElementRoute& out) {
    out = ElementRoute{};
    Walker walk(element, out);
    const bool lfe = !element.tracks.empty() && element.tracks.front().lfe;
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
            return fail(DecodeError::kUnsupported, "the immersive channel element is not decoded to PCM yet");
    }
    const bool five_x_acpl = element.kind == ElementKind::k5X && acpl;
    // ASPX_ACPL_3 sends no coding_config: its channel data is stereo_data().
    const bool has_config = element.kind == ElementKind::kSingle || element.kind == ElementKind::kPair ||
                            element.coding_config.has_value() ||
                            (five_x_acpl && mode == codec_mode::kAspxAcpl3);
    const bool needs_two_ch_mode =
        (element.kind == ElementKind::k5X || element.kind == ElementKind::k7X) && config == 0 && !five_x_acpl;
    const bool lfe_expected = (element.kind == ElementKind::k5X || element.kind == ElementKind::k7X) &&
                              ctx.has_lfe();
    const bool needs_sap_add_ch = is_7x(ctx.ch_mode) && !acpl;
    if (!has_config || (needs_two_ch_mode && !element.two_ch_mode.has_value()) || lfe != lfe_expected ||
        (needs_sap_add_ch && !element.b_use_sap_add_ch.has_value()) || !walk.complete()) {
        return fail(DecodeError::kInvalidStream, "a channel element whose parts do not match its coding_config");
    }
    return {};
}

std::vector<AspxUnit> aspx_units(int ch_mode, int codec_mode) {
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
