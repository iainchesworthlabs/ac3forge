#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ac3::audio {

// Roadmap UX11. An audio endpoint arriving, leaving, changing state or
// becoming the default: the events an application that follows the sink
// (docs/platforms/windows-demo.md, "Output modes and hot switching") needs
// in order to re-probe and switch outputs, instead of polling
// enumerate_render_devices() on a timer and hoping to notice.
//
// Windows only for now, over IMMNotificationClient. Every other backend
// refuses start() with kNoBackend, the way the rest of this tree does - the
// API stays, the answer is no. Ask audio_backend().device_watch first if the
// answer wants to be a sentence rather than an error code.

enum class DeviceWatchError : std::uint8_t {
    kNoBackend,       // built without a device-notification backend
    kComFailure,      // the platform refused to register the listener
    kAlreadyRunning,
};

[[nodiscard]] std::string_view describe(DeviceWatchError error);

enum class DeviceChange : std::uint8_t {
    kDefaultRenderChanged,   // the default render endpoint moved (or went away)
    kDefaultCaptureChanged,  // likewise for capture
    kAdded,                  // a new endpoint appeared
    kRemoved,                // an endpoint was removed
    kStateChanged,           // active <-> disabled / unplugged / not present
};

struct DeviceChangeEvent {
    DeviceChange change = DeviceChange::kStateChanged;
    // The endpoint id enumerate_devices()/enumerate_render_devices() report,
    // so the receiver can match it against a list it already holds. Empty
    // when a default changed to nothing at all (the last device went away).
    std::string device_id;
};

struct DeviceWatchStats {
    std::uint64_t events_delivered = 0;
};

class DeviceWatcher {
public:
    // Invoked on a platform thread of the audio subsystem's choosing, while
    // the watcher holds an internal lock so that stop() can guarantee no
    // callback is still in flight when it returns. Do the minimum inside it:
    // record the event and wake the thread that owns the sinks. Never
    // start or stop a sink from inside it, and never call stop() on this
    // watcher from inside it.
    using Callback = std::function<void(const DeviceChangeEvent&)>;

    DeviceWatcher();
    ~DeviceWatcher();
    DeviceWatcher(const DeviceWatcher&) = delete;
    DeviceWatcher& operator=(const DeviceWatcher&) = delete;

    // Registers with the platform and returns once registration has
    // succeeded or failed; events flow from then until stop().
    [[nodiscard]] std::expected<void, DeviceWatchError> start(Callback callback);
    // Unregisters, and returns only once no further callback can be in
    // flight. Safe to call when not running.
    void stop();
    [[nodiscard]] bool running() const;
    [[nodiscard]] DeviceWatchStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::audio
