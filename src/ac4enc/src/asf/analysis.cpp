#include "asf/analysis.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "dsp/kbd.hpp"

namespace ac4::detail {
namespace {

constexpr int kLengthsPerFrame = 5;  // the full length and its halves to a sixteenth
constexpr double kLineScale = 65536.0;

}  // namespace

Analysis::Analysis(int frame_length, int rate_multiplier) : frame_length_(frame_length) {
    for (int k = 0; k < kLengthsPerFrame; ++k) {
        const int length = frame_length >> k;
        const double alpha = dsp::kbd_alpha(length, rate_multiplier);
        if (alpha == 0.0) {
            return;
        }
        dsp::Mdct<double> mdct(static_cast<std::size_t>(length));
        if (!mdct.valid()) {
            return;
        }
        mdct_.push_back(std::move(mdct));
        kbd_.push_back(dsp::kbd_left(length, alpha));
    }
    valid_ = true;
}

int Analysis::slot(int length) const noexcept {
    for (std::size_t k = 0; k < mdct_.size(); ++k) {
        if ((frame_length_ >> k) == length) {
            return static_cast<int>(k);
        }
    }
    return -1;
}

void Analysis::transform(std::span<const double> frame, const FrameLayout& layout, int previous, int next,
                         std::vector<double>& spectrum) {
    spectrum.assign(static_cast<std::size_t>(frame_length_), 0.0);
    const auto full = static_cast<std::size_t>(frame_length_);
    std::size_t start = 0;
    const std::size_t count = layout.window_length.size();
    for (std::size_t j = 0; j < count; ++j) {
        const int n_int = layout.window_length[j];
        const auto n = static_cast<std::size_t>(n_int);
        const int before = j == 0 ? previous : layout.window_length[j - 1];
        const int after = j + 1 == count ? next : layout.window_length[j + 1];
        const auto nw_left = static_cast<std::size_t>(std::min(n_int, before));
        const auto nw_right = static_cast<std::size_t>(std::min(n_int, after));
        const std::vector<double>& left = kbd_[static_cast<std::size_t>(slot(static_cast<int>(nw_left)))];
        const std::vector<double>& right = kbd_[static_cast<std::size_t>(slot(static_cast<int>(nw_right)))];

        segment_.assign(2 * n, 0.0);
        const std::size_t origin = start + (full - n) / 2;
        const std::size_t skip_left = (n - nw_left) / 2;
        const std::size_t skip_right = (n - nw_right) / 2;
        for (std::size_t i = 0; i < n; ++i) {
            // Pseudocode 63's window over the first half.
            double w = 0.0;
            if (i >= skip_left && i < skip_left + nw_left) {
                w = left[i - skip_left];
            } else if (i >= skip_left + nw_left) {
                w = 1.0;
            }
            segment_[i] = frame[origin + i] * w;
            // Pseudocode 64's window over the second half, KBD_RIGHT at the
            // same position being KBD_LEFT reversed.
            double r = 0.0;
            if (i < skip_right) {
                r = 1.0;
            } else if (i < skip_right + nw_right) {
                r = right[nw_right - 1 - (i - skip_right)];
            }
            segment_[n + i] = frame[origin + n + i] * r;
        }
        std::span<double> lines = std::span<double>(spectrum).subspan(start, n);
        mdct_[static_cast<std::size_t>(slot(n_int))].forward(segment_, lines);
        for (double& line : lines) {
            line *= kLineScale;
        }
        start += n;
    }
}

}  // namespace ac4::detail
