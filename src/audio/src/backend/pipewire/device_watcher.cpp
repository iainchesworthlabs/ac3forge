#include "ac3/audio/device_watcher.hpp"

#include <pipewire/extensions/metadata.h>

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "pipewire_support.hpp"

// The PipeWire device watcher (roadmap UX12). PipeWire already tells every
// client what the graph is doing; this turns two of its streams of events
// into the four this library's callers care about.
//
// **Added and removed** come from the registry. `global` carries a property
// dictionary, so an arriving node can be filtered to the sinks and sources
// this backend deals with and reported with the same id enumerate_devices()
// would give it. `global_remove` carries only the numeric id, so the
// mapping from that id to the device id has to be kept while the node
// exists - which is what `nodes_` is.
//
// **The default changed** is not a registry event at all here. PipeWire
// keeps the default sink and source in a metadata object under the keys
// `default.audio.sink` and `default.audio.source`, so the watcher binds the
// metadata global and listens to its `property` event. That is also how
// `wpctl status` reads the default, and how this application will move it.
//
// Callbacks run on the thread loop's own thread. stop() calls
// pw_thread_loop_stop(), which joins that thread, so once it returns no
// callback can still be in flight - the guarantee the header documents. The
// mutex guards the callback and the stats against a caller reading stats()
// from its own thread at the same time.

namespace ac3::audio {

using ac3::pipewire::Context;
using ac3::pipewire::Core;
using ac3::pipewire::Registry;
using ac3::pipewire::ThreadLoop;

std::string_view describe(DeviceWatchError error) {
    switch (error) {
        case DeviceWatchError::kNoBackend: return "no device-notification backend on this platform";
        case DeviceWatchError::kComFailure: return "a PipeWire call failed";
        case DeviceWatchError::kAlreadyRunning: return "the device watcher is already running";
    }
    return "unknown device watch error";
}

struct DeviceWatcher::Impl {
    ThreadLoop loop;
    Context context;
    Core core;
    Registry registry;
    spa_hook registry_listener{};
    pw_registry_events registry_events{};

    // The metadata object that holds default.audio.sink/source, bound for as
    // long as the watcher runs.
    pw_proxy* metadata = nullptr;
    spa_hook metadata_listener{};
    pw_metadata_events metadata_events{};

    std::mutex mutex;
    Callback callback;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> events{0};

    // Global id -> the device id this library reports for it, kept because
    // global_remove says only which number went away.
    std::unordered_map<std::uint32_t, std::string> nodes;

    void deliver(DeviceChange change, std::string device_id) {
        const std::lock_guard lock(mutex);
        if (!callback) {
            return;
        }
        events.fetch_add(1, std::memory_order_relaxed);
        callback(DeviceChangeEvent{.change = change, .device_id = std::move(device_id)});
    }

    static void on_global(void* data, std::uint32_t id, std::uint32_t /*permissions*/,
                          const char* type, std::uint32_t /*version*/, const spa_dict* props) {
        auto* self = static_cast<Impl*>(data);
        if (type == nullptr || props == nullptr) {
            return;
        }
        const std::string_view kind{type};
        if (kind == PW_TYPE_INTERFACE_Node) {
            if (!ac3::pipewire::is_audio_sink(*props) && !ac3::pipewire::is_audio_source(*props)) {
                return;
            }
            std::string device_id = ac3::pipewire::node_id(*props);
            self->nodes.emplace(id, device_id);
            self->deliver(DeviceChange::kAdded, std::move(device_id));
            return;
        }
        // One metadata object matters: the one named "default", which is
        // where the session manager keeps the default sink and source.
        if (kind == PW_TYPE_INTERFACE_Metadata && self->metadata == nullptr) {
            const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
            if (name == nullptr || std::string_view{name} != "default") {
                return;
            }
            auto* proxy = static_cast<pw_proxy*>(
                pw_registry_bind(self->registry.get(), id, type, PW_VERSION_METADATA, 0));
            if (proxy == nullptr) {
                return;
            }
            self->metadata = proxy;
            self->metadata_events.version = PW_VERSION_METADATA_EVENTS;
            self->metadata_events.property = &Impl::on_metadata_property;
            pw_proxy_add_object_listener(proxy, &self->metadata_listener, &self->metadata_events,
                                         self);
        }
    }

    static void on_global_remove(void* data, std::uint32_t id) {
        auto* self = static_cast<Impl*>(data);
        const auto it = self->nodes.find(id);
        if (it == self->nodes.end()) {
            return;
        }
        std::string device_id = std::move(it->second);
        self->nodes.erase(it);
        self->deliver(DeviceChange::kRemoved, std::move(device_id));
    }

    static int on_metadata_property(void* data, std::uint32_t /*subject*/, const char* key,
                                    const char* /*type*/, const char* value) {
        auto* self = static_cast<Impl*>(data);
        if (key == nullptr) {
            return 0;
        }
        const std::string_view name{key};
        if (name != "default.audio.sink" && name != "default.audio.source") {
            return 0;
        }
        // The value is JSON - {"name":"alsa_output...."} - and the name
        // inside it is the node.name this library uses as a device id. A
        // cleared default (the last device went away) arrives as a null
        // value, which the header says to report as an empty id.
        std::string device_id;
        if (value != nullptr) {
            const std::string_view json{value};
            if (const auto at = json.find("\"name\""); at != std::string_view::npos) {
                const auto open = json.find('"', json.find(':', at) + 1);
                const auto close = open == std::string_view::npos
                                       ? std::string_view::npos
                                       : json.find('"', open + 1);
                if (open != std::string_view::npos && close != std::string_view::npos) {
                    device_id = std::string{json.substr(open + 1, close - open - 1)};
                }
            }
        }
        self->deliver(name == "default.audio.sink" ? DeviceChange::kDefaultRenderChanged
                                                   : DeviceChange::kDefaultCaptureChanged,
                      std::move(device_id));
        return 0;
    }
};

DeviceWatcher::DeviceWatcher() : impl_(std::make_unique<Impl>()) {}

DeviceWatcher::~DeviceWatcher() {
    stop();
}

std::expected<void, DeviceWatchError> DeviceWatcher::start(Callback callback) {
    if (impl_->running.load(std::memory_order_acquire)) {
        return std::unexpected(DeviceWatchError::kAlreadyRunning);
    }

    ac3::pipewire::ensure_initialized();

    impl_->loop = ThreadLoop{pw_thread_loop_new("ac3audio-devicewatch", nullptr)};
    if (!impl_->loop) {
        return std::unexpected(DeviceWatchError::kComFailure);
    }

    // Built before the loop starts, so the first registry events - PipeWire
    // replays every existing global to a new listener - cannot arrive while
    // this is half-constructed.
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->callback = std::move(callback);
    }

    impl_->context = Context{
        pw_context_new(pw_thread_loop_get_loop(impl_->loop.get()), nullptr, 0)};
    if (!impl_->context) {
        impl_->loop.reset();
        return std::unexpected(DeviceWatchError::kComFailure);
    }
    impl_->core = Core{pw_context_connect(impl_->context.get(), nullptr, 0)};
    if (!impl_->core) {
        // No session running: a container, a CI runner, a desktop where the
        // daemon is not started. The same "nothing to ask" case the rest of
        // this backend reports rather than pretending to have registered.
        impl_->context.reset();
        impl_->loop.reset();
        return std::unexpected(DeviceWatchError::kComFailure);
    }
    impl_->registry = Registry{pw_core_get_registry(impl_->core.get(), PW_VERSION_REGISTRY, 0)};
    if (!impl_->registry) {
        impl_->core.reset();
        impl_->context.reset();
        impl_->loop.reset();
        return std::unexpected(DeviceWatchError::kComFailure);
    }

    impl_->registry_events.version = PW_VERSION_REGISTRY_EVENTS;
    impl_->registry_events.global = &Impl::on_global;
    impl_->registry_events.global_remove = &Impl::on_global_remove;
    pw_registry_add_listener(impl_->registry.get(), &impl_->registry_listener,
                             &impl_->registry_events, impl_.get());

    if (pw_thread_loop_start(impl_->loop.get()) < 0) {
        spa_hook_remove(&impl_->registry_listener);
        impl_->registry.reset();
        impl_->core.reset();
        impl_->context.reset();
        impl_->loop.reset();
        return std::unexpected(DeviceWatchError::kComFailure);
    }

    impl_->running.store(true, std::memory_order_release);
    return {};
}

void DeviceWatcher::stop() {
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // Joins the loop's thread, so no callback can be in flight once this
    // returns - which is what the header promises the caller.
    pw_thread_loop_stop(impl_->loop.get());
    if (impl_->metadata != nullptr) {
        spa_hook_remove(&impl_->metadata_listener);
        pw_proxy_destroy(impl_->metadata);
        impl_->metadata = nullptr;
    }
    if (impl_->registry) {
        spa_hook_remove(&impl_->registry_listener);
    }
    impl_->registry.reset();
    impl_->core.reset();
    impl_->context.reset();
    impl_->loop.reset();
    impl_->nodes.clear();
    const std::lock_guard lock(impl_->mutex);
    impl_->callback = nullptr;
}

bool DeviceWatcher::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

DeviceWatchStats DeviceWatcher::stats() const {
    return DeviceWatchStats{.events_delivered = impl_->events.load(std::memory_order_relaxed)};
}

}  // namespace ac3::audio
