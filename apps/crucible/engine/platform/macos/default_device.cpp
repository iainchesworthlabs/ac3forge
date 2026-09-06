#include "default_device.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include "ac3/audio/passthrough.hpp"
#include "coreaudio_support.hpp"
#include "platform_services.hpp"

// The macOS DefaultDevice: the system output, read and never moved
// (docs/crucible/promotion.md, Phase 5).
//
// **THIS HAS NEVER BEEN RUN.** Written 2026-09-06; the only thing that will
// read it before somebody has a Mac is the macOS CI compiler
// (docs/crucible/promotion.md, "What cannot be verified, and why").
//
// Two of the five answers come free, the same two Linux gets free: the
// endpoint list is the library's own enumerate_render_devices(), which is what
// the output stage already probes, and the name search over it is the same
// substring match Windows and Linux both do. The list's ids are Core Audio
// device UIDs (src/audio/src/backend/macos/passthrough.cpp), which is what
// makes them comparable with the default read below.
//
// The default itself is one property read:
// kAudioHardwarePropertyDefaultOutputDevice on the system object, then that
// device's UID. coreaudio_support.hpp already has both, so this file is a
// small forwarder over the library's private header rather than a second copy
// of the two-call property idiom - the reason the Linux half reuses
// ac3::pipewire's helpers the same way.
//
// ---------------------------------------------------------------------------
// Nothing moves the default here, and that is the point
// ---------------------------------------------------------------------------
// moves_default() is FALSE, and it is the only one of the three platforms
// where it is.
//
// Windows and Linux both work by putting every application into a device that
// discards what it is given and tapping them there, so both have to move the
// system default and put it back afterwards. macOS does not need a silent
// device at all: a Core Audio process tap carries muteBehavior
// (.mutedWhenTapped), which silences each application AT THE POINT IT IS
// TAPPED. Nothing in a person's sound settings is touched, so there is nothing
// to move, nothing to restore on quit, and no window in which quitting badly
// leaves the machine silent.
//
// set_default() therefore refuses with a reason saying it does not need to,
// which default_device.hpp documents as not a failure.
//
// **What the UI does with that today, read on 2026-09-06 rather than assumed.**
// default_device.hpp's own text says the window "drops the whole first station
// of the signal path" when moves_default() is false. It does not, and this
// platform is the first to make that claim testable, so it is written down
// here instead of being left to be discovered on a Mac:
//
//   - FirstRunDialog.qml DOES branch, and correctly - the paragraph below
//     this one has the detail, read the same day.
//   - Every "Send applications to ..." control is DISABLED rather than hidden:
//     SignalPath.qml, OutputPage.qml and Main.qml's tray menu each gate on
//     `nullSinkPresent || silentDeviceCanCreate`, and both are false here, so
//     nothing can call moveDefaultToNullSink(). The launch-time move is behind
//     `behaviour/moveDefaultOnLaunch`, which defaults to false.
//   - SignalPath.qml's station 1 is NOT gated on movesDefault and would still
//     be drawn, with a warning telling the reader to "Send applications to the
//     silent device instead" - advice that is wrong on this platform. Recorded
//     in docs/crucible/promotion.md's Phase 5 "Not done" list rather than fixed
//     here: it is a QML change, and nobody can run the window to see the
//     result of one.
//
// The wording of the refusal matters, because it is printed verbatim. It says
// what this platform does instead, not what it lacks.
//
// **Checked on 2026-09-06 before relying on it:** the first-run dialog already
// reads this. apps/crucible/ui/qml/FirstRunDialog.qml line 46 computes
// `movesDefault: CrucibleController.movesDefault && CrucibleController.silentDeviceNeeded`,
// and its first step renders "Applications are silenced where they are tapped"
// with the body "Nothing in your sound settings changes here: each application
// is silenced where Crucible taps it" when that is false - and hides the
// device status, the blocker line, the Send button and the restore row with
// it. CrucibleController::movesDefault() (crucible_controller.hpp) is a
// CONSTANT property over DefaultDevice::moves_default(). No QML says the word
// "macOS", and none was changed for this platform - which is right for the
// first-run dialog and is the open gap for the signal path's station 1, as the
// list above says.
//
// open_sound_settings() is a no-op. On Windows it is the fallback for an
// IPolicyConfig refusal, because there the refusal means "do it by hand"; here
// the refusal means "there is nothing to do", so sending somebody to System
// Settings would be sending them to change something this application has just
// said it does not need changed.

namespace ac3::crucible {

namespace {

// Printed verbatim by the UI wherever a set is attempted. Says what happens
// instead, rather than naming a missing capability.
constexpr const char* kNoMoveNeeded =
    "macOS does not need this: Crucible silences each application where it taps it, so your "
    "sound settings are left exactly as they are";

class MacosDefaultDevice final : public DefaultDevice {
public:
    std::vector<RenderEndpoint> endpoints() override {
        std::vector<RenderEndpoint> out;
        const auto devices = ac3::audio::enumerate_render_devices();
        if (!devices) {
            return out;
        }
        out.reserve(devices->size());
        for (const auto& device : *devices) {
            // is_default comes from the same UID comparison this file's
            // default_id() makes; taken from the library's answer rather than
            // asked a second time, so the two cannot disagree within one call.
            out.push_back(
                {.id = device.id, .name = device.name, .is_default = device.is_default});
        }
        // Default first, the order the header documents.
        std::stable_partition(out.begin(), out.end(),
                              [](const RenderEndpoint& e) { return e.is_default; });
        return out;
    }

    std::string default_id() override {
        return coreaudio::device_uid(coreaudio::default_device(/*input=*/false));
    }

    // Never moves it, and says why rather than failing. See the file header:
    // the tap mutes where it taps, so there is nothing to point at.
    std::expected<void, std::string> set_default(std::string_view) override {
        return std::unexpected(std::string{kNoMoveNeeded});
    }

    bool moves_default() const override { return false; }

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

    // Nothing to open: see the file header. A no-op, which is what
    // default_device.hpp asks of a platform with nothing to fall back to.
    void open_sound_settings() override {}
};

}  // namespace

std::shared_ptr<DefaultDevice> platform_default_device() {
    return std::make_shared<MacosDefaultDevice>();
}

}  // namespace ac3::crucible
