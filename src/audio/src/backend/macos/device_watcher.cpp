#include "ac3/audio/device_watcher.hpp"

// The Core Audio device watcher: there isn't one yet. A property listener on
// kAudioHardwarePropertyDevices and kAudioHardwarePropertyDefaultOutputDevice
// would deliver exactly these events; not attempted without a Mac to run it
// on (roadmap DR9). Every entry point fails with kNoBackend rather than the
// API disappearing, matching platform/posix/device_watcher.cpp.

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
