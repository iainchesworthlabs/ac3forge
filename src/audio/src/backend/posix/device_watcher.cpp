#include "ac3/audio/device_watcher.hpp"

// The Unix device watcher: there isn't one. CMake compiles this directory's
// device_watcher.cpp on a Linux host with neither ALSA nor PipeWire
// development headers, and every entry point fails with kNoBackend rather
// than the API disappearing from the library - callers keep compiling, and
// get told no instead of getting nothing. See
// platform/posix/audio_backend.cpp for why there is no backend here.

namespace ac3::audio {

std::string_view describe(DeviceWatchError error) {
    switch (error) {
        case DeviceWatchError::kNoBackend: return "no device-notification backend on this platform";
        case DeviceWatchError::kComFailure: return "a platform audio call failed";
        case DeviceWatchError::kAlreadyRunning: return "the device watcher is already running";
    }
    return "unknown device watch error";
}

struct DeviceWatcher::Impl {};

DeviceWatcher::DeviceWatcher() : impl_(nullptr) {}
DeviceWatcher::~DeviceWatcher() = default;

std::expected<void, DeviceWatchError> DeviceWatcher::start(Callback) {
    return std::unexpected(DeviceWatchError::kNoBackend);
}

void DeviceWatcher::stop() {}
bool DeviceWatcher::running() const { return false; }
DeviceWatchStats DeviceWatcher::stats() const { return {}; }

}  // namespace ac3::audio
