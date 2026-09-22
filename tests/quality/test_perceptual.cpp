#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <vector>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitalloc_tables.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/quality/distortion.hpp"
#include "ac3/quality/perceptual.hpp"

namespace {

constexpr double kRate = 48000.0;

// One block of MDCT coefficients from 512 time samples, exactly the way the
// encoder produces them (step 1 of encoder.cpp): window, then transform.
std::array<double, 256> transform(std::span<const double, 512> samples) {
    std::array<double, 512> windowed{};
    ac3::apply_analysis_window(samples, windowed);
    std::array<double, 256> coefficients{};
    ac3::mdct512_forward(windowed, coefficients);
    return coefficients;
}

// A continuous sinusoid sampled across successive 256-hop blocks, so the
// phase carries over the way it does in a real encode - which is the whole
// point when what is being measured is inter-block predictability.
std::array<double, 512> sine_block(double hz, double amplitude, int block) {
    std::array<double, 512> samples{};
    for (std::size_t n = 0; n < samples.size(); ++n) {
        const double t = static_cast<double>((block * 256) + static_cast<int>(n)) / kRate;
        samples[n] = amplitude * std::sin(2.0 * std::numbers::pi * hz * t);
    }
    return samples;
}

std::array<double, 512> noise_block(std::mt19937& rng, double amplitude) {
    std::uniform_real_distribution<double> dist(-amplitude, amplitude);
    std::array<double, 512> samples{};
    for (double& sample : samples) {
        sample = dist(rng);
    }
    return samples;
}

int band_of_hz(double hz) {
    const auto bin = static_cast<int>(std::lround(hz / (kRate / 512.0)));
    return ac3::bin_to_band(std::clamp(bin, 0, 255));
}

// Runs `blocks` blocks through the model and returns the last analysis, so
// the tonality estimate has the two blocks of history it needs.
ac3::quality::BlockAnalysis settle(ac3::quality::PerceptualModel& model,
                                   const std::vector<std::array<double, 512>>& blocks, int end) {
    ac3::quality::BlockAnalysis analysis;
    for (const auto& samples : blocks) {
        const auto coefficients = transform(samples);
        model.analyse(0, coefficients, end, analysis);
    }
    return analysis;
}

}  // namespace

// kFullScaleBlockEnergy is the one number in the model that ties the
// coefficient domain to sound pressure level, and therefore the one the
// absolute threshold is positioned by. It is a measurement of this
// project's own transform, so it is asserted rather than commented: a
// change to the window or to the MDCT's scaling has to come here and
// explain itself rather than quietly move the threshold.
TEST_CASE("the SPL calibration matches the transform's actual scaling", "[quality][perceptual]") {
    // A bin-centred full-scale sine, where the transform's energy is not
    // split across neighbouring bins.
    const double bin_centre_hz = 8.0 * (kRate / 512.0);
    const auto coefficients = transform(sine_block(bin_centre_hz, 1.0, 0));
    double energy = 0.0;
    for (const double c : coefficients) {
        energy += c * c;
    }
    CHECK_THAT(energy, Catch::Matchers::WithinRel(ac3::quality::kFullScaleBlockEnergy, 1e-6));
}

// The behaviour the whole model rests on: a held tone must read as tonal and
// noise must not. Nothing downstream is meaningful if this is backwards, and
// it is the claim most at risk from the MDCT reduction documented in the
// header - a per-bin magnitude would fail this outright.
TEST_CASE("a steady tone reads as tonal and noise does not", "[quality][perceptual]") {
    constexpr double kToneHz = 1500.0;
    const int tone_band = band_of_hz(kToneHz);

    ac3::quality::PerceptualModel tonal(ac3::SampleRate::k48000, 1);
    std::vector<std::array<double, 512>> tone_blocks;
    for (int block = 0; block < 6; ++block) {
        tone_blocks.push_back(sine_block(kToneHz, 0.5, block));
    }
    const auto tone = settle(tonal, tone_blocks, 253);

    ac3::quality::PerceptualModel noisy(ac3::SampleRate::k48000, 1);
    std::mt19937 rng(0x9E11);
    std::vector<std::array<double, 512>> noise_blocks;
    for (int block = 0; block < 6; ++block) {
        noise_blocks.push_back(noise_block(rng, 0.5));
    }
    const auto noise = settle(noisy, noise_blocks, 253);

    CAPTURE(tone_band, tone.tonality[static_cast<std::size_t>(tone_band)]);
    CHECK(tone.tonality[static_cast<std::size_t>(tone_band)] > 0.7);

    // Averaged over the bands that carry signal: white noise has no band
    // that extrapolates.
    double sum = 0.0;
    int counted = 0;
    for (std::size_t b = 0; b < static_cast<std::size_t>(ac3::quality::kBands); ++b) {
        if (noise.energy[b] > 0.0) {
            sum += noise.tonality[b];
            ++counted;
        }
    }
    REQUIRE(counted > 20);
    const double mean_tonality = sum / counted;
    CAPTURE(mean_tonality);
    CHECK(mean_tonality < 0.35);
}

// A transient is unpredictable even though it may be spectrally sparse,
// which is exactly the distinction a per-block spectral-flatness estimate
// cannot make and this one can. It matters because a tonal verdict buys a
// band up to 30 dB of extra demanded precision.
TEST_CASE("a transient does not read as tonal", "[quality][perceptual]") {
    ac3::quality::PerceptualModel model(ac3::SampleRate::k48000, 1);
    std::vector<std::array<double, 512>> blocks;
    for (int block = 0; block < 3; ++block) {
        blocks.push_back({});  // silence, to settle the history
    }
    // A sudden loud tone burst in the fourth block: same spectrum as the
    // steady case above, no history to predict it from.
    blocks.push_back(sine_block(1500.0, 0.9, 3));
    const auto analysis = settle(model, blocks, 253);

    const int tone_band = band_of_hz(1500.0);
    CAPTURE(analysis.tonality[static_cast<std::size_t>(tone_band)]);
    CHECK(analysis.tonality[static_cast<std::size_t>(tone_band)] < 0.5);
}

TEST_CASE("with no history nothing is claimed to be tonal", "[quality][perceptual]") {
    ac3::quality::PerceptualModel model(ac3::SampleRate::k48000, 1);
    ac3::quality::BlockAnalysis analysis;
    const auto coefficients = transform(sine_block(1500.0, 0.5, 0));
    model.analyse(0, coefficients, 253, analysis);
    for (std::size_t b = 0; b < static_cast<std::size_t>(ac3::quality::kBands); ++b) {
        CHECK(analysis.tonality[b] == 0.0);
    }
    // And reset() puts it back to that state.
    for (int block = 1; block < 6; ++block) {
        const auto later = transform(sine_block(1500.0, 0.5, block));
        model.analyse(0, later, 253, analysis);
    }
    REQUIRE(analysis.tonality[static_cast<std::size_t>(band_of_hz(1500.0))] > 0.7);
    model.reset();
    const auto after = transform(sine_block(1500.0, 0.5, 6));
    model.analyse(0, after, 253, analysis);
    CHECK(analysis.tonality[static_cast<std::size_t>(band_of_hz(1500.0))] == 0.0);
}

// Johnston §II: a tone demands far more headroom over the noise it is
// hiding than noise does. This is the mechanism by which the model differs
// from A/52's own exponent-only curve at all, so it is worth asserting
// directly rather than inferring from a downstream number.
TEST_CASE("a tonal band demands a lower noise threshold than a noisy one at equal energy",
          "[quality][perceptual]") {
    constexpr double kHz = 1500.0;
    const auto band = static_cast<std::size_t>(band_of_hz(kHz));

    ac3::quality::PerceptualModel tonal(ac3::SampleRate::k48000, 1);
    std::vector<std::array<double, 512>> tone_blocks;
    for (int block = 0; block < 6; ++block) {
        tone_blocks.push_back(sine_block(kHz, 0.5, block));
    }
    const auto tone = settle(tonal, tone_blocks, 253);

    // Narrowband noise centred on the same band, so the energies are
    // comparable and only the predictability differs.
    ac3::quality::PerceptualModel noisy(ac3::SampleRate::k48000, 1);
    std::mt19937 rng(0x3C1F);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::vector<std::array<double, 512>> narrow_blocks;
    for (int block = 0; block < 6; ++block) {
        std::array<double, 512> samples{};
        for (std::size_t n = 0; n < samples.size(); ++n) {
            const double t = static_cast<double>((block * 256) + static_cast<int>(n)) / kRate;
            // A tone whose phase is randomised every block: same band, no
            // usable prediction.
            samples[n] = 0.5 * std::sin((2.0 * std::numbers::pi * kHz * t) + gauss(rng));
        }
        narrow_blocks.push_back(samples);
    }
    const auto narrow = settle(noisy, narrow_blocks, 253);

    CAPTURE(tone.energy[band], narrow.energy[band], tone.tonality[band], narrow.tonality[band],
            tone.threshold[band], narrow.threshold[band]);
    REQUIRE(tone.tonality[band] > narrow.tonality[band]);
    // Same band, similar energy, more tonal: strictly less noise may hide
    // there. Normalised by energy so the comparison does not rest on the
    // two signals having identical level.
    CHECK((tone.threshold[band] / tone.energy[band]) <
          (narrow.threshold[band] / narrow.energy[band]));
}

TEST_CASE("masking spreads to neighbouring bands", "[quality][perceptual]") {
    ac3::quality::PerceptualConfig config;
    config.absolute_threshold = false;  // isolate the spreading term
    ac3::quality::PerceptualModel model(ac3::SampleRate::k48000, 2, config);

    // Channel 0: one loud band. Channel 1: the same band plus a quiet
    // neighbour that on its own would demand a far lower threshold.
    std::vector<std::array<double, 512>> loud;
    for (int block = 0; block < 6; ++block) {
        loud.push_back(sine_block(4000.0, 0.8, block));
    }
    ac3::quality::BlockAnalysis with_masker;
    for (const auto& samples : loud) {
        const auto coefficients = transform(samples);
        model.analyse(0, coefficients, 253, with_masker);
    }

    const auto masker_band = static_cast<std::size_t>(band_of_hz(4000.0));
    // A band a little above the masker must have been lifted by it - it
    // carries no energy of its own at all.
    bool lifted = false;
    const std::size_t bands = static_cast<std::size_t>(ac3::quality::kBands);
    for (std::size_t b = masker_band + 1; b < std::min(masker_band + 4, bands); ++b) {
        if (with_masker.energy[b] < with_masker.energy[masker_band] * 1e-6 &&
            with_masker.threshold[b] > 0.0) {
            lifted = true;
        }
    }
    CHECK(lifted);
}

TEST_CASE("the absolute threshold floors quiet bands and can be switched off",
          "[quality][perceptual]") {
    // A very quiet low-frequency tone: below the absolute threshold, which
    // rises steeply under 100 Hz.
    std::vector<std::array<double, 512>> blocks;
    for (int block = 0; block < 6; ++block) {
        blocks.push_back(sine_block(60.0, 1e-4, block));
    }

    ac3::quality::PerceptualModel floored(ac3::SampleRate::k48000, 1);
    const auto with_floor = settle(floored, blocks, 253);

    ac3::quality::PerceptualConfig without;
    without.absolute_threshold = false;
    ac3::quality::PerceptualModel unfloored(ac3::SampleRate::k48000, 1, without);
    const auto no_floor = settle(unfloored, blocks, 253);

    const auto band = static_cast<std::size_t>(band_of_hz(60.0));
    CAPTURE(with_floor.threshold[band], no_floor.threshold[band], with_floor.energy[band]);
    CHECK(with_floor.threshold[band] > no_floor.threshold[band]);
    // The floor is above the signal itself here, which is the whole point:
    // there is nothing audible in this band to protect.
    CHECK(with_floor.threshold[band] > with_floor.energy[band]);
}

TEST_CASE("perceptual entropy is zero for silence and grows with level",
          "[quality][perceptual]") {
    ac3::quality::PerceptualModel model(ac3::SampleRate::k48000, 1);
    std::vector<std::array<double, 512>> silence(6);
    CHECK(settle(model, silence, 253).perceptual_entropy == 0.0);

    double previous = 0.0;
    for (const double amplitude : {0.01, 0.1, 0.5}) {
        ac3::quality::PerceptualModel fresh(ac3::SampleRate::k48000, 1);
        std::mt19937 rng(0x7A2C);
        std::vector<std::array<double, 512>> blocks;
        for (int block = 0; block < 6; ++block) {
            blocks.push_back(noise_block(rng, amplitude));
        }
        const double entropy = settle(fresh, blocks, 253).perceptual_entropy;
        CAPTURE(amplitude, entropy, previous);
        CHECK(entropy > previous);
        previous = entropy;
    }
}

// The reason noise_to_mask averages ratios rather than dividing sums: the
// composite SNR offset's failure mode is that a loud band's slack pays for
// a quiet band's excess, and a replacement measure that did the same thing
// would be no replacement at all.
TEST_CASE("noise_to_mask does not let a loud band hide a quiet one",
          "[quality][perceptual]") {
    std::array<double, ac3::quality::kBands> threshold{};
    ac3::quality::BandNoise measured;

    // Band 10: loud, and comfortably under threshold.
    measured.signal[10] = 1.0;
    measured.noise[10] = 1e-6;
    threshold[10] = 1e-3;
    // Band 30: quiet, and 20 dB OVER its threshold.
    measured.signal[30] = 1e-4;
    measured.noise[30] = 1e-5;
    threshold[30] = 1e-7;

    const auto result = ac3::quality::noise_to_mask(measured, threshold);
    CHECK(result.bands == 2);
    CHECK(result.worst_band == 30);
    CHECK_THAT(result.worst_db, Catch::Matchers::WithinAbs(20.0, 1e-9));
    // The ratio of the sums would be 10*log10(1.1e-5 / 1.0001e-3) = -19.6 dB,
    // i.e. "comfortable", which is the wrong answer.
    CHECK(result.mean_db > 15.0);

    // Nothing coded at all is not a failure state.
    ac3::quality::BandNoise empty;
    const auto nothing = ac3::quality::noise_to_mask(empty, threshold);
    CHECK(nothing.bands == 0);
    CHECK(nothing.worst_band == -1);
    CHECK(nothing.mean_db == 0.0);
}

// End to end over the two halves together: a real allocation at a low SNR
// offset must score worse against the model's thresholds than the same
// signal at a high one. This is the comparison the search will make, made
// once by hand.
TEST_CASE("measured noise against the model tracks the allocation's generosity",
          "[quality][perceptual]") {
    ac3::quality::PerceptualModel model(ac3::SampleRate::k48000, 1);
    std::mt19937 rng(0x1D4A);

    std::array<double, ac3::quality::kBands> threshold{};
    ac3::quality::BlockAnalysis analysis;
    constexpr int kEnd = 253;

    // Six blocks of a tonal-plus-noise mix, accumulated the way a frame is.
    std::vector<std::array<double, 256>> spectra;
    for (int block = 0; block < 6; ++block) {
        auto samples = sine_block(1500.0, 0.4, block);
        const auto extra = noise_block(rng, 0.05);
        for (std::size_t n = 0; n < samples.size(); ++n) {
            samples[n] += extra[n];
        }
        const auto coefficients = transform(samples);
        model.analyse(0, coefficients, kEnd, analysis);
        for (std::size_t b = 0; b < static_cast<std::size_t>(ac3::quality::kBands); ++b) {
            threshold[b] += analysis.threshold[b];
        }
        spectra.push_back(coefficients);
    }

    const auto score_at = [&](int csnroffst) {
        ac3::quality::BandNoise measured;
        for (const auto& coefficients : spectra) {
            std::vector<std::int32_t> fixed(kEnd);
            std::vector<std::uint8_t> exps(kEnd);
            for (int bin = 0; bin < kEnd; ++bin) {
                fixed[static_cast<std::size_t>(bin)] =
                    ac3::to_fixed25(coefficients[static_cast<std::size_t>(bin)]);
                exps[static_cast<std::size_t>(bin)] = static_cast<std::uint8_t>(
                    ac3::exponent_from_fixed(fixed[static_cast<std::size_t>(bin)]));
            }
            std::vector<std::uint8_t> bap(kEnd);
            ac3::compute_bit_allocation(exps, ac3::SampleRate::k48000, ac3::BitAllocCodes{},
                                        csnroffst, 0, bap);
            ac3::quality::accumulate_block(fixed, exps, bap, 0, kEnd, measured);
        }
        return ac3::quality::noise_to_mask(measured, threshold);
    };

    const auto stingy = score_at(12);
    const auto generous = score_at(20);
    CAPTURE(stingy.mean_db, generous.mean_db, stingy.worst_db, generous.worst_db);
    CHECK(generous.mean_db < stingy.mean_db);
    CHECK(generous.worst_db < stingy.worst_db);
}
