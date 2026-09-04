#include "ac3/audio/capture.hpp"

// The Unix capture backend: there isn't one. CMake compiles this directory's
// capture.cpp on Linux and macOS, and every entry point fails with kNoBackend
// rather than the API disappearing from the library - callers keep compiling,
// and get told no instead of getting nothing.
//
// Ask ac3::audio::audio_backend() BEFORE calling any of this if the answer
// wants to be a sentence rather than an error code; see
// platform/posix/audio_backend.cpp for why there is no backend here.

namespace ac3::audio {

std::string_view describe(CaptureError error) {
    switch (error) {
        case CaptureError::kNoBackend: return "no capture backend on this platform";
        case CaptureError::kComFailure: return "a platform audio call failed";
        case CaptureError::kDeviceNotFound: return "the requested capture device was not found";
        case CaptureError::kFormatUnsupported: return "the device sample format is unsupported";
        case CaptureError::kAlreadyRunning: return "capture is already running";
        case CaptureError::kProcessLoopbackUnavailable:
            return "per-process loopback capture is not available on this platform";
        case CaptureError::kProcessNotFound: return "no process has the requested id";
    }
    return "unknown capture error";
}

std::expected<std::vector<DeviceInfo>, CaptureError> enumerate_devices() {
    return std::unexpected(CaptureError::kNoBackend);
}

struct Capture::Impl {};

Capture::Capture() : impl_(nullptr) {}
Capture::~Capture() = default;

std::expected<void, CaptureError> Capture::start(const std::string&, DeviceKind, std::size_t) {
    return std::unexpected(CaptureError::kNoBackend);
}

void Capture::stop() {}
bool Capture::running() const { return false; }
std::uint32_t Capture::sample_rate() const { return 0; }
std::uint16_t Capture::channels() const { return 0; }
CaptureStats Capture::stats() const { return {}; }
RingBuffer* Capture::buffer() { return nullptr; }

// Roadmap UX11's per-process tap is a Windows 10 build 20348+ WASAPI
// activation; nothing here has an equivalent, so the answer is a constant.
bool process_loopback_available() {
    return false;
}

std::expected<void, CaptureError> Capture::start_process_loopback(std::uint32_t,
                                                                 ProcessLoopbackMode,
                                                                 ProcessLoopbackFormat,
                                                                 std::size_t) {
    return std::unexpected(CaptureError::kNoBackend);
}

}  // namespace ac3::audio
