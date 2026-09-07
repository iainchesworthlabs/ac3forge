#include "virtual_device.hpp"

#include <memory>
#include <string>

#include "platform_services.hpp"

// The macOS VirtualDevice: there is no silent device here, and the seam says
// so rather than being absent (docs/crucible/promotion.md, "The silent device,
// per platform").
//
// **THIS RUNS ON CI, AND ON NOBODY'S DESK.** Written 2026-09-06 without a
// Mac; since the same day the Crucible Qt Quick suites reach it for real on
// both macOS legs, through the controller's refreshDriver()
// (docs/crucible/promotion.md, Phase 5). It is also the file here with least
// to be wrong about: it calls nothing, so what it says is what it does.
//
// The three platforms answer the same question three ways, and only this one
// answers it by not needing the thing.
//
//   Windows needs a signed kernel driver, because Windows gives user mode no
//   way to create a render endpoint at all.
//
//   Linux needs no driver but does need a device: the application asks the
//   PipeWire daemon for a support.null-audio-sink node, creates it on the
//   first Send and takes it away when it exits.
//
//   macOS needs neither. A Core Audio process tap carries muteBehavior, and
//   .mutedWhenTapped silences the application AT THE POINT IT IS TAPPED. So
//   there is no device to install, none to create, none to point the system
//   default at, and nothing left behind on a machine that ran Crucible once.
//
// That is why `needed` is false, and everything the seam carries follows from
// it. default_device.hpp's moves_default() is false for the same one fact:
// with nothing to move applications INTO, nothing has to be moved.
//
// ---------------------------------------------------------------------------
// The wording, which is the part of this file that will actually be read
// ---------------------------------------------------------------------------
// how_to_get_one() is printed by the signal path and the first-run dialog
// wherever a person is told what stands between them and a working setup. On
// Windows it is "install the Desktop Atmos driver (Settings)". On Linux it is
// "Crucible creates it when you send applications to it; nothing to install" -
// a sentence written so that a person reads it as a thing that happens by
// itself rather than as a missing feature. This one has to go one step
// further, because here there is nothing at all: it must read as "nothing to
// do here", not as "this platform is short of something the others have". So
// it names what happens instead, in the same breath.
//
// device_name() is EMPTY, and deliberately. On the other two platforms it is
// the name the endpoint carries in the system's own sound settings and the
// string the engine matches endpoints against; here no such endpoint exists,
// so there is no name to give and inventing one would put a device in the
// room's vocabulary that is not on the machine. Every consumer is written for
// an empty answer: CrucibleController::refreshDefault() and
// OutputStage::probe() both guard their substring match with `!needle.empty()`
// before searching, and find_endpoint() returns nothing for an empty search.
// The one place that reads oddly is SettingsPage.qml's `silentDeviceNote`,
// which is not gated on silentDeviceNeeded the way the block below it is and
// would print an empty pair of quotes; that is a UI gap on a platform nobody
// has run, recorded here rather than papered over with a made-up name.

namespace ac3::crucible {

namespace {

class MacosVirtualDevice final : public VirtualDevice {
public:
    // No endpoint, so no name. The file header says why this is empty rather
    // than a placeholder, and which callers are written for it.
    std::string device_name() const override { return {}; }

    // Read as "nothing to do here". Names what happens instead, so that a
    // person is not left looking for an install they have missed.
    std::string how_to_get_one() const override {
        return "nothing to install: macOS silences each application at the point Crucible "
               "taps it, so no silent device is needed";
    }

    // No package to point at, and nothing this application creates either -
    // the two cases from_package() distinguishes are both about a device, and
    // there is none. False is the answer that keeps the Settings page's
    // driver-folder wording off this platform.
    bool from_package() const override { return false; }

    // `needed` false makes every other field meaningless, which the header
    // states, so they are left at their defaults rather than filled with
    // answers about a device that does not exist. The one detail line is what
    // a person opening the disclosure should find there.
    SilentDeviceState state(const SilentDeviceQuery&) override {
        SilentDeviceState out;
        out.needed = false;
        out.detail.push_back(
            "no silent device is needed here: each application is silenced where it is "
            "tapped, and your sound settings are left as they are");
        return out;
    }

    // Refused rather than quietly succeeding. The UI never offers either
    // action while `needed` is false, so this is what a caller that reached
    // past the UI gets: a sentence, not a no-op that would report an install
    // nobody performed.
    std::expected<void, std::string> install() override {
        return std::unexpected(std::string{
            "there is no silent device to install on macOS: applications are silenced where "
            "they are tapped"});
    }
    std::expected<void, std::string> remove() override {
        return std::unexpected(
            std::string{"there is no silent device on macOS, so there is none to remove"});
    }

    // Nothing here is asynchronous: there is no elevation prompt to wait on
    // and no daemon to ask, because there is no action.
    DeviceActionStatus action_status() override { return {}; }
};

}  // namespace

std::shared_ptr<VirtualDevice> platform_virtual_device() {
    return std::make_shared<MacosVirtualDevice>();
}

}  // namespace ac3::crucible
