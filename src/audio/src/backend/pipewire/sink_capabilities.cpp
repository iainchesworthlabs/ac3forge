#include "ac3/audio/sink_capabilities.hpp"

// The PipeWire sink-capability backend (the appliance plan's UX9 gap 2).
//
// PipeWire has no call that hands over a sink's raw Short Audio Descriptors,
// and mapping a node back to the /proc/asound ELD file the alsa/ backend
// reads would mean trusting property names nobody has confirmed stable. What
// it does have is the session manager's own reading of that descriptor:
// WirePlumber sets `iec958.codecs` on an HDMI or S/PDIF node from the ELD -
// `["PCM","AC3","EAC3",...]` - and the passthrough backend beside this file
// already gates enumeration and start() on exactly that property. So this
// reports what it says, and nothing it does not: the codecs, but neither an
// LPCM channel count nor its rates, which the property does not carry.
//
// A node with no `iec958.codecs` at all is an ordinary PCM output, or a
// digital one whose descriptor the session manager has not read - kNoEdid,
// the same answer the alsa/ backend gives for a port with no ELD file.

#include <expected>
#include <string>

#include "pipewire_support.hpp"

namespace ac3::audio {

std::expected<SinkAudioCapabilities, EdidError> read_sink_capabilities(
    const std::string& device_id) {
    for (const auto& sink : ac3::pipewire::audio_sinks_with_info()) {
        if (sink.name != device_id) {
            continue;
        }
        if (sink.codecs.empty()) {
            return std::unexpected(EdidError::kNoEdid);
        }
        SinkAudioCapabilities capabilities;
        capabilities.pcm = ac3::pipewire::codec_listed(sink.codecs, "PCM");
        capabilities.ac3 = ac3::pipewire::codec_listed(sink.codecs, "AC3");
        capabilities.eac3 = ac3::pipewire::codec_listed(sink.codecs, "EAC3");
        return capabilities;
    }
    return std::unexpected(EdidError::kDeviceNotFound);
}

}  // namespace ac3::audio