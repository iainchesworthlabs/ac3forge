// The AC-4 shared core's transforms (src/ac4core/src/dsp), each against a
// direct evaluation of the formula it computes: the FFT against the DFT, the
// inverse MDCT against a verbatim transcription of ETSI TS 103 190-1 V1.4.1
// Pseudocodes 60 to 63 and against the cosine sum they come to, the forward
// MDCT against its own sum, and the KBD windows against values computed with
// numpy's Kaiser window (a Bessel function written by others). Then the
// synthesis's windows and overlap-add, fed by an analysis written here from
// the same windows, reconstruct their input across every block transition
// Part 1 Table 187 allows.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dsp/fft.hpp"
#include "dsp/kbd.hpp"
#include "dsp/mdct.hpp"
#include "dsp/synthesis.hpp"

namespace {

namespace dsp = ac4::detail::dsp;
using Complex = std::complex<double>;

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
                error = std::max(error, std::abs(fast[k] - slow[k]));
                scale = std::max(scale, std::abs(slow[k]));
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
