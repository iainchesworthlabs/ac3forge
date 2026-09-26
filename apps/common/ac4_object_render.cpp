#include "ac4_object_render.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/oba/oamd.hpp"

namespace ac3::apps {
namespace {

using Location = ac3::eac3::chanmap::Location;
using S = ac4::Speaker;

// Where each of AC-4's speakers sits among E-AC-3's locations (Table E2.5),
// which the layout renderer places: the back pair at the rear surrounds, the
// top front pair at the front heights, and the top back pair and an X.2
// layout's top side pair at the top surrounds.
[[nodiscard]] Location location_of(S speaker) {
    switch (speaker) {
        case S::kLeft:
            return Location::kLeft;
        case S::kRight:
            return Location::kRight;
        case S::kCentre:
            return Location::kCentre;
        case S::kLfe:
            return Location::kLfe;
        case S::kLeftSurround:
            return Location::kLeftSurround;
        case S::kRightSurround:
            return Location::kRightSurround;
        case S::kLeftBack:
            return Location::kLrs;
        case S::kRightBack:
            return Location::kRrs;
        case S::kLeftWide:
            return Location::kLw;
        case S::kRightWide:
            return Location::kRw;
        case S::kTopFrontLeft:
            return Location::kVhl;
        case S::kTopFrontRight:
            return Location::kVhr;
        case S::kTopBackLeft:
        case S::kTopSideLeft:
            return Location::kLts;
        case S::kTopBackRight:
        case S::kTopSideRight:
            return Location::kRts;
        case S::kLfe2:
            return Location::kLfe2;
    }
    return Location::kCentre;
}

[[nodiscard]] std::vector<S> speakers_for(ac4::DownmixTarget target) {
    switch (target) {
        case ac4::DownmixTarget::k7X2:
            return {S::kLeft,         S::kRight,         S::kCentre,   S::kLfe,
                    S::kLeftSurround, S::kRightSurround, S::kLeftBack, S::kRightBack,
                    S::kTopSideLeft,  S::kTopSideRight};
        case ac4::DownmixTarget::k7X0:
            return {S::kLeft,         S::kRight,         S::kCentre,   S::kLfe,
                    S::kLeftSurround, S::kRightSurround, S::kLeftBack, S::kRightBack};
        case ac4::DownmixTarget::k5X4:
            return {S::kLeft,         S::kRight,         S::kCentre,       S::kLfe,
                    S::kLeftSurround, S::kRightSurround, S::kTopFrontLeft, S::kTopFrontRight,
                    S::kTopBackLeft,  S::kTopBackRight};
        case ac4::DownmixTarget::k5X2:
            return {S::kLeft,         S::kRight,         S::kCentre,      S::kLfe,
                    S::kLeftSurround, S::kRightSurround, S::kTopSideLeft, S::kTopSideRight};
        case ac4::DownmixTarget::k5X:
            return {S::kLeft, S::kRight, S::kCentre, S::kLfe, S::kLeftSurround, S::kRightSurround};
        case ac4::DownmixTarget::kStereo:
        case ac4::DownmixTarget::kLoRo:
        case ac4::DownmixTarget::kLtRt:
            return {S::kLeft, S::kRight};
        case ac4::DownmixTarget::kMono:
            return {S::kCentre};
        case ac4::DownmixTarget::kAsCoded:
        case ac4::DownmixTarget::k7X4:
            break;
    }
    return {S::kLeft,         S::kRight,         S::kCentre,      S::kLfe,
            S::kLeftSurround, S::kRightSurround, S::kLeftBack,    S::kRightBack,
            S::kTopFrontLeft, S::kTopFrontRight, S::kTopBackLeft, S::kTopBackRight};
}

[[nodiscard]] render::OutputLayout output_layout(std::span<const S> speakers) {
    std::array<Location, render::OutputLayout::kMaxSlots> locations{};
    const std::size_t count = std::min(speakers.size(), locations.size());
    for (std::size_t i = 0; i < count; ++i) {
        locations[i] = location_of(speakers[i]);
    }
    return render::OutputLayout::from_locations(std::span<const Location>(locations.data(), count))
        .value_or(render::OutputLayout::stereo());
}

[[nodiscard]] float linear_gain(const ac4::ObjectProperties& p) {
    return p.active ? static_cast<float>(std::pow(10.0, p.gain_db / 20.0)) : 0.0F;
}

}  // namespace

Ac4ObjectRenderer::Ac4ObjectRenderer(ac4::DownmixTarget target, std::uint32_t sample_rate_hz)
    : speakers_(speakers_for(target)), renderer_(output_layout(speakers_), sample_rate_hz) {}

std::span<const ac4::Speaker> Ac4ObjectRenderer::speakers() const noexcept {
    return speakers_;
}

void Ac4ObjectRenderer::reset() {
    tracks_.clear();
}

Ac4ObjectRenderer::Gains Ac4ObjectRenderer::speaker_gains(ac4::Speaker speaker) {
    // The bed as the one channel: the layout renderer's gain from it to each
    // slot, 1 to the slot of its own location where the layout has one.
    ac3::eac3::chanmap::Layout bed;
    bed.items[0] = location_of(speaker);
    bed.count = 1;
    renderer_.set_bed(bed);
    Gains out{};
    for (std::size_t slot = 0; slot < speakers_.size(); ++slot) {
        out[slot] = renderer_.bed_gain(0, slot);
    }
    return out;
}

std::vector<float> Ac4ObjectRenderer::object_gains(const ac4::ObjectProperties& properties) {
    ac3::oba::DisplayObject object;
    object.position = {
        .x = properties.position[0], .y = properties.position[1], .z = properties.position[2]};
    object.gain_db = properties.gain_db;
    object.active = properties.active;
    renderer_.set_objects(std::span<const ac3::oba::DisplayObject>(&object, 1));
    std::vector<float> out(speakers_.size());
    for (std::size_t slot = 0; slot < speakers_.size(); ++slot) {
        out[slot] = renderer_.object_gain(0, slot);
    }
    return out;
}

Ac4ObjectRenderer::Gains Ac4ObjectRenderer::gains_of(const ac4::DecodedObject& object,
                                                     const ac4::ObjectProperties& properties) {
    Gains out{};
    if (object.speaker && (object.kind == ac4::ObjectKind::kBed || object.lfe)) {
        out = speaker_gains(*object.speaker);
        const float gain = linear_gain(properties);
        for (float& g : out) {
            g *= gain;
        }
        return out;
    }
    if (object.lfe) {
        out = speaker_gains(ac4::Speaker::kLfe);
        const float gain = linear_gain(properties);
        for (float& g : out) {
            g *= gain;
        }
        return out;
    }
    const std::vector<float> gains = object_gains(properties);
    std::ranges::copy(gains, out.begin());
    return out;
}

void Ac4ObjectRenderer::render(const ac4::DecodedFrame& frame,
                               std::vector<std::vector<float>>& out) {
    const std::size_t slots = speakers_.size();
    const std::size_t samples = frame.samples;
    out.resize(slots);
    for (std::vector<float>& slot : out) {
        slot.assign(samples, 0.0F);
    }
    // The frame's channels, at their speakers.
    for (std::size_t c = 0; c < frame.channels.size() && c < frame.speakers.size(); ++c) {
        const Gains gains = speaker_gains(frame.speakers[c]);
        const std::vector<float>& x = frame.channels[c];
        for (std::size_t slot = 0; slot < slots; ++slot) {
            const float g = gains[slot];
            if (g == 0.0F) {
                continue;
            }
            for (std::size_t k = 0; k < std::min(samples, x.size()); ++k) {
                out[slot][k] += g * x[k];
            }
        }
    }
    // The objects: each at the gains of the properties in force, moving to
    // each update's from its sample over its ramp. A frame of another count
    // of objects starts every object afresh.
    if (tracks_.size() != frame.objects.size()) {
        tracks_.assign(frame.objects.size(), Track{});
        for (std::size_t o = 0; o < frame.objects.size(); ++o) {
            tracks_[o].gains = gains_of(frame.objects[o], frame.objects[o].properties);
        }
    }
    for (std::size_t o = 0; o < frame.objects.size(); ++o) {
        const ac4::DecodedObject& object = frame.objects[o];
        Track& track = tracks_[o];
        std::size_t u = 0;
        const std::size_t n = std::min(samples, object.samples.size());
        for (std::size_t k = 0; k < n; ++k) {
            while (u < object.updates.size() && object.updates[u].sample <= k) {
                const ac4::ObjectUpdate& update = object.updates[u++];
                track.target = gains_of(object, update.properties);
                track.left = std::max(update.ramp_samples, 0);
                for (std::size_t slot = 0; slot < slots; ++slot) {
                    track.step[slot] = track.left > 0 ? (track.target[slot] - track.gains[slot]) /
                                                            static_cast<float>(track.left)
                                                      : 0.0F;
                }
                if (track.left == 0) {
                    track.gains = track.target;
                }
            }
            if (track.left > 0) {
                if (--track.left == 0) {
                    track.gains = track.target;
                } else {
                    for (std::size_t slot = 0; slot < slots; ++slot) {
                        track.gains[slot] += track.step[slot];
                    }
                }
            }
            const float x = object.samples[k];
            for (std::size_t slot = 0; slot < slots; ++slot) {
                out[slot][k] += track.gains[slot] * x;
            }
        }
    }
}

}  // namespace ac3::apps
