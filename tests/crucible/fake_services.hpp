#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "default_device.hpp"
#include "foreground.hpp"
#include "session_monitor.hpp"
#include "virtual_device.hpp"

// Scriptable stand-ins for the four platform seams, beside fake_devices.hpp's
// AudioDevices (docs/crucible/promotion.md, Phase 2). Between them the engine,
// the output stage and the UI controller's rules run in a plain Catch2 process
// on any platform, including a Linux CI leg with no audio hardware and no
// window manager.
//
// Each records what was asked of it and answers from a list a test set, so a
// case can say "there are two applications, one of them is full-screen, the
// default is a real device" and assert what the engine does about it.
// Thread-safe where the engine's frame thread and the test thread both touch
// them, matching fake_devices.hpp.

namespace ac3::crucible::testing {

class FakeSessionMonitor final : public SessionMonitor {
public:
    // What the next refresh() returns. Set from the test thread at any time.
    void set_apps(std::vector<AppSession> apps) {
        const std::lock_guard lock(mutex_);
        apps_ = std::move(apps);
    }

    [[nodiscard]] std::string listing_rule() const override { return "a fake list of applications"; }

    [[nodiscard]] std::size_t refreshes() const {
        const std::lock_guard lock(mutex_);
        return refreshes_;
    }

    // The `keep` list the engine passed on the last refresh, which is how a
    // test checks that a placed application survives a silent spell.
    [[nodiscard]] std::vector<std::uint32_t> last_keep() const {
        const std::lock_guard lock(mutex_);
        return last_keep_;
    }

    std::vector<AppSession> refresh(const std::vector<std::uint32_t>& keep = {}) override {
        const std::lock_guard lock(mutex_);
        ++refreshes_;
        last_keep_ = keep;
        return apps_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<AppSession> apps_;
    std::vector<std::uint32_t> last_keep_;
    std::size_t refreshes_ = 0;
};

class FakeForeground final : public Foreground {
public:
    void set_fullscreen_pid(std::optional<std::uint32_t> pid) {
        const std::lock_guard lock(mutex_);
        pid_ = pid;
    }

    // Stands in for Wayland: no answer is possible, and the reason is shown
    // rather than the absence being read as "nothing is full-screen".
    // The reason is copied, not referenced: production implementations hand
    // back views of string literals, but a test naturally builds one, and a
    // view of a temporary would dangle by the time support() is read.
    void set_unsupported(std::string reason) {
        const std::lock_guard lock(mutex_);
        supported_ = false;
        reason_ = std::move(reason);
        pid_ = std::nullopt;
    }

    std::optional<std::uint32_t> fullscreen_pid() override {
        const std::lock_guard lock(mutex_);
        return pid_;
    }

    ForegroundSupport support() const override {
        const std::lock_guard lock(mutex_);
        return {.available = supported_, .reason = reason_};
    }

private:
    mutable std::mutex mutex_;
    std::optional<std::uint32_t> pid_;
    bool supported_ = true;
    std::string reason_;
};

class FakeDefaultDevice final : public DefaultDevice {
public:
    void set_endpoints(std::vector<RenderEndpoint> endpoints) {
        const std::lock_guard lock(mutex_);
        endpoints_ = std::move(endpoints);
    }

    // Makes set_default() refuse, the way IPolicyConfig can.
    void refuse_set_default(std::string reason) {
        const std::lock_guard lock(mutex_);
        refusal_ = std::move(reason);
    }

    // Stands in for macOS, which never moves the default.
    void set_moves_default(bool moves) {
        const std::lock_guard lock(mutex_);
        moves_ = moves;
    }

    [[nodiscard]] std::size_t settings_opened() const {
        const std::lock_guard lock(mutex_);
        return settings_opened_;
    }

    std::vector<RenderEndpoint> endpoints() override {
        const std::lock_guard lock(mutex_);
        return endpoints_;
    }

    std::string default_id() override {
        const std::lock_guard lock(mutex_);
        for (const auto& e : endpoints_) {
            if (e.is_default) {
                return e.id;
            }
        }
        return {};
    }

    std::expected<void, std::string> set_default(std::string_view endpoint_id) override {
        const std::lock_guard lock(mutex_);
        if (!refusal_.empty()) {
            return std::unexpected(refusal_);
        }
        bool found = false;
        for (auto& e : endpoints_) {
            e.is_default = (e.id == endpoint_id);
            found = found || e.is_default;
        }
        if (!found) {
            return std::unexpected("no such endpoint");
        }
        return {};
    }

    bool moves_default() const override {
        const std::lock_guard lock(mutex_);
        return moves_;
    }

    std::string find_endpoint(std::string_view name_substring) override {
        const std::lock_guard lock(mutex_);
        if (name_substring.empty()) {
            return {};
        }
        for (const auto& e : endpoints_) {
            if (e.name.find(name_substring) != std::string::npos) {
                return e.id;
            }
        }
        return {};
    }

    void open_sound_settings() override {
        const std::lock_guard lock(mutex_);
        ++settings_opened_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<RenderEndpoint> endpoints_;
    std::string refusal_;
    bool moves_ = true;
    std::size_t settings_opened_ = 0;
};

class FakeVirtualDevice final : public VirtualDevice {
public:
    void set_state(SilentDeviceState state) {
        const std::lock_guard lock(mutex_);
        state_ = std::move(state);
    }

    void set_action_status(DeviceActionStatus status) {
        const std::lock_guard lock(mutex_);
        action_ = std::move(status);
    }

    [[nodiscard]] std::size_t installs() const {
        const std::lock_guard lock(mutex_);
        return installs_;
    }

    [[nodiscard]] std::size_t removes() const {
        const std::lock_guard lock(mutex_);
        return removes_;
    }

    // The query the caller passed last, so a test can check the seam is
    // asked about the right device.
    [[nodiscard]] SilentDeviceQuery last_query() const {
        const std::lock_guard lock(mutex_);
        return last_query_;
    }

    [[nodiscard]] std::string package_dir() const {
        const std::lock_guard lock(mutex_);
        return package_dir_;
    }

    void set_package_dir(std::string_view dir) override {
        const std::lock_guard lock(mutex_);
        package_dir_ = dir;
    }

    void set_device_name(std::string name) {
        const std::lock_guard lock(mutex_);
        device_name_ = std::move(name);
    }

    void set_from_package(bool from_package) {
        const std::lock_guard lock(mutex_);
        from_package_ = from_package;
    }

    bool from_package() const override {
        const std::lock_guard lock(mutex_);
        return from_package_;
    }

    std::string device_name() const override {
        const std::lock_guard lock(mutex_);
        return device_name_;
    }

    std::string how_to_get_one() const override { return "a fake has no advice"; }

    SilentDeviceState state(const SilentDeviceQuery& query) override {
        const std::lock_guard lock(mutex_);
        last_query_ = query;
        return state_;
    }

    std::expected<void, std::string> install() override {
        const std::lock_guard lock(mutex_);
        ++installs_;
        if (!install_refusal_.empty()) {
            return std::unexpected(install_refusal_);
        }
        return {};
    }

    std::expected<void, std::string> remove() override {
        const std::lock_guard lock(mutex_);
        ++removes_;
        return {};
    }

    DeviceActionStatus action_status() override {
        const std::lock_guard lock(mutex_);
        return action_;
    }

    void refuse_install(std::string reason) {
        const std::lock_guard lock(mutex_);
        install_refusal_ = std::move(reason);
    }

private:
    mutable std::mutex mutex_;
    SilentDeviceState state_;
    DeviceActionStatus action_;
    std::string install_refusal_;
    SilentDeviceQuery last_query_;
    std::string package_dir_;
    std::string device_name_ = "Fake Silent";
    bool from_package_ = false;
    std::size_t installs_ = 0;
    std::size_t removes_ = 0;
};

}  // namespace ac3::crucible::testing
