#include "ac3/quality/perceptual.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "ac3/core/bitalloc_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/internal/profiling.hpp"

namespace ac3::quality {

namespace {

// The transform's own bin count. A/52's banding table covers 256 bins
// whatever the coded bandwidth is.
constexpr int kBins = 256;

// Zwicker and Terhardt's analytical Bark expression (JASA 68(5), 1980).
double bark_of(double hz) {
    const double khz = hz / 1000.0;
    return 13.0 * std::atan(0.76 * khz) + 3.5 * std::atan(khz * khz / 56.25);
}

// Schroeder, Atal and Hall's spreading function (JASA 66(6), 1979), in dB,
// as a function of Bark distance from the masker to the maskee. Positive
// `distance` is upward in frequency, which is the direction masking
// actually spreads furthest.
double spreading_db(double distance) {
    const double x = distance + 0.474;
    return 15.81 + (7.5 * x) - (17.5 * std::sqrt(1.0 + (x * x)));
}

// Terhardt's approximation to the absolute threshold of hearing, dB SPL.
double absolute_threshold_db(double hz) {
    const double khz = std::max(hz, 20.0) / 1000.0;
    return (3.64 * std::pow(khz, -0.8)) -
           (6.5 * std::exp(-0.6 * (khz - 3.3) * (khz - 3.3))) + (1e-3 * std::pow(khz, 4.0));
}

// Johnston §II's two limits. Tone masking noise is the harder requirement
// and rises with frequency; noise masking tone is nearly flat. A band's
// requirement is interpolated between them by its tonality.
constexpr double kNoiseMaskingToneDb = 5.5;
double tone_masking_noise_db(double bark) { return 14.5 + bark; }

// Below this the spreading contribution is not worth the multiply: 60 dB
// down from the masker, against a masking curve whose useful dynamic range
// is nothing like that wide. Bounding the spread to the bands that clear it
// turns a 50x50 accumulation into roughly 50x12.
constexpr double kSpreadFloorDb = -60.0;

}  // namespace

void BlockAnalysis::reset() {
    threshold.fill(0.0);
    energy.fill(0.0);
    tonality.fill(0.0);
    perceptual_entropy = 0.0;
}

struct PerceptualModel::Impl {
    PerceptualConfig config;
    int channels = 0;

    // Band geometry for this sample rate.
    std::array<double, kBands> centre_hz{};
    std::array<double, kBands> centre_bark{};
    std::array<double, kBands> width_bark{};      // the band's own span in Bark
    std::array<double, kBands> absolute_power{};  // per LINE, coefficient power

    // Row b of the spreading matrix: the bands that mask band b, as a
    // contiguous range plus the weights. Contiguous because the spreading
    // function is monotone away from zero, so the bands clearing the floor
    // are exactly an interval around b.
    struct SpreadRow {
        int first = 0;
        int last = -1;  // inclusive
        double normalisation = 1.0;
    };
    std::array<SpreadRow, kBands> spread{};
    std::array<std::array<double, kBands>, kBands> weight{};

    // Per channel, the previous two blocks' smoothed magnitudes, and how
    // many blocks of history are actually valid (0, 1 or 2).
    std::vector<std::array<double, kBins>> previous;
    std::vector<std::array<double, kBins>> before;
    std::vector<int> history_depth;

    // Scratch, reused across calls so a per-block analysis allocates
    // nothing at steady state.
    std::array<double, kBins> magnitude{};
    std::array<double, kBands> band_energy{};
    std::array<double, kBands> band_unpredictability{};
    std::array<double, kBands> excitation{};
    std::array<double, kBands> peak{};
    std::array<int, kBands> lines{};

    void build_geometry(SampleRate sample_rate);
};

void PerceptualModel::Impl::build_geometry(SampleRate sample_rate) {
    const double rate = static_cast<double>(sample_rate_hz(sample_rate));
    for (std::size_t b = 0; b < kBands; ++b) {
        const double start = tables::kBandStart[b];
        const double size = tables::kBandSize[b];
        // The band's centre bin, in the 256-bin transform whose bin k spans
        // k * rate / 512.
        centre_hz[b] = (start + (size / 2.0)) * rate / 512.0;
        centre_bark[b] = bark_of(centre_hz[b]);
        // And its span, which is the number the spreading below actually
        // needs. Table 7.13's bands are nothing like equal on this scale:
        // band 0 is one bin covering 0.93 Bark, band 49 is twelve bins
        // covering 0.08. A floor keeps the reciprocal finite for any
        // sample rate.
        const double low = bark_of(start * rate / 512.0);
        const double high = bark_of((start + size) * rate / 512.0);
        width_bark[b] = std::max(high - low, 1e-3);
    }

    for (std::size_t b = 0; b < kBands; ++b) {
        const double db =
            std::min(absolute_threshold_db(centre_hz[b]), config.max_absolute_threshold_db);
        // dB SPL to the coefficient-power domain, through the measured
        // full-scale block energy. Per LINE: a band's floor is this times
        // its width, applied where the band totals are formed.
        absolute_power[b] =
            kFullScaleBlockEnergy * std::pow(10.0, (db - config.full_scale_spl_db) / 10.0);
    }

    for (std::size_t b = 0; b < kBands; ++b) {
        auto& row = spread[b];
        double sum = 0.0;
        row.first = kBands;
        row.last = -1;
        for (std::size_t source = 0; source < kBands; ++source) {
            const double db = spreading_db(centre_bark[b] - centre_bark[source]);
            if (db < kSpreadFloorDb) {
                weight[b][source] = 0.0;
                continue;
            }
            const double linear = std::pow(10.0, db / 10.0);
            weight[b][source] = linear;
            // Weighted by the SOURCE band's Bark width, which is what makes
            // the normalisation below partition-consistent.
            sum += linear * width_bark[source];
            row.first = std::min(row.first, static_cast<int>(source));
            row.last = std::max(row.last, static_cast<int>(source));
        }
        // MPEG-1 model 2 renormalises the spread energy by the spreading
        // function's own gain, so that a flat spectrum produces an
        // excitation equal to its own energy rather than several times it.
        // It divides by the plain row sum, which is right for ITS partitions
        // - Annex D's are about a third of a Bark each and near enough
        // uniform - and wrong for A/52's, which vary twelvefold in Bark
        // width across the band (0.93 at DC, 0.08 at 23 kHz).
        //
        // With a plain row sum on partitions that unequal, the excitation of
        // a band is compared against a weighted MEAN band width rather than
        // against its own: wide low bands come out over-demanded and narrow
        // high bands under-demanded, by up to about 11 dB across the range.
        // Measured, that starves the top of the spectrum - the search
        // preferred dbpbcod 2, log-spectral distance rose 0.45 dB and SNR
        // fell over a decibel.
        //
        // Weighting the sum by each source band's Bark width, and scaling
        // the result by this band's own, is the density form of the same
        // expression: the spread quantity is energy per Bark, and a band's
        // share of it is its own width. A flat spectral density then
        // reproduces itself in every band whatever the partition, which is
        // the property Annex D's normalisation is reaching for and gets by
        // assuming the partitions are uniform.
        row.normalisation = sum > 0.0 ? width_bark[b] / sum : 1.0;
        if (row.last < row.first) {
            row.first = static_cast<int>(b);
            row.last = static_cast<int>(b);
            weight[b][b] = 1.0;
            row.normalisation = 1.0;
        }
    }
}

PerceptualModel::PerceptualModel(SampleRate sample_rate, int channels, PerceptualConfig config)
    : impl_(std::make_unique<Impl>()) {
    assert(channels > 0);
    impl_->config = config;
    impl_->channels = channels;
    impl_->build_geometry(sample_rate);
    impl_->previous.assign(static_cast<std::size_t>(channels), {});
    impl_->before.assign(static_cast<std::size_t>(channels), {});
    impl_->history_depth.assign(static_cast<std::size_t>(channels), 0);
}

PerceptualModel::~PerceptualModel() = default;
PerceptualModel::PerceptualModel(PerceptualModel&&) noexcept = default;
PerceptualModel& PerceptualModel::operator=(PerceptualModel&&) noexcept = default;

void PerceptualModel::reset() {
    std::ranges::fill(impl_->history_depth, 0);
}

void PerceptualModel::reset(int channel) {
    assert(channel >= 0 && channel < impl_->channels);
    impl_->history_depth[static_cast<std::size_t>(channel)] = 0;
}

std::span<const double> PerceptualModel::band_centre_hz() const {
    return impl_->centre_hz;
}

void PerceptualModel::analyse(int channel, std::span<const double> coefficients, int end,
                              BlockAnalysis& out) {
    AC3_ZONE_SCOPED_N("perceptual_analyse");
    assert(channel >= 0 && channel < impl_->channels);
    assert(end >= 0 && end <= kBins);
    assert(coefficients.size() >= static_cast<std::size_t>(end));
    const auto ch = static_cast<std::size_t>(channel);
    Impl& impl = *impl_;
    out.reset();

    // --- Smoothed magnitude -------------------------------------------------
    // Three bins, because a stationary sinusoid's MDCT magnitude is
    // modulated block to block by time-domain aliasing and the per-bin value
    // is not a stable thing to extrapolate. The neighbourhood energy is.
    auto& magnitude = impl.magnitude;
    for (int bin = 0; bin < kBins; ++bin) {
        if (bin >= end) {
            magnitude[static_cast<std::size_t>(bin)] = 0.0;
            continue;
        }
        double sum = 0.0;
        for (int offset = -1; offset <= 1; ++offset) {
            const int neighbour = bin + offset;
            if (neighbour >= 0 && neighbour < end) {
                const double value = coefficients[static_cast<std::size_t>(neighbour)];
                sum += value * value;
            }
        }
        magnitude[static_cast<std::size_t>(bin)] = std::sqrt(sum);
    }

    // --- Band energy, peak and unpredictability -----------------------------
    auto& band_energy = impl.band_energy;
    auto& unpredictability = impl.band_unpredictability;
    auto& peak = impl.peak;
    auto& lines = impl.lines;
    band_energy.fill(0.0);
    unpredictability.fill(0.0);
    peak.fill(0.0);
    lines.fill(0);

    const int depth = impl.history_depth[ch];
    const auto& previous = impl.previous[ch];
    const auto& before = impl.before[ch];

    for (int band = 0; band < kBands; ++band) {
        const int start = tables::kBandStart[static_cast<std::size_t>(band)];
        const int stop = std::min(start + tables::kBandSize[static_cast<std::size_t>(band)], end);
        if (start >= stop) {
            continue;
        }
        double energy = 0.0;
        double weighted = 0.0;
        double largest = 0.0;
        for (int bin = start; bin < stop; ++bin) {
            const double value = coefficients[static_cast<std::size_t>(bin)];
            energy += value * value;
            largest = std::max(largest, std::abs(value));
            const double current = magnitude[static_cast<std::size_t>(bin)];
            // §D.2.4, magnitude only: extrapolate linearly from the last two
            // blocks and normalise the miss. With less than two blocks of
            // history there is nothing to predict from, and claiming
            // tonality on no evidence is the expensive direction to be
            // wrong in - so it reads as fully unpredictable.
            double unpredictable = 1.0;
            if (depth >= 2) {
                const double predicted = (2.0 * previous[static_cast<std::size_t>(bin)]) -
                                         before[static_cast<std::size_t>(bin)];
                const double denominator = current + std::abs(predicted);
                unpredictable =
                    denominator > 0.0 ? std::abs(current - predicted) / denominator : 1.0;
            }
            // Energy-weighted within the band, so a loud tone is not
            // outvoted by the quiet bins beside it.
            weighted += current * current * std::clamp(unpredictable, 0.0, 1.0);
        }
        const auto b = static_cast<std::size_t>(band);
        band_energy[b] = energy;
        peak[b] = largest;
        lines[b] = stop - start;
        double magnitude_energy = 0.0;
        for (int bin = start; bin < stop; ++bin) {
            const double current = magnitude[static_cast<std::size_t>(bin)];
            magnitude_energy += current * current;
        }
        unpredictability[b] = magnitude_energy > 0.0 ? weighted / magnitude_energy : 1.0;
    }

    // --- Spreading ----------------------------------------------------------
    auto& excitation = impl.excitation;
    excitation.fill(0.0);
    for (int band = 0; band < kBands; ++band) {
        const auto b = static_cast<std::size_t>(band);
        const auto& row = impl.spread[b];
        double sum = 0.0;
        for (int source = row.first; source <= row.last; ++source) {
            sum += band_energy[static_cast<std::size_t>(source)] *
                   impl.weight[b][static_cast<std::size_t>(source)];
        }
        excitation[b] = sum * row.normalisation;
    }

    // --- Threshold and perceptual entropy -----------------------------------
    double entropy = 0.0;
    for (int band = 0; band < kBands; ++band) {
        const auto b = static_cast<std::size_t>(band);
        out.energy[b] = band_energy[b];
        if (lines[b] == 0) {
            continue;
        }
        // §D.2.7's mapping from unpredictability to tonality: a well
        // extrapolated band (c near 0) is tonal, c at or above 0.5 is not.
        const double c = std::clamp(unpredictability[b], 1e-9, 1.0);
        const double tonality = std::clamp(-0.299 - (0.43 * std::log(c)), 0.0, 1.0);
        out.tonality[b] = tonality;

        const double requirement_db =
            (tonality * tone_masking_noise_db(impl.centre_bark[b])) +
            ((1.0 - tonality) * kNoiseMaskingToneDb);
        double threshold = excitation[b] * std::pow(10.0, -requirement_db / 10.0);

        if (impl.config.absolute_threshold) {
            threshold = std::max(threshold, impl.absolute_power[b] * lines[b]);
        }
        out.threshold[b] = threshold;

        // Johnston §III, for a real spectrum: the number of quantiser steps
        // the band's largest line needs at a step size that keeps the noise
        // at threshold, summed over the band's lines. The imaginary term of
        // the original expression has no counterpart in an MDCT.
        if (threshold > 0.0 && peak[b] > 0.0) {
            const double step = std::sqrt(threshold / lines[b]);
            const double levels = std::floor(peak[b] / step);
            entropy += lines[b] * std::log2((2.0 * levels) + 1.0);
        }
    }
    out.perceptual_entropy = entropy;

    // --- Advance the history ------------------------------------------------
    impl.before[ch] = impl.previous[ch];
    impl.previous[ch] = magnitude;
    impl.history_depth[ch] = std::min(depth + 1, 2);
}

NoiseToMask noise_to_mask(const BandNoise& measured, std::span<const double> threshold) {
    assert(threshold.size() >= static_cast<std::size_t>(kBands));
    NoiseToMask result;
    double sum = 0.0;
    double worst = 0.0;
    for (std::size_t b = 0; b < static_cast<std::size_t>(kBands); ++b) {
        if (measured.signal[b] <= 0.0 || threshold[b] <= 0.0) {
            continue;
        }
        const double ratio = measured.noise[b] / threshold[b];
        sum += ratio;
        ++result.bands;
        // Table 7.13's own width. The topmost coded band may be cut short by
        // endmant, so its weight can be over-stated by up to eleven bins out
        // of the ~250 a channel codes; carrying the exact coded width would
        // mean threading it through every caller for a correction smaller
        // than the model's own approximations.
        const double width = tables::kBandSize[b];
        result.audible_bits += width * std::log2(1.0 + ratio);
        if (result.worst_band < 0 || ratio > worst) {
            worst = ratio;
            result.worst_band = static_cast<int>(b);
        }
    }
    if (result.bands == 0) {
        return result;
    }
    const double mean = sum / result.bands;
    // A band whose noise is exactly zero would take the log to -infinity;
    // the floor is far below any ratio that could matter.
    constexpr double kRatioFloor = 1e-30;
    result.mean_db = 10.0 * std::log10(std::max(mean, kRatioFloor));
    result.worst_db = 10.0 * std::log10(std::max(worst, kRatioFloor));
    return result;
}

}  // namespace ac3::quality
