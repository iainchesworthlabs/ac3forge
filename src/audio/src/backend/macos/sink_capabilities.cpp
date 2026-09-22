#include "ac3/audio/sink_capabilities.hpp"

// The macOS EDID/ELD backend: there isn't one.
//
// CoreAudio's HAL exposes device properties (kAudioDevicePropertyStreams and
// friends) and IOKit's IODisplayEDID reaches a connected display's own EDID,
// but neither is documented to expose the CEA-861 Short Audio Descriptor
// block for an HDMI AUDIO endpoint specifically - and an audio-only optical
// output has no display EDID to read in the first place. 'ac3cli play' falls
// back to enumerate_render_devices()'s device-property read here - see
// docs/platforms/macos.md.

namespace ac3::audio {

std::expected<SinkAudioCapabilities, EdidError> read_sink_capabilities(const std::string&) {
    return std::unexpected(EdidError::kNoBackend);
}

}  // namespace ac3::audio
