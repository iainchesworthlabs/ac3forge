// The transforms of src/ac4core/include/iclforge/ac4core/dsp against verbatim copies of the code
// they replaced (planning/ac4.md, D14e): the FFT passes, the inverse MDCT and the windowed
// overlap-add are held to the BITS of the plan's generic radix loop, the old inverse transform and
// the old block synthesis, at the decoder's scalar and at double, in both directions, on dense data
// and on spectra with runs of zeros of either sign. A sign of zero is a bit, and a stream whose
// output is silent for a while shows it in its PCM hash.
//
// test_ac4core_dsp.cpp holds the same code to the formulas it computes; this file holds the
// speed-ups to what the code did before them, so that a decoder's output moves by no bit.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <numbers>
#include <random>
#include <span>
#include <utility>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4core/aspx/hf_generator.hpp"
#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"
#include "iclforge/ac4core/dsp/fft.hpp"
#include "iclforge/ac4core/dsp/kbd.hpp"
#include "iclforge/ac4core/dsp/mdct.hpp"
#include "iclforge/ac4core/dsp/synthesis.hpp"
#include "iclforge/arithmetic/scalar_math.hpp"

namespace {

namespace dsp = iclforge::ac4::detail::dsp;
using iclforge::ac4::detail::Real;

// What the plan's loop was before the passes took the radix and the direction as arguments: the
// factorisation, the factors, the generic butterfly with its runtime radix and the Stockham loop,
// as src/ac4core/src/dsp/fft.cpp had them.
namespace reference {

std::vector<int> factor(std::size_t length, bool& ok) {
    std::vector<int> radices;
    ok = length > 0;
    if (!ok) {
        return radices;
    }
    for (const int radix : {4, 2, 3, 5}) {
        const auto r = static_cast<std::size_t>(radix);
        while (length % r == 0) {
            radices.push_back(radix);
            length /= r;
        }
    }
    ok = length == 1;
    if (!ok) {
        radices.clear();
    }
    return radices;
}

std::complex<double> root(std::size_t num, std::size_t den) {
    const double angle =
        -2.0 * std::numbers::pi * static_cast<double>(num % den) / static_cast<double>(den);
    return {std::cos(angle), std::sin(angle)};
}

template <typename Complex>
Complex times_minus_i(Complex z) {
    return {z.imag(), -z.real()};
}

template <typename Complex>
void butterfly(int radix, const std::array<Complex, 5>& a, std::array<Complex, 5>& b, bool inverse,
               const std::array<Complex, 5>& roots3, const std::array<Complex, 5>& roots5) {
    switch (radix) {
        case 2:
            b[0] = a[0] + a[1];
            b[1] = a[0] - a[1];
            return;
        case 4: {
            const Complex s02 = a[0] + a[2];
            const Complex d02 = a[0] - a[2];
            const Complex s13 = a[1] + a[3];
            const Complex d13 = inverse ? -times_minus_i(a[1] - a[3]) : times_minus_i(a[1] - a[3]);
            b[0] = s02 + s13;
            b[1] = d02 + d13;
            b[2] = s02 - s13;
            b[3] = d02 - d13;
            return;
        }
        default: {
            const auto& roots = radix == 3 ? roots3 : roots5;
            for (int k = 0; k < radix; ++k) {
                Complex sum = a[0];
                for (int i = 1; i < radix; ++i) {
                    const Complex w = roots[static_cast<std::size_t>((i * k) % radix)];
                    sum += a[static_cast<std::size_t>(i)] * (inverse ? conj(w) : w);
                }
                b[static_cast<std::size_t>(k)] = sum;
            }
            return;
        }
    }
}

template <typename Scalar>
class Fft {
   public:
    using Complex = dsp::Complex<Scalar>;

    explicit Fft(std::size_t length) : length_(length) {
        const std::vector<int> radices = factor(length, valid_);
        if (!valid_) {
            return;
        }
        std::size_t n = length;
        std::size_t stride = 1;
        for (const int radix : radices) {
            const auto r = static_cast<std::size_t>(radix);
            const std::size_t m = n / r;
            stages_.push_back(Stage{radix, n, stride, twiddles_.size()});
            for (std::size_t p = 0; p < m; ++p) {
                for (std::size_t k = 0; k < r; ++k) {
                    const std::complex<double> w = root(p * k, n);
                    twiddles_.emplace_back(static_cast<Scalar>(w.real()),
                                           static_cast<Scalar>(w.imag()));
                }
            }
            n = m;
            stride *= r;
        }
        for (std::size_t j = 0; j < 5; ++j) {
            const std::complex<double> w3 = root(j, 3);
            const std::complex<double> w5 = root(j, 5);
            roots3_[j] = Complex(static_cast<Scalar>(w3.real()), static_cast<Scalar>(w3.imag()));
            roots5_[j] = Complex(static_cast<Scalar>(w5.real()), static_cast<Scalar>(w5.imag()));
        }
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    void run(std::span<Complex> data, std::span<Complex> work, bool inverse) {
        Complex* x = data.data();
        Complex* y = work.data();
        for (const Stage& stage : stages_) {
            const auto r = static_cast<std::size_t>(stage.radix);
            const std::size_t m = stage.n / r;
            const std::size_t s = stage.stride;
            const Complex* tw = twiddles_.data() + stage.twiddle;
            std::array<Complex, 5> a{};
            std::array<Complex, 5> b{};
            for (std::size_t p = 0; p < m; ++p) {
                for (std::size_t q = 0; q < s; ++q) {
                    for (std::size_t i = 0; i < r; ++i) {
                        a[i] = x[q + s * (p + i * m)];
                    }
                    butterfly(stage.radix, a, b, inverse, roots3_, roots5_);
                    for (std::size_t k = 0; k < r; ++k) {
                        const Complex w = tw[p * r + k];
                        y[q + s * (r * p + k)] = b[k] * (inverse ? conj(w) : w);
                    }
                }
            }
            std::swap(x, y);
        }
        if (x != data.data()) {
            std::copy(x, x + static_cast<std::ptrdiff_t>(length_), data.data());
        }
    }

   private:
    struct Stage {
        int radix = 0;
        std::size_t n = 0;
        std::size_t stride = 0;
        std::size_t twiddle = 0;
    };

    std::size_t length_ = 0;
    bool valid_ = false;
    std::vector<Stage> stages_;
    std::vector<Complex> twiddles_;
    std::array<Complex, 5> roots3_{};
    std::array<Complex, 5> roots5_{};
};

template <typename Complex>
std::vector<Complex> pre_twiddles(std::size_t length) {
    using Scalar = typename Complex::value_type;
    std::vector<Complex> twiddle(length / 2);
    const double n16 = 16.0 * static_cast<double>(length);
    for (std::size_t k = 0; k < twiddle.size(); ++k) {
        const double angle = 2.0 * std::numbers::pi * static_cast<double>(8 * k + 1) / n16;
        twiddle[k] =
            Complex(static_cast<Scalar>(-std::cos(angle)), static_cast<Scalar>(-std::sin(angle)));
    }
    return twiddle;
}

// Imdct::inverse() as it was: the pre-twiddle, the plan's inverse transform, the post-twiddle and
// its 1/N, and Pseudocode 63's unfolding.
template <typename Scalar>
class Imdct {
   public:
    using Complex = dsp::Complex<Scalar>;

    explicit Imdct(std::size_t length)
        : length_(length), fft_(length / 2), twiddle_(pre_twiddles<Complex>(length)) {}

    void inverse(std::span<const Scalar> spectrum, std::span<Scalar> out,
                 std::span<Complex> scratch) {
        const std::size_t n = length_;
        const std::size_t half = n / 2;
        const std::size_t quarter = n / 4;
        const std::span<Complex> z = scratch.first(half);
        const std::span<Complex> work = scratch.subspan(half, half);
        for (std::size_t k = 0; k < half; ++k) {
            z[k] = Complex(spectrum[n - 2 * k - 1], spectrum[2 * k]) * twiddle_[k];
        }
        fft_.run(z, work, true);
        const Scalar scale = Scalar(1) / static_cast<Scalar>(n);
        for (std::size_t k = 0; k < half; ++k) {
            z[k] = z[k] * twiddle_[k] * scale;
        }
        const Complex* y = z.data();
        for (std::size_t m = 0; m < quarter; ++m) {
            out[2 * m] = y[quarter + m].imag();
            out[2 * m + 1] = -y[quarter - m - 1].real();
            out[half + 2 * m] = y[m].real();
            out[half + 2 * m + 1] = -y[half - m - 1].imag();
            out[n + 2 * m] = y[quarter + m].real();
            out[n + 2 * m + 1] = -y[quarter - m - 1].imag();
            out[n + half + 2 * m] = -y[m].imag();
            out[n + half + 2 * m + 1] = y[half - m - 1].real();
        }
    }

   private:
    std::size_t length_ = 0;
    Fft<Scalar> fft_;
    std::vector<Complex> twiddle_;
};

// ChannelSynthesis::block() as it was: the whole of a block's inverse transform into a scratch, the
// windows, the overlap-add and the shift of the overlap buffer. The windows are the library's own
// (kbd_left).
template <typename Scalar>
class ChannelSynthesis {
   public:
    using Complex = dsp::Complex<Scalar>;

    explicit ChannelSynthesis(int full_length)
        : full_length_(full_length),
          previous_length_(full_length),
          overlap_(static_cast<std::size_t>(full_length)),
          block_(2 * static_cast<std::size_t>(full_length)),
          transform_(static_cast<std::size_t>(full_length)) {}

    bool block(dsp::TransformSet<Scalar>& transforms, std::span<const Scalar> spectrum,
               std::span<Scalar> pcm) {
        const std::size_t n = spectrum.size();
        const auto n_int = static_cast<int>(n);
        Imdct<Scalar>& imdct = imdct_for(n);
        const auto n_prev = static_cast<std::size_t>(previous_length_);
        const std::size_t nw = std::min(n, n_prev);
        const std::span<const Scalar> kbd = transforms.kbd_left(static_cast<int>(nw));
        const auto full = static_cast<std::size_t>(full_length_);

        const std::span<Scalar> x = std::span<Scalar>(block_).first(2 * n);
        imdct.inverse(spectrum, x, std::span<Complex>(transform_).first(n));

        const std::size_t skip_left = (n - nw) / 2;
        std::fill_n(x.begin(), skip_left, Scalar(0));
        for (std::size_t i = 0; i < nw; ++i) {
            x[skip_left + i] *= kbd[i];
        }

        const std::size_t nskip = (full - n) / 2;
        const std::size_t nskip_prev = (full - n_prev) / 2;
        const std::size_t skip_right = (n_prev - nw) / 2;
        Scalar* previous = overlap_.data() + nskip_prev;
        for (std::size_t i = 0; i < nw; ++i) {
            previous[skip_right + i] *= kbd[nw - 1 - i];
        }
        std::fill(previous + skip_right + nw, previous + n_prev, Scalar(0));
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

   private:
    Imdct<Scalar>& imdct_for(std::size_t length) {
        auto& slot = imdcts_[length];
        if (!slot) {
            slot = std::make_unique<Imdct<Scalar>>(length);
        }
        return *slot;
    }

    int full_length_ = 0;
    int previous_length_ = 0;
    std::vector<Scalar> overlap_;
    std::vector<Scalar> block_;
    std::vector<Complex> transform_;
    std::map<std::size_t, std::unique_ptr<Imdct<Scalar>>> imdcts_;
};

}  // namespace reference

// Dense normal values, then a stretch of zeros of random sign where `zeros` says so: a spectrum
// above a coded bandwidth, a channel in silence.
template <typename Scalar>
std::vector<Scalar> values(std::size_t count, unsigned seed, double zero_fraction) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> normal;
    std::bernoulli_distribution zero(zero_fraction);
    std::bernoulli_distribution negative(0.5);
    std::vector<Scalar> out(count);
    for (Scalar& v : out) {
        v = zero(rng) ? (negative(rng) ? Scalar(-0.0) : Scalar(0.0))
                      : static_cast<Scalar>(normal(rng));
    }
    return out;
}

template <typename Scalar>
std::vector<Scalar> spectrum_with_zero_tail(std::size_t count, unsigned seed) {
    std::vector<Scalar> out = values<Scalar>(count, seed, 0.1);
    std::mt19937 rng(seed + 99);
    const std::size_t coded = count / 3 + rng() % (count / 2);
    std::bernoulli_distribution negative(0.5);
    for (std::size_t i = coded; i < count; ++i) {
        out[i] = negative(rng) ? Scalar(-0.0) : Scalar(0.0);
    }
    return out;
}

template <typename Scalar>
bool same_bits(std::span<const Scalar> a, std::span<const Scalar> b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Scalar)) == 0;
}

template <typename Scalar>
std::vector<dsp::Complex<Scalar>> complexes(std::size_t count, unsigned seed,
                                            double zero_fraction) {
    const std::vector<Scalar> re = values<Scalar>(count, seed, zero_fraction);
    const std::vector<Scalar> im = values<Scalar>(count, seed + 5, zero_fraction);
    std::vector<dsp::Complex<Scalar>> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = dsp::Complex<Scalar>{re[i], im[i]};
    }
    return out;
}

template <typename Scalar>
bool same_bits(const std::vector<dsp::Complex<Scalar>>& a,
               const std::vector<dsp::Complex<Scalar>>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(dsp::Complex<Scalar>)) == 0;
}

// Every transform length of clause 5.5.3 (the MDCT lengths N; an FFT of N / 2 runs for each).
constexpr std::array<int, 21> kBlockLengths = {2048, 1920, 1536, 1024, 960,  768,  512,
                                               480,  384,  256,  240,  192,  128,  120,
                                               96,   8192, 7680, 6144, 4096, 3840, 3072};

template <typename Scalar>
void fft_matches_reference() {
    // The lengths an MDCT of each block length runs, and others that put every radix, alone and in
    // combination, in first, middle and last position.
    std::vector<std::size_t> lengths = {1U,  2U,  3U,  4U,  5U,   6U,   8U,   9U,   10U,
                                        12U, 15U, 16U, 20U, 25U,  27U,  30U,  32U,  45U,
                                        48U, 60U, 64U, 75U, 100U, 125U, 128U, 240U, 360U};
    for (const int length : kBlockLengths) {
        lengths.push_back(static_cast<std::size_t>(length) / 2);
    }
    unsigned seed = 1;
    for (const std::size_t n : lengths) {
        CAPTURE(n);
        dsp::Fft<Scalar> fast(n);
        reference::Fft<Scalar> slow(n);
        REQUIRE(fast.valid());
        REQUIRE(slow.valid());
        for (const bool inverse : {false, true}) {
            CAPTURE(inverse);
            for (const double zeros : {0.0, 0.3, 0.9}) {
                CAPTURE(zeros);
                const std::vector<dsp::Complex<Scalar>> x = complexes<Scalar>(n, seed++, zeros);
                std::vector<dsp::Complex<Scalar>> a = x;
                std::vector<dsp::Complex<Scalar>> b = x;
                std::vector<dsp::Complex<Scalar>> work_a(n);
                std::vector<dsp::Complex<Scalar>> work_b(n);
                if (inverse) {
                    fast.inverse(a, work_a);
                } else {
                    fast.forward(a, work_a);
                }
                slow.run(b, work_b, inverse);
                CHECK(same_bits(a, b));
            }
        }
    }
}

template <typename Scalar>
void imdct_matches_reference() {
    unsigned seed = 100;
    for (const int length : kBlockLengths) {
        CAPTURE(length);
        const auto n = static_cast<std::size_t>(length);
        dsp::Imdct<Scalar> fast(n);
        reference::Imdct<Scalar> slow(n);
        REQUIRE(fast.valid());
        std::vector<dsp::Complex<Scalar>> scratch_fast(n);
        std::vector<dsp::Complex<Scalar>> scratch_slow(n);
        for (const bool zero_tail : {false, true}) {
            CAPTURE(zero_tail);
            const std::vector<Scalar> spectrum = zero_tail
                                                     ? spectrum_with_zero_tail<Scalar>(n, seed++)
                                                     : values<Scalar>(n, seed++, 0.0);
            std::vector<Scalar> a(2 * n);
            std::vector<Scalar> b(2 * n);
            fast.inverse(spectrum, a, scratch_fast);
            slow.inverse(spectrum, b, scratch_slow);
            CHECK(same_bits<Scalar>(a, b));
            // The form that works in a scratch of its own.
            std::vector<Scalar> c(2 * n);
            fast.inverse(spectrum, c);
            CHECK(same_bits<Scalar>(c, b));
        }
    }
}

// Block lengths of a frame: the full length, or a power of two of blocks of the full length over a
// power of two, as Table 187 has it. The sequences here are free of that table's rules, since the
// synthesis does the same sums whatever the order, and run the long block more often than the
// others so that the transitions into and out of it, and the runs of it, are all met.
template <typename Scalar>
void synthesis_matches_reference(int full, int rate_multiplier) {
    dsp::TransformSet<Scalar> transforms(full, rate_multiplier);
    if (!transforms.valid()) {
        return;
    }
    std::vector<int> lengths;
    for (int k = 0; k < 5 && full % (1 << k) == 0; ++k) {
        if (transforms.imdct(full >> k) != nullptr) {
            lengths.push_back(full >> k);
        }
    }
    dsp::ChannelSynthesis<Scalar> fast(full);
    reference::ChannelSynthesis<Scalar> slow(full);
    std::mt19937 rng(static_cast<unsigned>(full) * 31U + static_cast<unsigned>(rate_multiplier));
    for (int block = 0; block < 60; ++block) {
        const int length =
            (rng() % 3 != 0 || lengths.size() == 1) ? full : lengths[rng() % lengths.size()];
        const auto n = static_cast<std::size_t>(length);
        const std::vector<Scalar> spectrum =
            (block % 4 == 3)
                ? spectrum_with_zero_tail<Scalar>(n, static_cast<unsigned>(rng()))
                : values<Scalar>(n, static_cast<unsigned>(rng()), block % 7 == 0 ? 0.8 : 0.0);
        std::vector<Scalar> a(n, Scalar(7));
        std::vector<Scalar> b(n, Scalar(7));
        CAPTURE(block);
        CAPTURE(length);
        REQUIRE(fast.block(transforms, spectrum, a));
        REQUIRE(slow.block(transforms, spectrum, b));
        CHECK(same_bits<Scalar>(a, b));
    }
}

// Pseudocode 85's pre-flattening gains as they were before the cubic fit's vectors were kept
// between calls: src/ac4core/src/aspx/hf_generator.cpp's fit_cubic and preflattening_gains, with
// its two dB conversions, verbatim.
namespace reference_aspx {

constexpr double kTenOverLog2Of10 = 3.010299956639812;
constexpr double kLog2Of10Over20 = 0.16609640474436812;

template <typename Scalar>
Scalar power_db(Scalar x) {
    if constexpr (std::is_same_v<Scalar, double>) {
        return Scalar{10} * std::log10(x);
    } else {
        return static_cast<Scalar>(kTenOverLog2Of10) * iclforge::internal::scalar_log2(x);
    }
}

template <typename Scalar>
Scalar from_power_db(Scalar db) {
    if constexpr (std::is_same_v<Scalar, double>) {
        return std::pow(Scalar{10}, db / Scalar{20});
    } else {
        return iclforge::internal::scalar_exp2(static_cast<Scalar>(kLog2Of10Over20) * db);
    }
}

template <typename Scalar>
void fit_cubic(std::span<const Scalar> y, std::span<Scalar> fitted) {
    const std::size_t n = y.size();
    std::array<std::vector<double>, 4> basis;
    for (std::size_t k = 0; k < basis.size(); ++k) {
        basis[k].resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = n > 1 ? (2.0 * static_cast<double>(i) - static_cast<double>(n - 1)) /
                                         static_cast<double>(n - 1)
                                   : 0.0;
            basis[k][i] = std::pow(t, static_cast<double>(k));
        }
    }
    std::ranges::fill(fitted, Scalar{});
    for (std::size_t k = 0; k < basis.size(); ++k) {
        for (std::size_t m = 0; m < k; ++m) {
            double dot = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                dot += basis[k][i] * basis[m][i];
            }
            for (std::size_t i = 0; i < n; ++i) {
                basis[k][i] -= dot * basis[m][i];
            }
        }
        double norm = 0.0;
        for (const double v : basis[k]) {
            norm += v * v;
        }
        if (norm < 1e-18) {
            std::ranges::fill(basis[k], 0.0);
            continue;
        }
        norm = std::sqrt(norm);
        double projection = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            basis[k][i] /= norm;
            projection += basis[k][i] * static_cast<double>(y[i]);
        }
        for (std::size_t i = 0; i < n; ++i) {
            fitted[i] += static_cast<Scalar>(projection * basis[k][i]);
        }
    }
}

template <typename Scalar>
void preflattening_gains(std::span<const dsp::Complex<Scalar>> q_low, int sbx, int ts_begin,
                         int ts_end, std::span<Scalar> gain_vec) {
    const auto n = static_cast<std::size_t>(sbx);
    if (ts_end <= ts_begin || n == 0) {
        std::ranges::fill(gain_vec.first(n), Scalar{1});
        return;
    }
    std::vector<Scalar> pow_env(n);
    Scalar mean_energy{};
    for (std::size_t sb = 0; sb < n; ++sb) {
        Scalar energy{};
        for (int ts = ts_begin; ts < ts_end; ++ts) {
            energy += norm(q_low[static_cast<std::size_t>(ts) * 64 + sb]);
        }
        energy /= static_cast<Scalar>(ts_end - ts_begin);
        pow_env[sb] = power_db(energy + Scalar{1});
        mean_energy += pow_env[sb];
    }
    mean_energy /= static_cast<Scalar>(n);
    std::vector<Scalar> slope(n);
    fit_cubic<Scalar>(pow_env, slope);
    for (std::size_t sb = 0; sb < n; ++sb) {
        gain_vec[sb] = from_power_db(mean_energy - slope[sb]);
    }
}

}  // namespace reference_aspx

template <typename Scalar>
void preflattening_matches_reference() {
    namespace aspx = iclforge::ac4::detail::aspx;
    std::mt19937 rng(7);
    std::normal_distribution<double> normal;
    // One channel's state across frames whose crossover moves: the cache is for the last count and
    // is made again for a new one.
    aspx::CubicBasis cubic;
    std::vector<int> sbx_sequence;
    for (int sbx = 1; sbx <= 63; ++sbx) {
        sbx_sequence.push_back(sbx);
        sbx_sequence.push_back(sbx);  // the same count again: the vectors kept
    }
    for (int sbx = 63; sbx >= 1; sbx -= 7) {
        sbx_sequence.push_back(sbx);
    }
    for (const int sbx : sbx_sequence) {
        CAPTURE(sbx);
        const int slots = 8 + static_cast<int>(rng() % 24);
        std::vector<dsp::Complex<Scalar>> q_low(static_cast<std::size_t>(slots) * 64);
        for (auto& v : q_low) {
            v = dsp::Complex<Scalar>{static_cast<Scalar>(normal(rng) * 100.0),
                                     static_cast<Scalar>(normal(rng) * 100.0)};
        }
        std::array<Scalar, 64> kept{};
        std::array<Scalar, 64> fresh{};
        std::array<Scalar, 64> before{};
        aspx::preflattening_gains<Scalar>(q_low, sbx, 0, slots, kept, cubic);
        aspx::preflattening_gains<Scalar>(q_low, sbx, 0, slots, fresh);
        reference_aspx::preflattening_gains<Scalar>(q_low, sbx, 0, slots, before);
        CHECK(same_bits<Scalar>(
            std::span<const Scalar>(kept).first(static_cast<std::size_t>(sbx)),
            std::span<const Scalar>(before).first(static_cast<std::size_t>(sbx))));
        CHECK(same_bits<Scalar>(
            std::span<const Scalar>(fresh).first(static_cast<std::size_t>(sbx)),
            std::span<const Scalar>(before).first(static_cast<std::size_t>(sbx))));
    }
}
}  // namespace

TEST_CASE("the FFT passes give the bits of the generic radix loop at the decoder's scalar",
          "[ac4core][dsp][exact]") {
    // These rewrites are identities of IEEE rounding; Fixed32 rounds a product half up,
    // so a negated product and the product of a negation differ by a raw unit there, and the
    // fixed decoder runs Fft::inverse_scaled. A generic lambda, so that a fixed build
    // does not instantiate the floating comparison.
    []<typename R>() {
        if constexpr (std::is_floating_point_v<R>) {
            fft_matches_reference<R>();
        }
    }.template operator()<Real>();
}

TEST_CASE("the FFT passes give the bits of the generic radix loop at double",
          "[ac4core][dsp][exact]") {
    fft_matches_reference<double>();
}

TEST_CASE("the inverse MDCT gives the bits of the old inverse transform at the decoder's scalar",
          "[ac4core][dsp][exact]") {
    // As the FFT's: the fixed decoder runs the inverse that carries a block's exponent. A generic lambda, so that a fixed build
    // does not instantiate the floating comparison.
    []<typename R>() {
        if constexpr (std::is_floating_point_v<R>) {
            imdct_matches_reference<R>();
        }
    }.template operator()<Real>();
}

TEST_CASE("the inverse MDCT gives the bits of the old inverse transform at double",
          "[ac4core][dsp][exact]") {
    imdct_matches_reference<double>();
}

TEST_CASE(
    "the windowed overlap-add gives the bits of the old block synthesis at the decoder's scalar",
    "[ac4core][dsp][exact]") {
    for (const auto& [full, multiplier] :
         {std::pair{2048, 1}, std::pair{1920, 1}, std::pair{1536, 1}, std::pair{1024, 1},
          std::pair{960, 1}, std::pair{4096, 2}, std::pair{3840, 2}, std::pair{8192, 4}}) {
        CAPTURE(full);
        CAPTURE(multiplier);
        // At Fixed32 every block takes the inverse that carries the block's exponent and no
        // fused path (dsp/synthesis.cpp); the fixed tier's agreement with double is held by
        // tools/checks/check_ac4_decode_scalar_snr.py. A generic lambda, so that a fixed build
        // does not instantiate the floating comparison.
        [&]<typename R>() {
            if constexpr (std::is_floating_point_v<R>) {
                synthesis_matches_reference<R>(full, multiplier);
            }
        }.template operator()<Real>();
    }
}

TEST_CASE("the windowed overlap-add gives the bits of the old block synthesis at double",
          "[ac4core][dsp][exact]") {
    for (const auto& [full, multiplier] :
         {std::pair{2048, 1}, std::pair{1920, 1}, std::pair{1536, 1}, std::pair{4096, 2}}) {
        CAPTURE(full);
        CAPTURE(multiplier);
        synthesis_matches_reference<double>(full, multiplier);
    }
}

TEST_CASE(
    "pre-flattening's gains keep the bits of the fit made afresh each frame at the decoder's "
    "scalar",
    "[ac4core][aspx][exact]") {
    // At Fixed32 the gains are MantExp values, which this floating reference does not form.
    []<typename R>() {
        if constexpr (std::is_floating_point_v<R>) {
            preflattening_matches_reference<R>();
        }
    }.template operator()<Real>();
}

TEST_CASE("pre-flattening's gains keep the bits of the fit made afresh each frame at double",
          "[ac4core][aspx][exact]") {
    preflattening_matches_reference<double>();
}
