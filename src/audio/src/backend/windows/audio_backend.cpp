#include "ac3/audio/audio_backend.hpp"

// Windows: all four capabilities are real. capture.cpp is WASAPI in shared
// mode (input endpoints plus render endpoints opened for loopback),
// passthrough.cpp is WASAPI in exclusive mode with an IEC 61937 format,
// monitor.cpp is WASAPI in shared mode for ordinary PCM playback, and
// spatial.cpp activates ISpatialAudioClient/ISpatialAudioObjectRenderStream -
// so no Capability carries a reason - there is nothing to excuse. Whether a
// given endpoint currently has a spatial sound format (Windows Sonic, Dolby
// Atmos for Home Theater/Headphones, DTS:X) turned on is a per-endpoint,
// runtime question answered by SpatialObjectSink::start()'s own
// kNoSpatialFormat, not by this static report - exactly like passthrough's
// per-device IsFormatSupported split.

namespace ac3::audio {

const AudioBackend& audio_backend() {
    static constexpr AudioBackend kBackend{
        .capture = {.available = true, .reason = {}},
        .passthrough = {.available = true, .reason = {}},
        .monitor = {.available = true, .reason = {}},
        .spatial = {.available = true, .reason = {}},
    };
    return kBackend;
}

}  // namespace ac3::audio
