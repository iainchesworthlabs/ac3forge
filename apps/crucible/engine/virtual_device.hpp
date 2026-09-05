#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The silent device: whether this machine has one, whether applications are
// playing into it, and how one is installed or removed
// (docs/crucible/promotion.md, "The silent device, per platform").
//
// This is the seam whose Windows shape most needed generalising. It was
// `driver_tools.hpp` and it spoke about drivers, test signing and elevated
// PowerShell - none of which exists on the other two platforms. What the UI
// actually needs to know is smaller and platform-neutral: is there a device
// that discards what it is given, are applications playing to it, and can
// this application do anything about it. That is what `SilentDeviceState`
// carries, and the platform-specific detail lives in `blocker` and `detail`
// as text the UI prints rather than as fields it has to reason about.
//
// The three answers differ:
//
// **Windows** needs a kernel driver, because Windows has no user-mode way to
// create a render endpoint. `install()` runs the package's script elevated.
// While the driver is test-signed the machine must also have test signing on
// and memory integrity off, which is what `blocker` says when it cannot load.
//
// **Linux** needs no driver: the silent device is a PipeWire
// `support.null-audio-sink` node this application creates and tears down, so
// `install()` needs no elevation and cannot fail for signing reasons.
//
// **macOS** needs no silent device at all. Its process taps mute each
// application at the point they tap it, so `needed` is false, `present` is
// meaningless, and the UI shows the tap's consent state instead.

namespace ac3::crucible {

struct SilentDeviceState {
    // False on a platform that silences at the tap instead (macOS). When
    // false every other field is meaningless and the UI shows nothing about
    // a device.
    bool needed = true;
    // A silent render endpoint exists on this machine.
    bool present = false;
    // ...and it is the system default, so applications are playing into it.
    bool in_use = false;
    // This application can install one right now (a package is available, a
    // module can be loaded).
    bool can_install = false;
    // Empty when nothing is in the way. Otherwise one line saying what is,
    // in the platform's own terms - "test signing is off", "no package was
    // built" - printed verbatim by the UI.
    std::string blocker;
    // Optional extra lines for a disclosure: where the package is, what the
    // kernel reports, what a module load said. Never required reading.
    std::vector<std::string> detail;
};

// Progress of an install or remove that is still running. Both are
// asynchronous everywhere: Windows shows a UAC prompt, Linux waits on the
// session manager.
struct DeviceActionStatus {
    bool running = false;
    // Set once it has ended: 0 for success, anything else for failure.
    std::optional<int> exit_code;
    // The last few lines of whatever the action logged, for the UI's
    // disclosure. Empty when there is nothing to show.
    std::vector<std::string> log_tail;
};

// What the caller already knows when it asks. Enumerating endpoints is
// DefaultDevice's job, not this one's, so the two facts that come from there
// are passed in rather than looked up twice - and a platform whose silent
// device is not an endpoint at all (macOS, which has none) can ignore them.
struct SilentDeviceQuery {
    // An endpoint matching the silent device's name exists.
    bool endpoint_present = false;
    // ...and it is the system default, so applications are playing into it.
    bool endpoint_is_default = false;
};

class VirtualDevice {
public:
    virtual ~VirtualDevice() = default;

    // What the silent device is called, as the platform's own sound settings
    // show it and as the engine matches it by name: "Desktop Atmos" on
    // Windows (the driver's endpoint, until the driver is renamed with its
    // signing), "Crucible (silent)" on Linux. The first Linux screenshot
    // labelled every station with the Windows name because this lived in
    // the window as a default rather than in the platform that owns it.
    [[nodiscard]] virtual std::string device_name() const = 0;

    // One sentence on how a person gets a silent device on this platform,
    // for the signal path's warning when there is none: "install the
    // driver" is Windows advice and wrong everywhere else.
    [[nodiscard]] virtual std::string how_to_get_one() const = 0;

    // Where a platform that installs from a built package should look. A
    // no-op where the concept does not apply: Linux loads a module and macOS
    // needs no device, so neither has a package directory.
    virtual void set_package_dir(std::string_view) {}

    // Whether the silent device comes from a package in a folder the person
    // can point at - a driver, installed and removed by tools that live
    // beside it - or is made by this application itself. The settings page
    // shows the folder, the package's state and the driver wording only in
    // the first case; in the second, "install" means "create". The first
    // Linux run of the settings tests failed on exactly this: the page
    // asserted a bogus folder meant no package, on a platform with no
    // package at all.
    [[nodiscard]] virtual bool from_package() const { return false; }

    // Cheap enough for the UI's poll.
    [[nodiscard]] virtual SilentDeviceState state(const SilentDeviceQuery& query) = 0;

    // Starts an install or a remove. Returns a one-line reason when it could
    // not be started at all (no package, refused prompt, unsupported here).
    [[nodiscard]] virtual std::expected<void, std::string> install() = 0;
    [[nodiscard]] virtual std::expected<void, std::string> remove() = 0;

    // Polled by the UI while an action runs.
    [[nodiscard]] virtual DeviceActionStatus action_status() = 0;
};

}  // namespace ac3::crucible
