#include <memory>

#include "platform_services.hpp"

// ac3tests compiles the application's engine core on every platform
// (tests/CMakeLists.txt), and that core falls back to the platform's own
// services when a test hands it none - a path the tests never take, since
// every one of them passes a fake. These definitions satisfy the linker
// where no platform directory is built, and answer with nothing if they are
// ever reached.
//
// They are inert rather than absent on purpose: a platform that cannot do
// something reports that through its own support()/state(), so the callers
// above never test for a null. The same rule holds here, where the
// "platform" is a Linux CI leg with no audio hardware and no window manager.

namespace ac3::crucible {

namespace {

class NoDevices final : public AudioDevices {
public:
    std::vector<DeviceFacts> render_devices(std::uint32_t) override { return {}; }
    std::unique_ptr<BurstSink> burst_sink() override { return nullptr; }
    std::unique_ptr<PcmSink> pcm_sink() override { return nullptr; }
    std::unique_ptr<ObjectSink> object_sink() override { return nullptr; }
    std::unique_ptr<TapSource> tap() override { return nullptr; }
};

class NoSessions final : public SessionMonitor {
public:
    std::vector<AppSession> refresh(const std::vector<std::uint32_t>&) override { return {}; }
};

class NoForeground final : public Foreground {
public:
    std::optional<std::uint32_t> fullscreen_pid() override { return std::nullopt; }
    ForegroundSupport support() const override {
        return {.available = false, .reason = "this build has no window-manager backend"};
    }
};

class NoDefaultDevice final : public DefaultDevice {
public:
    std::vector<RenderEndpoint> endpoints() override { return {}; }
    std::string default_id() override { return {}; }
    std::expected<void, std::string> set_default(std::string_view) override {
        return std::unexpected("this build has no audio-policy backend");
    }
    bool moves_default() const override { return false; }
    std::string find_endpoint(std::string_view) override { return {}; }
    void open_sound_settings() override {}
};

class NoVirtualDevice final : public VirtualDevice {
public:
    SilentDeviceState state(const SilentDeviceQuery&) override {
        return {.needed = false,
                .present = false,
                .in_use = false,
                .can_install = false,
                .blocker = "this build has no silent-device backend",
                .detail = {}};
    }
    std::expected<void, std::string> install() override {
        return std::unexpected("this build has no silent-device backend");
    }
    std::expected<void, std::string> remove() override {
        return std::unexpected("this build has no silent-device backend");
    }
    DeviceActionStatus action_status() override { return {}; }
};

}  // namespace

std::shared_ptr<AudioDevices> platform_audio_devices() { return std::make_shared<NoDevices>(); }
std::shared_ptr<SessionMonitor> platform_session_monitor() { return std::make_shared<NoSessions>(); }
std::shared_ptr<Foreground> platform_foreground() { return std::make_shared<NoForeground>(); }
std::shared_ptr<DefaultDevice> platform_default_device() { return std::make_shared<NoDefaultDevice>(); }
std::shared_ptr<VirtualDevice> platform_virtual_device() { return std::make_shared<NoVirtualDevice>(); }

}  // namespace ac3::crucible
