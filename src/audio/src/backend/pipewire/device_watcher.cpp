#include "ac3/audio/device_watcher.hpp"

// The PipeWire device watcher: there isn't one yet. PipeWire's registry does
// deliver node add/remove events, so this is the one non-Windows backend
// where a real implementation is a matter of writing it rather than of the
// platform lacking the facility. Until then every entry point fails with
// kNoBackend rather than the API disappearing, matching
// platform/posix/device_watcher.cpp.

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
