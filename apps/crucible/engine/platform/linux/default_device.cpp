#include "default_device.hpp"

#include <pipewire/extensions/metadata.h>

#include <cstdlib>
#include <memory>
#include <string>

#include "ac3/audio/passthrough.hpp"
#include "pipewire_support.hpp"
#include "platform_services.hpp"

// The Linux DefaultDevice (docs/crucible/promotion.md, Phase 4).
//
// Two of the five answers come free: the endpoint list is the library's own
// enumerate_render_devices(), which is what the output stage already probes,
// and the name match over it is the same string search Windows does.
//
// The default itself is not a device property here. PipeWire keeps it in a
// metadata object named "default", under the key `default.audio.sink`, whose
// value is JSON: {"name":"alsa_output...."}. Reading it is a registry walk
// plus a bind; writing it is pw_metadata_set_property on the same object,
// which is exactly what `wpctl set-default` does. Unlike Windows'
// IPolicyConfig this is a documented, supported interface, so a refusal here
// means something went wrong rather than that the call was never meant to be
// made - and there is no Settings app to fall back to, which is what
// open_sound_settings() being a no-op reflects.

namespace ac3::crucible {

namespace {

constexpr const char* kDefaultSinkKey = "default.audio.sink";

// The node name inside PipeWire's JSON metadata value.
[[nodiscard]] std::string name_from_json(const char* value) {
    if (value == nullptr) {
        return {};
    }
    const std::string_view json{value};
    const auto at = json.find("\"name\"");
    if (at == std::string_view::npos) {
        return {};
    }
    const auto colon = json.find(':', at);
    if (colon == std::string_view::npos) {
        return {};
    }
    const auto open = json.find('"', colon + 1);
    if (open == std::string_view::npos) {
        return {};
    }
    const auto close = json.find('"', open + 1);
    if (close == std::string_view::npos) {
        return {};
    }
    return std::string{json.substr(open + 1, close - open - 1)};
}

// One connect - act - disconnect round trip against the "default" metadata
// object, the same synchronous shape for_each_audio_node() uses. `act` is
// given the bound proxy and may read or write; it returns true on success.
template <typename Action>
bool with_default_metadata(Action&& act) {
    ac3::pipewire::ensure_initialized();

    ac3::pipewire::MainLoop loop{pw_main_loop_new(nullptr)};
    if (!loop) {
        return false;
    }
    ac3::pipewire::Context context{
        pw_context_new(pw_main_loop_get_loop(loop.get()), nullptr, 0)};
    if (!context) {
        return false;
    }
    ac3::pipewire::Core core{pw_context_connect(context.get(), nullptr, 0)};
    if (!core) {
        return false;  // no session
    }
    ac3::pipewire::Registry registry{
        pw_core_get_registry(core.get(), PW_VERSION_REGISTRY, 0)};
    if (!registry) {
        return false;
    }

    struct State {
        pw_registry* registry = nullptr;
        pw_proxy* metadata = nullptr;
        pw_main_loop* loop = nullptr;
        int pending = 0;
        bool done = false;
    } state{registry.get(), nullptr, loop.get(), 0, false};

    spa_hook registry_listener{};
    pw_registry_events registry_events{};
    registry_events.version = PW_VERSION_REGISTRY_EVENTS;
    registry_events.global = [](void* data, std::uint32_t id, std::uint32_t, const char* type,
                                std::uint32_t, const spa_dict* props) {
        auto* self = static_cast<State*>(data);
        if (self->metadata != nullptr || props == nullptr || type == nullptr) {
            return;
        }
        if (std::string_view{type} != PW_TYPE_INTERFACE_Metadata) {
            return;
        }
        const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (name == nullptr || std::string_view{name} != "default") {
            return;
        }
        self->metadata = static_cast<pw_proxy*>(
            pw_registry_bind(self->registry, id, type, PW_VERSION_METADATA, 0));
    };
    pw_registry_add_listener(registry.get(), &registry_listener, &registry_events, &state);

    // Wait for the server to confirm it has sent every existing global, the
    // same sequence-matched sync for_each_audio_node() documents.
    spa_hook core_listener{};
    pw_core_events core_events{};
    core_events.version = PW_VERSION_CORE_EVENTS;
    core_events.done = [](void* data, std::uint32_t id, int seq) {
        auto* self = static_cast<State*>(data);
        if (id == PW_ID_CORE && seq == self->pending) {
            self->done = true;
            pw_main_loop_quit(self->loop);
        }
    };
    pw_core_add_listener(core.get(), &core_listener, &core_events, &state);
    state.pending = pw_core_sync(core.get(), PW_ID_CORE, 0);
    pw_main_loop_run(loop.get());

    bool ok = false;
    if (state.metadata != nullptr) {
        ok = act(state.metadata, core.get(), loop.get());
        pw_proxy_destroy(state.metadata);
    }
    spa_hook_remove(&core_listener);
    spa_hook_remove(&registry_listener);
    return ok;
}

class LinuxDefaultDevice final : public DefaultDevice {
public:
    std::vector<RenderEndpoint> endpoints() override {
        std::vector<RenderEndpoint> out;
        const auto devices = ac3::audio::enumerate_render_devices();
        if (!devices) {
            return out;
        }
        const std::string current = default_id();
        out.reserve(devices->size());
        for (const auto& device : *devices) {
            out.push_back({.id = device.id,
                           .name = device.name,
                           .is_default = !current.empty() && device.id == current});
        }
        // Default first, the order the header documents.
        std::stable_partition(out.begin(), out.end(),
                              [](const RenderEndpoint& e) { return e.is_default; });
        return out;
    }

    std::string default_id() override {
        std::string name;
        with_default_metadata([&name](pw_proxy* metadata, pw_core* core, pw_main_loop* loop) {
            struct Read {
                std::string* out;
                pw_main_loop* loop;
            } read{&name, loop};
            spa_hook listener{};
            pw_metadata_events events{};
            events.version = PW_VERSION_METADATA_EVENTS;
            events.property = [](void* data, std::uint32_t, const char* key, const char*,
                                 const char* value) {
                auto* self = static_cast<Read*>(data);
                if (key != nullptr && std::string_view{key} == kDefaultSinkKey) {
                    *self->out = name_from_json(value);
                }
                return 0;
            };
            pw_proxy_add_object_listener(metadata, &listener, &events, &read);
            // Binding replays every property; one sync round trip is enough
            // to know they have all arrived.
            pw_core_sync(core, PW_ID_CORE, 0);
            pw_main_loop_run(loop);
            spa_hook_remove(&listener);
            return true;
        });
        return name;
    }

    std::expected<void, std::string> set_default(std::string_view endpoint_id) override {
        if (endpoint_id.empty()) {
            return std::unexpected("no endpoint given");
        }
        const std::string json = "{\"name\":\"" + std::string{endpoint_id} + "\"}";
        const bool ok = with_default_metadata(
            [&json](pw_proxy* metadata, pw_core* core, pw_main_loop* loop) {
                pw_metadata_set_property(reinterpret_cast<pw_metadata*>(metadata), PW_ID_CORE,
                                         kDefaultSinkKey, "Spa:String:JSON", json.c_str());
                pw_core_sync(core, PW_ID_CORE, 0);
                pw_main_loop_run(loop);
                return true;
            });
        if (!ok) {
            return std::unexpected(
                "could not reach PipeWire's default metadata; is a session running?");
        }
        return {};
    }

    bool moves_default() const override { return true; }

    std::string find_endpoint(std::string_view name_substring) override {
        if (name_substring.empty()) {
            return {};
        }
        for (const auto& endpoint : endpoints()) {
            if (endpoint.name.find(name_substring) != std::string::npos ||
                endpoint.id.find(name_substring) != std::string::npos) {
                return endpoint.id;
            }
        }
        return {};
    }

    // Nothing to open: there is no one sound settings page across desktops,
    // and set_default() here is a supported call rather than the unsupported
    // one Windows falls back from.
    void open_sound_settings() override {}
};

}  // namespace

std::shared_ptr<DefaultDevice> platform_default_device() {
    return std::make_shared<LinuxDefaultDevice>();
}

}  // namespace ac3::crucible
