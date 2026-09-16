#include "ac3/audio/pcm_output.hpp"

#include <algorithm>
#include <vector>

#include "ac3/audio/speakers.hpp"

// See pcm_output.hpp's header for what this is for. Nothing here is
// platform-specific: it is MonitorSink, the enumeration both share, and
// arithmetic over a patch - the same reason speakers.cpp sits beside the
// backend tree rather than inside it.

namespace ac3::audio {

std::uint16_t output_width(const RenderDeviceInfo& device, std::uint16_t rendered_channels) {
    return device.channels != 0 ? device.channels : rendered_channels;
}

render::Routing speaker_routing(const render::OutputLayout& layout, std::uint32_t speakers,
                                 std::uint16_t outputs) {
    const auto identity = render::Routing::identity(layout.slots(), outputs);
    // A backend that cannot say gets the standard arrangement for the width,
    // which RenderDeviceInfo::speakers' own comment names as the most a
    // caller can assume - and is a far better guess than counting outputs
    // off from zero, since a rendered programme's slots are in the coded
    // channel order (L C R Ls Rs, then the LFE feed) while a device's
    // outputs are in WAVEFORMATEXTENSIBLE's. The identity is left for a
    // width no single arrangement fits, where there is nothing to match.
    const std::uint32_t assumed = speakers != 0 ? speakers : default_speakers(outputs);
    if (assumed == 0) {
        return identity.value_or(render::Routing{});
    }
    // Each mask bit's speaker, in the order an interleaved stream carries
    // them, which is the order of the device's outputs.
    const auto at_output = locations_of(assumed);
    std::vector<int> output_of(layout.slots(), render::Routing::kUnassigned);
    for (std::size_t slot = 0; slot < layout.slots(); ++slot) {
        const auto& speaker = layout.slot(slot);
        // An empty slot is rendered as zeros, so there is nothing to place;
        // a slot placed by angle alone has no speaker to match against, and
        // guessing an output for it would be worse than leaving it to the
        // caller's own patch (Routing::unpatched_channels() reports both).
        if (speaker.kind == render::Speaker::Kind::kEmpty || !speaker.location) {
            continue;
        }
        for (std::size_t output = 0; output < at_output.size() && output < outputs; ++output) {
            if (at_output[output] == *speaker.location) {
                output_of[slot] = static_cast<int>(output);
                break;
            }
        }
    }
    const auto built = render::Routing::from_outputs(output_of, outputs);
    return built.value_or(identity.value_or(render::Routing{}));
}

struct PcmOutput::Impl {
    MonitorSink sink;
    PcmOutputInfo info;
    render::Routing routing;
    // One block, interleaved at the device's width, filled by the patch.
    // Grown by submit() and then reused, so a steady caller allocates once.
    std::vector<float> block;
};

PcmOutput::PcmOutput() : impl_(std::make_unique<Impl>()) {}

PcmOutput::~PcmOutput() = default;

bool PcmOutput::running() const {
    return impl_->sink.running();
}

const PcmOutputInfo& PcmOutput::info() const {
    return impl_->info;
}

const render::Routing& PcmOutput::routing() const {
    return impl_->routing;
}

bool PcmOutput::set_routing(const render::Routing& routing) {
    if (!running() || routing.outputs() != impl_->info.outputs) {
        return false;
    }
    impl_->routing = routing;
    return true;
}

std::expected<PcmOutputInfo, MonitorError> PcmOutput::start(const std::string& device_id,
                                                              std::uint32_t sample_rate,
                                                              const render::OutputLayout& layout,
                                                              bool low_latency) {
    if (running()) {
        return std::unexpected(MonitorError::kAlreadyRunning);
    }
    if (layout.slots() == 0) {
        return std::unexpected(MonitorError::kComFailure);
    }

    // The enumeration is what knows the device's width, its speakers and its
    // name; a backend that cannot enumerate at all (kNoBackend, or a
    // permission refusal) is not fatal here - the layout's own width is then
    // what the stream opens at, which is what MonitorSink did before this
    // class existed.
    PcmOutputInfo info{.device_id = device_id,
                       .device_name = {},
                       .outputs = static_cast<std::uint16_t>(layout.slots()),
                       .from_device = false,
                       .speakers = 0,
                       .sample_rate = sample_rate};
    if (const auto devices = enumerate_render_devices(sample_rate)) {
        for (const auto& candidate : *devices) {
            const bool wanted = device_id.empty() ? candidate.is_default : candidate.id == device_id;
            if (!wanted) {
                continue;
            }
            info.device_id = candidate.id;
            info.device_name = candidate.name;
            info.outputs = output_width(candidate, static_cast<std::uint16_t>(layout.slots()));
            info.from_device = candidate.channels != 0;
            info.speakers = candidate.speakers;
            break;
        }
    }
    if (info.outputs > render::Routing::kMaxOutputs) {
        return std::unexpected(MonitorError::kDeviceNotFound);
    }
    // A mask is only usable as "which speaker each channel is" if it has one
    // bit per channel. Nothing guarantees a backend reports the two
    // consistently - a Core Audio layout can carry a different number of
    // descriptions than the device has output channels, and an ALSA channel
    // map can arrive for a width the device could not report - and a mask
    // narrower or wider than the stream would both misplace channels and, on
    // WASAPI, be refused outright beside a mismatched channel count. Where
    // they disagree the mask is discarded: speaker_routing() then assumes the
    // standard arrangement for the width, which is the same fallback a
    // backend that reports no mask at all gets.
    if (info.speakers != 0 && speaker_count(info.speakers) != info.outputs) {
        info.speakers = 0;
    }

    // The mask goes to the sink as well as into the patch: it is what tells a
    // shared-mode engine which speaker each channel of the stream is for, and
    // 0 leaves the platform to its own default for the width (monitor.hpp).
    //
    // info.device_id, not the caller's: where the enumeration resolved an
    // endpoint, that is the one whose width and speakers everything above was
    // taken from, so it has to be the one opened. An empty id left to each
    // platform's own idea of "default" is not always the same endpoint the
    // enumeration marks default - on ALSA the enumeration prefers a digital
    // output while an empty name opens whatever the user's configuration
    // routes "default" to - and opening one device while describing another
    // would put a patch built for the second onto the first. A caller wanting
    // the configured default by name can still ask for it ("default").
    const auto started = impl_->sink.start(info.device_id, sample_rate, info.outputs,
                                            info.speakers, low_latency);
    if (!started) {
        return std::unexpected(started.error());
    }
    impl_->routing = speaker_routing(layout, info.speakers, info.outputs);
    impl_->info = info;
    return info;
}

void PcmOutput::stop() {
    impl_->sink.stop();
}

bool PcmOutput::can_submit() const {
    return impl_->sink.can_submit();
}

bool PcmOutput::submit(std::span<const std::span<const float>> rendered, std::size_t frames) {
    if (!running() || frames == 0 || impl_->info.outputs == 0) {
        return false;
    }
    const std::size_t samples = frames * impl_->info.outputs;
    if (impl_->block.size() < samples) {
        impl_->block.resize(samples);
    }
    const std::span<float> block{impl_->block.data(), samples};
    if (impl_->routing.apply_interleaved(rendered, block, frames) != frames) {
        // A patched channel was shorter than `frames`: the caller's block and
        // its spans disagree, and a short submit would desynchronise the
        // stream rather than report the mistake.
        return false;
    }
    return impl_->sink.submit(block);
}

std::optional<MonitorPosition> PcmOutput::position() const {
    return impl_->sink.position();
}

void PcmOutput::flush() {
    impl_->sink.flush();
}

std::expected<void, MonitorError> PcmOutput::pause() {
    return impl_->sink.pause();
}

std::expected<void, MonitorError> PcmOutput::resume() {
    return impl_->sink.resume();
}

bool PcmOutput::paused() const {
    return impl_->sink.paused();
}

MonitorStats PcmOutput::stats() const {
    return impl_->sink.stats();
}

}  // namespace ac3::audio
