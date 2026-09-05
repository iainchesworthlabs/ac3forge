#include "ac3/audio/audio_backend.hpp"

#include "ac3/audio/capture.hpp"

// PipeWire: capture, passthrough and monitor playback are real, over the
// native pw_stream API - not PipeWire's ALSA-compatibility shim, and not a
// fallback that quietly reaches for libasound instead. capture.cpp and
// monitor.cpp are ordinary PCM, PipeWire's flagship case. passthrough.cpp is
// the one worth a longer account, in its own file's header comment: PipeWire
// has a real, current, documented compressed-format path
// (SPA_MEDIA_SUBTYPE_iec958, spa_format_audio_iec958_build(),
// PW_STREAM_FLAG_EXCLUSIVE), but whether any given Audio/Sink node will
// actually accept it is a per-node question this library cannot answer by
// reading a property - it has to ask, the same way ALSA's own
// passthrough.cpp has no IsFormatSupported() and has to open a device to
// find out. See passthrough.cpp for the rest.
//
// For those three, "available" carries the same meaning it does on every
// other backend: the library was built against libpipewire-0.3, not that a
// PipeWire session is running on this particular machine, or that any of its
// outputs happen to have compressed passthrough enabled. enumerate_devices()
// and enumerate_render_devices() answer the per-machine, per-device
// questions; a machine with no PipeWire session running enumerates nothing,
// the same honest empty list a machine with no ALSA sound card returns.
//
// The two added for roadmap UX12 are different, so this table is computed
// once at first use the way the Windows one is. A per-application tap and a
// registry listener both need a session to talk to, and unlike enumeration -
// where "no session" and "no devices" are indistinguishable and an empty
// list is the right reply either way - there is no honest way to report
// having registered a listener with a daemon that is not running. On a
// container or a CI runner with no session, both say so and give a reason.
// This also keeps audio_backend().process_loopback and
// process_loopback_available() saying the same thing, which the backend
// contract test requires of every platform.

namespace ac3::audio {

const AudioBackend& audio_backend() {
    static const AudioBackend kBackend = [] {
        AudioBackend backend{
            .capture = {.available = true, .reason = {}},
            .passthrough = {.available = true, .reason = {}},
            .monitor = {.available = true, .reason = {}},
            .spatial = {.available = false,
                        .reason = "this build has no spatial backend: "
                                  "ISpatialAudioObjectRenderStream is a Windows-only API"},
            .process_loopback = {.available = true, .reason = {}},
            .device_watch = {.available = true, .reason = {}},
        };
        // One walk answers both: it connects, lists and disconnects, and
        // returns false only for the "no session to ask" case.
        if (!process_loopback_available()) {
            backend.process_loopback = {
                .available = false,
                .reason = "per-process loopback capture needs a running PipeWire session, to "
                          "link a capture stream to an application node; this machine has none"};
            backend.device_watch = {
                .available = false,
                .reason = "device notifications need a running PipeWire session, to register a "
                          "registry listener with; this machine has none"};
        }
        return backend;
    }();
    return kBackend;
}

}  // namespace ac3::audio
