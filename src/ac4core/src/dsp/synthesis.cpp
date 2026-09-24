#include "dsp/synthesis.hpp"

#include <algorithm>
#include <utility>

#include "dsp/kbd.hpp"

namespace ac4::detail::dsp {
namespace {

// Block lengths are the full length halved up to four times (clause 5.5.3).
constexpr int kLengthsPerFull = 5;

}  // namespace

// Below a full length of 1 536 the shortest block is a quarter or an eighth of
// the frame (Table 103), and the halving stops at the first length Table 186
// does not list.
template <typename Real>
TransformSet<Real>::TransformSet(int full_length, int rate_multiplier) : full_length_(full_length) {
    if (full_length <= 0) {
        return;
    }
    for (int k = 0; k < kLengthsPerFull && full_length % (1 << k) == 0; ++k) {
        const int length = full_length >> k;
        const double alpha = kbd_alpha(length, rate_multiplier);
        if (alpha == 0.0) {
            break;
        }
        Imdct<Real> imdct(static_cast<std::size_t>(length));
        if (!imdct.valid()) {
            break;
        }
        imdct_.push_back(std::move(imdct));
        const std::vector<double> window = dsp::kbd_left(length, alpha);
        windows_.emplace_back(window.begin(), window.end());
    }
    valid_ = !imdct_.empty();
}

template <typename Real>
int TransformSet<Real>::slot(int length) const noexcept {
    for (std::size_t k = 0; k < imdct_.size(); ++k) {
        if ((full_length_ >> k) == length) {
            return static_cast<int>(k);
        }
    }
    return -1;
}

template <typename Real>
Imdct<Real>* TransformSet<Real>::imdct(int length) noexcept {
    const int k = slot(length);
    return k < 0 ? nullptr : &imdct_[static_cast<std::size_t>(k)];
}

template <typename Real>
std::span<const Real> TransformSet<Real>::kbd_left(int length) const noexcept {
    const int k = slot(length);
    return k < 0 ? std::span<const Real>{} : std::span<const Real>(windows_[static_cast<std::size_t>(k)]);
}

template <typename Real>
ChannelSynthesis<Real>::ChannelSynthesis(int full_length)
    : full_length_(std::max(full_length, 0)), previous_length_(full_length_),
      overlap_(static_cast<std::size_t>(full_length_)) {}

template <typename Real>
void ChannelSynthesis<Real>::reset() {
    std::ranges::fill(overlap_, Real(0));
    previous_length_ = full_length_;
}

template <typename Real>
bool ChannelSynthesis<Real>::block(TransformSet<Real>& transforms, std::span<const Real> spectrum,
                                   std::span<Real> pcm) {
    const std::size_t n = spectrum.size();
    const auto n_int = static_cast<int>(n);
    if (transforms.full_length() != full_length_ || pcm.size() < n) {
        return false;
    }
    Imdct<Real>* imdct = transforms.imdct(n_int);
    const auto n_prev = static_cast<std::size_t>(previous_length_);
    const std::size_t nw = std::min(n, n_prev);
    const std::span<const Real> kbd = transforms.kbd_left(static_cast<int>(nw));
    if (imdct == nullptr || kbd.size() != nw) {
        return false;
    }
    const auto full = static_cast<std::size_t>(full_length_);

    // Steps 1 to 4 and Pseudocode 63's unfolding.
    x_.resize(2 * n);
    imdct->inverse(spectrum, x_);

    // Pseudocode 63's window over the first half.
    const std::size_t skip_left = (n - nw) / 2;
    std::fill_n(x_.begin(), skip_left, Real(0));
    for (std::size_t i = 0; i < nw; ++i) {
        x_[skip_left + i] *= kbd[i];
    }

    // Pseudocode 64. The previous block's second half, at nskip_prev, takes
    // the right window first.
    const std::size_t nskip = (full - n) / 2;
    const std::size_t nskip_prev = (full - n_prev) / 2;
    const std::size_t skip_right = (n_prev - nw) / 2;
    Real* previous = overlap_.data() + nskip_prev;
    for (std::size_t i = 0; i < nw; ++i) {
        previous[skip_right + i] *= kbd[nw - 1 - i];
    }
    std::fill(previous + skip_right + nw, previous + n_prev, Real(0));
    for (std::size_t i = 0; i < n; ++i) {
        overlap_[nskip + i] += x_[i];
    }
    std::copy_n(overlap_.begin(), n, pcm.begin());
    for (std::size_t i = 0; i < nskip; ++i) {
        overlap_[i] = overlap_[n + i];
    }
    std::copy_n(x_.begin() + static_cast<std::ptrdiff_t>(n), n,
                overlap_.begin() + static_cast<std::ptrdiff_t>(nskip));
    previous_length_ = n_int;
    return true;
}

template class TransformSet<double>;
template class ChannelSynthesis<double>;

}  // namespace ac4::detail::dsp
