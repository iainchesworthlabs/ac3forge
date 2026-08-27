#include "ac3/audio/sink_capabilities.hpp"

// The Windows EDID/ELD backend: there isn't one.
//
// WASAPI answers "will this endpoint accept this format" (IsFormatSupported,
// what enumerate_render_devices() already uses) but does not expose the
// sink's own raw EDID-carried CEA-861 Short Audio Descriptors to user-mode
// code - the audio driver consumes them internally to decide what to offer
// and does not re-expose the source data itself through any documented
// public API this project found. 'ac3cli play' falls back to
// enumerate_render_devices()'s live probe here - see docs/platforms/windows.md.

namespace ac3::audio {

std::expected<SinkAudioCapabilities, EdidError> read_sink_capabilities(const std::string&) {
    return std::unexpected(EdidError::kNoBackend);
}

}  // namespace ac3::audio
