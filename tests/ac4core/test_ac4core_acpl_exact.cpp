// A-CPL's interpolation and decorrelators (src/ac4core/src/acpl/acpl.cpp) against verbatim copies
// of the code they replaced (planning/ac4.md, D14e): Pseudocode 109 evaluated at every subband and
// the decorrelators' coefficients narrowed at every tap. Both are held to the BITS of what they
// gave, at the decoder's scalar and at double, on parameters of every framing and band count and on
// signals with zeros of either sign, because a stream's PCM hash moves by one bit of either.
//
// test_ac4core_acpl.cpp holds the same code to the formulas it computes; this file holds the
// speed-ups to what the code did before them.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4core/acpl/acpl.hpp"
#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"

namespace {

namespace acpl = iclforge::ac4::detail::acpl;
namespace dsp = iclforge::ac4::detail::dsp;
using iclforge::ac4::detail::Real;

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

namespace reference {

// Pseudocode 109 at every slot and subband, one evaluation each, as acpl.cpp had it.
void interpolate(const acpl::Framing& framing, int num_param_bands, const acpl::ParamSets& values,
                 const acpl::ParamPrev& prev, int num_ts, std::span<double> out) {
    if (num_ts <= 0 || out.size() < at(num_ts) * acpl::kSubbands) {
        return;
    }
    const bool two = framing.num_param_sets == 2;
    const int ts_2 = num_ts / 2;
    for (int sb = 0; sb < acpl::kSubbands; ++sb) {
        const int pb = std::max(acpl::sb_to_pb(num_param_bands, sb), 0);
        const double p = prev[at(sb)];
        const double v0 = values[0][at(pb)];
        const double v1 = values[1][at(pb)];
        for (int ts = 0; ts < num_ts; ++ts) {
            double value = 0.0;
            if (!framing.steep) {
                if (!two) {
                    value = p + (ts + 1) * (v0 - p) / num_ts;
                } else if (ts < ts_2) {
                    value = p + (ts + 1) * (v0 - p) / ts_2;
                } else {
                    value = v0 + (ts - ts_2 + 1) * (v1 - v0) / (num_ts - ts_2);
                }
            } else if (ts < framing.param_timeslot[0]) {
                value = p;
            } else if (!two || ts < framing.param_timeslot[1]) {
                value = v0;
            } else {
                value = v1;
            }
            out[at(ts) * acpl::kSubbands + at(sb)] = value;
        }
    }
}

// Pseudocode 111 as acpl.cpp had it: the coefficients narrowed to the scalar at every tap.
template <typename Scalar>
class Decorrelator {
   public:
    using Complex = dsp::Complex<Scalar>;

    explicit Decorrelator(int index) : index_(std::clamp(index, 0, acpl::kDecorrelators - 1)) {}

    void reset() {
        x_history_.fill(Complex{});
        y_history_.fill(Complex{});
    }

    void process(std::span<const Complex> in, std::span<Complex> out, int num_ts) {
        const std::size_t n = at(num_ts);
        if (num_ts <= 0 || num_ts > acpl::kMaxSlots || in.size() < n * acpl::kSubbands ||
            out.size() < n * acpl::kSubbands) {
            return;
        }
        constexpr auto kIn = static_cast<std::size_t>(kInputHistory);
        constexpr auto kOut = static_cast<std::size_t>(kOutputHistory);
        std::array<Complex, kIn + static_cast<std::size_t>(acpl::kMaxSlots)> x{};
        std::array<Complex, kOut + static_cast<std::size_t>(acpl::kMaxSlots)> y{};
        for (int sb = 0; sb < acpl::kSubbands; ++sb) {
            const auto s = at(sb);
            const int region = acpl::region_of(sb);
            const std::span<const double> a = acpl::coefficients(index_, region);
            const auto delay = at(acpl::kRegions[at(region)].delay);
            const auto length = at(acpl::kRegions[at(region)].length);
            for (std::size_t k = 0; k < kIn; ++k) {
                x[k] = x_history_[k * acpl::kSubbands + s];
            }
            for (std::size_t k = 0; k < kOut; ++k) {
                y[k] = y_history_[k * acpl::kSubbands + s];
            }
            for (std::size_t ts = 0; ts < n; ++ts) {
                x[kIn + ts] = in[ts * acpl::kSubbands + s];
            }
            for (std::size_t ts = 0; ts < n; ++ts) {
                Complex acc = static_cast<Scalar>(a[length]) * x[kIn + ts - delay];
                for (std::size_t i = 1; i <= length; ++i) {
                    acc += static_cast<Scalar>(a[length - i]) * x[kIn + ts - i - delay] -
                           static_cast<Scalar>(a[i]) * y[kOut + ts - i];
                }
                y[kOut + ts] = acc / static_cast<Scalar>(a[0]);
                out[ts * acpl::kSubbands + s] = y[kOut + ts];
            }
            for (std::size_t k = 0; k < kIn; ++k) {
                x_history_[k * acpl::kSubbands + s] = x[n + k];
            }
            for (std::size_t k = 0; k < kOut; ++k) {
                y_history_[k * acpl::kSubbands + s] = y[n + k];
            }
        }
    }

   private:
    static constexpr int kInputHistory = 14;
    static constexpr int kOutputHistory = 7;

    int index_ = 0;
    std::array<Complex, static_cast<std::size_t>(kInputHistory) * acpl::kSubbands> x_history_{};
    std::array<Complex, static_cast<std::size_t>(kOutputHistory) * acpl::kSubbands> y_history_{};
};

}  // namespace reference

template <typename T>
[[nodiscard]] bool same_bits(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0;
}

// What a parameter set holds in practice (the dequantisation tables' values and sums of them), and
// what it can when a stream is corrupt or built to find a difference: zeros of either sign, equal
// neighbours, plain doubles.
class Source {
   public:
    explicit Source(unsigned seed) : rng_(seed) {}

    int below(int n) { return static_cast<int>(rng_() % static_cast<unsigned>(n)); }

    double value() {
        switch (below(6)) {
            case 0:
                return 0.0;
            case 1:
                return -0.0;
            case 2:
                return 0.0625 * static_cast<double>(below(65) - 32);
            case 3:
                return 0.190625 * static_cast<double>(below(21) - 10);
            case 4:
                return 4.0 * std::generate_canonical<double, 53>(rng_) - 2.0;
            default:
                return normal_(rng_);
        }
    }

    acpl::ParamSets sets() {
        acpl::ParamSets out{};
        for (auto& set : out) {
            for (double& v : set) {
                v = value();
            }
        }
        // Some bands as the set before, as a stream that does not move a band sends it.
        for (std::size_t pb = 0; pb < acpl::kMaxParamBands; ++pb) {
            if (below(4) == 0) {
                out[1][pb] = out[0][pb];
            }
        }
        return out;
    }

    template <typename Scalar>
    std::vector<dsp::Complex<Scalar>> signal(std::size_t count, double zero_fraction) {
        std::vector<dsp::Complex<Scalar>> out(count);
        std::bernoulli_distribution zero(zero_fraction);
        std::bernoulli_distribution negative(0.5);
        for (auto& v : out) {
            const auto part = [&] {
                if (zero(rng_)) {
                    return negative(rng_) ? Scalar(-0.0) : Scalar(0.0);
                }
                return static_cast<Scalar>(normal_(rng_));
            };
            const Scalar re = part();
            const Scalar im = part();
            v = dsp::Complex<Scalar>(re, im);
        }
        return out;
    }

    std::mt19937& engine() { return rng_; }

   private:
    std::mt19937 rng_;
    std::normal_distribution<double> normal_;
};

constexpr std::array<int, 4> kBandCounts = {15, 12, 9, 7};
constexpr std::array<int, 8> kSlotCounts = {1, 2, 7, 16, 24, 30, 31, 32};

}  // namespace

TEST_CASE("interpolate gives the bits of Pseudocode 109 evaluated at every subband",
          "[ac4core][acpl][exact]") {
    Source source(20261002U);
    for (const bool steep : {false, true}) {
        for (const int sets : {1, 2}) {
            for (const int bands : kBandCounts) {
                for (const int num_ts : kSlotCounts) {
                    for (int round = 0; round < 6; ++round) {
                        acpl::Framing framing{
                            .steep = steep, .num_param_sets = sets, .param_timeslot = {}};
                        framing.param_timeslot[0] = source.below(num_ts + 1);
                        framing.param_timeslot[1] =
                            std::min(num_ts, framing.param_timeslot[0] + source.below(num_ts + 1));
                        const acpl::ParamSets values = source.sets();
                        acpl::ParamPrev prev{};
                        switch (round % 3) {
                            case 0:  // the frame before's last set in each subband's band, as
                                     // end_frame() leaves it
                                acpl::end_frame(framing, bands, source.sets(), prev);
                                break;
                            case 1:  // the first frame
                                break;
                            default:  // not constant over a band: another band count's mapping, or
                                      // a corrupt state
                                for (double& p : prev) {
                                    p = source.value();
                                }
                                break;
                        }
                        std::vector<double> expected(at(num_ts) * acpl::kSubbands, -1.0);
                        std::vector<double> actual(at(num_ts) * acpl::kSubbands, -2.0);
                        reference::interpolate(framing, bands, values, prev, num_ts, expected);
                        acpl::interpolate(framing, bands, values, prev, num_ts, actual);
                        CAPTURE(steep, sets, bands, num_ts, round);
                        REQUIRE(same_bits(actual, expected));
                    }
                }
            }
        }
    }
}

TEST_CASE("interpolate follows a band count that changes between frames",
          "[ac4core][acpl][exact]") {
    // acpl_param_prev is per subband and a frame can change the band count: the previous frame's 15
    // bands' values stand in a new frame's 7, so a band's subbands start from different values.
    Source source(7U);
    for (int round = 0; round < 40; ++round) {
        const acpl::Framing framing{
            .steep = false, .num_param_sets = 1 + round % 2, .param_timeslot = {}};
        acpl::ParamPrev prev{};
        acpl::end_frame(framing, kBandCounts[at(round % 4)], source.sets(), prev);
        const int bands = kBandCounts[at((round + 1 + round / 4) % 4)];
        const int num_ts = kSlotCounts[at(round % 8)];
        const acpl::ParamSets values = source.sets();
        std::vector<double> expected(at(num_ts) * acpl::kSubbands, -1.0);
        std::vector<double> actual(at(num_ts) * acpl::kSubbands, -2.0);
        reference::interpolate(framing, bands, values, prev, num_ts, expected);
        acpl::interpolate(framing, bands, values, prev, num_ts, actual);
        CAPTURE(round, bands, num_ts);
        REQUIRE(same_bits(actual, expected));
    }
}

namespace {

template <typename Scalar>
void check_decorrelators() {
    Source source(31U);
    for (int index = 0; index < acpl::kDecorrelators; ++index) {
        acpl::Decorrelator<Scalar> decorrelator(index);
        reference::Decorrelator<Scalar> original(index);
        // Frames of different lengths, one after another, so that the histories carry; then a
        // reset.
        const std::array<int, 6> slots = {32, 32, 24, 32, 30, 16};
        for (std::size_t frame = 0; frame < slots.size(); ++frame) {
            if (frame == 4) {
                decorrelator.reset();
                original.reset();
            }
            const int num_ts = slots[frame];
            const auto in =
                source.signal<Scalar>(at(num_ts) * acpl::kSubbands, frame == 2 ? 0.5 : 0.05);
            std::vector<dsp::Complex<Scalar>> expected(in.size());
            std::vector<dsp::Complex<Scalar>> actual(in.size());
            original.process(in, expected, num_ts);
            decorrelator.process(in, actual, num_ts);
            CAPTURE(index, frame);
            REQUIRE(same_bits(actual, expected));
        }
    }
}

}  // namespace

TEST_CASE(
    "a decorrelator gives the bits of its coefficients narrowed at every tap at the decoder's "
    "scalar",
    "[ac4core][acpl][exact]") {
    // At Fixed32 the decorrelators sum their taps in 64 bits with Q1.30 coefficients. A generic lambda, so that a fixed build
    // does not instantiate the floating comparison.
    []<typename R>() {
        if constexpr (std::is_floating_point_v<R>) {
            check_decorrelators<R>();
        }
    }.template operator()<Real>();
}

TEST_CASE("a decorrelator gives the bits of its coefficients narrowed at every tap at double",
          "[ac4core][acpl][exact]") {
    check_decorrelators<double>();
}
