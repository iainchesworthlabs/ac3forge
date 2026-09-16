#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <vector>

#include "ac3/audio/device_watcher.hpp"
#include "ac3/audio/passthrough.hpp"

// A render-device list that keeps itself current, however the platform says
// so - or does not.
//
// enumerate_render_devices() answers once, and DeviceWatcher reports that
// something changed on the three backends whose audio system has a
// notification mechanism (Windows, PipeWire, Core Audio). ALSA has none -
// that is udev's job on Linux - so a player that wants the receiver it was
// just plugged into to appear has to re-probe there instead. Both halves are
// the same job from a caller's point of view, so this is the one place that
// does it: notifications where they exist, a timer where they do not, and a
// list plus a generation either way.
//
// Two things it deliberately does NOT do on the platform's own notification
// thread: enumerate, and call the caller back. A WASAPI notification arrives
// on a thread inside the audio service's own COM apartment, and re-entering
// the enumerator from there is a documented way to deadlock; the same
// caution applies to a PipeWire loop thread and a Core Audio property
// listener. So an event only wakes this class's worker, which does the
// enumeration and the callback - a caller's callback therefore always runs
// on one thread it can reason about, and may take its own locks.
//
// Tested without a sound card in tests/audio/test_render_devices.cpp, through
// the Sources seam below: an enumeration of the test's own and no platform
// watcher is exactly the shape ALSA runs in.

namespace ac3::audio {

class RenderDeviceWatch {
public:
    using Enumerate = std::function<std::expected<std::vector<RenderDeviceInfo>, PassthroughError>(
        std::uint32_t sample_rate)>;
    // Called on this class's worker thread, after the list has changed and
    // the new one is readable through snapshot(). Never called with a lock of
    // this class held, so it may call snapshot() - but not stop(), which
    // would be waiting for this thread.
    using Callback = std::function<void()>;

    struct Options {
        // What enumerate_render_devices() probes passthrough support at.
        std::uint32_t sample_rate = 48000;
        // How often to re-enumerate where the platform reports nothing. Also
        // the longest a watched backend goes without a re-probe, since a
        // notification can be missed and a device's own capabilities can
        // change without one (a receiver renegotiating HDMI, say).
        std::chrono::milliseconds reprobe{std::chrono::seconds{2}};
    };

    // The test seam, and the way to run this over a list from somewhere else.
    struct Sources {
        // Empty uses enumerate_render_devices().
        Enumerate enumerate{};
        // False skips registering a platform watcher: every refresh is then
        // the timer's, which is what ALSA does in any case.
        bool platform_watcher = true;
    };

    struct Snapshot {
        std::vector<RenderDeviceInfo> devices;
        // Incremented once per change; 1 is the list start() established.
        // A caller that holds a copy compares this rather than the lists.
        std::uint64_t generation = 0;
        // Whether a platform watcher is behind this list. False means the
        // list is only as fresh as the last re-probe - true on ALSA always,
        // and wherever registering the watcher was refused.
        bool watched = false;
    };

    RenderDeviceWatch();
    ~RenderDeviceWatch();
    RenderDeviceWatch(const RenderDeviceWatch&) = delete;
    RenderDeviceWatch& operator=(const RenderDeviceWatch&) = delete;

    // Enumerates once, then follows the platform and the timer. The first
    // enumeration's failure is this call's failure: a machine with no audio
    // backend at all has nothing to watch (kNoBackend). A platform watcher
    // that will not register is NOT a failure - the timer alone still keeps
    // the list current, and Snapshot::watched says which it is.
    [[nodiscard]] std::expected<void, DeviceWatchError> start(Options options, Callback on_change);

    // The same, over an enumeration of the caller's own or with no platform
    // watcher - see Sources. An overload rather than a defaulted third
    // parameter because Sources' own member defaults are not usable in a
    // default argument declared inside this class (they are only complete at
    // the end of it); MSVC accepts that, GCC refuses it, and it is GCC that
    // is right.
    [[nodiscard]] std::expected<void, DeviceWatchError> start(Options options, Callback on_change,
                                                               Sources sources);

    // Stops the worker and unregisters, and returns once no callback can be
    // in flight. Safe when not running, and safe to call twice.
    void stop();
    [[nodiscard]] bool running() const;

    [[nodiscard]] Snapshot snapshot() const;

    // Re-enumerates as soon as the worker can, without waiting for the timer:
    // what to call after a failure that suggests the list is stale. Returns
    // once the worker has been asked, not once it has finished.
    void refresh();

    struct Stats {
        // Platform notifications received, whether or not the list changed.
        std::uint64_t events = 0;
        // Enumerations run, including start()'s.
        std::uint64_t probes = 0;
        // Times the list came back different, which is what the callback
        // counts too (start()'s first list is not a change).
        std::uint64_t changes = 0;
        // Enumerations that failed. The last good list is kept: a transient
        // refusal (a device held exclusively, a service restarting) must not
        // empty a picker.
        std::uint64_t probe_failures = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::audio
