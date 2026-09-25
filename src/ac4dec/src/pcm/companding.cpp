#include "pcm/companding.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ac4::detail {
namespace {

constexpr double kAlpha = 0.65;
constexpr std::size_t kSubbands = 64;
// Q_low's slots: num_qmf_timeslots + ts_offset_hfgen, at most 32 + 6.
constexpr int kMaxSlots = 64;

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

[[nodiscard]] std::span<QmfValue> q_low_slot(const CompandingChannel& channel, int ts) noexcept {
    return channel.ext.subspan(at(ts + aspx::kTsOffsetHfadj) * kSubbands, kSubbands);
}

// L(ts): 0.9105 times the mean over [sb0, sb1) of max(|Re|, |Im|) + min(|Re|,
// |Im|) / 2.
[[nodiscard]] double slot_level(std::span<const QmfValue> slot, int sb0, int sb1) noexcept {
    if (sb1 <= sb0) {
        return 0.0;
    }
    double sum = 0.0;
    for (int sb = sb0; sb < sb1; ++sb) {
        const double re = std::abs(slot[at(sb)].real());
        const double im = std::abs(slot[at(sb)].imag());
        sum += std::max(re, im) + 0.5 * std::min(re, im);
    }
    return 0.9105 * sum / static_cast<double>(sb1 - sb0);
}

// L^((1 - alpha) / alpha). The text prints the average gain's exponent as
// "1alpha / alpha"; it is read as the per-slot gain's (src/ac4dec/ERRATA.md,
// "The companding average").
[[nodiscard]] double gain_of(double level) noexcept {
    return std::pow(level, (1.0 - kAlpha) / kAlpha);
}

void scale(const CompandingChannel& channel, int sb0, int ts, double factor) noexcept {
    const std::span<QmfValue> slot = q_low_slot(channel, ts);
    for (int sb = sb0; sb < channel.sb1; ++sb) {
        slot[at(sb)] *= factor;
    }
}

[[nodiscard]] bool fits(const CompandingChannel& channel) noexcept {
    return channel.interval.first >= 0 && channel.interval.last <= kMaxSlots &&
           channel.interval.first < channel.interval.last;
}

}  // namespace

void apply_companding(const CompandingControl& control, int sb0, double full_scale,
                      std::span<const CompandingChannel> channels) {
    const double big_g = std::exp2(1.0 / kAlpha);
    std::vector<std::array<double, kMaxSlots>> level(channels.size());
    std::vector<std::array<double, kMaxSlots>> gain(channels.size());
    for (std::size_t c = 0; c < channels.size(); ++c) {
        const CompandingChannel& channel = channels[c];
        if (!fits(channel)) {
            continue;
        }
        for (int ts = channel.interval.first; ts < channel.interval.last; ++ts) {
            level[c][at(ts)] = slot_level(q_low_slot(channel, ts), sb0, channel.sb1) / full_scale;
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
                    scale(channel, sb0, ts, gain[c][at(ts)] * big_g);
                }
            } else if (control.b_compand_avg) {
                // L_avg over the interval's slots [ts0, ts1), the range 5.7.5.2
                // defines, where the sum prints ts1 as its upper bound.
                double sum = 0.0;
                for (int ts = first; ts < last; ++ts) {
                    sum += level[c][at(ts)];
                }
                const double average = gain_of(sum / static_cast<double>(last - first));
                for (int ts = first; ts < last; ++ts) {
                    scale(channel, sb0, ts, average * big_g);
                }
            }
        }
        return;
    }

    // sync_flag: g_sync(ts) is the channels' mean gain. Where the channels'
    // intervals differ, each slot averages the channels whose interval holds
    // it (src/ac4dec/ERRATA.md, "The companding average").
    std::array<double, kMaxSlots> sync{};
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
    double sum = 0.0;
    int held = 0;
    for (int ts = first; ts < last; ++ts) {
        if (count[at(ts)] > 0) {
            sync[at(ts)] /= count[at(ts)];
            sum += sync[at(ts)];
            ++held;
        }
    }
    const bool on = control.b_compand_on[0];
    if (!on && !control.b_compand_avg) {
        return;
    }
    const double average = held > 0 ? sum / held : 0.0;
    for (const CompandingChannel& channel : channels) {
        if (!fits(channel)) {
            continue;
        }
        for (int ts = channel.interval.first; ts < channel.interval.last; ++ts) {
            scale(channel, sb0, ts, (on ? sync[at(ts)] : average) * big_g);
        }
    }
}

}  // namespace ac4::detail
