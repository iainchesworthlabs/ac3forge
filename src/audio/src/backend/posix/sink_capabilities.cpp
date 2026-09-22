#include "ac3/audio/sink_capabilities.hpp"

// This is now reachable only as ac3::audio's Linux fallback, when libasound's
// development headers are not present - see audio_backend.cpp in this same
// directory for the reasoning that applies here too. No sound backend means
// no device to read a descriptor from either way.

namespace ac3::audio {

std::expected<SinkAudioCapabilities, EdidError> read_sink_capabilities(const std::string&) {
    return std::unexpected(EdidError::kNoBackend);
}

}  // namespace ac3::audio
