#include "pcm/mixer.hpp"

#include <algorithm>
#include <cmath>

namespace ac4::detail {
namespace {

// Where a channel sits for panning, in degrees clockwise from the front: L, C
// and R where the pan clause puts them (Part 1 clause 4.3.12.4.9: "standard
// Right speaker is at +30 degree"; Table 216's 330 and 30), the others at Part
// 1 Table D.1's azimuths, which turn the other way (Ls and Rs at 110 degrees in
// the 5.X modes, 90 in the 7.X ones). The LFE and the top channels are not in
// the horizontal ring a pan moves round.
[[nodiscard]] std::optional<double> azimuth(Speaker speaker, bool seven) noexcept {
    switch (speaker) {
        case Speaker::kLeft:
            return 330.0;
        case Speaker::kCentre:
            return 0.0;
        case Speaker::kRight:
            return 30.0;
        case Speaker::kLeftSurround:
            return seven ? 270.0 : 250.0;
        case Speaker::kRightSurround:
            return seven ? 90.0 : 110.0;
        case Speaker::kLeftBack:
            return 225.0;
        case Speaker::kRightBack:
            return 135.0;
        case Speaker::kLeftWide:
            return 300.0;
        case Speaker::kRightWide:
            return 60.0;
        default:
            return std::nullopt;
    }
}

[[nodiscard]] bool is_seven(std::span<const Speaker> speakers) noexcept {
    return std::ranges::any_of(speakers, [](Speaker s) {
        return s == Speaker::kLeftBack || s == Speaker::kRightBack || s == Speaker::kLeftWide ||
               s == Speaker::kRightWide || s == Speaker::kTopFrontLeft || s == Speaker::kTopFrontRight;
    });
}

// From a to b clockwise, in (0, 360].
[[nodiscard]] double clockwise(double a, double b) noexcept {
    const double d = std::fmod(b - a + 720.0, 360.0);
    return d == 0.0 ? 360.0 : d;
}

struct RingPoint {
    double az = 0.0;
    std::size_t channel = 0;
};

}  // namespace

void pan_gains(double degrees, std::span<const Speaker> into, std::span<double> gains) {
    std::ranges::fill(gains, 0.0);
    const bool seven = is_seven(into);
    std::array<RingPoint, kMaxRing> ring{};
    std::size_t points = 0;
    for (std::size_t c = 0; c < into.size() && c < gains.size() && points < ring.size(); ++c) {
        if (const std::optional<double> az = azimuth(into[c], seven)) {
            ring[points++] = RingPoint{*az, c};
        }
    }
    if (points == 0) {
        return;
    }
    const std::span<RingPoint> used(ring.data(), points);
    std::ranges::sort(used, {}, &RingPoint::az);
    double t = std::fmod(degrees, 360.0);
    if (t < 0.0) {
        t += 360.0;
    }
    for (const RingPoint& p : used) {
        if (std::abs(p.az - t) < 1e-9) {
            gains[p.channel] = 1.0;
            return;
        }
    }
    if (points == 1) {
        gains[used.front().channel] = 1.0;
        return;
    }
    // The last channel before the angle, going clockwise, and the next.
    std::size_t a = points - 1;
    for (std::size_t i = 0; i < points; ++i) {
        if (used[i].az < t) {
            a = i;
        }
    }
    const std::size_t b = (a + 1) % points;
    const double span = clockwise(used[a].az, used[b].az);
    const double into_span = clockwise(used[a].az, t);
    gains[used[a].channel] = 1.0 - into_span / span;
    gains[used[b].channel] = into_span / span;
}

void member_matrix(const MixMember& member, std::span<const Speaker> from, std::span<const Speaker> into,
                   std::span<double> matrix, std::span<double> gains) {
    std::ranges::fill(matrix, 0.0);
    const std::size_t columns = from.size();
    for (std::size_t j = 0; j < columns; ++j) {
        double scale = member.scale_all;
        if (from[j] == Speaker::kLeft || from[j] == Speaker::kRight) {
            scale *= member.scale_front;
        } else if (from[j] == Speaker::kCentre) {
            scale *= member.scale_centre;
        }
        const double gain = member.gain * scale;
        std::optional<double> pan = j < member.pan.size() ? member.pan[j] : std::nullopt;
        if (!pan && columns == 1) {
            pan = 0.0;
        }
        if (pan) {
            pan_gains(*pan, into, gains);
            for (std::size_t c = 0; c < into.size(); ++c) {
                matrix[c * columns + j] = gain * gains[c];
            }
            continue;
        }
        for (std::size_t c = 0; c < into.size(); ++c) {
            if (into[c] == from[j]) {
                matrix[c * columns + j] = gain;
            }
        }
    }
}

void MixStage::mix(const MixValues& values, std::span<const Speaker> speakers,
                   std::span<std::vector<QmfValue>* const> matrices, std::span<std::vector<QmfValue>* const> side,
                   bool side_separate, std::span<const MixSource> sources) {
    const std::size_t channels = std::min(speakers.size(), matrices.size());
    for (std::size_t c = 0; c < channels; ++c) {
        double gain = values.main_gain * values.scale_all;
        if (speakers[c] == Speaker::kLeft || speakers[c] == Speaker::kRight) {
            gain *= values.scale_front;
        } else if (speakers[c] == Speaker::kCentre) {
            gain *= values.scale_centre;
        }
        if (gain == 1.0) {
            continue;
        }
        for (QmfValue& v : *matrices[c]) {
            v *= gain;
        }
        if (side_separate && c < side.size()) {
            for (QmfValue& v : *side[c]) {
                v *= gain;
            }
        }
    }
    const auto add = [](std::vector<QmfValue>& into, const std::vector<QmfValue>& from, double weight) {
        const std::size_t n = std::min(into.size(), from.size());
        for (std::size_t i = 0; i < n; ++i) {
            into[i] += weight * from[i];
        }
    };
    gains_.resize(speakers.size());
    for (std::size_t k = 0; k < values.count && k < values.members.size(); ++k) {
        const MixMember& member = values.members[k];
        const auto source = std::ranges::find(sources, member.key, &MixSource::key);
        if (source == sources.end()) {
            continue;
        }
        const std::size_t columns = std::min(source->speakers.size(), source->matrices.size());
        matrix_.resize(speakers.size() * columns);
        member_matrix(member, source->speakers.first(columns), speakers, matrix_, gains_);
        for (std::size_t c = 0; c < channels; ++c) {
            for (std::size_t j = 0; j < columns; ++j) {
                const double weight = matrix_[c * columns + j];
                if (weight == 0.0) {
                    continue;
                }
                add(*matrices[c], *source->matrices[j], weight);
                if (side_separate && c < side.size() && j < source->side.size()) {
                    add(*side[c], *source->side[j], weight);
                }
            }
        }
    }
}

}  // namespace ac4::detail
