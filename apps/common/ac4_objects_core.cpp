#include "ac4_objects_core.hpp"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <iterator>
#include <limits>
#include <numbers>

namespace ac3::apps {

std::vector<ObjectSlot> object_slots_from_assignment(
    const ac3::plan::Assignment& assignment, std::span<const ac3::plan::SourceShape> shapes) {
    // Where source `s`'s channel `c` lands in the flattened space.
    const auto flat = [&](std::size_t source, std::size_t channel) {
        std::size_t base = 0;
        for (std::size_t i = 0; i < source && i < shapes.size(); ++i) {
            base += shapes[i].channels;
        }
        return base + channel;
    };
    std::vector<ObjectSlot> slots;
    for (const auto& [source, channel] : assignment.rows_of(ac3::plan::DestinationKind::kObject)) {
        const auto dest = assignment.at(source, channel);
        slots.push_back({.taps = {{flat(source, channel), std::pow(10.0, dest.trim_db / 20.0)}}});
    }
    // rows_of() hands them back in (source, then channel) order, which is what
    // makes "the maximal contiguous run within one source" a well-defined
    // grouping - see DestinationKind::kObjectMono's own comment on why the
    // grouping is by adjacency rather than a stored group id.
    const auto mono_rows = assignment.rows_of(ac3::plan::DestinationKind::kObjectMono);
    for (std::size_t i = 0; i < mono_rows.size();) {
        std::size_t j = i + 1;
        while (j < mono_rows.size() && mono_rows[j].first == mono_rows[i].first &&
               mono_rows[j].second == mono_rows[j - 1].second + 1) {
            ++j;
        }
        const auto n = static_cast<double>(j - i);
        ObjectSlot slot;
        for (std::size_t k = i; k < j; ++k) {
            const auto dest = assignment.at(mono_rows[k].first, mono_rows[k].second);
            slot.taps.emplace_back(flat(mono_rows[k].first, mono_rows[k].second),
                                   std::pow(10.0, dest.trim_db / 20.0) / n);
        }
        slots.push_back(std::move(slot));
        i = j;
    }
    return slots;
}

std::optional<double> location_azimuth_deg(ac3::eac3::chanmap::Location location) {
    using ac3::eac3::chanmap::Location;
    // clang-format off
    switch (location) {
        case Location::kLeft: return 30.0;
        case Location::kCentre: return 0.0;
        case Location::kRight: return -30.0;
        case Location::kLeftSurround: return 110.0;
        case Location::kRightSurround: return -110.0;
        case Location::kLc: return 15.0;
        case Location::kRc: return -15.0;
        case Location::kLrs: return 135.0;
        case Location::kRrs: return -135.0;
        case Location::kCs: return 180.0;
        case Location::kTs: return 180.0;    // ceiling: overhead-rear
        case Location::kLsd: return 90.0;
        case Location::kRsd: return -90.0;
        case Location::kLw: return 60.0;
        case Location::kRw: return -60.0;
        case Location::kVhl: return 45.0;    // ceiling: front height
        case Location::kVhr: return -45.0;   // ceiling: front height
        case Location::kVhc: return 0.0;     // ceiling: centre height
        case Location::kLts: return 110.0;   // ceiling: rear height
        case Location::kRts: return -110.0;  // ceiling: rear height
        case Location::kLfe2:
        case Location::kLfe:
            return std::nullopt;
    }
    // clang-format on
    return std::nullopt;
}

bool ac4_objects_take_rate(std::uint32_t sample_rate_hz) {
    return sample_rate_hz == 48000 || sample_rate_hz == 44100;
}

std::vector<Ac4ObjectSlot> ac4_object_slots(const ac3::plan::Assignment& assignment,
                                            std::span<const ac3::plan::SourceShape> shapes) {
    const auto flat = [&](std::size_t source, std::size_t channel) {
        std::size_t base = 0;
        for (std::size_t i = 0; i < source && i < shapes.size(); ++i) {
            base += shapes[i].channels;
        }
        return base + channel;
    };
    std::vector<Ac4ObjectSlot> out;
    for (const ObjectSlot& slot : object_slots_from_assignment(assignment, shapes)) {
        out.push_back({.kind = Ac4ObjectSlot::Kind::kDynamic, .taps = slot.taps});
    }
    std::vector<Ac4ObjectSlot> lfes;
    for (const auto& [source, channel] :
         assignment.rows_of(ac3::plan::DestinationKind::kLocation)) {
        const ac3::plan::Destination dest = assignment.at(source, channel);
        Ac4ObjectSlot slot;
        slot.taps = {{flat(source, channel), std::pow(10.0, dest.trim_db / 20.0)}};
        if (const auto azimuth = location_azimuth_deg(dest.location)) {
            slot.kind = Ac4ObjectSlot::Kind::kPinned;
            slot.azimuth_deg = *azimuth;
            out.push_back(std::move(slot));
        } else {
            slot.kind = Ac4ObjectSlot::Kind::kLfe;
            lfes.push_back(std::move(slot));
        }
    }
    out.insert(out.end(), std::make_move_iterator(lfes.begin()),
               std::make_move_iterator(lfes.end()));
    return out;
}

ac3::oba::Position ac4_pin_position(double azimuth_deg) {
    const double radians = azimuth_deg * std::numbers::pi / 180.0;
    return {.x = 0.5 - 0.5 * std::sin(radians), .y = 0.5 - 0.5 * std::cos(radians), .z = 0.0};
}

std::vector<std::vector<float>> ac4_flat_planes(std::span<const Ac4SourceView> sources) {
    std::size_t total = 0;
    std::size_t channels = 0;
    for (const Ac4SourceView& source : sources) {
        const std::size_t frames = source.channels.empty() ? 0 : source.channels.front().size();
        total = std::max(total, source.offset_samples + frames);
        channels += source.channels.size();
    }
    std::vector<std::vector<float>> out;
    out.reserve(channels);
    for (const Ac4SourceView& source : sources) {
        for (const std::vector<float>& channel : source.channels) {
            std::vector<float> plane(total, 0.0F);
            std::copy(channel.begin(), channel.end(),
                      plane.begin() + static_cast<std::ptrdiff_t>(source.offset_samples));
            out.push_back(std::move(plane));
        }
    }
    return out;
}

std::vector<std::vector<float>> ac4_object_planes(std::span<const Ac4ObjectSlot> slots,
                                                  std::span<const std::vector<float>> flat_planes) {
    const std::size_t length = flat_planes.empty() ? 0 : flat_planes.front().size();
    std::vector<std::vector<float>> out(slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        out[i].assign(length, 0.0F);
        for (const auto& [flat, gain] : slots[i].taps) {
            if (flat >= flat_planes.size()) {
                continue;
            }
            const float scale = static_cast<float>(gain);
            const std::vector<float>& plane = flat_planes[flat];
            const std::size_t n = std::min(length, plane.size());
            for (std::size_t k = 0; k < n; ++k) {
                out[i][k] += plane[k] * scale;
            }
        }
    }
    return out;
}

ac4::ObjectProperties ac4_object_properties(const ac3::oba::ObjectPlacement& p) {
    ac4::ObjectProperties out;
    out.position = {p.position.x, p.position.y, p.position.z};
    out.gain_db =
        p.gain > 0.0 ? 20.0 * std::log10(p.gain) : -std::numeric_limits<double>::infinity();
    return out;
}

ac4::EncoderConfig ac4_objects_config(const Ac4ObjectsParams& params, const std::vector<bool>& lfe,
                                      std::span<const ac3::oba::ObjectPlacement> initial) {
    ac4::ObjectsConfig objects;
    objects.coding = params.coding;
    objects.objects.resize(initial.size());
    for (std::size_t i = 0; i < initial.size(); ++i) {
        objects.objects[i].properties = ac4_object_properties(initial[i]);
        objects.objects[i].lfe = i < lfe.size() && lfe[i];
    }
    ac4::EncoderConfig config;
    config.sample_rate_hz = static_cast<int>(params.sample_rate_hz);
    config.frame_rate_index = kAc4ObjectFrameRateIndex;
    config.bitrate_kbps = params.bitrate_kbps;
    config.dialnorm_db = -params.dialnorm_db;
    config.experimental.objects = true;
    ac4::SubstreamConfig substream;
    substream.objects = std::move(objects);
    config.substreams = {std::move(substream)};
    return config;
}

std::optional<std::string> ac4_objects_refusal(std::span<const Ac4ObjectSlot> slots,
                                               const Ac4ObjectsParams& params) {
    if (!ac4_objects_take_rate(params.sample_rate_hz)) {
        return fmt::format(
            "AC-4 objects are written at 48 or 44.1 kHz, in 2 048-sample frames; the source is {} "
            "Hz",
            params.sample_rate_hz);
    }
    const auto lfes = static_cast<std::size_t>(std::ranges::count_if(
        slots, [](const auto& slot) { return slot.kind == Ac4ObjectSlot::Kind::kLfe; }));
    if (slots.size() == lfes) {
        return std::string{
            "an AC-4 object stream needs at least one object that is not the LFE: send a channel "
            "to an object or to a speaker"};
    }
    if (slots.size() > kAc4MaxObjects) {
        return fmt::format("{} objects: an AC-4 object stream holds {} at most", slots.size(),
                           kAc4MaxObjects);
    }
    if (lfes > 1) {
        return fmt::format(
            "{} channels are assigned to an LFE: an AC-4 object stream has one LFE object", lfes);
    }
    return std::nullopt;
}

std::vector<ac3::oba::ObjectPlacement> ac4_scene_placements(std::span<const Ac4ObjectSlot> slots,
                                                            const ac3::oba::ObjectScene& motion,
                                                            double time_s) {
    std::vector<ac3::oba::ObjectPlacement> out(slots.size());
    const std::vector<ac3::oba::ObjectPlacement> moving = motion.evaluate(time_s);
    std::size_t dynamic = 0;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        switch (slots[i].kind) {
            case Ac4ObjectSlot::Kind::kDynamic:
                if (dynamic < moving.size()) {
                    out[i] = moving[dynamic];
                }
                ++dynamic;
                break;
            case Ac4ObjectSlot::Kind::kPinned:
                out[i] = {.position = ac4_pin_position(slots[i].azimuth_deg),
                          .gain = 1.0,
                          .lfe_send = 0.0};
                break;
            case Ac4ObjectSlot::Kind::kLfe:
                out[i] = {.position = {.x = 0.5, .y = 0.5, .z = 0.0}, .gain = 1.0, .lfe_send = 0.0};
                break;
        }
    }
    return out;
}

std::expected<Ac4ObjectsEncoded, Ac4ObjectsError> encode_ac4_objects(
    const Ac4ObjectsParams& params, const std::vector<bool>& lfe,
    std::span<const std::span<const float>> pcm, const Ac4Placements& placements) {
    const std::size_t count = pcm.size();
    const std::size_t total = pcm.empty() ? 0 : pcm.front().size();

    std::vector<ac4::ObjectMetadataUpdate> updates;
    for (std::int64_t start = 0; static_cast<std::uint64_t>(start) < total;
         start += kAc4ObjectFrameSamples) {
        const auto ramp = std::min<std::int64_t>(kAc4ObjectFrameSamples,
                                                 static_cast<std::int64_t>(total) - start);
        const double t =
            static_cast<double>(start + ramp) / static_cast<double>(params.sample_rate_hz);
        const std::vector<ac3::oba::ObjectPlacement> placed = placements(t);
        for (std::size_t i = 0; i < count; ++i) {
            updates.push_back({.object = static_cast<int>(i),
                               .sample = start,
                               .ramp_samples = static_cast<int>(ramp),
                               .properties = ac4_object_properties(placed[i])});
        }
    }

    const std::vector<ac3::oba::ObjectPlacement> initial = placements(0.0);
    const ac4::EncoderConfig config = ac4_objects_config(params, lfe, initial);
    auto encoder = ac4::Encoder::create(config);
    if (!encoder.has_value()) {
        return std::unexpected(
            Ac4ObjectsError{.kind = Ac4ObjectsError::Kind::kRefused,
                            .message = std::string{ac4::Encoder::refusal_reason(config)}});
    }
    auto frames = encoder->encode(pcm, updates);
    if (!frames.has_value()) {
        return std::unexpected(
            Ac4ObjectsError{.kind = Ac4ObjectsError::Kind::kEncode,
                            .message = std::string{ac4::describe(frames.error())}});
    }
    auto rest = encoder->flush();
    if (!rest.has_value()) {
        return std::unexpected(
            Ac4ObjectsError{.kind = Ac4ObjectsError::Kind::kFlush,
                            .message = std::string{ac4::describe(rest.error())}});
    }
    frames->insert(frames->end(), rest->begin(), rest->end());
    Ac4ObjectsEncoded out;
    out.frames = std::move(*frames);
    out.toc = encoder->toc();
    out.lag_samples = encoder->delay_samples() + encoder->decoder_delay_samples();
    return out;
}

std::expected<Ac4ObjectsEncoded, Ac4ObjectsError> encode_ac4_scene(
    const Ac4ObjectsParams& params, std::span<const Ac4ObjectSlot> slots,
    std::span<const std::vector<float>> flat_planes, const ac3::oba::ObjectScene& motion) {
    const std::vector<std::vector<float>> planes = ac4_object_planes(slots, flat_planes);
    const std::vector<std::span<const float>> pcm(planes.begin(), planes.end());
    // Sized, not reserved: GCC 16's -Wnull-dereference flags vector<bool>::reserve on an
    // empty vector.
    std::vector<bool> lfe(slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        lfe[i] = slots[i].kind == Ac4ObjectSlot::Kind::kLfe;
    }
    return encode_ac4_objects(params, lfe, pcm, [&](double time_s) {
        return ac4_scene_placements(slots, motion, time_s);
    });
}

}  // namespace ac3::apps
