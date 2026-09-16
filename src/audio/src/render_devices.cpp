#include "ac3/audio/render_devices.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

// See render_devices.hpp's header for what this is for and why the worker
// exists. Platform-independent: DeviceWatcher and the enumeration are what
// differ per backend, and both are already behind their own interfaces.
//
// std::thread with a flag and a condition variable, not std::jthread: this
// file is compiled into ac3audio for every platform including Android, whose
// NDK libc++ does not implement <stop_token> at all (see
// src/audio/src/backend/android/monitor.cpp's own note). The condition
// variable is what makes a stop prompt anyway - a re-probe interval of
// seconds must not be how long stop() takes.

namespace ac3::audio {

struct RenderDeviceWatch::Impl {
    Options options;
    Callback on_change;
    Enumerate enumerate;
    DeviceWatcher watcher;

    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    bool asked = false;  // a refresh, or a platform event, is pending
    Snapshot snapshot;
    Stats stats;
    std::thread worker;

    // Runs on the worker thread only, with `mutex` NOT held.
    void probe_once() {
        auto probed = enumerate(options.sample_rate);
        std::vector<RenderDeviceInfo> devices;
        bool ok = probed.has_value();
        if (ok) {
            devices = std::move(*probed);
        }
        Callback notify;
        {
            const std::lock_guard<std::mutex> guard{mutex};
            ++stats.probes;
            if (!ok) {
                // The last good list is kept deliberately: a device held
                // exclusively by something else, or an audio service
                // restarting, must not empty a caller's picker.
                ++stats.probe_failures;
                return;
            }
            if (devices == snapshot.devices) {
                return;
            }
            snapshot.devices = std::move(devices);
            ++snapshot.generation;
            ++stats.changes;
            notify = on_change;
        }
        if (notify) {
            notify();
        }
    }

    void run() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock{mutex};
                // A watched backend still waits on the timer, since a
                // notification can be missed and a device's own capabilities
                // can change without one.
                wake.wait_for(lock, options.reprobe, [this] { return stopping || asked; });
                if (stopping) {
                    return;
                }
                asked = false;
            }
            probe_once();
        }
    }
};

RenderDeviceWatch::RenderDeviceWatch() : impl_(std::make_unique<Impl>()) {}

RenderDeviceWatch::~RenderDeviceWatch() {
    stop();
}

bool RenderDeviceWatch::running() const {
    const std::lock_guard<std::mutex> guard{impl_->mutex};
    return impl_->worker.joinable() && !impl_->stopping;
}

RenderDeviceWatch::Snapshot RenderDeviceWatch::snapshot() const {
    const std::lock_guard<std::mutex> guard{impl_->mutex};
    return impl_->snapshot;
}

RenderDeviceWatch::Stats RenderDeviceWatch::stats() const {
    const std::lock_guard<std::mutex> guard{impl_->mutex};
    return impl_->stats;
}

void RenderDeviceWatch::refresh() {
    {
        const std::lock_guard<std::mutex> guard{impl_->mutex};
        impl_->asked = true;
    }
    impl_->wake.notify_all();
}

std::expected<void, DeviceWatchError> RenderDeviceWatch::start(Options options,
                                                                 Callback on_change) {
    return start(options, std::move(on_change), Sources{});
}

std::expected<void, DeviceWatchError> RenderDeviceWatch::start(Options options, Callback on_change,
                                                                 Sources sources) {
    if (running()) {
        return std::unexpected(DeviceWatchError::kAlreadyRunning);
    }
    stop();  // a previous run that has already been stopped leaves a joinable thread

    impl_->options = options;
    // A zero interval would be a spin, not a poll; one millisecond is as
    // eager as this is willing to be.
    impl_->options.reprobe = std::max(options.reprobe, std::chrono::milliseconds{1});
    impl_->on_change = std::move(on_change);
    impl_->enumerate = sources.enumerate
                           ? std::move(sources.enumerate)
                           : Enumerate{[](std::uint32_t rate) {
                                 return enumerate_render_devices(rate);
                             }};

    // The first list is this call's own: a machine with no audio backend at
    // all has nothing to watch, and saying so here is better than starting a
    // worker that will fail forever.
    auto first = impl_->enumerate(options.sample_rate);
    if (!first) {
        return std::unexpected(DeviceWatchError::kNoBackend);
    }
    {
        const std::lock_guard<std::mutex> guard{impl_->mutex};
        impl_->stopping = false;
        impl_->asked = false;
        impl_->snapshot = Snapshot{.devices = std::move(*first), .generation = 1, .watched = false};
        impl_->stats = Stats{.events = 0, .probes = 1, .changes = 0, .probe_failures = 0};
    }

    if (sources.platform_watcher) {
        // The platform's thread does nothing but count the event and wake the
        // worker - see the header on why it must not enumerate here.
        Impl* impl = impl_.get();
        const auto watching = impl_->watcher.start([impl](const DeviceChangeEvent&) {
            {
                const std::lock_guard<std::mutex> guard{impl->mutex};
                ++impl->stats.events;
                impl->asked = true;
            }
            impl->wake.notify_all();
        });
        if (watching) {
            const std::lock_guard<std::mutex> guard{impl_->mutex};
            impl_->snapshot.watched = true;
        }
        // A refusal is not an error: the timer alone keeps the list current,
        // which is all ALSA ever had. Snapshot::watched is what says so.
    }

    {
        const std::lock_guard<std::mutex> guard{impl_->mutex};
        impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
    }
    return {};
}

void RenderDeviceWatch::stop() {
    impl_->watcher.stop();
    // The thread handle is taken under the mutex rather than joined in place:
    // two threads calling stop() (or a destructor racing an explicit one)
    // would otherwise both see it joinable and both join, which is undefined.
    // Whoever takes it does the join; the other finds nothing to do.
    std::thread worker;
    {
        const std::lock_guard<std::mutex> guard{impl_->mutex};
        impl_->stopping = true;
        worker = std::move(impl_->worker);
    }
    impl_->wake.notify_all();
    if (worker.joinable()) {
        // This waits for an enumeration already in flight, which on ALSA is
        // an open and a channel-map query per playback PCM and can take a
        // noticeable fraction of a second (an HDMI output with no display
        // attached is the slow case). Bounded by the platform call rather
        // than by the re-probe interval, which is what the condition
        // variable above is for.
        worker.join();
    }
}

}  // namespace ac3::audio
