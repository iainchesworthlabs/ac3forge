#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/dsp/qmf.hpp"

using ac3::dsp::kQmfDelay;
using ac3::dsp::kQmfHop;
using ac3::dsp::kQmfSubbands;
using ac3::dsp::kQmfTaps;
using ac3::dsp::QmfAnalysis;
using ac3::dsp::QmfSynthesis;

namespace {

// Three frames and change of real, non-trivial material: a chord plus a
// swept tone, so nothing here can pass on silence or on a single stationary
// sinusoid the way a lazier fixture would (docs/verification.md's own rule
// about testing on more than frame 0).
[[nodiscard]] std::vector<float> material(std::size_t samples) {
    std::vector<float> out(samples);
    for (std::size_t n = 0; n < samples; ++n) {
        const double t = static_cast<double>(n) / 48000.0;
        const double sweep = 300.0 + 2400.0 * static_cast<double>(n) / static_cast<double>(samples);
        out[n] = static_cast<float>(
            0.30 * std::sin(2.0 * std::numbers::pi * 220.0 * t) +
            0.22 * std::sin(2.0 * std::numbers::pi * 1319.0 * t + 0.7) +
            0.18 * std::sin(2.0 * std::numbers::pi * 4400.0 * t + 1.9) +
            0.15 * std::sin(2.0 * std::numbers::pi * sweep * t));
    }
    return out;
}

// Runs the pair end to end and returns the reconstruction, already aligned:
// out[n] is the filterbank's estimate of in[n].
[[nodiscard]] std::vector<float> round_trip(std::span<const float> in) {
    QmfAnalysis analysis;
    QmfSynthesis synthesis;
    std::vector<float> out(in.size(), 0.0f);
    std::array<double, kQmfSubbands> real{};
    std::array<double, kQmfSubbands> imag{};
    std::array<float, kQmfHop> block{};

    const auto hop = static_cast<std::size_t>(kQmfHop);
    for (std::size_t start = 0; start + hop <= in.size(); start += hop) {
        std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(start), hop, block.begin());
        analysis.push(block, real, imag);
        std::array<float, kQmfHop> emitted{};
        synthesis.pull(real, imag, emitted);
        // The pair emits output block (b - kQmfDelay/kQmfHop) while block b
        // is pushed, so what comes back belongs kQmfDelay samples earlier.
        if (start >= static_cast<std::size_t>(kQmfDelay)) {
            std::copy_n(emitted.begin(), hop,
                        out.begin() + static_cast<std::ptrdiff_t>(start - kQmfDelay));
        }
    }
    return out;
}

[[nodiscard]] double snr_db(std::span<const float> want, std::span<const float> got,
                            std::size_t from, std::size_t to) {
    double signal = 0.0;
    double error = 0.0;
    for (std::size_t n = from; n < to; ++n) {
        const double w = static_cast<double>(want[n]);
        const double d = static_cast<double>(got[n]) - w;
        signal += w * w;
        error += d * d;
    }
    return 10.0 * std::log10(signal / std::max(error, 1e-300));
}

}  // namespace

TEST_CASE("the QMF prototype satisfies its perfect-reconstruction conditions", "[dsp][qmf]") {
    // The conditions themselves, on the committed coefficients, independent
    // of any transform code: writing the prototype's polyphase components on
    // the hop-64 lattice as q_j = p[n0 + 64 j], every coset must have unit
    // energy and vanishing EVEN-lag autocorrelation. Checking these here as
    // well as through a round trip separates a bad table from a bad
    // filterbank if this file ever goes red.
    const auto taps = ac3::dsp::qmf_prototype();
    constexpr std::size_t kCosets = static_cast<std::size_t>(kQmfHop);
    constexpr std::size_t kPerCoset = static_cast<std::size_t>(kQmfTaps) / kCosets;
    for (std::size_t offset = 0; offset < kCosets; ++offset) {
        CAPTURE(offset);
        std::array<double, kPerCoset> coset{};
        for (std::size_t j = 0; j < kPerCoset; ++j) {
            coset[j] = taps[offset + kCosets * j];
        }
        double energy = 0.0;
        for (const double value : coset) {
            energy += value * value;
        }
        CHECK(std::abs(energy - 1.0) < 1e-12);
        for (std::size_t lag = 2; lag < kPerCoset; lag += 2) {
            CAPTURE(lag);
            double correlation = 0.0;
            for (std::size_t j = 0; j + lag < kPerCoset; ++j) {
                correlation += coset[j] * coset[j + lag];
            }
            CHECK(std::abs(correlation) < 1e-12);
        }
    }
}

TEST_CASE("QMF analysis then synthesis is a delayed identity", "[dsp][qmf]") {
    // The identity test this whole feature is built on top of, written
    // before anything downstream trusted the filterbank - the project's own
    // rule after the MDCT one-block-delay lesson. Reconstruction here is
    // EXACT, not approximate: the prototype is designed to satisfy the
    // perfect-reconstruction conditions, so the only error left is
    // floating-point rounding, and the bound below says so.
    const auto in = material(6 * 1536);
    const auto out = round_trip(in);
    // Skip the pair's own warm-up (the first kQmfTaps samples see a
    // partially-filled window) and the tail the delay never reaches.
    const std::size_t from = static_cast<std::size_t>(kQmfTaps);
    const std::size_t to = in.size() - static_cast<std::size_t>(kQmfDelay + kQmfTaps);
    const double snr = snr_db(in, out, from, to);
    CAPTURE(snr);
    // Measured: the round trip is BIT-EXACT at the float boundary - the
    // double-precision internals land within ~1e-16 relative, far inside
    // float's own 6e-8 rounding step, so every sample casts back to the
    // float it came from and the error sum is identically zero (the
    // reported figure is then just the 1e-300 divide-by-zero guard). The
    // bound stays a loose SNR rather than an equality because FP
    // contraction differs across the toolchains this suite runs on, and
    // one ulp of drift on one sample is not a regression.
    CHECK(snr > 120.0);
}

TEST_CASE("QMF reconstruction holds for an impulse anywhere in the frame", "[dsp][qmf]") {
    // A tone can hide a delay error that a sparse signal cannot: an impulse
    // reconstructs at exactly one sample or the alignment is wrong.
    for (const std::size_t position : {size_t{700}, size_t{1536}, size_t{2101}}) {
        CAPTURE(position);
        std::vector<float> in(6 * 1536, 0.0f);
        in[position] = 1.0f;
        const auto out = round_trip(in);
        CHECK(std::abs(static_cast<double>(out[position]) - 1.0) < 1e-6);
        double stray = 0.0;
        for (std::size_t n = static_cast<std::size_t>(kQmfTaps);
             n < in.size() - static_cast<std::size_t>(kQmfDelay + kQmfTaps); ++n) {
            if (n != position) {
                stray = std::max(stray, std::abs(static_cast<double>(out[n])));
            }
        }
        CHECK(stray < 1e-6);
    }
}

TEST_CASE("a QMF subband captures a tone at its own centre", "[dsp][qmf]") {
    // Selectivity, the property the reconstruction test cannot see: a
    // perfect-reconstruction filterbank whose bands all overlapped
    // completely would still round-trip perfectly and separate nothing.
    constexpr int kBand = 20;
    const double centre = (static_cast<double>(kBand) + 0.5) / (2.0 * kQmfSubbands);
    std::vector<float> in(8192);
    for (std::size_t n = 0; n < in.size(); ++n) {
        in[n] = static_cast<float>(std::cos(2.0 * std::numbers::pi * centre *
                                            static_cast<double>(n)));
    }

    QmfAnalysis analysis;
    std::array<double, kQmfSubbands> real{};
    std::array<double, kQmfSubbands> imag{};
    std::array<float, kQmfHop> block{};
    std::array<double, kQmfSubbands> energy{};
    const auto hop = static_cast<std::size_t>(kQmfHop);
    for (std::size_t start = 0; start + hop <= in.size(); start += hop) {
        std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(start), hop, block.begin());
        analysis.push(block, real, imag);
        if (start < static_cast<std::size_t>(kQmfTaps)) {
            continue;  // steady state only
        }
        for (std::size_t k = 0; k < static_cast<std::size_t>(kQmfSubbands); ++k) {
            energy[k] += real[k] * real[k] + imag[k] * imag[k];
        }
    }

    double total = 0.0;
    for (const double value : energy) {
        total += value;
    }
    const double in_band = energy[static_cast<std::size_t>(kBand)];
    const double isolation = 10.0 * std::log10(in_band / std::max(total - in_band, 1e-300));
    CAPTURE(isolation);
    // 34.8 dB measured by the generator's own sweep; the floor allows for
    // the transform's rounding without allowing a regression in the table.
    CHECK(isolation > 33.0);
}
