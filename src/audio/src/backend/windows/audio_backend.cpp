#include "ac3/audio/audio_backend.hpp"

#include "ac3/audio/capture.hpp"

// The Windows report: everything this tree knows how to do, it does over
// WASAPI. One answer is the machine's rather than the build's - per-process
// loopback exists from Windows 10 build 20348 on - so unlike the other
// backends' constexpr tables this one is computed once, at first use.

namespace ac3::audio {

const AudioBackend& audio_backend() {
    static const AudioBackend kBackend = [] {
        AudioBackend backend{
            .capture = {.available = true, .reason = {}},
            .passthrough = {.available = true, .reason = {}},
            .monitor = {.available = true, .reason = {}},
            .spatial = {.available = true, .reason = {}},
            .process_loopback = {.available = true, .reason = {}},
            .device_watch = {.available = true, .reason = {}},
        };
        if (!process_loopback_available()) {
            backend.process_loopback = {
                .available = false,
                .reason = "per-process loopback capture needs Windows 10 build 20348 or "
                          "later (AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK); this "
                          "machine is older"};
        }
        return backend;
    }();
    return kBackend;
}

}  // namespace ac3::audio
