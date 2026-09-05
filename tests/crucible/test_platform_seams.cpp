#include <catch2/catch_test_macros.hpp>

#include <string>

#include "fake_services.hpp"
#include "platform_services.hpp"
#include "virtual_device.hpp"

// The platform seams (docs/crucible/promotion.md, Phase 2a). Two things are
// worth holding here, and neither needs a device, a window manager or a
// driver, which is the point of the seams existing.
//
// The first is the promise platform_services.hpp makes: every factory returns
// a working object, never null. Callers above the seam dereference these
// without checking, on the rule that a platform which cannot do something
// says so through its own support()/state() rather than by being absent. On
// a Linux CI leg the definitions linked here are the stub's, so this is a
// test of the production fallback and not only of a fake.
//
// The second is that "cannot tell" and "nothing is full-screen" stay
// different answers. Wayland gives a client no way to ask about another
// client's windows, so its Foreground reports no pid *and* an unavailable
// support(); a UI that read the missing pid alone would silently drop the
// full-screen rule instead of explaining why it cannot apply it.

using namespace ac3::crucible;
using namespace ac3::crucible::testing;

TEST_CASE("every platform service factory returns an object", "[crucible][seams]") {
    REQUIRE(platform_audio_devices() != nullptr);
    REQUIRE(platform_session_monitor() != nullptr);
    REQUIRE(platform_foreground() != nullptr);
    REQUIRE(platform_default_device() != nullptr);
    REQUIRE(platform_virtual_device() != nullptr);

    // The rule the header states, held where the definitions linked here
    // are the stub's, as it holds for the production Linux Foreground's
    // Wayland and no-display arms: a Foreground that cannot answer says why.
    const auto foreground = platform_foreground();
    if (const auto support = foreground->support(); !support.available) {
        REQUIRE_FALSE(support.reason.empty());
    }
}

TEST_CASE("a platform that cannot report the foreground says so", "[crucible][seams]") {
    FakeForeground foreground;
    REQUIRE(foreground.support().available);
    REQUIRE_FALSE(foreground.fullscreen_pid().has_value());

    foreground.set_fullscreen_pid(4242);
    REQUIRE(foreground.fullscreen_pid() == 4242U);

    // The Wayland case: no answer, and a reason for it. The absent pid alone
    // would be indistinguishable from an ordinary desktop.
    foreground.set_unsupported("Wayland does not let a client ask about another's windows");
    REQUIRE_FALSE(foreground.support().available);
    REQUIRE_FALSE(foreground.support().reason.empty());
    REQUIRE_FALSE(foreground.fullscreen_pid().has_value());
}

TEST_CASE("the session monitor carries the engine's keep list", "[crucible][seams]") {
    FakeSessionMonitor monitor;
    REQUIRE(monitor.refresh().empty());

    AppSession chrome;
    chrome.app = 900;
    chrome.name = "chrome";
    chrome.active = true;
    monitor.set_apps({chrome});

    const auto apps = monitor.refresh({900, 901});
    REQUIRE(apps.size() == 1);
    REQUIRE(apps.front().name == "chrome");
    // The engine passes the placed applications so a silent spell does not
    // unlist them; the seam has to hand that through unchanged.
    REQUIRE(monitor.last_keep() == std::vector<std::uint32_t>{900, 901});
    REQUIRE(monitor.refreshes() == 2);
}

TEST_CASE("the default device seam carries a refusal and a platform that never moves it",
          "[crucible][seams]") {
    FakeDefaultDevice device;
    device.set_endpoints({{.id = "hdmi", .name = "Denon AVR", .is_default = false},
                          {.id = "null", .name = "Speakers (Desktop Atmos)", .is_default = true}});

    REQUIRE(device.default_id() == "null");
    REQUIRE(device.find_endpoint("Desktop Atmos") == "null");
    REQUIRE(device.find_endpoint("Denon") == "hdmi");
    REQUIRE(device.find_endpoint("nothing like this").empty());

    REQUIRE(device.set_default("hdmi").has_value());
    REQUIRE(device.default_id() == "hdmi");

    SECTION("a refusal is a reason, not a crash") {
        // What IPolicyConfig does when Windows declines: the UI falls back to
        // opening the sound settings and waits for the watcher.
        device.refuse_set_default("the policy call was refused");
        const auto moved = device.set_default("null");
        REQUIRE_FALSE(moved.has_value());
        REQUIRE(moved.error() == "the policy call was refused");
        REQUIRE(device.default_id() == "hdmi");

        device.open_sound_settings();
        REQUIRE(device.settings_opened() == 1);
    }

    SECTION("macOS never moves the default at all") {
        // Its taps mute each application where they tap it, so there is no
        // silent device to point at and the UI drops that stage entirely.
        device.set_moves_default(false);
        REQUIRE_FALSE(device.moves_default());
    }
}

TEST_CASE("the silent device seam describes a platform that needs none", "[crucible][seams]") {
    FakeVirtualDevice device;

    // Windows, with the driver missing and unable to load.
    device.set_state({.needed = true,
                      .present = false,
                      .in_use = false,
                      .can_install = true,
                      .blocker = "test signing is off",
                      .detail = {"the package is built"}});
    auto state = device.state({.endpoint_present = false, .endpoint_is_default = false});
    REQUIRE(state.needed);
    REQUIRE_FALSE(state.present);
    REQUIRE(state.can_install);
    REQUIRE(state.blocker == "test signing is off");
    REQUIRE_FALSE(device.last_query().endpoint_present);
    device.set_package_dir("D:/pkg");
    REQUIRE(device.package_dir() == "D:/pkg");

    REQUIRE(device.install().has_value());
    REQUIRE(device.installs() == 1);
    device.set_action_status({.running = false, .exit_code = 0, .log_tail = {"installed"}});
    const auto status = device.action_status();
    REQUIRE(status.exit_code == 0);
    REQUIRE(status.log_tail.front() == "installed");

    SECTION("macOS needs no silent device") {
        // Every member named: GCC's -Werror=missing-field-initializers
        // rejects a partial designated initialiser that MSVC accepts, and
        // this file compiles on every platform.
        device.set_state({.needed = false,
                          .present = false,
                          .in_use = false,
                          .can_install = false,
                          .blocker = {},
                          .detail = {}});
        REQUIRE_FALSE(device.state({}).needed);
    }

    SECTION("an install that cannot start is a reason") {
        device.refuse_install("no package was built");
        const auto started = device.install();
        REQUIRE_FALSE(started.has_value());
        REQUIRE(started.error() == "no package was built");
    }
}

TEST_CASE("a platform half that never moves the default needs no silent device",
          "[crucible][seams]") {
    // What the first-run dialog rests on: its rows about the move and the
    // restore, and its Send button, show only where the platform moves the
    // default output, and a platform whose taps silence each application
    // where they tap it has no silent device to move to either. The stub
    // linked here (the leg with no platform half) answers no to both, the
    // way macOS will, and is the shape the dialog's other branch renders.
    const auto device = platform_default_device();
    const auto silent = platform_virtual_device();
    REQUIRE_FALSE(device->moves_default());
    const SilentDeviceState state =
        silent->state({.endpoint_present = false, .endpoint_is_default = false});
    REQUIRE_FALSE(state.needed);
}
