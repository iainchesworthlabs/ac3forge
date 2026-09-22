#include "ac3/audio/audio_backend.hpp"

#include "ac3/audio/capture.hpp"
#include "coreaudio_names.hpp"

// macOS: five of the six capabilities are implemented here. capture.cpp reads
// from any HAL input device via AudioDeviceCreateIOProcID, monitor.cpp plays
// ordinary float PCM back through a HAL output device the same way, and
// passthrough.cpp takes hog mode on a digital (HDMI/optical) output and
// retunes its physical stream format to a compressed IEC 60958 format
// (kAudioFormat60958AC3/kAudioFormatEnhancedAC3) before feeding it raw
// bursts - see that file's own header comment for the full mechanism, which
// resembles neither WASAPI's exclusive-mode subformat nor ALSA's
// channel-status device names. Those three carry no reason, because there is
// nothing to excuse.
//
// device_watch is CoreAudio property listeners on the system object
// (device_watcher.cpp), and it is available for the same reason it is on
// Windows: registering for notifications needs no device and no session, so a
// machine with no sound card at all still registers and then never hears
// anything.
//
// process_loopback is the one answer that belongs to the MACHINE rather than
// to the build, exactly as it is on Windows - there, a build number; here two
// things, an OS version (Core Audio's process tap arrived in macOS 14.2) and
// whether the path is entered at all. Since 2026-09-06 it is not, by default:
// the first machine ever to run it hung inside AudioDeviceCreateIOProcID and
// took the rest of the process's HAL with it, so "available" is no longer
// claimed on a version test alone. coreaudio_names.hpp carries the
// observation, the opt-in that reverses it, and both refusal sentences. So
// this table is computed once at first use rather than being a constexpr the
// way it used to be, and the refusal it carries is the same sentence
// capture.cpp's describe() prints, chosen by the same function.
//
// spatial is the only flat no. ISpatialAudioObjectRenderStream is a Windows
// API and neither CoreAudio nor anything else on this platform offers a
// third party an OS object renderer to hand Atmos objects to.
//
// "Available" means the same thing here as it does on Windows/Linux: the
// library was built with a backend, not that this particular machine has
// hardware that will do it - a MacBook with no digital output enumerates
// zero passthrough-capable devices rather than reporting itself unavailable,
// the same as ALSA's own documented behaviour for a machine with only an
// analog output. It does not mean any of it has been run, either. Both macOS
// CI legs compiled and linked this file, process_tap.mm and the watcher on
// 2026-09-06, and the library's own two device-free cases ran there - the
// version gate, and the watcher contract case, which starts and stops a
// watcher for real. Nothing else has executed: no tap has been created, no
// audio has passed through any of it, and no Mac has run the application
// (ROADMAP.md DR9).

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
        // Keeps audio_backend().process_loopback and
        // process_loopback_available() saying the same thing, which the
        // backend contract test (tests/audio/test_audio_backend.cpp) requires
        // of every platform.
        if (!process_loopback_available()) {
            backend.process_loopback = {.available = false,
                                        .reason = coreaudio::system_audio_tap_refusal()};
        }
        return backend;
    }();
    return kBackend;
}

}  // namespace ac3::audio
