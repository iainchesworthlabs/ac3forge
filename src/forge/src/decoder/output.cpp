#include "ac3/decoder/output.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "ac3/core/eac3_tables.hpp"  // eac3::chanmap::Location/Layout
#include "ac3/core/tables.hpp"
#include "ac3/internal/decode_scalar.hpp"
#include "ac3/internal/profiling.hpp"
#include "fixed32.hpp"
#include "ac3/meta/drc.hpp"  // to_db
#include "ac3/meta/mixing.hpp"

namespace ac3 {

namespace {

// The 90-degree phase shifter §7.8.2's Lt/Rt asks for, as an odd-length
// type III FIR: a windowed ideal Hilbert transformer, whose impulse response
// is 2/(pi*n) for odd n and zero for even n. 127 taps puts the usable band
// (within about half a decibel of true quadrature) from roughly 350 Hz to
// just under Nyquist at 48 kHz, which covers everything a Dolby Surround
// decoder steers on. Half the taps are exactly zero, so the convolution costs
// half of what the tap count suggests - see its own comment for which half,
// which is a parity that is easy to get backwards.
//
// The group delay is exactly the middle tap, and it is a whole number of
// samples - that is the reason for choosing an odd length. The direct (L, R,
// centre) path is delayed to match rather than the surround path advanced,
// because only one of those two is causal.
inline constexpr int kHilbertTaps = 127;
inline constexpr int kHilbertDelay = (kHilbertTaps - 1) / 2;  // 63

// Built once. The window is Hamming: its first sidelobe is low enough to keep
// the passband ripple under the half decibel above, and it does not zero the
// outermost taps the way a Hann window would - which would waste two of them.
const std::vector<double>& hilbert_kernel() {
    static const std::vector<double> kernel = [] {
        std::vector<double> taps(static_cast<std::size_t>(kHilbertTaps), 0.0);
        for (int i = 0; i < kHilbertTaps; ++i) {
            const int n = i - kHilbertDelay;
            if (n % 2 == 0) {
                continue;  // the ideal response is exactly zero on even taps
            }
            const double ideal = 2.0 / (std::numbers::pi * static_cast<double>(n));
            const double window =
                0.54 - 0.46 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                       static_cast<double>(kHilbertTaps - 1));
            taps[static_cast<std::size_t>(i)] = ideal * window;
        }
        return taps;
    }();
    return kernel;
}

// The same taps in the decode path's own scalar (ac3::internal::decode_scalar_t):
// the double table above, narrowed once, so the shift's per-sample products
// run at that width. The double build gets the double table back untouched.
template <typename Scalar>
const std::vector<Scalar>& hilbert_kernel_as() {
    if constexpr (std::is_same_v<Scalar, double>) {
        return hilbert_kernel();
    } else {
        static const std::vector<Scalar> narrowed = [] {
            const auto& wide = hilbert_kernel();
            std::vector<Scalar> out;
            out.reserve(wide.size());
            for (const double tap : wide) {
                out.push_back(static_cast<Scalar>(tap));
            }
            return out;
        }();
        return narrowed;
    }
}

// Every per-sample product in this file runs in the decode path's scalar -
// float under the minimum-footprint profile, double everywhere else, where
// these expressions are exactly the ones they always were. Gains are still
// derived in double (a handful per frame); it is the multiply per sample
// that an ESP32-S3 with a single-precision FPU cannot afford in double, and
// a player folding 5.1 to stereo every frame pays it every frame.
using Scalar = internal::decode_scalar_t;

// The folds work through a frame this many samples at a time, and their
// working storage is this long rather than a frame long. Measured on an
// ESP32-S3 (docs/platforms/esp32.md, "Folded to stereo"), the frame-long
// version spent a third of a 7.1.4 fold copying finished frames between
// buffers, and its six seats and two outputs were 49 KB of heap at 1,536
// samples. A block is also what the decoders' own output already comes in.
//
// Nothing about the result depends on the block: every sample of a fold is
// its own sum, taken in the same order whatever block it falls in, and the
// two things that do carry from sample to sample - the Lt/Rt shifter's
// history and RF mode's per-frame gain - carry across blocks exactly as they
// always carried across frames, or wait for the whole frame (see
// limit_frame below).
inline constexpr std::size_t kFoldBlock = 256;

// dst[i] += gain * src[i] for `count` samples: the one per-sample expression
// every fold here is built from, in exactly the form each loop used to write
// out. Through local pointers, because the loops written over vectors and
// spans reloaded both base pointers from memory on every sample (on the
// ESP32-S3 at -Os, some 22 cycles a sample); and four samples to a pass, so
// one sample's loads and multiply overlap the previous one's add. Each
// sample is its own sum, so neither changes a value. `dst` and `src` never
// overlap.
void accumulate(float* dst, const float* src, Scalar gain, std::size_t count) {
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const auto a = static_cast<float>(gain * static_cast<Scalar>(src[i]));
        const auto b = static_cast<float>(gain * static_cast<Scalar>(src[i + 1]));
        const auto c = static_cast<float>(gain * static_cast<Scalar>(src[i + 2]));
        const auto d = static_cast<float>(gain * static_cast<Scalar>(src[i + 3]));
        dst[i] += a;
        dst[i + 1] += b;
        dst[i + 2] += c;
        dst[i + 3] += d;
    }
    for (; i < count; ++i) {
        dst[i] += static_cast<float>(gain * static_cast<Scalar>(src[i]));
    }
}

// samples[i] *= gain in place, the dialnorm normalisation's expression, the
// same way.
void scale(float* samples, Scalar gain, std::size_t count) {
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        samples[i] = static_cast<float>(static_cast<Scalar>(samples[i]) * gain);
        samples[i + 1] = static_cast<float>(static_cast<Scalar>(samples[i + 1]) * gain);
        samples[i + 2] = static_cast<float>(static_cast<Scalar>(samples[i + 2]) * gain);
        samples[i + 3] = static_cast<float>(static_cast<Scalar>(samples[i + 3]) * gain);
    }
    for (; i < count; ++i) {
        samples[i] = static_cast<float>(static_cast<Scalar>(samples[i]) * gain);
    }
}

// How many of `count` samples from `offset` a channel actually has. The
// folds read min(frame, channel) samples of each channel, and a channel
// shorter than the frame contributes nothing past its own end.
std::size_t available(std::span<const float> channel, std::size_t offset, std::size_t count) {
    return channel.size() > offset ? std::min(count, channel.size() - offset) : 0;
}

// Which coded positions of Table 5.8 carry surround. ac3::meta's own downmix
// builders keep a fuller version of this privately; what is needed here is
// only "which indices form the surround sum", and a shared layout type
// serving both would be a worse fit for each.
struct Positions {
    int first = -1;
    int second = -1;
};

Positions surround_positions(Acmod acmod) {
    switch (acmod) {
        case Acmod::k2_1: return {.first = 2};
        case Acmod::k3_1: return {.first = 3};
        case Acmod::k2_2: return {.first = 2, .second = 3};
        case Acmod::k3_2: return {.first = 3, .second = 4};
        case Acmod::k1_0:
        case Acmod::k2_0:
        case Acmod::k3_0:
        case Acmod::kDualMono: return {};
    }
    return {};
}

// One frame's fold, worked out once: which coded channels reach each output
// and at what gain - §7.8's coefficients for the target, narrowed to the
// decode scalar, the zero ones dropped - so that the per-block passes do
// nothing but multiply and add. The three folds differ only in which
// coefficients they use and whether a surround sum exists at all, so one
// plan serves all three.
struct FoldPlan {
    struct Term {
        std::size_t channel = 0;
        Scalar gain{};
    };
    std::array<Term, 5> left{};
    std::size_t left_terms = 0;
    std::array<Term, 5> right{};
    std::size_t right_terms = 0;
    // Lt/Rt's surround sum. Not thinned of zero coefficients: §5.4.2.5's
    // '10' - surrounds dropped from the downmix altogether - arrives as a
    // coefficient of exactly zero and needs no special case, the sum staying
    // silent and the shifter filtering silence.
    std::array<Term, 2> surround{};
    std::size_t surround_terms = 0;
    bool stereo = true;
    bool ltrt = false;
    // The LFE's contribution, when it is wanted AND the stream did not
    // disable it.
    bool lfe = false;
    std::size_t lfe_channel = 0;
    Scalar lfe_gain{};
};

FoldPlan plan_fold(const OutputConfig& config, std::size_t inputs, Acmod acmod, bool lfe,
                   const MixLevels& levels) {
    FoldPlan plan;
    const auto nfchans = static_cast<std::size_t>(fullbw_channel_count(acmod));
    plan.stereo = config.target != DownmixTarget::kMono;

    const auto terms = [&](const std::array<double, 5>& coeffs, std::array<FoldPlan::Term, 5>& into,
                           std::size_t& count) {
        for (std::size_t ch = 0; ch < nfchans && ch < inputs && ch < coeffs.size(); ++ch) {
            const double gain = coeffs[ch];
            if (gain == 0.0) {
                continue;
            }
            into[count++] = {.channel = ch, .gain = static_cast<Scalar>(gain)};
        }
    };
    if (config.target == DownmixTarget::kMono) {
        terms(meta::mono_downmix(acmod, levels.loro_clev, levels.loro_slev), plan.left,
              plan.left_terms);
    } else if (config.target == DownmixTarget::kLoRo) {
        const auto coeffs = meta::stereo_downmix(acmod, levels.loro_clev, levels.loro_slev);
        terms(coeffs.left, plan.left, plan.left_terms);
        terms(coeffs.right, plan.right, plan.right_terms);
    } else {
        plan.ltrt = true;
        const auto coeffs = meta::ltrt_downmix(acmod, levels.ltrt_clev, levels.ltrt_slev);
        terms(coeffs.direct.left, plan.left, plan.left_terms);
        terms(coeffs.direct.right, plan.right, plan.right_terms);
        // The surround sum, formed once and then shifted once: §7.8.2 puts ONE
        // surround signal into the matrix, so summing first and filtering the
        // sum is not an optimisation over filtering each channel, it is the
        // correct order. It also halves the filter work for a 3/2 source.
        //
        // ltrt_downmix normalises, so the sum has to carry whatever
        // attenuation the direct path just got. Reading the coefficient back
        // out rather than re-deriving it from levels.ltrt_slev keeps the two
        // paths in step whatever normalisation decided.
        const Positions positions = surround_positions(acmod);
        for (const int position : {positions.first, positions.second}) {
            if (position < 0 || static_cast<std::size_t>(position) >= inputs) {
                continue;
            }
            plan.surround[plan.surround_terms++] = {
                .channel = static_cast<std::size_t>(position),
                .gain = static_cast<Scalar>(coeffs.surround[static_cast<std::size_t>(position)])};
        }
    }

    // §7.8's ideal is +10 dB relative to left and right; an E-AC-3 stream
    // carrying its own lfemixlevcod overrides that with whatever it chose.
    const std::size_t lfe_index = nfchans;
    const bool have_lfe = lfe && inputs > lfe_index;
    const double lfe_gain = (config.mix_lfe && have_lfe && levels.lfe_mix_level_db)
                                ? meta::lfe_mix_gain(*levels.lfe_mix_level_db)
                                : 0.0;
    if (lfe_gain != 0.0) {
        plan.lfe = true;
        plan.lfe_channel = lfe_index;
        plan.lfe_gain = static_cast<Scalar>(lfe_gain);
    }
    return plan;
}

// The Lt/Rt shifter's state, which lives on the stage because it carries from
// one call to the next.
struct Shifter {
    std::vector<float>& history;                     // the surround sum's last taps
    std::vector<std::vector<float>>& direct_history;  // the direct path's delay lines
    std::vector<float>& scratch;
};

// out'[i] = (history ++ out)[i], with the last history.size() samples of that
// same concatenation becoming the next call's history - a call shorter than
// the delay line included. `scratch` swaps with `history` at the end, so it
// leaves holding a buffer of the same size and the next call's assign() never
// reallocates.
void delay_direct(std::span<float> out, std::vector<float>& history,
                  std::vector<float>& scratch) {
    const std::size_t length = out.size();
    const std::size_t depth = history.size();
    scratch.assign(depth, 0.0F);
    const std::size_t keep = std::min(depth, length);
    std::memcpy(scratch.data() + (depth - keep), out.data() + (length - keep),
                keep * sizeof(float));
    if (keep < depth) {
        // A call shorter than the delay: the older tail has not aged all the
        // way out yet, and shuffles along.
        std::memcpy(scratch.data(), history.data() + keep, (depth - keep) * sizeof(float));
    }
    for (std::size_t i = length; i-- > depth;) {
        out[i] = out[i - depth];
    }
    for (std::size_t i = 0; i < keep; ++i) {
        out[i] = history[i];
    }
    history.swap(scratch);
}

// One block of a fold: `count` samples of `inputs` from `offset`, the planned
// terms summed into `left` and (for the two stereo targets) `right`, then the
// Lt/Rt matrix's surround and the LFE on top, in the order the frame-long
// fold always applied them. `left` and `right` are `count` long and alias no
// input.
void fold_block(const FoldPlan& plan, const OutputConfig& config,
                std::span<const std::span<float>> inputs, std::size_t offset, std::size_t count,
                float* left, float* right, std::vector<float>& surround_sum, Shifter shifter) {
    const auto sum = [&](const auto& terms, std::size_t term_count, float* out) {
        std::fill_n(out, count, 0.0F);
        for (std::size_t t = 0; t < term_count; ++t) {
            const auto& source = inputs[terms[t].channel];
            accumulate(out, source.data() + offset, terms[t].gain,
                       available(source, offset, count));
        }
    };
    sum(plan.left, plan.left_terms, left);
    if (plan.stereo) {
        sum(plan.right, plan.right_terms, right);
    }

    if (plan.ltrt) {
        float* const surround = surround_sum.data();
        sum(plan.surround, plan.surround_terms, surround);
        if (config.ltrt_phase_shift) {
            AC3_ZONE_SCOPED_N("output_ltrt_shift");
            // Delay the direct path by the filter's own group delay, then
            // filter the surround sum, so the two arrive together. Both delay
            // lines carry across calls, which is what makes a stream decoded
            // block by block identical to the same stream decoded in one go.
            const auto& kernel = hilbert_kernel_as<Scalar>();
            shifter.history.resize(static_cast<std::size_t>(kHilbertTaps) - 1U, 0.0F);
            shifter.direct_history.resize(2);
            for (auto& history : shifter.direct_history) {
                history.resize(static_cast<std::size_t>(kHilbertDelay), 0.0F);
            }
            delay_direct(std::span<float>(left, count), shifter.direct_history[0], shifter.scratch);
            delay_direct(std::span<float>(right, count), shifter.direct_history[1],
                         shifter.scratch);

            // The shifted sum, convolved across the call boundary through the
            // carried history. Even taps are exactly zero (see the kernel's own
            // comment), so only the odd ones are visited.
            const std::size_t history_size = shifter.history.size();
            const auto sample_at = [&](std::ptrdiff_t index) -> Scalar {
                if (index >= 0) {
                    return static_cast<Scalar>(surround[static_cast<std::size_t>(index)]);
                }
                const auto back = static_cast<std::size_t>(-index);
                return back <= history_size
                           ? static_cast<Scalar>(shifter.history[history_size - back])
                           : Scalar{0};
            };
            for (std::size_t i = 0; i < count; ++i) {
                Scalar shifted{0};
                // The kernel's non-zero taps sit at EVEN indices, not odd
                // ones: a tap is zero for even n, and n is (index -
                // kHilbertDelay) with kHilbertDelay itself odd, so the parity
                // flips between the two. Walking the odd indices instead
                // visits nothing but zeros, which is a silent failure - a
                // shift of zero still produces plausible Lt/Rt audio, just
                // with the surround left in phase.
                for (int tap = 0; tap < kHilbertTaps; tap += 2) {
                    shifted += kernel[static_cast<std::size_t>(tap)] *
                               sample_at(static_cast<std::ptrdiff_t>(i) - tap);
                }
                // Lt takes the shifted surround negated and Rt positive - the
                // 180-degree relationship a Dolby Surround decoder recovers the
                // surround channel from.
                left[i] -= static_cast<float>(shifted);
                right[i] += static_cast<float>(shifted);
            }
            const std::size_t keep = std::min(history_size, count);
            if (history_size > keep) {
                std::copy(shifter.history.begin() + static_cast<std::ptrdiff_t>(keep),
                          shifter.history.end(), shifter.history.begin());
            }
            std::memcpy(shifter.history.data() + (history_size - keep), surround + (count - keep),
                        keep * sizeof(float));
        } else {
            for (std::size_t i = 0; i < count; ++i) {
                left[i] -= surround[i];
                right[i] += surround[i];
            }
        }
    }

    if (plan.lfe) {
        const auto& source = inputs[plan.lfe_channel];
        const float* const lfe = source.data() + offset;
        const std::size_t n = available(source, offset, count);
        for (std::size_t i = 0; i < n; ++i) {
            const auto contribution = static_cast<float>(plan.lfe_gain * static_cast<Scalar>(lfe[i]));
            left[i] += contribution;
            if (plan.stereo) {
                right[i] += contribution;
            }
        }
    }
}

// The largest magnitude in a finished block, folded into `peak`: RF mode's
// scan, taken a block at a time because the fold's outputs are only all in
// one place once the frame is done.
void scan_peak(Scalar& peak, const float* left, const float* right, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        peak = std::max(peak, internal::scalar_abs(static_cast<Scalar>(left[i])));
        if (right != nullptr) {
            peak = std::max(peak, internal::scalar_abs(static_cast<Scalar>(right[i])));
        }
    }
}

// §7.7's ceiling, extended to whichever fold was actually asked for.
//
// §7.8.1's normalisation already bounds a fold by the largest coded sample, so
// on its own the matrix cannot overload. What can is the LFE - deliberately
// outside that normalisation, since §7.8 treats its contribution as an
// addition at up to +10 dB - and a compr word whose ceiling was computed for
// the MONO downmix rather than for this one. RF mode is the mode that promises
// neither happens.
//
// The gain is derived per frame from that frame's own peak and then RAMPED
// across the frame rather than stepped at its boundary, because a step in gain
// is a click. Ramping means the bound is not quite exact where a peak lands
// early in a frame that has just got much louder, so a final clamp backs it
// up: the ramp is what keeps the limiter inaudible, the clamp is what makes the
// ceiling true. `right` is empty for a mono fold, and for a stereo fold with
// nowhere to put its right channel - whose peak `peak` has counted anyway.
void limit_frame(const OutputConfig& config, double& protection_gain, Scalar scanned,
                 std::span<float> left, std::span<float> right, std::size_t length) {
    const auto peak = static_cast<double>(scanned);
    double target = 1.0;
    if (peak > config.rf_ceiling && peak > 0.0) {
        target = config.rf_ceiling / peak;
    }
    // Recover towards unity no faster than 0.5 dB per frame (about 15 dB per
    // second at 48 kHz), so one loud transient does not leave the whole
    // programme audibly ducked and a sustained one does not pump.
    constexpr double kReleasePerFrame = 1.0594630943592953;  // 0.5 dB
    const double released = std::min(1.0, protection_gain * kReleasePerFrame);
    const double frame_gain = std::min(target, released);
    const double start = protection_gain;
    // The ramp and the clamp per sample, in the decode scalar; the frame's own
    // gains above stay double.
    const auto ramp_start = static_cast<Scalar>(start);
    const auto ramp_end = static_cast<Scalar>(frame_gain);
    const auto ceiling = static_cast<Scalar>(config.rf_ceiling);
    // The position along the ramp per sample. The floating tiers divide by the
    // length as they always did; the fixed one cannot hold a sample count
    // (Q7.24 stops at 128), so it walks the ramp in steps sized once in double.
    [[maybe_unused]] const auto span = static_cast<Scalar>(length);
    [[maybe_unused]] const auto step = static_cast<Scalar>(
        (frame_gain - start) / static_cast<double>(std::max<std::size_t>(length, 2) - 1));
    [[maybe_unused]] Scalar walked = ramp_start;
    for (std::size_t i = 0; i < length; ++i) {
        Scalar gain{};
        if constexpr (std::is_same_v<Scalar, internal::Fixed32>) {
            gain = walked;
            walked += step;
        } else {
            const Scalar t =
                span > Scalar{1} ? static_cast<Scalar>(i) / (span - Scalar{1}) : Scalar{1};
            gain = ramp_start + (ramp_end - ramp_start) * t;
        }
        const auto limited = [&](float sample) {
            return static_cast<float>(
                std::clamp(static_cast<Scalar>(sample) * gain, -ceiling, ceiling));
        };
        left[i] = limited(left[i]);
        if (!right.empty()) {
            right[i] = limited(right[i]);
        }
    }
    protection_gain = frame_gain;
}

// Where each Table E2.5 location folds to, when a program has to be reduced
// to one of §7.8's own acmod layouts before §7.8 can fold it at all.
//
// This IS an extension beyond §7.8, and worth being plain about: §7.8 defines
// folds FROM the eight AC-3 acmods and says nothing about the wide layouts
// Annex E's chanmap can express - a 7.1.4 program has no §7.8 fold, because
// §7.8 predates anything that could code one. Reducing first is the least
// invented thing available: every extra location has an obvious §7.8 seat (a
// wide left is a left, a rear surround is a surround, a top front left is a
// left), and once it is in that seat the actual downmix is the spec's own,
// with the stream's own levels. The alternative - dropping the channels §7.8
// cannot name - would silently discard the whole height layer.
//
// -3 dB on every secondary contribution, so two locations sharing one seat
// sum to the power of one. A location already IN the reduced layout arrives
// at unity, which makes the reduction an exact identity for every plain
// acmod bed - the overwhelmingly common case, and the one that must not
// change by so much as a bit.
enum class Seat : std::uint8_t { kLeft, kCentre, kRight, kLeftSurround, kRightSurround, kLfe };

struct SeatMix {
    Seat first = Seat::kLeft;
    double first_gain = 1.0;
    // A second seat, for the locations with no side of their own: a mono
    // surround or a top surround belongs equally to both surrounds.
    bool has_second = false;
    Seat second = Seat::kRight;
    double second_gain = 0.0;
};

SeatMix seat_of(eac3::chanmap::Location location) {
    using L = eac3::chanmap::Location;
    constexpr double kHalfPower = meta::level::kMinus3dB;
    switch (location) {
        case L::kLeft: return {.first = Seat::kLeft};
        case L::kCentre: return {.first = Seat::kCentre};
        case L::kRight: return {.first = Seat::kRight};
        case L::kLeftSurround: return {.first = Seat::kLeftSurround};
        case L::kRightSurround: return {.first = Seat::kRightSurround};
        case L::kLfe: return {.first = Seat::kLfe};
        // Front pairs inside the mains, the wides and the front heights: all
        // left or right, at the shared-seat level.
        case L::kLc:
        case L::kLw:
        case L::kVhl: return {.first = Seat::kLeft, .first_gain = kHalfPower};
        case L::kRc:
        case L::kRw:
        case L::kVhr: return {.first = Seat::kRight, .first_gain = kHalfPower};
        case L::kVhc: return {.first = Seat::kCentre, .first_gain = kHalfPower};
        // Rear, side and top surrounds keep their side.
        case L::kLrs:
        case L::kLsd:
        case L::kLts: return {.first = Seat::kLeftSurround, .first_gain = kHalfPower};
        case L::kRrs:
        case L::kRsd:
        case L::kRts: return {.first = Seat::kRightSurround, .first_gain = kHalfPower};
        // A mono surround and a top (overhead centre) surround have no side,
        // so they go to both - which for the 2/1 and 3/1 beds reproduces
        // §7.8's own single-surround branch exactly, since slev then reaches
        // each front through this -3 dB rather than through the branch's own.
        case L::kCs:
        case L::kTs:
            return {.first = Seat::kLeftSurround,
                    .first_gain = kHalfPower,
                    .has_second = true,
                    .second = Seat::kRightSurround,
                    .second_gain = kHalfPower};
        // §E2.3.1.8's second LFE joins the first.
        case L::kLfe2: return {.first = Seat::kLfe, .first_gain = kHalfPower};
    }
    return {.first = Seat::kLeft, .first_gain = 0.0};
}

// Which acmod the occupied seats amount to, so §7.8's own coefficients and
// normalisation apply to the layout that is actually there rather than to a
// 3/2 with silent channels in it.
Acmod reduced_acmod(bool centre, bool mains, bool surrounds) {
    if (!mains) {
        return Acmod::k1_0;  // a centre-only program; nothing else can reach here
    }
    if (centre) {
        return surrounds ? Acmod::k3_2 : Acmod::k3_0;
    }
    return surrounds ? Acmod::k2_2 : Acmod::k2_0;
}

}  // namespace

MixLevels mix_levels(std::optional<meta::CentreMixLevel> cmixlev,
                     std::optional<meta::SurroundMixLevel> surmixlev) {
    MixLevels out;
    if (cmixlev.has_value()) {
        out.loro_clev = meta::coefficient(*cmixlev);
    }
    if (surmixlev.has_value()) {
        out.loro_slev = meta::coefficient(*surmixlev);
    }
    // AC-3 has no separate Lt/Rt levels. §7.8.2's own -3 dB is the right
    // stand-in for the centre, but a stream that explicitly dropped its
    // surrounds from the downmix (§5.4.2.5's '10') meant that for any fold and
    // not only the plain one - carrying it across is the only reading that
    // does not put back channels the operator deliberately removed.
    if (surmixlev.has_value() && *surmixlev == meta::SurroundMixLevel::kSilent) {
        out.ltrt_slev = meta::level::kSilent;
    }
    return out;
}

MixLevels mix_levels(const std::optional<meta::MixMetadata>& mix) {
    MixLevels out;
    if (!mix.has_value()) {
        return out;
    }
    out.loro_clev = meta::coefficient(mix->lorocmixlev);
    out.loro_slev = meta::coefficient(mix->lorosurmixlev);
    out.ltrt_clev = meta::coefficient(mix->ltrtcmixlev);
    out.ltrt_slev = meta::coefficient(mix->ltrtsurmixlev);
    out.preferred = mix->dmixmod;
    // §E2.3.1.10: an absent lfemixlevcod is not "use the default", it is "LFE
    // mixing is disabled" - a decision the encoder made, which
    // OutputConfig::mix_lfe deliberately cannot talk it out of.
    out.lfe_mix_level_db = mix->lfemixlevcod
                               ? std::optional{meta::lfe_mix_level_db(*mix->lfemixlevcod)}
                               : std::nullopt;
    return out;
}

std::size_t output_channel_count(const OutputConfig& config, Acmod acmod, bool lfe) {
    const auto coded = static_cast<std::size_t>(fullbw_channel_count(acmod)) + (lfe ? 1U : 0U);
    if (config.target == DownmixTarget::kAsCoded || acmod == Acmod::kDualMono) {
        return coded;
    }
    return config.target == DownmixTarget::kMono ? 1U : 2U;
}

int OutputStage::latency_samples() const {
    return config_.target == DownmixTarget::kLtRt && config_.ltrt_phase_shift ? kHilbertDelay : 0;
}

double OutputStage::rf_protection_db() const { return meta::to_db(protection_gain_); }

void OutputStage::reset() {
    shift_history_.clear();
    direct_history_.clear();
    delay_scratch_.clear();
    protection_gain_ = 1.0;
}

void OutputStage::apply(std::vector<std::vector<float>>& channels, Acmod acmod, bool lfe,
                        const MixLevels& levels, int dialnorm) {
    // The span form below is the whole implementation; this one only lends it
    // views of the vectors and then trims them to what the fold left behind.
    views_.clear();
    views_.reserve(channels.size());
    for (auto& channel : channels) {
        views_.emplace_back(channel);
    }
    apply(views_, acmod, lfe, levels, dialnorm);
    if (!channels.empty()) {
        channels.resize(output_channel_count(config_, acmod, lfe));
    }
}

void OutputStage::apply(std::span<const std::span<float>> channels, Acmod acmod, bool lfe,
                        const MixLevels& levels, int dialnorm) {
    const bool downmixing =
        config_.target != DownmixTarget::kAsCoded && acmod != Acmod::kDualMono;
    const bool normalising = config_.apply_dialnorm || config_.mode != OperatingMode::kCustom;
    if (!downmixing && !normalising) {
        return;
    }
    if (channels.empty() || channels.front().empty()) {
        return;
    }
    const std::size_t length = channels.front().size();

    // §5.4.2.8 first. dialnorm describes the CODED programme, so normalising
    // before the fold or after it gives the same answer for a linear matrix -
    // but not once RF mode's limiter is in the chain, which reacts to level.
    // Doing it here means the limiter sees the levels a listener would, which
    // is the only order in which its ceiling means anything.
    const double dialnorm_gain = normalising ? meta::dialnorm_gain(dialnorm) : 1.0;
    if (dialnorm_gain != 1.0) {
        AC3_ZONE_SCOPED_N("output_dialnorm");
        const auto gain = static_cast<Scalar>(dialnorm_gain);
        for (const auto& channel : channels) {
            scale(channel.data(), gain, channel.size());
        }
    }
    if (!downmixing) {
        return;
    }

    // The fold lands in the caller's own first one or two channels, which are
    // also two of its inputs - so each block is folded into out_left_ and
    // out_right_ and copied over its inputs only once it is complete, by which
    // point nothing still to come reads those samples. out_left_/out_right_
    // keep their capacity for the next frame, which is what keeps a
    // steady-state decode off the heap.
    const FoldPlan plan = plan_fold(config_, channels.size(), acmod, lfe, levels);
    out_left_.resize(kFoldBlock);
    if (plan.stereo) {
        out_right_.resize(kFoldBlock);
    }
    if (plan.ltrt) {
        surround_sum_.resize(kFoldBlock);
    }
    const bool rf = config_.mode == OperatingMode::kRf;
    const bool right_out = plan.stereo && channels.size() > 1;
    Scalar peak{0};
    for (std::size_t offset = 0; offset < length; offset += kFoldBlock) {
        const std::size_t count = std::min(kFoldBlock, length - offset);
        {
            AC3_ZONE_SCOPED_N("output_fold");
            fold_block(plan, config_, channels, offset, count, out_left_.data(),
                       plan.stereo ? out_right_.data() : nullptr, surround_sum_,
                       {shift_history_, direct_history_, delay_scratch_});
        }
        if (rf) {
            scan_peak(peak, out_left_.data(), plan.stereo ? out_right_.data() : nullptr, count);
        }
        // std::memcpy, not std::copy: the two ranges never overlap, and on the
        // ESP32-S3 std::copy lowers to the mask ROM's memmove, which moves
        // some twelve cycles a byte to memcpy's one.
        std::memcpy(channels[0].data() + offset, out_left_.data(), count * sizeof(float));
        if (right_out) {
            std::memcpy(channels[1].data() + offset, out_right_.data(), count * sizeof(float));
        }
    }
    if (rf) {
        AC3_ZONE_SCOPED_N("output_rf_limiter");
        limit_frame(config_, protection_gain_, peak, channels[0].first(length),
                    right_out ? channels[1].first(length) : std::span<float>{}, length);
    }
}

void OutputStage::apply(std::span<const std::span<float>> channels,
                        const eac3::chanmap::Layout& layout, Acmod acmod, bool lfe,
                        const MixLevels& levels, int dialnorm) {
    if (channels.empty() || channels.front().empty()) {
        return;
    }
    // Dual mono has no layout to reduce and no fold to apply (OutputStage
    // refuses it outright); a caller asking only for dialnorm normalisation
    // still gets it, which is what passing straight through does. `lfe` is
    // only consulted on this path - past it, what matters is which seat the
    // rendered layout actually filled, not what the bed's lfeon said.
    if (config_.target == DownmixTarget::kAsCoded || acmod == Acmod::kDualMono ||
        layout.count == 0) {
        apply(channels, acmod, lfe, levels, dialnorm);
        return;
    }

    // Seat every rendered location, then fold the seats. See seat_of above
    // for why this step exists and what it does and does not claim.
    const std::size_t length = channels.front().size();
    const auto count = std::min(static_cast<std::size_t>(layout.count), channels.size());
    std::array<SeatMix, eac3::chanmap::kMaxChannels> seats{};
    std::array<bool, 6> occupied{};
    for (std::size_t i = 0; i < count; ++i) {
        seats[i] = seat_of(layout[static_cast<int>(i)]);
        occupied[static_cast<std::size_t>(seats[i].first)] = true;
        if (seats[i].has_second) {
            occupied[static_cast<std::size_t>(seats[i].second)] = true;
        }
    }
    const bool has_centre = occupied[static_cast<std::size_t>(Seat::kCentre)];
    const bool has_mains = occupied[static_cast<std::size_t>(Seat::kLeft)] ||
                           occupied[static_cast<std::size_t>(Seat::kRight)];
    const bool has_surrounds = occupied[static_cast<std::size_t>(Seat::kLeftSurround)] ||
                               occupied[static_cast<std::size_t>(Seat::kRightSurround)];
    const bool has_lfe = occupied[static_cast<std::size_t>(Seat::kLfe)];
    const Acmod folded = reduced_acmod(has_centre, has_mains, has_surrounds);

    // A block of each seat, and views onto them in Table 5.8 coded order for
    // `folded`, which is the order the fold reads. A centre-only program has
    // its audio in the centre seat, and 1/0 codes that one channel at index 0.
    fold_scratch_.resize(6);
    for (auto& seat : fold_scratch_) {
        seat.resize(kFoldBlock);
    }
    fold_views_.clear();
    std::array<bool, 6> lent{};
    const auto lend = [&](Seat seat) {
        lent[static_cast<std::size_t>(seat)] = true;
        fold_views_.emplace_back(fold_scratch_[static_cast<std::size_t>(seat)]);
    };
    if (folded == Acmod::k1_0) {
        lend(Seat::kCentre);
    } else {
        lend(Seat::kLeft);
        if (has_centre) {
            lend(Seat::kCentre);
        }
        lend(Seat::kRight);
        if (has_surrounds) {
            lend(Seat::kLeftSurround);
            lend(Seat::kRightSurround);
        }
    }
    if (has_lfe) {
        lend(Seat::kLfe);
    }

    // The seats are §7.8's input, so dialnorm and the fold apply to them as
    // they would to any coded programme - and they are this stage's own
    // storage, so the fold goes straight into the caller's first one or two
    // channels, a block at a time, each block's inputs read before its
    // outputs land.
    const FoldPlan plan = plan_fold(config_, fold_views_.size(), folded, has_lfe, levels);
    const bool normalising = config_.apply_dialnorm || config_.mode != OperatingMode::kCustom;
    const double dialnorm_gain = normalising ? meta::dialnorm_gain(dialnorm) : 1.0;
    if (plan.ltrt) {
        surround_sum_.resize(kFoldBlock);
    }
    // A stereo fold of a one-channel layout still forms its right channel,
    // which RF mode's scan counts, but has nowhere to put it.
    const bool right_out = plan.stereo && channels.size() > 1;
    if (plan.stereo && !right_out) {
        out_right_.resize(kFoldBlock);
    }
    const bool rf = config_.mode == OperatingMode::kRf;
    Scalar peak{0};
    for (std::size_t offset = 0; offset < length; offset += kFoldBlock) {
        const std::size_t block = std::min(kFoldBlock, length - offset);
        {
            AC3_ZONE_SCOPED_N("output_seat");
            for (std::size_t seat = 0; seat < fold_scratch_.size(); ++seat) {
                if (occupied[seat] || lent[seat]) {
                    std::fill_n(fold_scratch_[seat].data(), block, 0.0F);
                }
            }
            const auto pour = [&](Seat seat, double gain, std::span<const float> source) {
                accumulate(fold_scratch_[static_cast<std::size_t>(seat)].data(),
                           source.data() + offset, static_cast<Scalar>(gain),
                           available(source, offset, block));
            };
            for (std::size_t i = 0; i < count; ++i) {
                pour(seats[i].first, seats[i].first_gain, channels[i]);
                if (seats[i].has_second) {
                    pour(seats[i].second, seats[i].second_gain, channels[i]);
                }
            }
        }
        if (dialnorm_gain != 1.0) {
            AC3_ZONE_SCOPED_N("output_dialnorm");
            const auto gain = static_cast<Scalar>(dialnorm_gain);
            for (const auto& seat : fold_views_) {
                scale(seat.data(), gain, block);
            }
        }
        float* const left = channels[0].data() + offset;
        float* const right = right_out ? channels[1].data() + offset
                                       : (plan.stereo ? out_right_.data() : nullptr);
        {
            AC3_ZONE_SCOPED_N("output_fold");
            fold_block(plan, config_, fold_views_, 0, block, left, right, surround_sum_,
                       {shift_history_, direct_history_, delay_scratch_});
        }
        if (rf) {
            scan_peak(peak, left, right, block);
        }
    }
    if (rf) {
        AC3_ZONE_SCOPED_N("output_rf_limiter");
        limit_frame(config_, protection_gain_, peak, channels[0].first(length),
                    right_out ? channels[1].first(length) : std::span<float>{}, length);
    }
}


}  // namespace ac3
