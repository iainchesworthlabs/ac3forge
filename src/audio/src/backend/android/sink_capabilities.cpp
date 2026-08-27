#include "ac3/audio/sink_capabilities.hpp"

// The Android EDID/ELD backend: there isn't one.
//
// AudioTrack/AudioDeviceInfo expose the platform's own already-negotiated
// answer (isDirectPlaybackSupported, getEncodings()) - which is what
// passthrough.cpp's JNI shim already uses - not the sink's raw EDID. There is
// exactly one addressable output route on this build's target hardware (see
// passthrough.cpp's own header comment), so a second, lower-level path to the
// same answer would not add anything the existing probe does not already
// give. 'ac3cli play' falls back to that probe here - see
// docs/platforms/android.md.

namespace ac3::audio {

std::expected<SinkAudioCapabilities, EdidError> read_sink_capabilities(const std::string&) {
    return std::unexpected(EdidError::kNoBackend);
}

}  // namespace ac3::audio
