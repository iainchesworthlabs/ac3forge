#include "pcm/isf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "tables/isf_tables.hpp"

namespace ac4::detail {
namespace {

using S = Speaker;

// Annex A.2.1's columns are the layout's speakers in Table A.27's order, the
// LFE left out: checked against the matrices' values, a source at M1 (the
// front) the same in L and R, at M2 (the left of the middle ring) larger in
// the left speakers. The 9.X layouts' screen pair (Table A.27's 24 and 25) is
// no Speaker, so the decoder renders none of the four 9.X layouts.
constexpr std::array kIsf2 = {S::kLeft, S::kRight};
constexpr std::array kIsf5 = {S::kLeft, S::kRight, S::kCentre, S::kLeftSurround, S::kRightSurround};
constexpr std::array kIsf7 = {S::kLeft,          S::kRight,    S::kCentre,   S::kLeftSurround,
                              S::kRightSurround, S::kLeftBack, S::kRightBack};
constexpr std::array kIsf502 = {S::kLeft,         S::kRight,         S::kCentre,
                                S::kLeftSurround, S::kRightSurround, S::kTopSideLeft,
                                S::kTopSideRight};
constexpr std::array kIsf504 = {S::kLeft,          S::kRight,         S::kCentre,
                                S::kLeftSurround,  S::kRightSurround, S::kTopFrontLeft,
                                S::kTopFrontRight, S::kTopBackLeft,   S::kTopBackRight};
constexpr std::array kIsf702 = {S::kLeft,         S::kRight,         S::kCentre,
                                S::kLeftSurround, S::kRightSurround, S::kLeftBack,
                                S::kRightBack,    S::kTopSideLeft,   S::kTopSideRight};
constexpr std::array kIsf704 = {S::kLeft,         S::kRight,         S::kCentre,
                                S::kLeftSurround, S::kRightSurround, S::kLeftBack,
                                S::kRightBack,    S::kTopFrontLeft,  S::kTopFrontRight,
                                S::kTopBackLeft,  S::kTopBackRight};

struct IsfLayout {
    std::size_t matrix = 0;  // the layout's index in tables::kIsfMatrices' rows
    std::span<const Speaker> speakers;
};

// kIsfMatrices' layouts 2.x, 5.x, 7.x, 5.x.2, 5.x.4, 7.x.2 and 7.x.4.
constexpr std::array<IsfLayout, 7> kLayouts = {{
    {.matrix = 0, .speakers = kIsf2},
    {.matrix = 1, .speakers = kIsf5},
    {.matrix = 2, .speakers = kIsf7},
    {.matrix = 4, .speakers = kIsf502},
    {.matrix = 5, .speakers = kIsf504},
    {.matrix = 6, .speakers = kIsf702},
    {.matrix = 7, .speakers = kIsf704},
}};

constexpr bool columns_match() {
    for (const IsfLayout& layout : kLayouts) {
        if (static_cast<int>(layout.speakers.size()) != tables::kIsfOutputs[layout.matrix]) {
            return false;
        }
    }
    return true;
}
static_assert(columns_match(), "an ISF layout's speakers are not its matrix's columns");

[[nodiscard]] const IsfLayout& layout_for(DownmixTarget target) noexcept {
    switch (target) {
        case DownmixTarget::k5X:
            return kLayouts[1];
        case DownmixTarget::kStereo:
        case DownmixTarget::kLoRo:
        case DownmixTarget::kLtRt:
        case DownmixTarget::kMono:
            return kLayouts[0];
        case DownmixTarget::k7X2:
            return kLayouts[5];
        case DownmixTarget::k7X0:
            return kLayouts[2];
        case DownmixTarget::k5X4:
            return kLayouts[4];
        case DownmixTarget::k5X2:
            return kLayouts[3];
        case DownmixTarget::kAsCoded:
        case DownmixTarget::k7X4:
            break;
    }
    return kLayouts[6];
}

// The layout `speakers` are, the LFEs aside, in any order.
[[nodiscard]] const IsfLayout* layout_of(std::span<const Speaker> speakers) noexcept {
    const auto fullband = static_cast<std::size_t>(
        std::ranges::count_if(speakers, [](Speaker s) { return s != S::kLfe && s != S::kLfe2; }));
    for (const IsfLayout& layout : kLayouts) {
        if (layout.speakers.size() == fullband &&
            std::ranges::all_of(layout.speakers, [speakers](Speaker s) {
                return std::ranges::find(speakers, s) != speakers.end();
            })) {
            return &layout;
        }
    }
    return nullptr;
}

}  // namespace

void IsfGain::apply(std::span<float> samples, std::span<const ObjectUpdate> updates) noexcept {
    std::size_t u = 0;
    for (std::size_t n = 0; n < samples.size(); ++n) {
        while (u < updates.size() && updates[u].sample <= n) {
            const ObjectProperties& p = updates[u].properties;
            target_ = p.active ? std::pow(10.0, p.gain_db / 20.0) : 0.0;
            left_ = std::max(updates[u].ramp_samples, 0);
            step_ = left_ > 0 ? (target_ - gain_) / static_cast<double>(left_) : 0.0;
            if (left_ == 0) {
                gain_ = target_;
            }
            ++u;
        }
        if (left_ > 0) {
            gain_ = --left_ == 0 ? target_ : gain_ + step_;
        }
        samples[n] = static_cast<float>(static_cast<double>(samples[n]) * gain_);
    }
}

int isf_config_of(int objects) noexcept {
    const auto it = std::ranges::find(tables::kIsfChannels, objects);
    return it == tables::kIsfChannels.end() ? -1
                                            : static_cast<int>(it - tables::kIsfChannels.begin());
}

bool render_isf(std::span<const IsfInput> inputs, DownmixTarget target, std::size_t length,
                std::vector<std::vector<float>>& channels, std::vector<Speaker>& speakers) {
    // Each matrix column's output channel: a mono channel takes both of the
    // 2.X layout's.
    std::array<std::size_t, 13> column_channel{};
    const IsfLayout* layout = nullptr;
    const bool mono = speakers.empty() ? target == DownmixTarget::kMono
                                       : speakers.size() == 1 && speakers.front() == S::kCentre;
    if (speakers.empty()) {
        layout = &layout_for(target);
        if (mono) {
            speakers.assign(1, S::kCentre);
        } else {
            speakers.assign(layout->speakers.begin(), layout->speakers.end());
        }
        channels.resize(speakers.size());
        for (std::vector<float>& channel : channels) {
            channel.assign(length, 0.0F);
        }
    } else {
        layout = mono ? kLayouts.data() : layout_of(speakers);
        if (layout == nullptr || channels.size() != speakers.size()) {
            return false;
        }
    }
    for (std::size_t c = 0; c < layout->speakers.size(); ++c) {
        column_channel[c] =
            mono ? 0
                 : static_cast<std::size_t>(std::ranges::find(speakers, layout->speakers[c]) -
                                            speakers.begin());
    }
    const std::size_t columns = layout->speakers.size();
    for (const IsfInput& input : inputs) {
        if (input.config < 0 ||
            static_cast<std::size_t>(input.config) >= tables::kIsfMatrices.size() ||
            input.index < 0 ||
            input.index >= tables::kIsfChannels[static_cast<std::size_t>(input.config)]) {
            continue;
        }
        const std::span<const float> matrix =
            tables::kIsfMatrices[static_cast<std::size_t>(input.config)][layout->matrix];
        const std::span<const float> row =
            matrix.subspan(static_cast<std::size_t>(input.index) * columns, columns);
        const std::size_t n = std::min(length, input.samples.size());
        for (std::size_t c = 0; c < columns; ++c) {
            const float coefficient = row[c];
            if (coefficient == 0.0F) {
                continue;
            }
            std::vector<float>& out = channels[column_channel[c]];
            for (std::size_t i = 0; i < std::min(n, out.size()); ++i) {
                out[i] += coefficient * input.samples[i];
            }
        }
    }
    return true;
}

}  // namespace ac4::detail
