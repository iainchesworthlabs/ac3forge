// The AC-4 shared core's transforms (src/ac4core/include/iclforge/ac4core/dsp), each against a
// direct evaluation of the formula it computes: the FFT against the DFT, the
// inverse MDCT against a verbatim transcription of ETSI TS 103 190-1 V1.4.1
// Pseudocodes 60 to 63 and against the cosine sum they come to, the forward
// MDCT against its own sum, and the KBD windows against values computed with
// numpy's Kaiser window (a Bessel function written by others). Then the
// synthesis's windows and overlap-add, fed by an analysis written here from
// the same windows, reconstruct their input across every block transition
// Part 1 Table 187 allows. The QMF analysis and synthesis banks are held to
// Pseudocodes 65 and 66 as printed, and the pair to its delay and
// reconstruction.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstring>
#include <limits>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"
#include "iclforge/ac4core/dsp/fft.hpp"
#include "iclforge/ac4core/dsp/kbd.hpp"
#include "iclforge/ac4core/dsp/mdct.hpp"
#include "iclforge/ac4core/dsp/qmf.hpp"
#include "iclforge/ac4core/dsp/scalar_traits.hpp"
#include "iclforge/ac4core/dsp/qmf_constants.hpp"
#include "iclforge/ac4core/dsp/qmf_kernels.hpp"
#include "iclforge/ac4core/dsp/qmf_vector.hpp"
#include "iclforge/ac4core/dsp/synthesis.hpp"
#include "iclforge/ac4core/tables/qmf_tables.hpp"

namespace {

namespace dsp = iclforge::ac4::detail::dsp;
// ac4core's own complex type (dsp/complex.hpp), not std::complex: every
// function under test takes this type since D14a (planning/ac4.md).
using Complex = dsp::Complex<double>;

// Every transform length of clause 5.5.3: the fifteen at 44.1 and 48 kHz, and
// the ones only 96 and 192 kHz add.
constexpr std::array<int, 15> kLengths48 = {2048, 1920, 1536, 1024, 960, 768, 512, 480,
                                            384,  256,  240,  192,  128, 120, 96};
constexpr std::array<int, 6> kLengthsHigh = {8192, 7680, 6144, 4096, 3840, 3072};

std::vector<double> random_values(std::size_t count, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> normal;
    std::vector<double> values(count);
    for (double& v : values) {
        v = normal(rng);
    }
    return values;
}

double max_abs(std::span<const double> values) {
    double peak = 0.0;
    for (const double v : values) {
        peak = std::max(peak, std::abs(v));
    }
    return peak;
}

double max_abs_difference(std::span<const double> a, std::span<const double> b) {
    double peak = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        peak = std::max(peak, std::abs(a[i] - b[i]));
    }
    return peak;
}

// The DFT by its definition, sign -1 forward and +1 inverse, unscaled.
std::vector<Complex> dft(std::span<const Complex> x, int sign) {
    const std::size_t n = x.size();
    std::vector<Complex> out(n);
    for (std::size_t k = 0; k < n; ++k) {
        Complex sum{};
        for (std::size_t j = 0; j < n; ++j) {
            const double angle = static_cast<double>(sign) * 2.0 * std::numbers::pi *
                                 static_cast<double>((j * k) % n) / static_cast<double>(n);
            sum += x[j] * Complex(std::cos(angle), std::sin(angle));
        }
        out[k] = sum;
    }
    return out;
}

// Pseudocodes 60 to 63 as printed, with Pseudocode 61's direct sum and no
// window (w[n] = 1).
std::vector<double> imdct_pseudocode(std::span<const double> X) {
    const std::size_t n = X.size();
    const std::size_t half = n / 2;
    const std::size_t quarter = n / 4;
    const auto big_n = static_cast<double>(n);
    std::vector<double> xcos1(half);
    std::vector<double> xsin1(half);
    for (std::size_t k = 0; k < half; ++k) {
        xcos1[k] = -std::cos(2.0 * std::numbers::pi * static_cast<double>(8 * k + 1) / (16.0 * big_n));
        xsin1[k] = -std::sin(2.0 * std::numbers::pi * static_cast<double>(8 * k + 1) / (16.0 * big_n));
    }
    std::vector<double> zr(half);
    std::vector<double> zi(half);
    for (std::size_t k = 0; k < half; ++k) {
        zr[k] = X[n - 2 * k - 1] * xcos1[k] - X[2 * k] * xsin1[k];
        zi[k] = X[2 * k] * xcos1[k] + X[n - 2 * k - 1] * xsin1[k];
    }
    std::vector<double> z_re(half);
    std::vector<double> z_im(half);
    for (std::size_t m = 0; m < half; ++m) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t k = 0; k < half; ++k) {
            const double angle = 4.0 * std::numbers::pi * static_cast<double>((k * m) % half) / big_n;
            const double c = std::cos(angle);
            const double s = std::sin(angle);
            re += zr[k] * c - zi[k] * s;
            im += zr[k] * s + zi[k] * c;
        }
        z_re[m] = re;
        z_im[m] = im;
    }
    std::vector<double> yr(half);
    std::vector<double> yi(half);
    for (std::size_t m = 0; m < half; ++m) {
        yr[m] = (z_re[m] * xcos1[m] - z_im[m] * xsin1[m]) / big_n;
        yi[m] = (z_im[m] * xcos1[m] + z_re[m] * xsin1[m]) / big_n;
    }
    std::vector<double> x(2 * n);
    for (std::size_t m = 0; m < quarter; ++m) {
        x[2 * m] = yi[quarter + m];
        x[2 * m + 1] = -yr[quarter - m - 1];
        x[half + 2 * m] = yr[m];
        x[half + 2 * m + 1] = -yi[half - m - 1];
        x[n + 2 * m] = yr[quarter + m];
        x[n + 2 * m + 1] = -yi[quarter - m - 1];
        x[n + half + 2 * m] = -yi[m];
        x[n + half + 2 * m + 1] = yr[half - m - 1];
    }
    return x;
}

// cos(pi/N (n + 1/2 + N/2)(k + 1/2)), the kernel both directions share,
// with the product reduced modulo 4N so the angle stays small.
double kernel(std::size_t n, std::size_t k, std::size_t big_n) {
    // (2n + 1 + N)(2k + 1) / 4, in units of pi/N: take the product in quarter
    // units of pi/N and reduce it by 8N quarter units (2 pi).
    const std::size_t quarters = ((2 * n + 1 + big_n) * (2 * k + 1)) % (8 * big_n);
    return std::cos(std::numbers::pi * static_cast<double>(quarters) / (4.0 * static_cast<double>(big_n)));
}

// x[n] = (1/N) sum_k X[k] kernel(n, k) at the output samples `at`.
std::vector<double> imdct_sum(std::span<const double> X, std::span<const std::size_t> at) {
    const std::size_t n = X.size();
    std::vector<double> x(at.size());
    for (std::size_t i = 0; i < at.size(); ++i) {
        double sum = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            sum += X[k] * kernel(at[i], k, n);
        }
        x[i] = sum / static_cast<double>(n);
    }
    return x;
}

// X[k] = sum_n x[n] kernel(n, k) at the lines `at`.
std::vector<double> mdct_sum(std::span<const double> x, std::span<const std::size_t> at) {
    const std::size_t n = x.size() / 2;
    std::vector<double> X(at.size());
    for (std::size_t i = 0; i < at.size(); ++i) {
        double sum = 0.0;
        for (std::size_t j = 0; j < 2 * n; ++j) {
            sum += x[j] * kernel(j, at[i], n);
        }
        X[i] = sum;
    }
    return X;
}

std::vector<std::size_t> all_indices(std::size_t count) {
    std::vector<std::size_t> at(count);
    for (std::size_t i = 0; i < count; ++i) {
        at[i] = i;
    }
    return at;
}

// A spread of indices through [0, count), for the lengths where a full direct
// sum would take too long: both ends and every step between.
std::vector<std::size_t> some_indices(std::size_t count) {
    std::vector<std::size_t> at;
    const std::size_t step = std::max<std::size_t>(1, count / 61);
    for (std::size_t i = 0; i < count; i += step) {
        at.push_back(i);
    }
    at.push_back(count - 1);
    return at;
}

}  // namespace

TEST_CASE("the FFT equals the DFT at every 2, 3 and 5 smooth length it is given", "[ac4core][dsp]") {
    for (const std::size_t n : {1U, 2U, 3U, 4U, 5U, 6U, 8U, 9U, 10U, 12U, 15U, 16U, 25U, 27U, 30U, 45U, 48U, 60U,
                                64U, 96U, 120U, 125U, 240U, 384U, 480U, 512U, 960U, 1024U}) {
        CAPTURE(n);
        dsp::Fft<double> fft(n);
        REQUIRE(fft.valid());
        const std::vector<double> re = random_values(n, static_cast<unsigned>(n));
        const std::vector<double> im = random_values(n, static_cast<unsigned>(n) + 7);
        std::vector<Complex> x(n);
        for (std::size_t i = 0; i < n; ++i) {
            x[i] = Complex(re[i], im[i]);
        }
        for (const int sign : {-1, 1}) {
            std::vector<Complex> fast = x;
            if (sign < 0) {
                fft.forward(fast);
            } else {
                fft.inverse(fast);
            }
            const std::vector<Complex> slow = dft(x, sign);
            double error = 0.0;
            double scale = 0.0;
            for (std::size_t k = 0; k < n; ++k) {
                error = std::max(error, abs(fast[k] - slow[k]));
                scale = std::max(scale, abs(slow[k]));
            }
            CHECK(error <= 1e-12 * scale);
        }
    }
}

TEST_CASE("the FFT refuses a length with a prime factor above 5", "[ac4core][dsp]") {
    for (const std::size_t n : {0U, 7U, 14U, 22U, 2048U * 7U}) {
        CAPTURE(n);
        dsp::Fft<double> fft(n);
        CHECK_FALSE(fft.valid());
    }
}

TEST_CASE("the inverse MDCT equals Pseudocodes 60 to 63 and their cosine sum at every length", "[ac4core][dsp]") {
    for (const int length : kLengths48) {
        CAPTURE(length);
        const auto n = static_cast<std::size_t>(length);
        dsp::Imdct<double> imdct(n);
        REQUIRE(imdct.valid());
        const std::vector<double> X = random_values(n, static_cast<unsigned>(length));
        std::vector<double> fast(2 * n);
        imdct.inverse(X, fast);
        const std::vector<double> printed = imdct_pseudocode(X);
        const std::vector<double> summed = imdct_sum(X, all_indices(2 * n));
        CHECK(max_abs_difference(fast, printed) <= 1e-12 * max_abs(printed));
        CHECK(max_abs_difference(fast, summed) <= 1e-12 * max_abs(summed));
    }
    for (const int length : kLengthsHigh) {
        CAPTURE(length);
        const auto n = static_cast<std::size_t>(length);
        dsp::Imdct<double> imdct(n);
        REQUIRE(imdct.valid());
        const std::vector<double> X = random_values(n, static_cast<unsigned>(length));
        std::vector<double> fast(2 * n);
        imdct.inverse(X, fast);
        const std::vector<std::size_t> at = some_indices(2 * n);
        const std::vector<double> summed = imdct_sum(X, at);
        std::vector<double> picked(at.size());
        for (std::size_t i = 0; i < at.size(); ++i) {
            picked[i] = fast[at[i]];
        }
        CHECK(max_abs_difference(picked, summed) <= 1e-12 * max_abs(fast));
    }
}

TEST_CASE("the forward MDCT equals its cosine sum at every length", "[ac4core][dsp]") {
    for (const int length : kLengths48) {
        CAPTURE(length);
        const auto n = static_cast<std::size_t>(length);
        dsp::Mdct<double> mdct(n);
        REQUIRE(mdct.valid());
        const std::vector<double> x = random_values(2 * n, static_cast<unsigned>(length) + 1);
        std::vector<double> fast(n);
        mdct.forward(x, fast);
        const std::vector<double> summed = mdct_sum(x, all_indices(n));
        CHECK(max_abs_difference(fast, summed) <= 1e-12 * max_abs(summed));
    }
    for (const int length : kLengthsHigh) {
        CAPTURE(length);
        const auto n = static_cast<std::size_t>(length);
        dsp::Mdct<double> mdct(n);
        REQUIRE(mdct.valid());
        const std::vector<double> x = random_values(2 * n, static_cast<unsigned>(length) + 1);
        std::vector<double> fast(n);
        mdct.forward(x, fast);
        const std::vector<std::size_t> at = some_indices(n);
        const std::vector<double> summed = mdct_sum(x, at);
        std::vector<double> picked(at.size());
        for (std::size_t i = 0; i < at.size(); ++i) {
            picked[i] = fast[at[i]];
        }
        CHECK(max_abs_difference(picked, summed) <= 1e-12 * max_abs(fast));
    }
}

TEST_CASE("Table 186 gives each transform length its KBD alpha", "[ac4core][dsp]") {
    CHECK(dsp::kbd_alpha(2048, 1) == 3.0);
    CHECK(dsp::kbd_alpha(1920, 1) == 3.0);
    CHECK(dsp::kbd_alpha(1536, 1) == 3.0);
    CHECK(dsp::kbd_alpha(960, 1) == 4.0);
    CHECK(dsp::kbd_alpha(384, 1) == 4.5);
    CHECK(dsp::kbd_alpha(256, 1) == 5.0);
    CHECK(dsp::kbd_alpha(96, 1) == 6.0);
    // The same lengths twice and four times over at 96 and 192 kHz.
    CHECK(dsp::kbd_alpha(4096, 2) == 3.0);
    CHECK(dsp::kbd_alpha(1024, 2) == 4.5);
    CHECK(dsp::kbd_alpha(192, 2) == 6.0);
    CHECK(dsp::kbd_alpha(8192, 4) == 3.0);
    CHECK(dsp::kbd_alpha(384, 4) == 6.0);
    // What the table does not list.
    CHECK(dsp::kbd_alpha(64, 1) == 0.0);
    CHECK(dsp::kbd_alpha(4096, 1) == 0.0);
    CHECK(dsp::kbd_alpha(96, 2) == 0.0);
    CHECK(dsp::kbd_alpha(2048, 3) == 0.0);
}

TEST_CASE("the KBD windows match an independent Kaiser window and meet Princen-Bradley", "[ac4core][dsp]") {
    struct Reference {
        int length;
        double alpha;
        std::size_t n;
        double value;
    };
    // numpy.kaiser(N + 1, pi * alpha), cumulated: sqrt(cumsum[n] / cumsum[N]).
    constexpr std::array<Reference, 14> kReferences{{
        {128, 6.0, 0, 4.379570409412748e-05},
        {128, 6.0, 31, 0.10309941483448865},
        {128, 6.0, 64, 0.7166758128747093},
        {128, 6.0, 127, 0.9999999990409681},
        {2048, 3.0, 0, 0.0008618285876066876},
        {2048, 3.0, 1000, 0.6866712047717016},
        {2048, 3.0, 2047, 0.9999996286256738},
        {960, 4.0, 100, 0.02766134484545758},
        {960, 4.0, 480, 0.7081585351193496},
        {480, 4.5, 7, 0.0010784390961399977},
        {480, 4.5, 300, 0.9124866197994359},
        {240, 5.0, 1, 0.000255824975544268},
        {240, 5.0, 200, 0.9990018936532645},
        {3840, 3.0, 17, 0.002936881849277194},
    }};
    for (const Reference& reference : kReferences) {
        CAPTURE(reference.length, reference.alpha, reference.n);
        const std::vector<double> window = dsp::kbd_left(reference.length, reference.alpha);
        REQUIRE(window.size() == static_cast<std::size_t>(reference.length));
        CHECK(std::abs(window[reference.n] - reference.value) <= 1e-12 * reference.value);
    }
    for (const int length : kLengths48) {
        CAPTURE(length);
        const std::vector<double> window = dsp::kbd_left(length, dsp::kbd_alpha(length, 1));
        const auto n = window.size();
        for (std::size_t i = 0; i < n; ++i) {
            // KBD_LEFT(N, n)^2 + KBD_RIGHT(N, N + n)^2, the right half being
            // the left reversed.
            CHECK(std::abs(window[i] * window[i] + window[n - 1 - i] * window[n - 1 - i] - 1.0) <= 1e-14);
        }
    }
}

TEST_CASE("the I0 series converges to the Bessel function", "[ac4core][dsp]") {
    // I0(0) = 1; the others from the series in closed-form tables
    // (Abramowitz and Stegun Table 9.8 gives e^-x I0(x)).
    CHECK(dsp::bessel_i0(0.0) == 1.0);
    CHECK(std::abs(dsp::bessel_i0(1.0) - 1.2660658777520084) <= 1e-15);
    CHECK(std::abs(dsp::bessel_i0(10.0) / 2815.716628466254 - 1.0) <= 1e-14);
}

namespace {

// The window an analysis applies to a block of `n` samples whose neighbours
// are `before` and `after` long: Pseudocode 63's left window over the first
// half, and over the second half the right window Pseudocode 64 gives the
// block when the next one is `after` long.
std::vector<double> analysis_window(dsp::TransformSet<double>& set, int before, int n, int after) {
    const auto size = static_cast<std::size_t>(n);
    std::vector<double> window(2 * size, 0.0);
    const auto nw_left = static_cast<std::size_t>(std::min(n, before));
    const std::span<const double> left = set.kbd_left(static_cast<int>(nw_left));
    const std::size_t skip_left = (size - nw_left) / 2;
    for (std::size_t i = 0; i < size; ++i) {
        if (i < skip_left) {
            window[i] = 0.0;
        } else if (i < skip_left + nw_left) {
            window[i] = left[i - skip_left];
        } else {
            window[i] = 1.0;
        }
    }
    const auto nw_right = static_cast<std::size_t>(std::min(n, after));
    const std::span<const double> right = set.kbd_left(static_cast<int>(nw_right));
    const std::size_t skip_right = (size - nw_right) / 2;
    for (std::size_t i = 0; i < size; ++i) {
        double w = 0.0;
        if (i < skip_right) {
            w = 1.0;
        } else if (i < skip_right + nw_right) {
            w = right[nw_right - 1 - (i - skip_right)];
        }
        window[size + i] = w;
    }
    return window;
}

// Table 187's partitions of one frame of `full` samples, as divisors of it.
std::vector<std::vector<int>> table_187(int full) {
    const int h = full / 2;
    const int q = full / 4;
    const int e = full / 8;
    const int s = full / 16;
    auto repeat = [](int count, int length) { return std::vector<int>(static_cast<std::size_t>(count), length); };
    auto join = [](std::vector<int> a, const std::vector<int>& b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    };
    return {
        {full},
        {h, h},
        join({h}, repeat(2, q)),
        join(repeat(2, q), {h}),
        join({h}, repeat(4, e)),
        join(repeat(4, e), {h}),
        join({h}, repeat(8, s)),
        join(repeat(8, s), {h}),
        repeat(4, q),
        join(repeat(2, q), repeat(4, e)),
        join(repeat(4, e), repeat(2, q)),
        join(repeat(2, q), repeat(8, s)),
        join(repeat(8, s), repeat(2, q)),
        repeat(8, e),
        join(repeat(4, e), repeat(8, s)),
        join(repeat(8, s), repeat(4, e)),
        repeat(16, s),
    };
}

}  // namespace

TEST_CASE("windowed blocks reconstruct their input across every Table 187 transition", "[ac4core][dsp]") {
    for (const int full : {2048, 1920, 1536}) {
        CAPTURE(full);
        dsp::TransformSet<double> set(full, 1);
        REQUIRE(set.valid());
        const std::vector<std::vector<int>> partitions = table_187(full);
        // Every partition, and every partition after every other one, so that
        // each transition within a frame and across frames is met.
        std::vector<int> blocks;
        for (const auto& first : partitions) {
            for (const auto& second : partitions) {
                blocks.insert(blocks.end(), first.begin(), first.end());
                blocks.insert(blocks.end(), second.begin(), second.end());
            }
        }
        std::size_t total = 0;
        for (const int length : blocks) {
            total += static_cast<std::size_t>(length);
        }
        const auto frame = static_cast<std::size_t>(full);
        // The analysis reads each block's 2N samples from where the synthesis
        // puts them: (full - N) / 2 into the frame-long stretch its output
        // starts, which is the sum of the blocks before it.
        const std::vector<double> signal = random_values(total + 2 * frame, 42);
        dsp::ChannelSynthesis<double> synthesis(full);
        std::vector<double> output(total, 0.0);
        std::size_t start = 0;
        for (std::size_t j = 0; j < blocks.size(); ++j) {
            const int n = blocks[j];
            const auto size = static_cast<std::size_t>(n);
            const int before = j == 0 ? full : blocks[j - 1];
            const int after = j + 1 < blocks.size() ? blocks[j + 1] : full;
            const std::vector<double> window = analysis_window(set, before, n, after);
            const std::size_t at = start + (frame - size) / 2;
            std::vector<double> segment(2 * size);
            for (std::size_t i = 0; i < 2 * size; ++i) {
                segment[i] = signal[at + i] * window[i];
            }
            dsp::Mdct<double> mdct(size);
            std::vector<double> spectrum(size);
            mdct.forward(segment, spectrum);
            // Through the literal inverse transform the round trip has a gain
            // of 1/2, so the analysis doubles.
            for (double& line : spectrum) {
                line *= 2.0;
            }
            REQUIRE(synthesis.block(set, spectrum, std::span<double>(output).subspan(start, size)));
            start += size;
        }
        // Output sample t is input sample t once every block that overlaps it
        // has been analysed from the signal: from the end of the first frame.
        double error = 0.0;
        for (std::size_t t = frame; t < total; ++t) {
            error = std::max(error, std::abs(output[t] - signal[t]));
        }
        CHECK(error <= 1e-12 * max_abs(signal));
    }
}

TEST_CASE("the transforms leave their output alone when given the wrong sizes", "[ac4core][dsp]") {
    dsp::Fft<double> fft(8);
    std::vector<Complex> short_data(4, Complex(1.0, 0.0));
    fft.forward(short_data);
    CHECK(short_data == std::vector<Complex>(4, Complex(1.0, 0.0)));

    // A length that is not a multiple of 4 has no MDCT, whatever its FFT.
    CHECK_FALSE(dsp::Imdct<double>(6).valid());
    CHECK_FALSE(dsp::Mdct<double>(6).valid());
    CHECK_FALSE(dsp::Imdct<double>(28).valid());  // 14 has a factor of 7

    dsp::Imdct<double> imdct(16);
    REQUIRE(imdct.valid());
    const std::vector<double> lines(16, 1.0);
    std::vector<double> wrong(16, -1.0);  // should be 32
    imdct.inverse(lines, wrong);
    CHECK(wrong == std::vector<double>(16, -1.0));

    dsp::Mdct<double> mdct(16);
    REQUIRE(mdct.valid());
    const std::vector<double> samples(16, 1.0);  // should be 32
    std::vector<double> spectrum(16, -1.0);
    mdct.forward(samples, spectrum);
    CHECK(spectrum == std::vector<double>(16, -1.0));

    CHECK(dsp::kbd_left(0, 4.0).empty());
    CHECK(dsp::kbd_alpha(0, 1) == 0.0);
    CHECK(dsp::kbd_alpha(-96, 1) == 0.0);
}

TEST_CASE("the synthesis refuses a block length its transform set does not have", "[ac4core][dsp]") {
    dsp::TransformSet<double> set(2048, 1);
    REQUIRE(set.valid());
    dsp::ChannelSynthesis<double> synthesis(2048);
    std::vector<double> spectrum(64, 0.0);
    std::vector<double> pcm(64, 0.0);
    CHECK_FALSE(synthesis.block(set, spectrum, pcm));
    dsp::TransformSet<double> other(1920, 1);
    std::vector<double> block(960, 0.0);
    std::vector<double> out(960, 0.0);
    CHECK_FALSE(synthesis.block(other, block, out));
    CHECK_FALSE(dsp::TransformSet<double>(2000, 1).valid());
}

namespace {

// Pseudocode 65 as printed, one slot at a time, with the complex sum written
// out term by term.
std::vector<Complex> qmf_analysis_as_printed(std::span<const double> pcm) {
    std::array<double, 640> qmf_filt{};
    std::vector<Complex> q;
    for (std::size_t ts = 0; ts < pcm.size() / 64; ++ts) {
        for (std::size_t sb = 639; sb >= 64; --sb) {
            qmf_filt[sb] = qmf_filt[sb - 64];
        }
        for (std::size_t sb = 0; sb < 64; ++sb) {
            qmf_filt[sb] = pcm[ts * 64 + 63 - sb];
        }
        std::array<double, 640> z{};
        for (std::size_t n = 0; n < 640; ++n) {
            z[n] = qmf_filt[n] * static_cast<double>(iclforge::ac4::detail::tables::kQwin[n]);
        }
        std::array<double, 128> u{};
        for (std::size_t n = 0; n < 128; ++n) {
            u[n] = z[n];
            for (std::size_t k = 1; k < 5; ++k) {
                u[n] = u[n] + z[n + k * 128];
            }
        }
        for (std::size_t sb = 0; sb < 64; ++sb) {
            const double f = (std::numbers::pi / 128.0) * (static_cast<double>(sb) + 0.5);
            Complex value = u[0] * Complex(std::cos(f * -1.0), std::sin(f * -1.0));
            for (std::size_t n = 1; n < 128; ++n) {
                const double angle = f * (2.0 * static_cast<double>(n) - 1.0);
                value += u[n] * Complex(std::cos(angle), std::sin(angle));
            }
            q.push_back(value);
        }
    }
    return q;
}

// Pseudocode 66 as printed.
std::vector<double> qmf_synthesis_as_printed(std::span<const Complex> q) {
    std::array<double, 1280> qsyn_filt{};
    std::vector<double> pcm;
    for (std::size_t ts = 0; ts < q.size() / 64; ++ts) {
        for (std::size_t n = 1279; n >= 128; --n) {
            qsyn_filt[n] = qsyn_filt[n - 128];
        }
        for (std::size_t n = 0; n < 128; ++n) {
            const double m = 2.0 * static_cast<double>(n) - 255.0;
            const double f0 = (std::numbers::pi / 128.0) * 0.5;
            qsyn_filt[n] = (q[ts * 64] / 64.0 * Complex(std::cos(f0 * m), std::sin(f0 * m))).real();
            for (std::size_t sb = 1; sb < 64; ++sb) {
                const double f = (std::numbers::pi / 128.0) * (static_cast<double>(sb) + 0.5);
                qsyn_filt[n] += (q[ts * 64 + sb] / 64.0 * Complex(std::cos(f * m), std::sin(f * m))).real();
            }
        }
        std::array<double, 640> g{};
        for (std::size_t n = 0; n < 5; ++n) {
            for (std::size_t sb = 0; sb < 64; ++sb) {
                g[128 * n + sb] = qsyn_filt[256 * n + sb];
                g[128 * n + 64 + sb] = qsyn_filt[256 * n + 192 + sb];
            }
        }
        std::array<double, 640> w{};
        for (std::size_t n = 0; n < 640; ++n) {
            w[n] = g[n] * static_cast<double>(iclforge::ac4::detail::tables::kQwin[n]);
        }
        for (std::size_t sb = 0; sb < 64; ++sb) {
            double temp = w[sb];
            for (std::size_t n = 1; n < 10; ++n) {
                temp = temp + w[64 * n + sb];
            }
            pcm.push_back(temp);
        }
    }
    return pcm;
}

double max_abs(std::span<const Complex> values) {
    double peak = 0.0;
    for (const Complex& v : values) {
        peak = std::max(peak, abs(v));
    }
    return peak;
}

}  // namespace

TEST_CASE("the QMF analysis equals Pseudocode 65 as printed", "[ac4core][dsp][qmf]") {
    const std::vector<double> pcm = random_values(64 * 24, 65);
    // Two calls, to carry qmf_filt across them as a frame boundary does.
    dsp::QmfAnalysis<double> analysis;
    std::vector<Complex> fast(pcm.size());
    const std::span<const double> all(pcm);
    analysis.process(all.first(64 * 10), std::span<Complex>(fast).first(64 * 10));
    analysis.process(all.subspan(64 * 10), std::span<Complex>(fast).subspan(64 * 10));
    const std::vector<Complex> printed = qmf_analysis_as_printed(pcm);
    REQUIRE(printed.size() == fast.size());
    double error = 0.0;
    for (std::size_t i = 0; i < fast.size(); ++i) {
        error = std::max(error, abs(fast[i] - printed[i]));
    }
    CHECK(error <= 1e-12 * max_abs(printed));
}

TEST_CASE("the QMF synthesis equals Pseudocode 66 as printed", "[ac4core][dsp][qmf]") {
    const std::vector<double> re = random_values(64 * 24, 66);
    const std::vector<double> im = random_values(64 * 24, 67);
    std::vector<Complex> q(re.size());
    for (std::size_t i = 0; i < q.size(); ++i) {
        q[i] = Complex(re[i], im[i]);
    }
    dsp::QmfSynthesis<double> synthesis;
    std::vector<double> fast(q.size());
    const std::span<const Complex> all(q);
    synthesis.process(all.first(64 * 7), std::span<double>(fast).first(64 * 7));
    synthesis.process(all.subspan(64 * 7), std::span<double>(fast).subspan(64 * 7));
    const std::vector<double> printed = qmf_synthesis_as_printed(q);
    REQUIRE(printed.size() == fast.size());
    CHECK(max_abs_difference(fast, printed) <= 1e-12 * max_abs(std::span<const double>(printed)));
}

TEST_CASE("the QMF pair gives back its input 577 samples later, to 78 dB", "[ac4core][dsp][qmf]") {
    const std::vector<double> x = random_values(64 * 400, 577);
    dsp::QmfAnalysis<double> analysis;
    dsp::QmfSynthesis<double> synthesis;
    std::vector<Complex> q(x.size());
    std::vector<double> y(x.size());
    analysis.process(x, q);
    synthesis.process(q, y);
    // The delay is where the output correlates best with the input.
    const auto correlation = [&](std::size_t delay) {
        double sum = 0.0;
        for (std::size_t n = 0; n + delay < x.size(); ++n) {
            sum += x[n] * y[n + delay];
        }
        return sum;
    };
    std::size_t best = 0;
    double best_value = correlation(0);
    for (std::size_t delay = 1; delay < 1200; ++delay) {
        const double value = correlation(delay);
        if (value > best_value) {
            best = delay;
            best_value = value;
        }
    }
    CHECK(best == 577);
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t n = 1280; n + 577 < x.size(); ++n) {
        signal += x[n] * x[n];
        noise += (y[n + 577] - x[n]) * (y[n + 577] - x[n]);
    }
    CHECK(10.0 * std::log10(signal / noise) >= 78.0);
}

TEST_CASE("the QMF banks leave their output alone when given the wrong sizes, and reset",
          "[ac4core][dsp][qmf]") {
    dsp::QmfAnalysis<double> analysis;
    const std::vector<double> ragged(100, 1.0);
    std::vector<Complex> q(128, Complex(-1.0, 0.0));
    analysis.process(ragged, q);
    CHECK(q == std::vector<Complex>(128, Complex(-1.0, 0.0)));
    const std::vector<double> two_slots(128, 1.0);
    std::vector<Complex> one_slot(64, Complex(-1.0, 0.0));
    analysis.process(two_slots, one_slot);
    CHECK(one_slot == std::vector<Complex>(64, Complex(-1.0, 0.0)));

    dsp::QmfSynthesis<double> synthesis;
    std::vector<double> pcm(64, -1.0);
    synthesis.process(std::vector<Complex>(100), pcm);
    CHECK(pcm == std::vector<double>(64, -1.0));

    // After reset() the banks start from silence again: the same input gives
    // the same output.
    const std::vector<double> input = random_values(64 * 12, 3);
    std::vector<Complex> first(input.size());
    std::vector<Complex> again(input.size());
    analysis.reset();
    analysis.process(input, first);
    analysis.reset();
    analysis.process(input, again);
    CHECK(first == again);
    std::vector<double> out_first(input.size());
    std::vector<double> out_again(input.size());
    synthesis.reset();
    synthesis.process(first, out_first);
    synthesis.reset();
    synthesis.process(first, out_again);
    CHECK(out_first == out_again);
}

TEST_CASE("the QMF banks give the same output however the slots are split across calls",
          "[ac4core][dsp][qmf]") {
    // 47 slots is more than four laps of the ten blocks of each delay line, and
    // calls of 1, 2, 3, 5, 7 and 11 slots start the laps at every block.
    constexpr std::size_t kSlots = 47;
    const std::vector<double> x = random_values(64 * kSlots, 4701);
    const std::array<std::size_t, 6> calls = {1, 2, 3, 5, 7, 11};

    dsp::QmfAnalysis<double> whole;
    std::vector<Complex> q_whole(x.size());
    whole.process(x, q_whole);
    dsp::QmfAnalysis<double> split;
    std::vector<Complex> q_split(x.size());
    // One scratch serves both banks, as a substream's does.
    dsp::QmfScratch<double> scratch{};
    std::size_t at = 0;
    for (std::size_t call = 0; at < kSlots; ++call) {
        const std::size_t n = std::min(calls[call % calls.size()], kSlots - at);
        split.process(std::span<const double>(x).subspan(at * 64, n * 64),
                      std::span<Complex>(q_split).subspan(at * 64, n * 64), scratch);
        at += n;
    }
    CHECK(q_split == q_whole);

    dsp::QmfSynthesis<double> synth_whole;
    std::vector<double> y_whole(x.size());
    synth_whole.process(q_whole, y_whole);
    dsp::QmfSynthesis<double> synth_split;
    std::vector<double> y_split(x.size());
    at = 0;
    for (std::size_t call = 0; at < kSlots; ++call) {
        const std::size_t n = std::min(calls[(call + 3) % calls.size()], kSlots - at);
        synth_split.process(std::span<const Complex>(q_whole).subspan(at * 64, n * 64),
                            std::span<double>(y_split).subspan(at * 64, n * 64), scratch);
        at += n;
    }
    CHECK(y_split == y_whole);
}

TEST_CASE("the QMF banks at the decoder's scalar agree with the banks at double",
          "[ac4core][dsp][qmf]") {
    using Scalar = iclforge::ac4::detail::Real;
    using ScalarComplex = dsp::Complex<Scalar>;
    // Where the decoder's scalar is double these are one type and the difference
    // is exactly 0; at float it is float's rounding through the window and the
    // three passes of the transform. At Fixed32 it is the fixed banks' rounding,
    // a raw unit of 2^-24 against a slot normalised to at least 1/4, and their
    // QMF domain is kTimeShift - kQmfShift bits below the time domain the input
    // is in (dsp/scalar_traits.hpp), which `qmf_scale` takes out.
    const double epsilon = dsp::kFixed<Scalar> ? 0x1p-22 : static_cast<double>(std::numeric_limits<Scalar>::epsilon());
    const double kBound = 64.0 * epsilon;
    const double qmf_scale = std::ldexp(1.0, dsp::kQmfShift<Scalar> - dsp::kTimeShift<Scalar>);
    const std::vector<double> x = random_values(64 * 40, 811);
    std::vector<Scalar> xs(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        xs[i] = static_cast<Scalar>(x[i]);
    }
    dsp::QmfAnalysis<double> analysis_double;
    dsp::QmfAnalysis<Scalar> analysis_scalar;
    std::vector<Complex> q(x.size());
    std::vector<ScalarComplex> qs(x.size());
    analysis_double.process(x, q);
    analysis_scalar.process(xs, qs);
    const double q_peak = max_abs(q);
    double q_error = 0.0;
    for (std::size_t i = 0; i < q.size(); ++i) {
        const Complex widened(static_cast<double>(qs[i].re) / qmf_scale,
                              static_cast<double>(qs[i].im) / qmf_scale);
        q_error = std::max(q_error, abs(widened - q[i]));
    }
    CHECK(q_error <= kBound * q_peak);

    // The synthesis takes the double analysis's subbands, narrowed to the scalar.
    std::vector<ScalarComplex> q_narrow(q.size());
    for (std::size_t i = 0; i < q.size(); ++i) {
        q_narrow[i] = ScalarComplex(static_cast<Scalar>(q[i].re * qmf_scale),
                                    static_cast<Scalar>(q[i].im * qmf_scale));
    }
    dsp::QmfSynthesis<double> synthesis_double;
    dsp::QmfSynthesis<Scalar> synthesis_scalar;
    std::vector<double> y(x.size());
    std::vector<Scalar> ys(x.size());
    synthesis_double.process(q, y);
    synthesis_scalar.process(q_narrow, ys);
    const double y_peak = max_abs(y);
    double y_error = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        y_error = std::max(y_error, std::abs(static_cast<double>(ys[i]) - y[i]));
    }
    CHECK(y_error <= kBound * y_peak);

    // The pair at the scalar reconstructs to the 78 dB QWIN allows: float's own
    // rounding sits far below that.
    std::vector<Scalar> back(x.size());
    synthesis_scalar.reset();
    synthesis_scalar.process(qs, back);
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t n = 1280; n + 577 < x.size(); ++n) {
        const double error = static_cast<double>(back[n + 577]) - x[n];
        signal += x[n] * x[n];
        noise += error * error;
    }
    CHECK(10.0 * std::log10(signal / noise) >= 78.0);
}

TEST_CASE("the QMF twiddle factors are cosines and sines of whole units of pi over 256",
          "[ac4core][dsp][qmf]") {
    namespace q = dsp::qmf;
    for (long long j = -1100; j <= 1100; ++j) {
        const double c = q::cos_units(j);
        const double s = q::sin_units(j);
        // Against the library's own value of the angle, rounded once (up to
        // 14 radians here, so about 2e-15 either way).
        const double angle = std::numbers::pi * static_cast<double>(j) / 256.0;
        CHECK(std::abs(c - std::cos(angle)) <= 4e-15);
        CHECK(std::abs(s - std::sin(angle)) <= 4e-15);
        // The identities the reduction rests on hold exactly, since each side is
        // a lookup of the same table entry.
        CHECK(q::cos_units(j + 512) == c);
        CHECK(q::cos_units(-j) == c);
        CHECK(q::cos_units(256 - j) == -c);
        CHECK(q::sin_units(128 - j) == c);
        CHECK(std::abs(c * c + s * s - 1.0) <= 4e-16);
    }
    CHECK(q::cos_units(0) == 1.0);
    CHECK(q::sin_units(128) == 1.0);
    CHECK(q::cos_units(128) == 0.0);
    CHECK(q::cos_units(256) == -1.0);
    CHECK(q::cos_units(64) == q::sin_units(64));

    // Each constant is the scalar nearest its exact value, so the float table is
    // the double table rounded, and the scale factors are powers of two.
    const auto& d = q::kConstants<double>;
    const auto& f = q::kConstants<float>;
    for (std::size_t m = 0; m < 64; ++m) {
        CHECK(f.rot_re[m] == static_cast<float>(d.rot_re[m]));
        CHECK(f.rot_im[m] == static_cast<float>(d.rot_im[m]));
    }
    for (std::size_t k = 0; k < 32; ++k) {
        CHECK(std::abs(d.pre_re[k] * d.pre_re[k] + d.pre_im[k] * d.pre_im[k] - 1.0 / 16384.0) <=
              1e-19);
        CHECK(std::abs(d.prho_re[k] * d.prho_re[k] + d.prho_im[k] * d.prho_im[k] - 1.0 / 16384.0) <=
              1e-19);
        CHECK(std::abs(4.0 * (d.post_cos[k] * d.post_cos[k] + d.post_sin[k] * d.post_sin[k]) -
                       1.0) <= 4e-16);
    }
}

TEST_CASE("the QMF's 64-point transform equals the DFT", "[ac4core][dsp][qmf]") {
    const std::vector<double> re = random_values(64, 641);
    const std::vector<double> im = random_values(64, 642);
    std::vector<Complex> expected(64);
    double peak = 0.0;
    for (std::size_t k = 0; k < 64; ++k) {
        Complex sum{};
        for (std::size_t m = 0; m < 64; ++m) {
            // The angle 2 pi (k m mod 64) / 64 reduced first, as the Fft's are.
            const double angle = 2.0 * std::numbers::pi * static_cast<double>((k * m) % 64) / 64.0;
            sum += Complex(re[m], im[m]) * Complex(std::cos(angle), std::sin(angle));
        }
        expected[k] = sum;
        peak = std::max(peak, abs(sum));
    }
    const auto run = [&](auto tag) {
        using R = decltype(tag);
        std::array<R, 64> xr{};
        std::array<R, 64> xi{};
        std::array<R, 64> yr{};
        std::array<R, 64> yi{};
        for (std::size_t m = 0; m < 64; ++m) {
            xr[m] = static_cast<R>(re[m]);
            xi[m] = static_cast<R>(im[m]);
        }
        dsp::qmf::fft64<R>(xr.data(), xi.data(), yr.data(), yi.data());
        double error = 0.0;
        for (std::size_t k = 0; k < 64; ++k) {
            const Complex got(static_cast<double>(yr[k]), static_cast<double>(yi[k]));
            error = std::max(error, abs(got - expected[k]));
        }
        return error / peak;
    };
    CHECK(run(double{}) <= 1e-14);
    // Float's rounding through three passes: a few times its epsilon.
    CHECK(run(float{}) <= 16.0 * static_cast<double>(std::numeric_limits<float>::epsilon()));
}

TEST_CASE("each QMF vector kernel gives the bits of the scalar loop it replaces",
          "[ac4core][dsp][qmf][simd]") {
    namespace k = dsp::qmf;
    namespace v = dsp::qmf::vec;
    const auto run = [](auto tag, unsigned seed) {
        using R = decltype(tag);
        const auto draw = [&](std::size_t count, unsigned s) {
            const std::vector<double> x = random_values(count, s);
            std::vector<R> out(x.size());
            for (std::size_t i = 0; i < x.size(); ++i) {
                out[i] = static_cast<R>(x[i]);
            }
            return out;
        };
        // Equal as bit patterns: a vector kernel that fused a multiply and an add,
        // or summed in another order, differs in the last bit of some value here.
        const auto same = [](const auto& a, const auto& b) {
            return a.size() == b.size() &&
                   std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0;
        };
        for (unsigned round = 0; round < 8; ++round) {
            const std::vector<R> analysis_line = draw(640, seed + 10 * round);
            const std::vector<R> synthesis_line = draw(1280, seed + 10 * round + 1);
            const std::vector<R> plane_re = draw(64, seed + 10 * round + 2);
            const std::vector<R> plane_im = draw(64, seed + 10 * round + 3);
            const std::vector<R> samples = draw(128, seed + 10 * round + 4);
            std::vector<dsp::Complex<R>> subbands(64);
            for (std::size_t i = 0; i < 64; ++i) {
                subbands[i] = {plane_re[i], plane_im[63 - i]};
            }
            // Both windows, at every position of the delay line's head.
            for (std::size_t head = 0; head < 10; ++head) {
                std::vector<R> u0(128);
                std::vector<R> u1(128);
                k::analysis_window<R>(analysis_line.data(), head, u0.data());
                v::analysis_window<R>(analysis_line.data(), head, u1.data());
                CHECK(same(u0, u1));
                std::vector<R> w0(64);
                std::vector<R> w1(64);
                k::synthesis_window<R>(synthesis_line.data(), head, w0.data());
                v::synthesis_window<R>(synthesis_line.data(), head, w1.data());
                CHECK(same(w0, w1));
            }
            std::vector<R> zr0(64);
            std::vector<R> zi0(64);
            std::vector<R> zr1(64);
            std::vector<R> zi1(64);
            k::analysis_rotate<R>(samples.data(), zr0.data(), zi0.data());
            v::analysis_rotate<R>(samples.data(), zr1.data(), zi1.data());
            CHECK(same(zr0, zr1));
            CHECK(same(zi0, zi1));
            // The transform clobbers its input, so each takes its own copy.
            std::vector<R> xr0 = plane_re;
            std::vector<R> xi0 = plane_im;
            std::vector<R> xr1 = plane_re;
            std::vector<R> xi1 = plane_im;
            std::vector<R> yr0(64);
            std::vector<R> yi0(64);
            std::vector<R> yr1(64);
            std::vector<R> yi1(64);
            k::fft64<R>(xr0.data(), xi0.data(), yr0.data(), yi0.data());
            v::fft64<R>(xr1.data(), xi1.data(), yr1.data(), yi1.data());
            CHECK(same(yr0, yr1));
            CHECK(same(yi0, yi1));
            CHECK(same(xr0, xr1));
            CHECK(same(xi0, xi1));
            std::vector<dsp::Complex<R>> q0(64);
            std::vector<dsp::Complex<R>> q1(64);
            k::analysis_unpack<R>(plane_re.data(), plane_im.data(), q0.data());
            v::analysis_unpack<R>(plane_re.data(), plane_im.data(), q1.data());
            CHECK(same(q0, q1));
            std::vector<R> tr0(64);
            std::vector<R> ti0(64);
            std::vector<R> tr1(64);
            std::vector<R> ti1(64);
            k::synthesis_pack<R>(subbands.data(), tr0.data(), ti0.data());
            v::synthesis_pack<R>(subbands.data(), tr1.data(), ti1.data());
            CHECK(same(tr0, tr1));
            CHECK(same(ti0, ti1));
            std::vector<R> b0(128);
            std::vector<R> b1(128);
            k::synthesis_rotate<R>(plane_re.data(), plane_im.data(), b0.data());
            v::synthesis_rotate<R>(plane_re.data(), plane_im.data(), b1.data());
            CHECK(same(b0, b1));
        }
    };
    run(double{}, 7100);
    run(float{}, 7200);
}

TEST_CASE("the QMF banks give the bits of the scalar kernels run one after another",
          "[ac4core][dsp][qmf][simd]") {
    namespace k = dsp::qmf;
    const auto run = [](auto tag, unsigned seed) {
        using R = decltype(tag);
        using RComplex = dsp::Complex<R>;
        // 33 slots: the delay line's head goes round more than three times.
        constexpr std::size_t kSlots = 33;
        const std::vector<double> x = random_values(64 * kSlots, seed);
        std::vector<R> pcm(x.size());
        for (std::size_t i = 0; i < x.size(); ++i) {
            pcm[i] = static_cast<R>(x[i]);
        }
        // The analysis as the scalar kernels compose it: the newest block at the
        // head of ten, reversed, then the four steps.
        std::vector<RComplex> expected(pcm.size());
        {
            std::array<R, 640> filt{};
            std::size_t head = 0;
            std::array<R, 128> u{};
            std::array<R, 64> ar{};
            std::array<R, 64> ai{};
            std::array<R, 64> br{};
            std::array<R, 64> bi{};
            for (std::size_t ts = 0; ts < kSlots; ++ts) {
                head = head == 0 ? 9 : head - 1;
                for (std::size_t sb = 0; sb < 64; ++sb) {
                    filt[head * 64 + sb] = pcm[ts * 64 + 63 - sb];
                }
                k::analysis_window<R>(filt.data(), head, u.data());
                k::analysis_rotate<R>(u.data(), ar.data(), ai.data());
                k::fft64<R>(ar.data(), ai.data(), br.data(), bi.data());
                k::analysis_unpack<R>(br.data(), bi.data(), expected.data() + ts * 64);
            }
        }
        dsp::QmfAnalysis<R> analysis;
        std::vector<RComplex> got(pcm.size());
        analysis.process(pcm, got);
        CHECK(std::memcmp(got.data(), expected.data(), got.size() * sizeof(RComplex)) == 0);

        // And the synthesis, from the analysis's own subbands.
        std::vector<R> expected_pcm(pcm.size());
        {
            std::array<R, 1280> filt{};
            std::size_t head = 0;
            std::array<R, 64> ar{};
            std::array<R, 64> ai{};
            std::array<R, 64> br{};
            std::array<R, 64> bi{};
            for (std::size_t ts = 0; ts < kSlots; ++ts) {
                head = head == 0 ? 9 : head - 1;
                k::synthesis_pack<R>(got.data() + ts * 64, ar.data(), ai.data());
                k::fft64<R>(ar.data(), ai.data(), br.data(), bi.data());
                k::synthesis_rotate<R>(br.data(), bi.data(), filt.data() + head * 128);
                k::synthesis_window<R>(filt.data(), head, expected_pcm.data() + ts * 64);
            }
        }
        dsp::QmfSynthesis<R> synthesis;
        std::vector<R> got_pcm(pcm.size());
        synthesis.process(got, got_pcm);
        CHECK(std::memcmp(got_pcm.data(), expected_pcm.data(), got_pcm.size() * sizeof(R)) == 0);
    };
    // The banks are instantiated at the decoder's scalar and at double, whichever
    // that is (AC4CORE_ALSO_AT_DOUBLE), and at nothing else. At Fixed32 the banks
    // have kernels of their own, with a block exponent per slot (dsp/qmf_fixed.hpp),
    // which the test above holds to the banks at double.
    run(double{}, 8100);
    if (!dsp::kFixed<iclforge::ac4::detail::Real>) {
        run(iclforge::ac4::detail::Real{}, 8200);
    }
}

TEST_CASE("the transforms take a scratch of the caller's and give the same values",
          "[ac4core][dsp]") {
    // The FFT and the inverse MDCT work in buffers of their own, made by the first
    // call, or in one the caller lends; the values do not depend on which.
    for (const std::size_t n :
         {std::size_t{48}, std::size_t{120}, std::size_t{512}, std::size_t{960}}) {
        const std::vector<double> re = random_values(n, static_cast<unsigned>(n) + 1);
        const std::vector<double> im = random_values(n, static_cast<unsigned>(n) + 2);
        std::vector<Complex> input(n);
        for (std::size_t i = 0; i < n; ++i) {
            input[i] = Complex(re[i], im[i]);
        }
        dsp::Fft<double> own(n);
        dsp::Fft<double> lent(n);
        std::vector<Complex> scratch(n);
        std::vector<Complex> a = input;
        std::vector<Complex> b = input;
        own.forward(a);
        lent.forward(b, scratch);
        CHECK(a == b);
        own.inverse(a);
        lent.inverse(b, scratch);
        CHECK(a == b);
        // A scratch too short for the plan leaves the data alone.
        std::vector<Complex> short_scratch(n - 1);
        b = input;
        lent.forward(b, short_scratch);
        CHECK(b == input);
    }
    for (const std::size_t n :
         {std::size_t{128}, std::size_t{480}, std::size_t{512}, std::size_t{2048}}) {
        const std::vector<double> spectrum = random_values(n, static_cast<unsigned>(n) + 3);
        dsp::Imdct<double> own(n);
        dsp::Imdct<double> lent(n);
        std::vector<double> out_own(2 * n);
        std::vector<double> out_lent(2 * n);
        std::vector<Complex> scratch(n);
        own.inverse(spectrum, out_own);
        lent.inverse(spectrum, out_lent, scratch);
        CHECK(out_own == out_lent);
        std::vector<Complex> short_scratch(n - 1);
        std::vector<double> untouched(2 * n, -1.0);
        lent.inverse(spectrum, untouched, short_scratch);
        CHECK(untouched == std::vector<double>(2 * n, -1.0));
    }
}

TEST_CASE("channels that share one transform set give what channels with their own give",
          "[ac4core][dsp]") {
    // A substream's channels inverse transform one block after another in the set's
    // scratch, at block lengths that change from one block to the next.
    constexpr int kFull = 2048;
    const std::array<int, 12> lengths = {2048, 1024, 1024, 256, 256,  256,
                                         256,  512,  512,  128, 2048, 2048};
    dsp::TransformSet<double> shared(kFull, 1);
    dsp::TransformSet<double> set_a(kFull, 1);
    dsp::TransformSet<double> set_b(kFull, 1);
    REQUIRE(shared.valid());
    dsp::ChannelSynthesis<double> a_shared(kFull);
    dsp::ChannelSynthesis<double> b_shared(kFull);
    dsp::ChannelSynthesis<double> a_alone(kFull);
    dsp::ChannelSynthesis<double> b_alone(kFull);
    unsigned seed = 900;
    for (const int length : lengths) {
        const auto n = static_cast<std::size_t>(length);
        const std::vector<double> spectrum_a = random_values(n, seed++);
        const std::vector<double> spectrum_b = random_values(n, seed++);
        std::vector<double> pcm_a_shared(n);
        std::vector<double> pcm_b_shared(n);
        std::vector<double> pcm_a_alone(n);
        std::vector<double> pcm_b_alone(n);
        REQUIRE(a_shared.block(shared, spectrum_a, pcm_a_shared));
        REQUIRE(b_shared.block(shared, spectrum_b, pcm_b_shared));
        REQUIRE(a_alone.block(set_a, spectrum_a, pcm_a_alone));
        REQUIRE(b_alone.block(set_b, spectrum_b, pcm_b_alone));
        CHECK(pcm_a_shared == pcm_a_alone);
        CHECK(pcm_b_shared == pcm_b_alone);
    }
    // The set owns the working space: a block of the full length, and the values
    // the longest transform takes.
    CHECK(shared.block_scratch().size() == 2 * static_cast<std::size_t>(kFull));
    CHECK(shared.transform_scratch().size() == static_cast<std::size_t>(kFull));
}
