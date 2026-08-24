#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "ac3/export.hpp"

// The 64-band complex QMF that TS 103 420 §7.1 puts the JOC reconstruction
// in - the filterbank this tree did not have, and whose absence made
// joc::reconstruct apply §6.6.6's matrix in the MDCT domain instead.
//
// Why a decoder needs a COMPLEX filterbank here rather than reusing the
// MDCT: the MDCT is critically sampled and real, so its subbands rely on
// time-domain alias cancellation between neighbouring blocks. That
// cancellation assumes the two blocks saw the same processing. A JOC matrix
// is per-band and changes every frame, so applying it in the MDCT domain
// breaks the assumption and leaves the aliasing uncancelled - it lands in
// the output as distortion that no amount of matrix accuracy removes. A
// complex filterbank at 2x oversampling (64 complex subbands per 64 real
// input samples) has no such dependency: a per-band gain is just a gain.
//
// Structure, all of it fixed here: M = 64 subbands, hop M, prototype length
// L = 640 (10 taps per subband), fold period 2M = 128 with alternating
// sign, odd-stacked (k + 1/2) modulation, and a 128-point complex FFT from
// the same shared radix-2 core mdct.cpp's fast fold and dft512 both run on.
//
// Perfect reconstruction is exact, not approximate - analysis followed by
// synthesis returns the input to floating-point precision (measured: 300 dB
// SNR, tests/dsp/test_qmf.cpp). That is a property of the prototype, which
// is designed for it rather than merely tested for it; see
// tools/generators/gen_qmf_prototype.py for the conditions and the design.
//
// The prototype is NOT Dolby's. §7.1 fixes the filterbank's shape and does
// not publish its coefficients, and this project is clean-room; what §6.6.6
// actually requires of a decoder is that the matrix be applied per subband
// in an oversampled complex domain, which any prototype of this structure
// provides.

namespace ac3::dsp {

// §7.1's subband count, and the hop that goes with it: one timeslot of 64
// complex subband samples per 64 input samples.
inline constexpr int kQmfSubbands = 64;
inline constexpr int kQmfHop = 64;
inline constexpr int kQmfTaps = 640;

// Analysis plus synthesis is delayed by L - M samples. Nothing in the pair
// can shorten it: the timeslot whose window opens at input sample s is only
// complete once sample s + L - 1 has arrived, and it is the last timeslot
// contributing to output sample s.
inline constexpr int kQmfDelay = kQmfTaps - kQmfHop;  // 576

// Whole timeslots per 1 536-sample frame, and how many of a frame's own
// timeslots the delay above pushes into the NEXT frame's call.
inline constexpr int kQmfSlotsPerFrame = 24;
inline constexpr int kQmfDelaySlots = kQmfDelay / kQmfHop;  // 9

// One channel's streaming analysis. Construct once per channel per run,
// push() once per 64 input samples in order, reset() only at a run
// boundary - the same contract Biquad and audio::DriftResampler carry.
class AC3FORGE_EXPORT QmfAnalysis {
public:
    void reset();

    // Consumes the next 64 input samples and produces the one timeslot they
    // complete, as separate real/imaginary arrays (the split-array form the
    // FFT core works in, so nothing has to be repacked around it).
    //
    // The timeslot produced is NOT the one covering the samples just pushed:
    // it is the one whose 640-sample window ENDS on them, kQmfDelaySlots
    // timeslots back. That offset is the whole of the pair's delay and the
    // caller has to account for it - see kQmfDelay.
    void push(std::span<const float, kQmfHop> block, std::span<double, kQmfSubbands> real,
              std::span<double, kQmfSubbands> imag);

private:
    // The last kQmfTaps input samples, oldest first.
    std::array<double, kQmfTaps> history_{};
};

// The matching synthesis, likewise one per reconstructed signal.
class AC3FORGE_EXPORT QmfSynthesis {
public:
    void reset();

    // Consumes one timeslot and emits the 64 output samples it completes.
    void pull(std::span<const double, kQmfSubbands> real,
              std::span<const double, kQmfSubbands> imag, std::span<float, kQmfHop> out);

private:
    // Overlap accumulator: position 0 is the oldest not-yet-emitted sample.
    std::array<double, kQmfTaps> overlap_{};
};

// The designed prototype, for tests that check the perfect-reconstruction
// conditions on the coefficients directly rather than only through a
// round trip.
[[nodiscard]] AC3FORGE_EXPORT std::span<const double, kQmfTaps> qmf_prototype();

}  // namespace ac3::dsp
