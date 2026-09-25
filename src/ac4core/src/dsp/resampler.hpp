#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// The sample rate converter of ETSI TS 103 190-1 V1.4.1 clause 6.2.15, and the
// same converter the other way round, which the encoder uses. At every
// frame_rate_index but 13 a frame is coded at an internal rate (46 080 Hz for
// 24 and 30 fps, 46 033.97 Hz for 23.976 and 29.97, 51 200 Hz for 25) and the
// decoder converts by the resampling ratio of Tables 83 and 84: 25/24,
// 1001/1000 x 25/24 = 1001/960, or 15/16. The encoder converts by the inverse.
//
// Part 1 asks only for "high-quality anti-aliasing filters". This one is a
// Kaiser-windowed sinc, polyphase, with every phase the ratio needs tabulated
// (up phases of taps() coefficients, one for each position an output sample
// can take between two input samples): the passband runs to 0.86 of the lower
// rate's Nyquist frequency (19.8 kHz at 46 080 Hz, 20.6 kHz at 48 kHz), the
// stopband starts at that Nyquist frequency, and the stopband is 100 dB down,
// which puts the passband ripple near 0.0001 dB. Each phase sums to 1, so a
// constant passes unchanged. tests/ac4core/test_ac4core_resampler.cpp
// measures the ripple, the attenuation and the sample counts.
//
// The output grid. Output sample m (from 0) is complete once (m + 1) * down /
// up input samples have arrived, so after n input samples there are exactly
// floor(n * up / down) outputs. Frame by frame, frame t of N input samples
// gives floor((t + 1) R) - floor(t R) outputs, R = N * up / down: at 29.97 fps
// 1 601, 1 602, 1 601, 1 602 and 1 602 in turn, the sequence Part 2 clause
// 5.11 (Table 47) locks to sequence_counter. A converter reset as if `t`
// frames had passed (reset(t * N)) starts that sequence at its phase t.
//
// Output m stands for the input at (m + 1) * down / up - 1 - taps() / 2 input
// samples, so the converter delays by taps() / 2 + 1 - down / up input
// samples, a fraction of a sample included (delay()).

namespace ac4::detail::dsp {

class ResamplerFilter {
   public:
    // A converter from one rate to that rate times up / down; up and down need
    // not be reduced, and a ratio of 1 gives a filter of one tap that copies.
    ResamplerFilter(int up, int down);

    [[nodiscard]] int up() const noexcept { return up_; }
    [[nodiscard]] int down() const noexcept { return down_; }
    [[nodiscard]] int taps() const noexcept { return taps_; }

    // The design: passband and stopband edges in cycles per input sample, and
    // the stopband attenuation in dB.
    [[nodiscard]] double passband_edge() const noexcept { return passband_; }
    [[nodiscard]] double stopband_edge() const noexcept { return stopband_; }
    static constexpr double kAttenuationDb = 100.0;

    // The taps() coefficients for an output that falls p / up() of an input
    // sample past its taps' centre, 0 <= p < up(): coefficient k weights input
    // sample start + k, where the output's taps start (see the header comment).
    [[nodiscard]] std::span<const double> phase(int p) const noexcept;

    // The converter's delay, in input samples.
    [[nodiscard]] double delay() const noexcept;

   private:
    int up_ = 1;
    int down_ = 1;
    int taps_ = 1;
    double passband_ = 0.5;
    double stopband_ = 0.5;
    std::vector<double> table_;  // up_ phases of taps_ coefficients
};

template <typename Real>
class Resampler {
   public:
    explicit Resampler(std::shared_ptr<const ResamplerFilter> filter);

    // Forgets the input: silence before the next input sample, which is taken
    // to be input sample `inputs_before` of the grid, so that the outputs
    // start at output floor(inputs_before * up / down).
    void reset(std::int64_t inputs_before = 0);

    // Moves the grid so that the next input sample is number `inputs_before`,
    // keeping the input already taken: where Part 2 clause 5.11's phase jumps
    // in a stream the converter goes on converting.
    void rephase(std::int64_t inputs_before);

    // Takes `in` and appends to `out` every output sample it completes.
    void process(std::span<const Real> in, std::vector<Real>& out);

    // How many outputs the next `count` input samples will complete.
    [[nodiscard]] std::size_t outputs_for(std::size_t count) const noexcept;

    [[nodiscard]] const ResamplerFilter& filter() const noexcept { return *filter_; }

   private:
    std::shared_ptr<const ResamplerFilter> filter_;
    std::int64_t inputs_ = 0;   // input samples taken, on the grid
    std::int64_t outputs_ = 0;  // output samples given, on the grid
    std::int64_t first_ = 0;    // the grid number of history_[0]
    std::vector<double> history_;
};

extern template class Resampler<double>;

}  // namespace ac4::detail::dsp
