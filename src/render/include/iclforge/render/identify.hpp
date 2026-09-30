#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/render/float_biquad.hpp"

// The identify tone: pink noise at a known level, on one output at a time, so
// that whoever is setting up a room can hear which speaker an output reaches.
//
// Pink noise, as an AVR's speaker-level test tone is, because it has equal
// energy in every octave and so sounds equally present through a tweeter and a
// woofer. An output carrying the LFE or a subwoofer's feed gets the low band:
// the same noise through a 30 Hz high-pass and an 80 Hz low-pass, so that a
// subwoofer is not asked for midrange it cannot reproduce and a full-range
// speaker patched there by mistake is identifiable by its sound.
//
// Voss-McCartney generation: sixteen uniform random sources, one refreshed
// every sample and the others at octave-spaced rates, summed. The sources are
// integers, so the running sum never drifts, and the sequence is the same for
// every run from reset(), which is what lets a test hold it to its level. Float
// arithmetic per sample and no allocation, for a board as much as a computer.
//
// Levels are RMS relative to full scale (1.0), as ac3::analysis::LevelMeter
// reports them. Tested on the host in tests/render/test_identify.cpp.

namespace ac3::render {

class IdentifyTone {
   public:
    enum class Band : std::uint8_t {
        kFull,  // pink noise across the band
        kLow,   // pink noise through kLowBandHighPassHz and kLowBandLowPassHz
    };

    static constexpr double kDefaultLevelDb = -20.0;
    // What set_level_db() accepts. Pink noise peaks about 12 dB above its RMS,
    // so the ceiling leaves room before full scale.
    static constexpr double kMinLevelDb = -60.0;
    static constexpr double kMaxLevelDb = -12.0;
    static constexpr double kLowBandHighPassHz = 30.0;
    static constexpr double kLowBandLowPassHz = 80.0;

    explicit IdentifyTone(std::uint32_t sample_rate_hz = 48000) : sample_rate_hz_(sample_rate_hz) {
        low_highpass_.set_highpass(kLowBandHighPassHz, sample_rate_hz_);
        low_lowpass_[0].set_lowpass(kLowBandLowPassHz, sample_rate_hz_);
        low_lowpass_[1].set_lowpass(kLowBandLowPassHz, sample_rate_hz_);
        set_scale();
        reset();
    }

    [[nodiscard]] double level_db() const { return level_db_; }

    // The tone's RMS level. False, changing nothing, outside
    // [kMinLevelDb, kMaxLevelDb], NaN included.
    bool set_level_db(double db) {
        if (!(db >= kMinLevelDb && db <= kMaxLevelDb)) {
            return false;
        }
        level_db_ = db;
        set_scale();
        return true;
    }

    // The next out.size() samples of the tone in `band`. Changing band starts
    // the low band's filters from silence.
    void generate(std::span<float> out, Band band = Band::kFull) {
        if (band != last_band_) {
            reset_filters();
            last_band_ = band;
        }
        for (float& sample : out) {
            const float pink = next_pink();
            if (band == Band::kLow) {
                const float low = low_lowpass_[1].process(
                    low_lowpass_[0].process(low_highpass_.process(pink)));
                sample = low * low_band_scale_;
            } else {
                sample = pink * full_band_scale_;
            }
        }
    }

    // One block for a set of outputs: the tone on `output` and silence on
    // every other, each span written whole. An `output` past the set leaves
    // every output silent - the way to stop without a special case.
    void fill(std::span<const std::span<float>> outputs, std::size_t output,
              Band band = Band::kFull) {
        for (std::size_t o = 0; o < outputs.size(); ++o) {
            if (o == output) {
                generate(outputs[o], band);
            } else {
                std::fill(outputs[o].begin(), outputs[o].end(), 0.0F);
            }
        }
    }

    // Back to the first sample: the same sequence again, filters silent.
    void reset() {
        state_ = kSeed;
        counter_ = 0;
        sum_ = 0;
        for (std::int32_t& row : rows_) {
            row = next_white();
            sum_ += row;
        }
        reset_filters();
    }

   private:
    static constexpr std::size_t kRows = 15;
    // Each source is uniform on [-2^26, 2^26); sixteen of them sum well inside
    // an int32.
    static constexpr std::int32_t kHalfRange = std::int32_t{1} << 26;
    static constexpr std::uint32_t kSeed = 0x2545F491U;
    // The low band's RMS relative to the full band's for the same noise, -11.5
    // dB: measured over sixty seconds of this generator at 44.1, 48 and 96 kHz
    // (-11.50, -11.53 and -11.56 dB), and held to the stated level by
    // tests/render/test_identify.cpp at each of those rates.
    static constexpr double kLowBandRelativeRms = 0.2652;

    // xorshift32 (Marsaglia), as a signed source on [-kHalfRange, kHalfRange).
    std::int32_t next_white() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return static_cast<std::int32_t>(state_ >> 5) - kHalfRange;
    }

    float next_pink() {
        ++counter_;
        if (counter_ != 0) {
            const auto row = static_cast<std::size_t>(std::countr_zero(counter_));
            if (row < kRows) {
                sum_ -= rows_[row];
                rows_[row] = next_white();
                sum_ += rows_[row];
            }
        }
        return static_cast<float>(sum_ + next_white());
    }

    void reset_filters() {
        low_highpass_.reset();
        low_lowpass_[0].reset();
        low_lowpass_[1].reset();
    }

    // Sixteen independent uniform sources of half-range A have an RMS of
    // 4A/sqrt(3); the scales take that to the requested level.
    void set_scale() {
        const double level = std::pow(10.0, level_db_ / 20.0);
        const double unit = std::sqrt(3.0) / (4.0 * static_cast<double>(kHalfRange));
        full_band_scale_ = static_cast<float>(level * unit);
        low_band_scale_ = static_cast<float>(level * unit / kLowBandRelativeRms);
    }

    std::uint32_t sample_rate_hz_;
    double level_db_ = kDefaultLevelDb;
    float full_band_scale_ = 0.0F;
    float low_band_scale_ = 0.0F;
    std::uint32_t state_ = kSeed;
    std::uint32_t counter_ = 0;
    std::array<std::int32_t, kRows> rows_{};
    std::int32_t sum_ = 0;
    Band last_band_ = Band::kFull;
    FloatBiquad low_highpass_;
    std::array<FloatBiquad, 2> low_lowpass_{};
};

}  // namespace ac3::render
