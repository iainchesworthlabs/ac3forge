#include "ac3/decoder/output.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/eac3_tables.hpp"  // eac3::chanmap::Location/Layout
#include "ac3/core/tables.hpp"
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
    if (cmixlev) {
        out.loro_clev = meta::coefficient(*cmixlev);
    }
    if (surmixlev) {
        out.loro_slev = meta::coefficient(*surmixlev);
    }
    // AC-3 has no separate Lt/Rt levels. §7.8.2's own -3 dB is the right
    // stand-in for the centre, but a stream that explicitly dropped its
    // surrounds from the downmix (§5.4.2.5's '10') meant that for any fold and
    // not only the plain one - carrying it across is the only reading that
    // does not put back channels the operator deliberately removed.
    if (surmixlev && *surmixlev == meta::SurroundMixLevel::kSilent) {
        out.ltrt_slev = meta::level::kSilent;
    }
    return out;
}

MixLevels mix_levels(const std::optional<meta::MixMetadata>& mix) {
    MixLevels out;
    if (!mix) {
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
    const auto nfchans = static_cast<std::size_t>(fullbw_channel_count(acmod));
    const std::size_t lfe_index = nfchans;
    const bool have_lfe = lfe && channels.size() > lfe_index;

    // §5.4.2.8 first. dialnorm describes the CODED programme, so normalising
    // before the fold or after it gives the same answer for a linear matrix -
    // but not once RF mode's limiter is in the chain, which reacts to level.
    // Doing it here means the limiter sees the levels a listener would, which
    // is the only order in which its ceiling means anything.
    const double dialnorm_gain = normalising ? meta::dialnorm_gain(dialnorm) : 1.0;
    if (dialnorm_gain != 1.0) {
        for (const auto& channel : channels) {
            for (float& sample : channel) {
                sample = static_cast<float>(static_cast<double>(sample) * dialnorm_gain);
            }
        }
    }
    if (!downmixing) {
        return;
    }

    // The LFE's contribution, when it is wanted AND the stream did not
    // disable it. §7.8's ideal is +10 dB relative to left and right; an
    // E-AC-3 stream carrying its own lfemixlevcod overrides that with
    // whatever it chose.
    const double lfe_gain = (config_.mix_lfe && have_lfe && levels.lfe_mix_level_db)
                                ? meta::lfe_mix_gain(*levels.lfe_mix_level_db)
                                : 0.0;

    const bool stereo = config_.target != DownmixTarget::kMono;
    out_left_.assign(length, 0.0F);
    if (stereo) {
        out_right_.assign(length, 0.0F);
    }

    // The three folds differ only in which coefficients they use and whether a
    // phase-shifted path exists at all, so the accumulation is written once
    // over whichever set was built.
    const auto accumulate = [&](const std::array<double, 5>& coeffs, std::vector<float>& out) {
        for (std::size_t ch = 0; ch < nfchans && ch < channels.size() && ch < coeffs.size();
             ++ch) {
            const double gain = coeffs[ch];
            if (gain == 0.0) {
                continue;
            }
            const auto& source = channels[ch];
            const std::size_t n = std::min(length, source.size());
            for (std::size_t i = 0; i < n; ++i) {
                out[i] += static_cast<float>(gain * static_cast<double>(source[i]));
            }
        }
    };

    if (config_.target == DownmixTarget::kMono) {
        accumulate(meta::mono_downmix(acmod, levels.loro_clev, levels.loro_slev), out_left_);
    } else if (config_.target == DownmixTarget::kLoRo) {
        const auto coeffs = meta::stereo_downmix(acmod, levels.loro_clev, levels.loro_slev);
        accumulate(coeffs.left, out_left_);
        accumulate(coeffs.right, out_right_);
    } else {
        const auto coeffs = meta::ltrt_downmix(acmod, levels.ltrt_clev, levels.ltrt_slev);
        accumulate(coeffs.direct.left, out_left_);
        accumulate(coeffs.direct.right, out_right_);

        // The surround sum, formed once and then shifted once: §7.8.2 puts ONE
        // surround signal into the matrix, so summing first and filtering the
        // sum is not an optimisation over filtering each channel, it is the
        // correct order. It also halves the filter work for a 3/2 source.
        //
        // §5.4.2.5's '10' - surrounds dropped from the downmix altogether -
        // arrives as a coefficient of exactly zero and needs no special case:
        // the sum stays silent and the shifter filters silence.
        surround_sum_.assign(length, 0.0F);
        const Positions positions = surround_positions(acmod);
        for (const int position : {positions.first, positions.second}) {
            if (position < 0 || static_cast<std::size_t>(position) >= channels.size()) {
                continue;
            }
            // ltrt_downmix normalises, so the sum has to carry whatever
            // attenuation the direct path just got. Reading the coefficient
            // back out rather than re-deriving it from levels.ltrt_slev keeps
            // the two paths in step whatever normalisation decided.
            const double gain = coeffs.surround[static_cast<std::size_t>(position)];
            const auto& source = channels[static_cast<std::size_t>(position)];
            const std::size_t n = std::min(length, source.size());
            for (std::size_t i = 0; i < n; ++i) {
                surround_sum_[i] += static_cast<float>(gain * static_cast<double>(source[i]));
            }
        }

        if (config_.ltrt_phase_shift) {
            // Delay the direct path by the filter's own group delay, then
            // filter the surround sum, so the two arrive together. Both delay
            // lines carry across frames, which is what makes a stream decoded
            // frame by frame identical to the same stream decoded in one go.
            const auto& kernel = hilbert_kernel();
            shift_history_.resize(static_cast<std::size_t>(kHilbertTaps) - 1U, 0.0F);
            direct_history_.resize(2);
            for (auto& history : direct_history_) {
                history.resize(static_cast<std::size_t>(kHilbertDelay), 0.0F);
            }

            // out'[i] = (history ++ out)[i], with the last kHilbertDelay
            // samples of that same concatenation becoming the next frame's
            // history. delay_scratch_ is a member rather than a local so this
            // allocates nothing after the first frame: the swap at the end
            // leaves it holding the buffer `history` had, which is the same
            // size, so the assign() never reallocates either.
            const auto delay_direct = [&](std::vector<float>& out, std::vector<float>& history) {
                const std::size_t depth = history.size();
                delay_scratch_.assign(depth, 0.0F);
                const std::size_t keep = std::min(depth, length);
                std::copy(out.end() - static_cast<std::ptrdiff_t>(keep), out.end(),
                          delay_scratch_.end() - static_cast<std::ptrdiff_t>(keep));
                if (keep < depth) {
                    // A frame shorter than the delay line: the older tail has
                    // not aged all the way out yet, and shuffles along.
                    std::copy(history.begin() + static_cast<std::ptrdiff_t>(keep), history.end(),
                              delay_scratch_.begin());
                }
                for (std::size_t i = length; i-- > depth;) {
                    out[i] = out[i - depth];
                }
                for (std::size_t i = 0; i < keep; ++i) {
                    out[i] = history[i];
                }
                history.swap(delay_scratch_);
            };
            delay_direct(out_left_, direct_history_[0]);
            delay_direct(out_right_, direct_history_[1]);

            // The shifted sum, convolved across the frame boundary through the
            // carried history. Even taps are exactly zero (see the kernel's own
            // comment), so only the odd ones are visited.
            const std::size_t history_size = shift_history_.size();
            const auto sample_at = [&](std::ptrdiff_t index) -> double {
                if (index >= 0) {
                    return static_cast<double>(surround_sum_[static_cast<std::size_t>(index)]);
                }
                const auto back = static_cast<std::size_t>(-index);
                return back <= history_size
                           ? static_cast<double>(shift_history_[history_size - back])
                           : 0.0;
            };
            for (std::size_t i = 0; i < length; ++i) {
                double shifted = 0.0;
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
                out_left_[i] -= static_cast<float>(shifted);
                out_right_[i] += static_cast<float>(shifted);
            }
            const std::size_t keep = std::min(history_size, length);
            if (history_size > keep) {
                std::copy(shift_history_.begin() + static_cast<std::ptrdiff_t>(keep),
                          shift_history_.end(), shift_history_.begin());
            }
            std::copy(surround_sum_.end() - static_cast<std::ptrdiff_t>(keep), surround_sum_.end(),
                      shift_history_.end() - static_cast<std::ptrdiff_t>(keep));
        } else {
            for (std::size_t i = 0; i < length; ++i) {
                out_left_[i] -= surround_sum_[i];
                out_right_[i] += surround_sum_[i];
            }
        }
    }

    if (lfe_gain != 0.0) {
        const auto& source = channels[lfe_index];
        const std::size_t n = std::min(length, source.size());
        for (std::size_t i = 0; i < n; ++i) {
            const auto contribution =
                static_cast<float>(lfe_gain * static_cast<double>(source[i]));
            out_left_[i] += contribution;
            if (stereo) {
                out_right_[i] += contribution;
            }
        }
    }

    // §7.7.2's ceiling, extended to whichever fold was actually asked for.
    //
    // §7.8.1's normalisation already bounds a fold by the largest coded
    // sample, so on its own the matrix cannot overload. What can is the LFE -
    // deliberately outside that normalisation, since §7.8 treats its
    // contribution as an addition at up to +10 dB - and a compr word whose
    // ceiling was computed for the MONO downmix rather than for this one. RF
    // mode is the mode that promises neither happens.
    //
    // The gain is derived per frame from that frame's own peak and then RAMPED
    // across the frame rather than stepped at its boundary, because a step in
    // gain is a click. Ramping means the bound is not quite exact where a peak
    // lands early in a frame that has just got much louder, so a final clamp
    // backs it up: the ramp is what keeps the limiter inaudible, the clamp is
    // what makes the ceiling true.
    if (config_.mode == OperatingMode::kRf) {
        double peak = 0.0;
        for (std::size_t i = 0; i < length; ++i) {
            peak = std::max(peak, std::abs(static_cast<double>(out_left_[i])));
            if (stereo) {
                peak = std::max(peak, std::abs(static_cast<double>(out_right_[i])));
            }
        }
        double target = 1.0;
        if (peak > config_.rf_ceiling && peak > 0.0) {
            target = config_.rf_ceiling / peak;
        }
        // Recover towards unity no faster than 0.5 dB per frame (about 15 dB
        // per second at 48 kHz), so one loud transient does not leave the whole
        // programme audibly ducked and a sustained one does not pump.
        constexpr double kReleasePerFrame = 1.0594630943592953;  // 0.5 dB
        const double released = std::min(1.0, protection_gain_ * kReleasePerFrame);
        const double frame_gain = std::min(target, released);
        const double start = protection_gain_;
        const auto span = static_cast<double>(length);
        for (std::size_t i = 0; i < length; ++i) {
            const double t = span > 1.0 ? static_cast<double>(i) / (span - 1.0) : 1.0;
            const double gain = start + (frame_gain - start) * t;
            const auto limited = [&](float sample) {
                return static_cast<float>(std::clamp(static_cast<double>(sample) * gain,
                                                     -config_.rf_ceiling, config_.rf_ceiling));
            };
            out_left_[i] = limited(out_left_[i]);
            if (stereo) {
                out_right_[i] = limited(out_right_[i]);
            }
        }
        protection_gain_ = frame_gain;
    }

    // The fold lands back in the caller's own first one or two channels. A
    // copy rather than a swap, because the span form cannot own storage - and
    // out_left_/out_right_ keep their capacity for the next frame either way,
    // which is what actually keeps a steady-state decode off the heap.
    std::copy(out_left_.begin(), out_left_.end(), channels[0].begin());
    if (stereo && channels.size() > 1) {
        std::copy(out_right_.begin(), out_right_.end(), channels[1].begin());
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
    fold_scratch_.resize(6);
    for (auto& seat : fold_scratch_) {
        seat.assign(length, 0.0F);
    }
    std::array<bool, 6> occupied{};
    const auto pour = [&](Seat seat, double gain, std::span<const float> source) {
        const auto index = static_cast<std::size_t>(seat);
        occupied[index] = true;
        auto& target = fold_scratch_[index];
        const std::size_t n = std::min(length, source.size());
        for (std::size_t i = 0; i < n; ++i) {
            target[i] += static_cast<float>(gain * static_cast<double>(source[i]));
        }
    };
    const auto count = std::min(static_cast<std::size_t>(layout.count), channels.size());
    for (std::size_t i = 0; i < count; ++i) {
        const SeatMix seat = seat_of(layout[static_cast<int>(i)]);
        pour(seat.first, seat.first_gain, channels[i]);
        if (seat.has_second) {
            pour(seat.second, seat.second_gain, channels[i]);
        }
    }

    const bool has_centre = occupied[static_cast<std::size_t>(Seat::kCentre)];
    const bool has_mains = occupied[static_cast<std::size_t>(Seat::kLeft)] ||
                           occupied[static_cast<std::size_t>(Seat::kRight)];
    const bool has_surrounds = occupied[static_cast<std::size_t>(Seat::kLeftSurround)] ||
                               occupied[static_cast<std::size_t>(Seat::kRightSurround)];
    const bool has_lfe = occupied[static_cast<std::size_t>(Seat::kLfe)];
    const Acmod folded = reduced_acmod(has_centre, has_mains, has_surrounds);

    // Table 5.8 coded order for `folded`, which is the order OutputStage
    // reads. A centre-only program has its audio in the centre seat, and 1/0
    // codes that one channel at index 0.
    fold_views_.clear();
    const auto lend = [&](Seat seat) {
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
    apply(fold_views_, folded, has_lfe, levels, dialnorm);

    const std::size_t produced = output_channel_count(config_, folded, has_lfe);
    for (std::size_t i = 0; i < produced && i < channels.size(); ++i) {
        std::copy(fold_views_[i].begin(), fold_views_[i].end(), channels[i].begin());
    }
}


}  // namespace ac3
