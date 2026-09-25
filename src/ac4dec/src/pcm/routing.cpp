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
    switch (element.kind) {
        case ElementKind::kSingle:
            walk.mono(S::kCentre);
            break;
        case ElementKind::kPair:
            walk.pair(S::kLeft, S::kRight);
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
            // Table 180.
            if (lfe) {
                walk.lfe();
            }
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
            if (element.b_use_sap_add_ch.value_or(false)) {
                const int first = walk.take_chparams(2);
                const bool back = ctx.ch_mode == ch_mode::k7_0_340 || ctx.ch_mode == ch_mode::k7_1_340;
                out.steps.push_back({back ? S::kLeftSurround : S::kLeft, f, first});
                out.steps.push_back({back ? S::kRightSurround : S::kRight, g, first + 1});
            }
            walk.pair(f, g);
            if (config == 0 || config == 2) {
                walk.mono(S::kCentre);
            }
            break;
        }
    }
    const bool has_config = element.kind == ElementKind::kSingle || element.kind == ElementKind::kPair ||
                            element.coding_config.has_value();
    const bool needs_two_ch_mode =
        (element.kind == ElementKind::k5X || element.kind == ElementKind::k7X) && config == 0;
    const bool lfe_expected = (element.kind == ElementKind::k5X || element.kind == ElementKind::k7X) &&
                              ctx.has_lfe();
    if (!has_config || (needs_two_ch_mode && !element.two_ch_mode.has_value()) || lfe != lfe_expected ||
        (is_7x(ctx.ch_mode) && !element.b_use_sap_add_ch.has_value()) || !walk.complete()) {
        return fail(DecodeError::kInvalidStream, "a channel element whose parts do not match its coding_config");
    }
    return {};
}

std::vector<AspxUnit> aspx_units(int ch_mode) {
    switch (ch_mode) {
        case ch_mode::kMono:
            return {{.pair = false, .index = 0, .speakers = {S::kCentre, S::kCentre}}};
        case ch_mode::kStereo:
            return {{.pair = true, .index = 0, .speakers = {S::kLeft, S::kRight}}};
        case ch_mode::k3_0:
            return {{.pair = true, .index = 0, .speakers = {S::kLeft, S::kRight}},
                    {.pair = false, .index = 0, .speakers = {S::kCentre, S::kCentre}}};
        case ch_mode::k5_0:
        case ch_mode::k5_1:
            return {{.pair = true, .index = 0, .speakers = {S::kLeft, S::kRight}},
                    {.pair = true, .index = 1, .speakers = {S::kLeftSurround, S::kRightSurround}},
                    {.pair = false, .index = 0, .speakers = {S::kCentre, S::kCentre}}};
        default:
            break;
    }
    if (!is_7x(ch_mode)) {
        return {};
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

std::vector<Speaker> companded_speakers(int ch_mode) {
    switch (ch_mode) {
        case ch_mode::kMono:
            return {S::kCentre};
        case ch_mode::kStereo:
            return {S::kLeft, S::kRight};
        case ch_mode::k3_0:
            return {S::kLeft, S::kRight, S::kCentre};
        case ch_mode::k5_0:
        case ch_mode::k5_1:
            return {S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround};
        default:
            return {};
    }
}

}  // namespace ac4::detail
