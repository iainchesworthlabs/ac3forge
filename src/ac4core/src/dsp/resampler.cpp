#include "iclforge/ac4core/dsp/resampler.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <numeric>
#include <type_traits>
#include <utility>
#include <vector>

#include "iclforge/ac4core/detail/profiling.hpp"
#include "iclforge/ac4core/dsp/resampler_design.hpp"
#include "iclforge/ac4core/dsp/resampler_vector.hpp"

namespace iclforge::ac4::detail::dsp {
namespace {

// The design's functions as the C library has them, which design the table at double as the
// converter has always designed it.
struct LibmMath {
    [[nodiscard]] static double ceil(double x) noexcept { return std::ceil(x); }
    [[nodiscard]] static double sqrt(double x) noexcept { return std::sqrt(x); }
    [[nodiscard]] static double sin(double x) noexcept { return std::sin(x); }
    [[nodiscard]] static double bessel_i0(double x) noexcept { return dsp::bessel_i0(x); }
};

}  // namespace

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
        if constexpr (std::is_same_v<Coefficient, iclforge::internal::Fixed32>) {
            table_.assign(1, std::int32_t{1} << 30U);
        } else {
            table_.assign(1, Coefficient{1});
        }
        return;
    }
    if constexpr (std::is_same_v<Coefficient, iclforge::internal::Fixed32>) {
        halved_ = true;
        const auto adopt = [this]<int Up, int Down>() {
            if (up_ != Up || down_ != Down) {
                return false;
            }
            const HalfTableQ30<Up, Down>& compiled = kHalfTableQ30<Up, Down>;
            taps_ = compiled.kDesign.taps;
            passband_ = compiled.kDesign.passband;
            stopband_ = compiled.kDesign.stopband;
            if (sizeof(compiled.coefficients) <= kResamplerCopyLimit) {
                table_.assign(compiled.coefficients.begin(), compiled.coefficients.end());
            } else {
                in_place_ = compiled.coefficients.data();
            }
            return true;
        };
        if (adopt.template operator()<25, 24>() || adopt.template operator()<15, 16>() ||
            adopt.template operator()<1001, 960>()) {
            return;
        }
        const ResamplerDesign design = design_resampler<PortableMath>(up_, down_);
        taps_ = design.taps;
        passband_ = design.passband;
        stopband_ = design.stopband;
        table_.resize(static_cast<std::size_t>(up_ / 2 + 1) * static_cast<std::size_t>(taps_));
        std::vector<double> row(static_cast<std::size_t>(taps_));
        design_half_phases_q30<PortableMath>(design, table_.data(), row.data());
    } else if constexpr (std::is_same_v<Coefficient, float>) {
        // At float: the compiler's table for one of the decoder's ratios, and for any other the
        // same design made now, with the same functions. Either way phases 0 to up / 2, the rest
        // being those read backwards.
        halved_ = true;
        // One of the decoder's three ratios (Part 1 clause 6.2.15) has a table the compiler built;
        // naming it here is what makes the compiler evaluate it (dsp/resampler_design.hpp), and
        // only a build that has this scalar's filter reaches the names.
        const auto adopt = [this]<int Up, int Down>() {
            if (up_ != Up || down_ != Down) {
                return false;
            }
            const HalfTable<Up, Down>& compiled = kHalfTable<Up, Down>;
            taps_ = compiled.kDesign.taps;
            passband_ = compiled.kDesign.passband;
            stopband_ = compiled.kDesign.stopband;
            // The filter keeps a copy of the table, so that the converter reads it from the memory
            // the heap gives and not from the program's constants: on the ESP32-P4 and -S3 those
            // are flash behind a cache, where a miss costs ten times a PSRAM's, and the table of
            // 1001/960 (188 KB against a cache of 128 KB) misses on most of what it reads. Read in
            // place it took the converter 60 ms a frame at 23.976 fps on the P4; from the copy, in
            // the PSRAM that the heap puts a block of that size in, 17 ms (planning/ac4.md, D14a5).
            table_.assign(compiled.coefficients.begin(), compiled.coefficients.end());
            return true;
        };
        if (adopt.template operator()<25, 24>() || adopt.template operator()<15, 16>() ||
            adopt.template operator()<1001, 960>()) {
            return;
        }
        const ResamplerDesign design = design_resampler<PortableMath>(up_, down_);
        taps_ = design.taps;
        passband_ = design.passband;
        stopband_ = design.stopband;
        table_.resize(static_cast<std::size_t>(up_ / 2 + 1) * static_cast<std::size_t>(taps_));
        std::vector<double> row(static_cast<std::size_t>(taps_));
        design_half_phases<PortableMath>(design, table_.data(), row.data());
    } else {
        // At double: every phase designed with the C library's functions, as it always was.
        const ResamplerDesign design = design_resampler<LibmMath>(up_, down_);
        taps_ = design.taps;
        passband_ = design.passband;
        stopband_ = design.stopband;
        table_.resize(static_cast<std::size_t>(up_) * static_cast<std::size_t>(taps_));
        // One phase at a time, designed in double whatever the table is kept in, normalised to sum
        // to 1 in double and rounded to the table's scalar.
        std::vector<double> row(static_cast<std::size_t>(taps_));
        for (int p = 0; p < up_; ++p) {
            design_phase<LibmMath>(design, p, row.data());
            Coefficient* out =
                table_.data() + static_cast<std::size_t>(p) * static_cast<std::size_t>(taps_);
            for (std::size_t k = 0; k < row.size(); ++k) {
                out[k] = static_cast<Coefficient>(row[k]);
            }
        }
    }
}

template <typename Coefficient>
typename BasicResamplerFilter<Coefficient>::PhaseRef BasicResamplerFilter<Coefficient>::phase(
    int p) const noexcept {
    if (p < 0 || p >= up_) {
        return {};
    }
    const auto taps = static_cast<std::size_t>(taps_);
    const ResamplerStore<Coefficient>* table = in_place_ != nullptr ? in_place_ : table_.data();
    if (!halved_ || p <= up_ / 2) {
        return {table + static_cast<std::size_t>(p) * taps, false};
    }
    return {table + static_cast<std::size_t>(up_ - p) * taps, true};
}

template <typename Coefficient>
Coefficient BasicResamplerFilter<Coefficient>::coefficient(int p, int k) const noexcept {
    const PhaseRef ref = phase(p);
    if (ref.coefficients == nullptr || k < 0 || k >= taps_) {
        return Coefficient{};
    }
    const auto stored = ref.coefficients[ref.reversed ? taps_ - 1 - k : k];
    if constexpr (std::is_same_v<Coefficient, iclforge::internal::Fixed32>) {
        return Coefficient::from_raw(static_cast<std::int32_t>((static_cast<std::int64_t>(stored) + 32) >> 6U));
    } else {
        return stored;
    }
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
        // a part whose FPU is single precision. A float phase the table keeps
        // as the mirror of another is read backwards.
        const auto phase = filter_->phase(p);
        const Real* samples = history_.data() + (whole - taps - first_);
        const auto count = static_cast<std::size_t>(taps);
        Real sum{};
        if constexpr (std::is_same_v<Real, iclforge::internal::Fixed32>) {
            // Each product below 2^61, the history's values being audio, below 2^25; unsigned,
            // so that a stream that is not audio wraps rather than overflowing.
            std::uint64_t acc = 0;
            const std::int32_t* c = phase.coefficients;
            if (phase.reversed) {
                for (std::size_t k = 0; k < count; ++k) {
                    acc += static_cast<std::uint64_t>(static_cast<std::int64_t>(c[count - 1 - k]) * samples[k].raw);
                }
            } else {
                for (std::size_t k = 0; k < count; ++k) {
                    acc += static_cast<std::uint64_t>(static_cast<std::int64_t>(c[k]) * samples[k].raw);
                }
            }
            sum = Real::from_raw(static_cast<std::int32_t>(
                (static_cast<std::int64_t>(acc) + (std::int64_t{1} << 29U)) >> 30U));
        } else if constexpr (std::is_same_v<Real, float>) {
            sum = phase.reversed ? dot_four_lanes_reversed(phase.coefficients, samples, count)
                                 : dot_four_lanes(phase.coefficients, samples, count);
        } else {
            for (std::size_t k = 0; k < count; ++k) {
                sum += phase.coefficients[k] * samples[k];
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
