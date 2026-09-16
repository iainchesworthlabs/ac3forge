#include "output_selector.hpp"

#include <algorithm>
#include <utility>

// See output_selector.hpp.

namespace ac3::hearth {

namespace {

// The rate an item that has not been probed is asked about: the one nearly
// every AC-3 and E-AC-3 stream uses.
constexpr std::uint32_t kUnprobedRate = 48000;

[[nodiscard]] bool exclusive(OutputMode mode) {
    return mode == OutputMode::kBitstream || mode == OutputMode::kBitstreamAsAc3;
}

}  // namespace

EndpointFacts endpoint_facts(
    const audio::RenderDeviceInfo& device,
    const std::expected<audio::SinkAudioCapabilities, audio::EdidError>& descriptor) {
    EndpointFacts row;
    row.id = device.id;
    row.name = device.name;
    row.is_default = device.is_default;
    row.channels = device.channels;
    row.speakers = device.speakers;
    row.accepts_ac3 = device.supports_ac3_passthrough;
    row.accepts_eac3 = device.supports_eac3_passthrough;
    // A local output decodes into the endpoint's shared-mode mixer, which
    // every render endpoint the enumeration lists has.
    row.accepts_pcm = true;
    if (descriptor) {
        row.accepts_ac3 = row.accepts_ac3 && descriptor->ac3;
        row.accepts_eac3 = row.accepts_eac3 && descriptor->eac3;
        row.source = CapabilitySource::kDescriptor;
        return row;
    }
    switch (descriptor.error()) {
        case audio::EdidError::kNoEdid:
            // A receiver that is off, or on another input, or no receiver at
            // all: the probe's yes says only that the output would open.
            row.accepts_ac3 = false;
            row.accepts_eac3 = false;
            row.source = CapabilitySource::kNoDescriptor;
            break;
        case audio::EdidError::kNoBackend:
            row.source = CapabilitySource::kNoReader;
            break;
        case audio::EdidError::kDeviceNotFound:
        case audio::EdidError::kParseFailed:
            // The reader could not answer for this endpoint; the probe did.
            row.source = CapabilitySource::kProbe;
            break;
    }
    return row;
}

EndpointFacts endpoint_facts(const EndpointReading& reading) {
    return endpoint_facts(reading.device, reading.descriptor);
}

EndpointSource device_endpoints() {
    return [](std::uint32_t sample_rate) {
        std::vector<EndpointReading> readings;
        const auto devices = audio::enumerate_render_devices(sample_rate);
        if (!devices) {
            return readings;
        }
        readings.reserve(devices->size());
        for (const auto& device : *devices) {
            readings.push_back(EndpointReading{
                .device = device, .descriptor = audio::read_sink_capabilities(device.id)});
        }
        return readings;
    };
}

OutputSelector::OutputSelector(EndpointSource source, bool bitstream_output)
    : source_(std::move(source)), bitstream_output_(bitstream_output) {}

void OutputSelector::set_preferences(OutputPreferences preferences) {
    preferences_ = std::move(preferences);
}

void OutputSelector::refresh() {
    for (auto& [rate, readings] : readings_) {
        readings.fresh = false;
    }
}

const std::vector<EndpointFacts>& OutputSelector::endpoints(std::uint32_t sample_rate,
                                                             const HeldOutput& held) {
    Readings& cached = readings_[sample_rate];
    const std::string& held_by = held.held() ? held.endpoint_id : std::string{};
    if (!cached.fresh || cached.held_by != held_by) {
        std::vector<EndpointReading> taken;
        if (source_) {
            taken = source_(sample_rate);
        }
        if (!taken.empty()) {
            if (!held_by.empty()) {
                for (EndpointReading& reading : taken) {
                    if (reading.device.id != held_by) {
                        continue;
                    }
                    // What was read of it before, while it was free - or, if
                    // it never was, the one thing its open link proves.
                    const auto before = std::ranges::find_if(
                        cached.endpoints,
                        [&held_by](const EndpointReading& r) { return r.device.id == held_by; });
                    if (before != cached.endpoints.end()) {
                        reading.device = before->device;
                    } else if (exclusive(held.mode) && held.sample_rate == sample_rate &&
                               held.stream) {
                        if (*held.stream == audio::BitstreamFormat::kAc3) {
                            reading.device.supports_ac3_passthrough = true;
                        } else {
                            reading.device.supports_eac3_passthrough = true;
                        }
                    }
                }
            }
            cached.endpoints = std::move(taken);
            cached.fresh = true;
            cached.held_by = held_by;
        }
        // Nothing found keeps what was there, unmarked, so the next decision
        // looks again.
    }
    rows_.clear();
    rows_.reserve(cached.endpoints.size());
    for (const EndpointReading& reading : cached.endpoints) {
        EndpointFacts row = endpoint_facts(reading);
        if (!bitstream_output_) {
            row.accepts_ac3 = false;
            row.accepts_eac3 = false;
        }
        rows_.push_back(std::move(row));
    }
    return rows_;
}

OutputChoice OutputSelector::choose(const ItemFacts& item, const HeldOutput& held) {
    const std::uint32_t rate = item.sample_rate != 0 ? item.sample_rate : kUnprobedRate;
    OutputRequest request;
    request.endpoints = endpoints(rate, held);
    request.stream = item.stream;
    request.has_objects = item.has_objects;
    request.pinned = preferences_.pinned;
    request.preferred_endpoint_id = preferences_.endpoint_id;
    request.follow_sink = preferences_.follow_sink;
    // The streaming transcode to AC-3 is not in the engine yet, so a sink
    // that takes only AC-3 is decoded for rather than offered it.
    request.transcode_available = false;
    return choose_output(request);
}

}  // namespace ac3::hearth
