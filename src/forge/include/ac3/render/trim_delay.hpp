#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/render/routing.hpp"

// Per-output trim and delay: a level in dB and a time offset in samples for
// each output a patch writes (ac3/render/routing.hpp), the speaker-level and
// speaker-distance settings an AVR's speaker setup has.
//
// Speaker management only, as planning/hearth-reference-player.md scopes it:
// no measurement, no equalisation, no filter beyond the renderer's crossover.
//
// The delay lines are the caller's storage, sized with storage_floats() and
// handed to configure(), so that a board can place them in PSRAM and a
// computer in a vector; nothing here allocates. Sixteen outputs of 20 ms at
// 48 kHz is 15,360 floats, about 61 KB. Per sample the work is a swap into a
// ring and a float multiply, and an output at 0 dB with no delay costs
// nothing. Tested on the host in tests/render/test_trim_delay.cpp.

namespace ac3::render {

class TrimDelay {
   public:
    static constexpr std::size_t kMaxOutputs = Routing::kMaxOutputs;
    // What set_trim_db() accepts. A trim is for matching speakers to each
    // other; muting one is the patch's business.
    static constexpr double kMinTrimDb = -24.0;
    static constexpr double kMaxTrimDb = 12.0;

    // Floats of storage for `outputs` outputs, each delayable by up to
    // `max_delay_samples`.
    [[nodiscard]] static constexpr std::size_t storage_floats(std::size_t outputs,
                                                              std::size_t max_delay_samples) {
        return outputs * max_delay_samples;
    }

    // Milliseconds at `sample_rate_hz` as whole samples, to the nearest; 0 for
    // a negative or non-finite duration.
    [[nodiscard]] static std::size_t samples_for_ms(double ms, std::uint32_t sample_rate_hz) {
        if (!(ms > 0.0) || !std::isfinite(ms)) {
            return 0;
        }
        const double samples = ms * static_cast<double>(sample_rate_hz) / 1000.0;
        return static_cast<std::size_t>(std::lround(samples));
    }

    // Nothing configured: no outputs, and process() does nothing.
    TrimDelay() = default;

    // `outputs` outputs at 0 dB with no delay, each able to delay by up to
    // `max_delay_samples` over `storage`, which this zeroes and keeps a view
    // of. False, leaving the object unconfigured, for more than kMaxOutputs
    // outputs or less storage than storage_floats() says.
    bool configure(std::span<float> storage, std::size_t outputs, std::size_t max_delay_samples) {
        *this = TrimDelay{};
        if (outputs > kMaxOutputs || storage.size() < storage_floats(outputs, max_delay_samples)) {
            return false;
        }
        storage_ = storage.first(storage_floats(outputs, max_delay_samples));
        std::fill(storage_.begin(), storage_.end(), 0.0F);
        outputs_ = outputs;
        max_delay_ = max_delay_samples;
        return true;
    }

    [[nodiscard]] std::size_t outputs() const { return outputs_; }
    [[nodiscard]] std::size_t max_delay() const { return max_delay_; }

    // Sets an output's trim. False, changing nothing, for an output out of
    // range or a trim outside [kMinTrimDb, kMaxTrimDb], NaN included.
    bool set_trim_db(std::size_t output, double db) {
        if (output >= outputs_ || !(db >= kMinTrimDb && db <= kMaxTrimDb)) {
            return false;
        }
        trim_db_[output] = db;
        gain_[output] = static_cast<float>(std::pow(10.0, db / 20.0));
        return true;
    }

    // Sets an output's delay, in samples. A change empties that output's
    // line, so what was delayed under the old setting is dropped rather than
    // played out of order: the output is silent for the new delay's length.
    // False, changing nothing, for an output out of range or a delay over
    // max_delay().
    bool set_delay(std::size_t output, std::size_t samples) {
        if (output >= outputs_ || samples > max_delay_) {
            return false;
        }
        if (samples != delay_[output]) {
            delay_[output] = static_cast<std::uint32_t>(samples);
            position_[output] = 0;
            std::fill_n(line(output), samples, 0.0F);
        }
        return true;
    }

    [[nodiscard]] double trim_db(std::size_t output) const {
        return output < outputs_ ? trim_db_[output] : 0.0;
    }
    [[nodiscard]] std::size_t delay(std::size_t output) const {
        return output < outputs_ ? delay_[output] : 0;
    }

    // Whether any output has a trim or a delay; process() changes nothing
    // when none has.
    [[nodiscard]] bool active() const {
        for (std::size_t o = 0; o < outputs_; ++o) {
            if (delay_[o] != 0 || gain_[o] != 1.0F) {
                return true;
            }
        }
        return false;
    }

    // One block of each output, in place: delayed, then trimmed. `out` is one
    // span per output, in output order; spans past outputs() are left alone,
    // and each output takes its own span's length.
    void process(std::span<const std::span<float>> out) {
        const std::size_t count = std::min(out.size(), outputs_);
        for (std::size_t o = 0; o < count; ++o) {
            float* const samples = out[o].data();
            const std::size_t n = out[o].size();
            const std::size_t d = delay_[o];
            if (d != 0) {
                // The line holds the last d samples in arrival order from
                // position_; swapping a run in takes the oldest out.
                float* const ring = line(o);
                std::size_t p = position_[o];
                std::size_t k = 0;
                while (k < n) {
                    const std::size_t run = std::min(n - k, d - p);
                    std::swap_ranges(samples + k, samples + k + run, ring + p);
                    k += run;
                    p += run;
                    if (p == d) {
                        p = 0;
                    }
                }
                position_[o] = static_cast<std::uint32_t>(p);
            }
            const float g = gain_[o];
            if (g != 1.0F) {
                for (std::size_t k = 0; k < n; ++k) {
                    samples[k] *= g;
                }
            }
        }
    }

    // Silence in every delay line, for a new stream; the settings stay.
    void reset() {
        std::fill(storage_.begin(), storage_.end(), 0.0F);
        position_.fill(0);
    }

   private:
    static constexpr std::array<float, kMaxOutputs> unity() {
        std::array<float, kMaxOutputs> out{};
        for (float& g : out) {
            g = 1.0F;
        }
        return out;
    }

    float* line(std::size_t output) { return storage_.data() + (output * max_delay_); }

    std::span<float> storage_;
    std::size_t outputs_ = 0;
    std::size_t max_delay_ = 0;
    std::array<double, kMaxOutputs> trim_db_{};
    std::array<float, kMaxOutputs> gain_ = unity();
    std::array<std::uint32_t, kMaxOutputs> delay_{};
    std::array<std::uint32_t, kMaxOutputs> position_{};
};

}  // namespace ac3::render
