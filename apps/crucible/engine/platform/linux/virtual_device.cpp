#include "virtual_device.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <thread>

// pw_context_load_module() and pw_impl_module live in PipeWire's
// implementation header, not the client one: loading a module is something
// a host does, and a client that wants to own a node does it too.
#include <pipewire/impl-module.h>

#include "pipewire_support.hpp"
#include "platform_services.hpp"

// The Linux VirtualDevice: the silent device applications play into
// (docs/crucible/promotion.md, "The silent device, per platform").
//
// Windows needs a signed kernel driver for this, because Windows gives user
// mode no way to create a render endpoint. Linux does: a client loads
// `libpipewire-module-adapter` with the `support.null-audio-sink` factory
// and the node it creates is visible to every other client in the graph.
// No driver, no signing, no elevation, no reboot.
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
constexpr const char* kModule = "libpipewire-module-adapter";
constexpr const char* kArgs =
    "{ factory.name=support.null-audio-sink "
    "  node.name=ac3forge_crucible_sink "
    "  node.description=\"Crucible (silent)\" "
    "  media.class=Audio/Sink "
    "  object.linger=false "
    "  audio.position=[FL FR FC LFE SL SR RL RR] "
    "  audio.rate=48000 "
    "  monitor.channel-volumes=true }";

// The node lives on a context that has to stay alive for as long as it does,
// so this owns a thread loop of its own rather than a scoped round trip like
// default_device.cpp's reads.
class LinuxVirtualDevice final : public VirtualDevice {
public:
    ~LinuxVirtualDevice() override { teardown(); }

    // Meaningless here: there is no package to point at.
    void set_package_dir(std::string_view) override {}

    SilentDeviceState state(const SilentDeviceQuery& query) override {
        const std::lock_guard lock(mutex_);
        SilentDeviceState out;
        out.needed = true;
        out.present = query.endpoint_present || loaded_;
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
        module_ = pw_context_load_module(context_.get(), kModule, kArgs, nullptr);
        pw_thread_loop_unlock(loop_.get());
        if (module_ == nullptr) {
            teardown_locked();
            return fail("PipeWire refused to load the null-sink module");
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

    void teardown_locked() {
        if (loop_) {
            pw_thread_loop_stop(loop_.get());
        }
        // The module goes with the context that loaded it, and the node with
        // the connection, which is the whole point of object.linger=false.
        module_ = nullptr;
        core_.reset();
        context_.reset();
        loop_.reset();
        loaded_ = false;
    }

    std::mutex mutex_;
    ac3::pipewire::ThreadLoop loop_;
    ac3::pipewire::Context context_;
    ac3::pipewire::Core core_;
    pw_impl_module* module_ = nullptr;
    bool loaded_ = false;
    std::string last_error_;
};

}  // namespace

std::shared_ptr<VirtualDevice> platform_virtual_device() {
    return std::make_shared<LinuxVirtualDevice>();
}

}  // namespace ac3::crucible
