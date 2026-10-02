#include "pcm/companding.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "iclforge/ac4core/dsp/real_functions.hpp"
#include "iclforge/ac4core/dsp/scalar_traits.hpp"

namespace iclforge::ac4::detail {
namespace {

// A level, a gain: Real at double and float, a mantissa and a power of two at
// Fixed32 (dsp/scalar_traits.hpp).
using Energy = dsp::Energy<Real>;

constexpr Energy kAlpha = Energy(0.65);
constexpr std::size_t kSubbands = 64;
// Q_low's slots: num_qmf_timeslots + ts_offset_hfgen, at most 32 + 6.
constexpr int kMaxSlots = 64;

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

[[nodiscard]] std::span<QmfValue> q_low_slot(const CompandingChannel& channel, int ts) noexcept {
    return channel.ext.subspan(at(ts + aspx::kTsOffsetHfadj) * kSubbands, kSubbands);
}

// L(ts) against `full_scale`: 0.9105 times the mean over [sb0, sb1) of
// max(|Re|, |Im|) + min(|Re|, |Im|) / 2. At Fixed32 the sum is of the raw
// values, exactly, in 64 bits.
template <typename R>
[[nodiscard]] dsp::Energy<R> slot_level(std::span<const dsp::Complex<R>> slot, int sb0, int sb1,
                                        R full_scale) noexcept {
    if constexpr (dsp::kFixed<R>) {
        if (sb1 <= sb0) {
            return dsp::MantExp{};
        }
        std::int64_t twice = 0;
        for (int sb = sb0; sb < sb1; ++sb) {
            const std::int64_t re = std::abs(static_cast<std::int64_t>(slot[at(sb)].re.raw));
            const std::int64_t im = std::abs(static_cast<std::int64_t>(slot[at(sb)].im.raw));
            twice += 2 * std::max(re, im) + std::min(re, im);
        }
        const dsp::MantExp sum = dsp::MantExp::make(twice, -1 - R::kFractionBits);
        return dsp::MantExp(0.9105) * sum / dsp::MantExp{sb1 - sb0} / dsp::MantExp{full_scale};
    } else {
        if (sb1 <= sb0) {
            return R{} / full_scale;
        }
        R sum{};
        for (int sb = sb0; sb < sb1; ++sb) {
            const R re = std::abs(slot[at(sb)].real());
            const R im = std::abs(slot[at(sb)].imag());
            sum += std::max(re, im) + R(0.5) * std::min(re, im);
        }
        return R(0.9105) * sum / static_cast<R>(sb1 - sb0) / full_scale;
    }
}

// L^((1 - alpha) / alpha). The text prints the average gain's exponent as
// "1alpha / alpha"; it is read as the per-slot gain's (src/ac4dec/ERRATA.md,
// "The companding average").
//
// At double this is std::pow, as it always was. At float it is
// 2^(e log2 L) through iclforge::internal's scalar_exp2 and scalar_log2, which are
// plain float multiplies and adds (dsp::pow_of): the C libraries' powf differ
// in the last bit on some inputs, a gain that differs in its last bit scales
// the slot's samples by a different float, and the synthesis bank spreads the
// difference over the frame, so that the host, the Cortex-M3 leg and the
// ESP32s each gave a PCM of their own for a companded stream (planning/ac4.md,
// D14a4). A slot with no level gets no gain, as pow gives it.
[[nodiscard]] Energy gain_of(Energy level) noexcept {
    return dsp::pow_of(level, (Energy{1} - kAlpha) / kAlpha);
}

template <typename R>
void scale(const CompandingChannel& channel, int sb0, int ts, dsp::Energy<R> factor) noexcept {
    const std::span<QmfValue> slot = q_low_slot(channel, ts);
    for (int sb = sb0; sb < channel.sb1; ++sb) {
        if constexpr (dsp::kFixed<R>) {
            slot[at(sb)] = dsp::apply_gain<R>(factor, slot[at(sb)]);
        } else {
            slot[at(sb)] *= factor;
        }
    }
}

[[nodiscard]] bool fits(const CompandingChannel& channel) noexcept {
    return channel.interval.first >= 0 && channel.interval.last <= kMaxSlots &&
           channel.interval.first < channel.interval.last;
}

}  // namespace

void apply_companding(const CompandingControl& control, int sb0, Real full_scale,
                      std::span<const CompandingChannel> channels) {
    const Energy big_g = dsp::exp2_of(Energy{1} / kAlpha);
    std::vector<std::array<Energy, kMaxSlots>> level(channels.size());
    std::vector<std::array<Energy, kMaxSlots>> gain(channels.size());
    for (std::size_t c = 0; c < channels.size(); ++c) {
        const CompandingChannel& channel = channels[c];
        if (!fits(channel)) {
            continue;
        }
        for (int ts = channel.interval.first; ts < channel.interval.last; ++ts) {
            level[c][at(ts)] = slot_level<Real>(q_low_slot(channel, ts), sb0, channel.sb1, full_scale);
            gain[c][at(ts)] = gain_of(level[c][at(ts)]);
        }
    }

    if (!control.sync_flag) {
        for (std::size_t c = 0; c < channels.size(); ++c) {
            const CompandingChannel& channel = channels[c];
            if (!fits(channel)) {
                continue;
            }
            const int first = channel.interval.first;
            const int last = channel.interval.last;
            if (control.b_compand_on[c]) {
                for (int ts = first; ts < last; ++ts) {
                    scale<Real>(channel, sb0, ts, gain[c][at(ts)] * big_g);
                }
            } else if (control.b_compand_avg) {
                // L_avg over the interval's slots [ts0, ts1), the range 5.7.5.2
                // defines, where the sum prints ts1 as its upper bound.
                Energy sum{};
                for (int ts = first; ts < last; ++ts) {
                    sum += level[c][at(ts)];
                }
                const Energy average = gain_of(sum / static_cast<Energy>(last - first));
                for (int ts = first; ts < last; ++ts) {
                    scale<Real>(channel, sb0, ts, average * big_g);
                }
            }
        }
        return;
    }

    // sync_flag: g_sync(ts) is the channels' mean gain. Where the channels'
    // intervals differ, each slot averages the channels whose interval holds
    // it (src/ac4dec/ERRATA.md, "The companding average").
    std::array<Energy, kMaxSlots> sync{};
    std::array<int, kMaxSlots> count{};
    int first = kMaxSlots;
    int last = 0;
    for (std::size_t c = 0; c < channels.size(); ++c) {
        if (!fits(channels[c])) {
            continue;
        }
        first = std::min(first, channels[c].interval.first);
        last = std::max(last, channels[c].interval.last);
        for (int ts = channels[c].interval.first; ts < channels[c].interval.last; ++ts) {
            sync[at(ts)] += gain[c][at(ts)];
            ++count[at(ts)];
        }
    }
    if (first >= last) {
        return;
    }
    Energy sum{};
    int held = 0;
    for (int ts = first; ts < last; ++ts) {
        if (count[at(ts)] > 0) {
            sync[at(ts)] /= static_cast<Energy>(count[at(ts)]);
            sum += sync[at(ts)];
            ++held;
        }
    }
    const bool on = control.b_compand_on[0];
    if (!on && !control.b_compand_avg) {
        return;
    }
    const Energy average = held > 0 ? sum / static_cast<Energy>(held) : Energy{};
    for (const CompandingChannel& channel : channels) {
        if (!fits(channel)) {
            continue;
        }
        for (int ts = channel.interval.first; ts < channel.interval.last; ++ts) {
            scale<Real>(channel, sb0, ts, (on ? sync[at(ts)] : average) * big_g);
        }
    }
}

}  // namespace iclforge::ac4::detail
