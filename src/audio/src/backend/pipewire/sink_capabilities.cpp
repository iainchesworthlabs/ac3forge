#include "ac3/audio/sink_capabilities.hpp"

// The PipeWire EDID/ELD backend: there isn't one, honestly, not as a
// placeholder.
//
// A PipeWire node backed by ALSA hardware likely carries card/device
// identification in its properties (api.alsa.card, api.alsa.pcm.device are
// commonly seen in `pw-dump` output), which in principle could be used to
// find the same /proc/asound/<card>/eld#<dev>.<port> file the alsa/ backend
// reads (see src/backend/alsa/sink_capabilities.cpp). This backend does not
// attempt that: unlike this directory's own passthrough.cpp - which confirms
// its approach against a real shipped implementation (Kodi's PipeWire
// passthrough, xbmc PR #22560) - no such confirmation exists here for which
// property names are stable across PipeWire versions and session-manager
// configurations, and there is no PipeWire daemon available to verify
// against in this codebase's own development environment. Reading the wrong
// property, or reading it and mapping it to the wrong card, would be worse
// than reporting kNoBackend: it would hand a caller a confidently wrong
// answer about what a receiver accepts. 'ac3cli play' falls back to
// enumerate_render_devices()'s live probe here, the same as on every other
// backend without a real implementation - see docs/platforms/linux.md.

namespace ac3::audio {

std::expected<SinkAudioCapabilities, EdidError> read_sink_capabilities(const std::string&) {
    return std::unexpected(EdidError::kNoBackend);
}

}  // namespace ac3::audio
