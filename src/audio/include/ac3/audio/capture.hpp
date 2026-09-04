#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/audio/ring_buffer.hpp"

// Live audio capture. On Windows this is WASAPI in shared mode: either a
// real input endpoint (microphone, line in), a render endpoint opened in
// loopback mode, which captures whatever the machine is playing, or - roadmap
// UX11 - a process-loopback tap, which captures what one process (and its
// children) renders and nothing else, whichever endpoint it renders to.
//
// The capture thread only ever writes interleaved float samples into a
// RingBuffer; callers pull from that buffer at their own pace. Nothing on the
// capture side allocates, locks or blocks.

namespace ac3::audio {

enum class CaptureError : std::uint8_t {
    kNoBackend,          // built without a platform capture backend
    kComFailure,         // COM/WASAPI call failed
    kDeviceNotFound,
    kFormatUnsupported,  // endpoint delivers a format we cannot convert
    kAlreadyRunning,
    // start_process_loopback() only. This platform has no per-process tap:
    // every non-Windows backend, and Windows before 10 build 20348.
    kProcessLoopbackUnavailable,
    // start_process_loopback() only: no process has that id. Checked here
    // because the OS does not: a tap on an id nobody owns activates, starts,
    // and delivers zeros forever.
    kProcessNotFound,
};

[[nodiscard]] std::string_view describe(CaptureError error);

enum class DeviceKind : std::uint8_t {
    kInput,     // microphone, line in
    kLoopback,  // what the machine is playing (a render endpoint)
};

struct DeviceInfo {
    std::string id;    // endpoint id; stable across sessions
    std::string name;  // friendly name for a UI
    DeviceKind kind = DeviceKind::kInput;
    std::uint32_t sample_rate = 0;  // the endpoint's mixer rate
    std::uint16_t channels = 0;
    bool is_default = false;
};

// Every active input endpoint, plus every active render endpoint offered as
// a loopback source.
[[nodiscard]] std::expected<std::vector<DeviceInfo>, CaptureError> enumerate_devices();

// Whether start_process_loopback() can work on the machine this is running
// on - not the one it was built on. On Windows that is a build-number test
// (10.0.20348 introduced the activation); every other backend answers false.
// audio_backend().process_loopback says the same thing as a Capability.
[[nodiscard]] bool process_loopback_available();

enum class ProcessLoopbackMode : std::uint8_t {
    kIncludeProcessTree,  // only what this process and its children render
    kExcludeProcessTree,  // everything except this process and its children
};

// The shape a process-loopback tap delivers. There is no endpoint whose
// mixer format could be asked for - the tap is not a device - so the caller
// states what it wants and the audio engine converts to it. 48 kHz float
// stereo is the shape a live encoder wants; eight channels is granted too,
// and is how a surround-rendering application's tap reaches a bed intact.
struct ProcessLoopbackFormat {
    std::uint32_t sample_rate = 48000;
    std::uint16_t channels = 2;
};

struct CaptureStats {
    std::uint64_t frames_captured = 0;
    // Frames of silence synthesised to cover a loopback gap. A render
    // endpoint in loopback mode delivers nothing at all while the machine is
    // silent, so a continuous timeline has to be filled in. A process tap
    // behaves the same way while its process is quiet.
    std::uint64_t frames_silence_filled = 0;
    std::uint64_t frames_dropped = 0;  // ring buffer overruns (consumer too slow)
};

class Capture {
public:
    Capture();
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // Opens `device_id` (empty selects the default endpoint of `kind`) and
    // starts the capture thread. Samples land in buffer(), interleaved, at
    // sample_rate() x channels().
    [[nodiscard]] std::expected<void, CaptureError> start(const std::string& device_id,
                                                          DeviceKind kind,
                                                          std::size_t ring_capacity_samples = 1u
                                                                                              << 18);

    // Roadmap UX11. Taps what `process_id` (and, in kIncludeProcessTree
    // mode, its child processes) renders, whichever endpoint that is, and
    // starts the capture thread; samples land in buffer() at exactly
    // `format`. Refuses with kProcessLoopbackUnavailable where the platform
    // has no such tap (see process_loopback_available()), and with
    // kProcessNotFound when no process has that id.
    //
    // Two things the tap does NOT do, both found the hard way
    // (apps/crucible/spikes/README.md, S1): it does not survive the
    // process's audio session being muted - the tap sits after session
    // volume, so a muted session taps as silence - and it does not end when
    // the process does. A tap outlives its process delivering zeros, so a
    // caller that wants to know the process stopped playing has to watch
    // the audio session list, not this capture.
    [[nodiscard]] std::expected<void, CaptureError> start_process_loopback(
        std::uint32_t process_id,
        ProcessLoopbackMode mode = ProcessLoopbackMode::kIncludeProcessTree,
        ProcessLoopbackFormat format = {},
        std::size_t ring_capacity_samples = 1u << 18);

    void stop();

    [[nodiscard]] bool running() const;
    [[nodiscard]] std::uint32_t sample_rate() const;
    [[nodiscard]] std::uint16_t channels() const;
    [[nodiscard]] CaptureStats stats() const;

    // Valid while running; the consumer reads from here.
    [[nodiscard]] RingBuffer* buffer();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::audio
