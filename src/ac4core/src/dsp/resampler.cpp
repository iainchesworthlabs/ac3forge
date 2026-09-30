#include "iclforge/ac4core/dsp/resampler.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <type_traits>
#include <utility>

#include "iclforge/ac4core/detail/profiling.hpp"
#include "iclforge/ac4core/dsp/kbd.hpp"
#include "iclforge/ac4core/dsp/resampler_vector.hpp"

namespace iclforge::ac4::detail::dsp {

template <typename Coefficient>
BasicResamplerFilter<Coefficient>::BasicResamplerFilter(int up, int down) {
    if (up <= 0 || down <= 0) {
        up = 1;
        down = 1;
    }
    const int common = std::gcd(up, down);
    up_ = up / common;
    down_ = down / common;
    if (up_ == down_) {
        table_.assign(1, Coefficient{1});
        return;
    }
    // In cycles per input sample: the lower rate's Nyquist frequency is the
    // stopband edge, and the passband runs to 0.86 of it.
    const double nyquist =
        0.5 * std::min(1.0, static_cast<double>(up_) / static_cast<double>(down_));
    passband_ = 0.86 * nyquist;
    stopband_ = nyquist;
    const double cutoff = 0.5 * (passband_ + stopband_);
    // Kaiser's estimates for the window's beta and length at the attenuation.
    const double beta = 0.1102 * (kAttenuationDb - 8.7);
    const double length =
        (kAttenuationDb - 7.95) / (2.285 * 2.0 * std::numbers::pi * (stopband_ - passband_)) + 1.0;
    taps_ = 2 * static_cast<int>(std::ceil(length / 2.0));
    const double half_width = static_cast<double>(taps_) / 2.0;
    const double norm = bessel_i0(beta);
    table_.resize(static_cast<std::size_t>(up_) * static_cast<std::size_t>(taps_));
    // One phase at a time, designed in double whatever the table is kept in,
    // then normalised to sum to 1 in double and rounded to the table's scalar.
    std::vector<double> design(static_cast<std::size_t>(taps_));
    for (int p = 0; p < up_; ++p) {
        double sum = 0.0;
        for (int k = 0; k < taps_; ++k) {
            // The distance from the output's position to the tap's input sample.
            const double t = static_cast<double>(k - taps_ / 2 + 1) -
                             static_cast<double>(p) / static_cast<double>(up_);
            const double x = t / half_width;
            const double window = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - x * x))) / norm;
            const double arg = std::numbers::pi * 2.0 * cutoff * t;
            const double sinc = t == 0.0 ? 1.0 : std::sin(arg) / arg;
            design[static_cast<std::size_t>(k)] = 2.0 * cutoff * sinc * window;
            sum += design[static_cast<std::size_t>(k)];
        }
        const std::span<Coefficient> row(
            table_.data() + static_cast<std::size_t>(p) * static_cast<std::size_t>(taps_),
            static_cast<std::size_t>(taps_));
        for (std::size_t k = 0; k < row.size(); ++k) {
            row[k] = static_cast<Coefficient>(design[k] / sum);
        }
    }
}

template <typename Coefficient>
std::span<const Coefficient> BasicResamplerFilter<Coefficient>::phase(int p) const noexcept {
    if (p < 0 || p >= up_) {
        return {};
    }
    return std::span<const Coefficient>(table_).subspan(
        static_cast<std::size_t>(p) * static_cast<std::size_t>(taps_),
        static_cast<std::size_t>(taps_));
}

template <typename Coefficient>
double BasicResamplerFilter<Coefficient>::delay() const noexcept {
    if (up_ == down_) {
        return 0.0;
    }
    return static_cast<double>(taps_ / 2 + 1) -
           static_cast<double>(down_) / static_cast<double>(up_);
}

template <typename Real>
Resampler<Real>::Resampler(std::shared_ptr<const BasicResamplerFilter<Real>> filter)
    : filter_(std::move(filter)) {
    reset();
}

template <typename Real>
void Resampler<Real>::reset(std::int64_t inputs_before) {
    const std::int64_t up = filter_->up();
    const std::int64_t down = filter_->down();
    const std::int64_t taps = filter_->taps();
    inputs_ = inputs_before;
    // floor(inputs * up / down), for a negative count too.
    const std::int64_t scaled = inputs_ * up;
    outputs_ = scaled >= 0 ? scaled / down : -((-scaled + down - 1) / down);
    first_ = inputs_ - taps;
    history_.assign(static_cast<std::size_t>(taps), Real{});
}

template <typename Real>
void Resampler<Real>::rephase(std::int64_t inputs_before) {
    const std::int64_t up = filter_->up();
    const std::int64_t down = filter_->down();
    const std::int64_t shift = inputs_before - inputs_;
    inputs_ = inputs_before;
    first_ += shift;
    const std::int64_t scaled = inputs_ * up;
    outputs_ = scaled >= 0 ? scaled / down : -((-scaled + down - 1) / down);
    // The next output's taps may now reach before the history kept.
    const std::int64_t position = (outputs_ + 1) * down;
    const std::int64_t whole = position >= 0 ? position / up : -((-position + up - 1) / up);
    const std::int64_t missing = first_ - (whole - filter_->taps());
    if (missing > 0) {
        history_.insert(history_.begin(), static_cast<std::size_t>(missing), Real{});
        first_ -= missing;
    }
}

template <typename Real>
void Resampler<Real>::process(std::span<const Real> in, std::vector<Real>& out) {
    AC4_ZONE_SCOPED_N("ac4_resampler");
    const std::int64_t up = filter_->up();
    const std::int64_t down = filter_->down();
    const std::int64_t taps = filter_->taps();
    history_.insert(history_.end(), in.begin(), in.end());
    inputs_ += static_cast<std::int64_t>(in.size());
    const auto floor_div = [](std::int64_t a, std::int64_t b) {
        return a >= 0 ? a / b : -((-a + b - 1) / b);
    };
    while ((outputs_ + 1) * down <= inputs_ * up) {
        const std::int64_t position = (outputs_ + 1) * down;
        const std::int64_t whole = floor_div(position, up);
        const auto p = static_cast<int>(position - whole * up);
        // The dot product in Real: at double the taps in order, as it always
        // was; at float over four lanes (dsp/resampler_vector.hpp), a float
        // multiply and add a tap, which is the whole of the converter's cost on
        // a part whose FPU is single precision.
        const std::span<const Real> coefficients = filter_->phase(p);
        const Real* samples = history_.data() + (whole - taps - first_);
        Real sum{};
        if constexpr (std::is_same_v<Real, float>) {
            sum = dot_four_lanes(coefficients.data(), samples, coefficients.size());
        } else {
            for (std::size_t k = 0; k < coefficients.size(); ++k) {
                sum += coefficients[k] * samples[k];
            }
        }
        out.push_back(sum);
        ++outputs_;
    }
    // Keep what the next output's taps reach back to, and one sample more for
    // a grid that rephase() moves back by up to a sample.
    const std::int64_t next_start = floor_div((outputs_ + 1) * down, up) - taps - 1;
    if (next_start > first_) {
        const std::int64_t drop =
            std::min<std::int64_t>(next_start - first_, static_cast<std::int64_t>(history_.size()));
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(drop));
        first_ += drop;
    }
}

template <typename Real>
std::size_t Resampler<Real>::outputs_for(std::size_t count) const noexcept {
    const std::int64_t up = filter_->up();
    const std::int64_t down = filter_->down();
    const std::int64_t after = ((inputs_ + static_cast<std::int64_t>(count)) * up) / down;
    return static_cast<std::size_t>(std::max<std::int64_t>(0, after - outputs_));
}

template class BasicResamplerFilter<Real>;
template class Resampler<Real>;
// The encoder's own sample rate handling (src/ac4enc/src/encoder.cpp) calls
// this at double regardless of the decoder's scalar (see this target's
// CMakeLists.txt, AC4CORE_ALSO_AT_DOUBLE), with the double filter.
AC4CORE_ALSO_AT_DOUBLE(template class BasicResamplerFilter<double>;)
AC4CORE_ALSO_AT_DOUBLE(template class Resampler<double>;)

}  // namespace iclforge::ac4::detail::dsp
