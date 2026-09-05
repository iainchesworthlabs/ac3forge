#include "virtual_device.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <string_view>

#include "pipewire_support.hpp"
#include "platform_services.hpp"

// The Linux VirtualDevice: the silent device applications play into
// (docs/crucible/promotion.md, "The silent device, per platform").
//
// Windows needs a signed kernel driver for this, because Windows gives user
// mode no way to create a render endpoint. Linux does: a client asks the
// daemon to make one, through the `adapter` factory with
// `factory.name=support.null-audio-sink`. No driver, no signing, no
// elevation, no reboot.
//
// It has to be `pw_core_create_object()` and not `pw_context_load_module()`.
// Loading the adapter module into this process's own context does succeed -
// it returns a module and reports no error - but the node it makes lives in
// that local context and is never exported to the daemon's registry, so no
// other client can see it, target it, or be made to play into it. The first
// version here did exactly that and reported the device as present on the
// strength of the module load; the graph had no such node, and the device
// watcher never saw one arrive, which is how it was caught.
// pw_core_create_object() asks the daemon for the object instead, so it is a
// real global that other clients can use.
//
// It also behaves better than the Windows device in one way worth stating.
// The node belongs to **this application's own connection**, so it exists
// exactly as long as the application runs and disappears when it exits -
// where Windows leaves an installed driver and an endpoint behind until
// somebody uninstalls it. A person who tries Crucible on Linux and quits is
// left with their machine exactly as they found it, and there is nothing to
// remove.
//
// The consequence is that `install()` and `remove()` are not installs at
// all; they create and destroy the node, immediately, with no elevation
// prompt to wait for. `action_status()` therefore never reports anything as
// running.

namespace ac3::crucible {

namespace {

// Enough of a node for other clients to pick it as a sink. 48 kHz, eight
// channels, so a surround-rendering application's tap arrives as eight
// channels the way the Windows driver's 7.1 advertisement makes it - see
// EngineConfig::tap_channels.
constexpr const char* kNodeName = "ac3forge_crucible_sink";

// The node lives on a context that has to stay alive for as long as it does,
// so this owns a thread loop of its own rather than a scoped round trip like
// default_device.cpp's reads.
class LinuxVirtualDevice final : public VirtualDevice {
public:
    ~LinuxVirtualDevice() override { teardown(); }

    // Meaningless here: there is no package to point at.
    void set_package_dir(std::string_view) override {}
    bool from_package() const override { return false; }

    std::string device_name() const override { return "Crucible (silent)"; }
    std::string how_to_get_one() const override {
        return "Crucible creates it when you send applications to it; nothing to install";
    }

    SilentDeviceState state(const SilentDeviceQuery& query) override {
        const std::lock_guard lock(mutex_);
        SilentDeviceState out;
        out.needed = true;
        // Asked of the graph, not of a flag. A local module load reports
        // success for a node nobody else can see, so "did the call return"
        // is not the same question as "is there a silent device", and only
        // the second one matters to anything downstream.
        out.present = query.endpoint_present || node_in_graph();
        out.in_use = query.endpoint_is_default;
        out.can_install = !loaded_;
        if (!out.present && !last_error_.empty()) {
            out.blocker = last_error_;
        }
        out.detail.push_back(
            loaded_ ? "the silent device is this application's own PipeWire node; it goes when "
                      "the application does"
                    : "no driver is needed here: the silent device is a PipeWire module load");
        return out;
    }

    std::expected<void, std::string> install() override {
        const std::lock_guard lock(mutex_);
        if (loaded_) {
            return {};
        }
        ac3::pipewire::ensure_initialized();

        loop_ = ac3::pipewire::ThreadLoop{pw_thread_loop_new("ac3crucible-sink", nullptr)};
        if (!loop_) {
            return fail("could not create a PipeWire loop for the silent device");
        }
        if (pw_thread_loop_start(loop_.get()) < 0) {
            loop_.reset();
            return fail("could not start a PipeWire loop for the silent device");
        }

        pw_thread_loop_lock(loop_.get());
        context_ = ac3::pipewire::Context{
            pw_context_new(pw_thread_loop_get_loop(loop_.get()), nullptr, 0)};
        if (!context_) {
            pw_thread_loop_unlock(loop_.get());
            teardown_locked();
            return fail("could not create a PipeWire context for the silent device");
        }
        core_ = ac3::pipewire::Core{pw_context_connect(context_.get(), nullptr, 0)};
        if (!core_) {
            pw_thread_loop_unlock(loop_.get());
            teardown_locked();
            return fail("no PipeWire session to create the silent device in");
        }
        // The daemon's own adapter factory, so the node is a real global.
        // object.linger=false ties it to this connection: it goes when the
        // application does, which is the behaviour worth keeping.
        pw_properties* props = pw_properties_new(
            "factory.name", "support.null-audio-sink", PW_KEY_NODE_NAME, kNodeName,
            PW_KEY_NODE_DESCRIPTION, "Crucible (silent)", PW_KEY_MEDIA_CLASS, "Audio/Sink",
            "object.linger", "false", "audio.position", "[FL,FR,FC,LFE,SL,SR,RL,RR]",
            "audio.rate", "48000", "monitor.channel-volumes", "true", nullptr);
        node_ = static_cast<pw_proxy*>(pw_core_create_object(
            core_.get(), "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0));
        pw_properties_free(props);
        pw_thread_loop_unlock(loop_.get());
        if (node_ == nullptr) {
            teardown_locked();
            return fail("PipeWire refused to create the silent device");
        }
        loaded_ = true;
        last_error_.clear();
        return {};
    }

    std::expected<void, std::string> remove() override {
        const std::lock_guard lock(mutex_);
        if (!loaded_) {
            return {};
        }
        teardown_locked();
        return {};
    }

    // Nothing here is asynchronous: there is no elevation prompt to wait on.
    DeviceActionStatus action_status() override { return {}; }

private:
    std::expected<void, std::string> fail(std::string reason) {
        last_error_ = reason;
        return std::unexpected(std::move(reason));
    }

    void teardown() {
        const std::lock_guard lock(mutex_);
        teardown_locked();
    }

    // Whether the graph actually holds our node, asked of the daemon.
    [[nodiscard]] static bool node_in_graph() {
        bool found = false;
        ac3::pipewire::for_each_audio_node([&found](std::uint32_t, const spa_dict& props) {
            if (found) {
                return;
            }
            const char* name = spa_dict_lookup(&props, PW_KEY_NODE_NAME);
            found = name != nullptr && std::string_view{name} == kNodeName;
        });
        return found;
    }

    void teardown_locked() {
        if (loop_) {
            pw_thread_loop_stop(loop_.get());
            pw_thread_loop_lock(loop_.get());
            // The proxy first, then the core it belongs to: a core destroyed
            // with a live proxy is the "impl_ext_end_proxy: Device or
            // resource busy" the window logged. The node itself goes with
            // the connection either way (object.linger=false).
            if (node_ != nullptr) {
                pw_proxy_destroy(node_);
                node_ = nullptr;
            }
            core_.reset();
            context_.reset();
            pw_thread_loop_unlock(loop_.get());
        }
        node_ = nullptr;
        core_.reset();
        context_.reset();
        loop_.reset();
        loaded_ = false;
    }

    std::mutex mutex_;
    ac3::pipewire::ThreadLoop loop_;
    ac3::pipewire::Context context_;
    ac3::pipewire::Core core_;
    pw_proxy* node_ = nullptr;
    bool loaded_ = false;
    std::string last_error_;
};

}  // namespace

std::shared_ptr<VirtualDevice> platform_virtual_device() {
    return std::make_shared<LinuxVirtualDevice>();
}

}  // namespace ac3::crucible
