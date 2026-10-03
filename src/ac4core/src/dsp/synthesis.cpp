#include "iclforge/ac4core/dsp/synthesis.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>

#include "iclforge/ac4core/detail/profiling.hpp"
#include "iclforge/ac4core/dsp/kbd.hpp"
#include "iclforge/ac4core/dsp/transform_tables.hpp"

namespace iclforge::ac4::detail::dsp {
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
        // In flash at the float and fixed tiers for the lengths dsp/transform_tables.hpp
        // builds in, at 44.1 and 48 kHz, where Table 186's alpha is the one it was built with.
        const TransformTable<Real>* const table =
            rate_multiplier == 1 ? transform_table<Real>(static_cast<std::size_t>(length)) : nullptr;
        if (table != nullptr) {
            window_tables_.push_back(table->kbd_left);
            windows_.emplace_back();
        } else {
            window_tables_.emplace_back();
            windows_.push_back(computed_kbd_left(length, rate_multiplier));
        }
    }
    valid_ = !imdct_.empty();
    if (valid_) {
        block_.assign(2 * static_cast<std::size_t>(full_length), Real{});
        transform_.assign(static_cast<std::size_t>(full_length), Complex{});
    }
}

template <typename Real>
std::vector<Real> TransformSet<Real>::computed_kbd_left(int length, int rate_multiplier) {
    // kbd_left() computes the window in double (the Kaiser-Bessel Bessel
    // function series wants the precision); narrowed to Real explicitly,
    // once, here - the vector<double>-to-vector<Real> range constructor
    // narrows implicitly per element, which -Wdouble-promotion's sibling
    // warning (MSVC's C4244) rightly flags as an error on the float build.
    const std::vector<double> window = dsp::kbd_left(length, kbd_alpha(length, rate_multiplier));
    std::vector<Real> narrowed(window.size());
    std::ranges::transform(window, narrowed.begin(), [](double w) { return static_cast<Real>(w); });
    return narrowed;
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
    if (k < 0) {
        return {};
    }
    const auto index = static_cast<std::size_t>(k);
    return window_tables_[index].empty() ? std::span<const Real>(windows_[index]) : window_tables_[index];
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
    return block(transforms, spectrum, 0, pcm);
}

template <typename Real>
bool ChannelSynthesis<Real>::block(TransformSet<Real>& transforms, std::span<const Real> spectrum,
                                   int exponent, std::span<Real> pcm) {
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
    AC4_ZONE_SCOPED_N("ac4_imdct");
    const auto full = static_cast<std::size_t>(full_length_);

    // A full-length block after a full-length block has no skipped samples, and its transform,
    // window and overlap-add are one pass over the samples (Imdct::inverse_overlap). At Fixed32
    // every block takes the inverse that carries its exponent.
    if (std::is_floating_point_v<Real> && n == full && n_prev == full) {
        imdct->inverse_overlap(spectrum, kbd, overlap_, pcm,
                               transforms.transform_scratch().first(n));
        previous_length_ = n_int;
        return true;
    }

    // Steps 1 to 4 and Pseudocode 63's unfolding, into the set's block scratch.
    const std::span<Real> x = transforms.block_scratch().first(2 * n);
    imdct->inverse(spectrum, exponent, x, transforms.transform_scratch().first(n));

    // Pseudocode 63's window over the first half.
    const std::size_t skip_left = (n - nw) / 2;
    std::fill_n(x.begin(), skip_left, Real(0));
    for (std::size_t i = 0; i < nw; ++i) {
        x[skip_left + i] *= kbd[i];
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
        overlap_[nskip + i] += x[i];
    }
    std::copy_n(overlap_.begin(), n, pcm.begin());
    for (std::size_t i = 0; i < nskip; ++i) {
        overlap_[i] = overlap_[n + i];
    }
    std::copy_n(x.begin() + static_cast<std::ptrdiff_t>(n), n,
                overlap_.begin() + static_cast<std::ptrdiff_t>(nskip));
    previous_length_ = n_int;
    return true;
}

template class TransformSet<Real>;
template class ChannelSynthesis<Real>;
// ac4core's own tests (tests/ac4core/test_ac4core_dsp.cpp) exercise both at
// double directly, alongside Real (see this target's CMakeLists.txt,
// AC4CORE_ALSO_AT_DOUBLE).
AC4CORE_ALSO_AT_DOUBLE(
    template class TransformSet<double>;
    template class ChannelSynthesis<double>;)

}  // namespace iclforge::ac4::detail::dsp
