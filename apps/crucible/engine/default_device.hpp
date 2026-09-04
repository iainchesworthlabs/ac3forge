#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

// The system default render endpoint - reading it, moving it to the silent
// device, and putting it back on exit: the seam between the application and
// the platform's audio policy (docs/crucible/promotion.md, "The seams to
// extract").
//
// The three platforms differ in a way this interface has to carry rather
// than hide.
//
// **Windows** documents no API for setting the default. Every
// default-switcher uses IPolicyConfig::SetDefaultEndpoint, stable since
// Vista and declared by hand because no SDK header has it. Being unsupported,
// a refusal is not an error: open_sound_settings() is the fallback and the
// DeviceWatcher reports the change once the user makes it by hand.
//
// **Linux** sets the `default.audio.sink` metadata key through PipeWire,
// which is documented and needs no elevation.
//
// **macOS** never moves the default at all. Its process taps silence each
// application where they tap it (CATapDescription's mutedWhenTapped), so
// there is no silent device to point at and nothing to restore. Its
// implementation reports the default for display and refuses set_default()
// with a reason saying why it does not need to - which the UI shows in place
// of a button, rather than offering an action that would do nothing.

namespace ac3::crucible {

struct RenderEndpoint {
    std::string id;
    std::string name;
    bool is_default = false;
};

class DefaultDevice {
public:
    virtual ~DefaultDevice() = default;

    // Every active render endpoint, default first.
    [[nodiscard]] virtual std::vector<RenderEndpoint> endpoints() = 0;

    // The console default's endpoint id, or empty when there is none.
    [[nodiscard]] virtual std::string default_id() = 0;

    // Makes `endpoint_id` the default for every role. Returns a one-line
    // reason on refusal - including "this platform does not need to", which
    // is macOS's answer and is not a failure.
    [[nodiscard]] virtual std::expected<void, std::string> set_default(
        std::string_view endpoint_id) = 0;

    // Whether moving the default is part of this platform's model at all.
    // False on macOS; the UI drops the whole first station of the signal
    // path rather than showing a stage that does not exist there.
    [[nodiscard]] virtual bool moves_default() const = 0;

    // The endpoint whose friendly name contains `name_substring`
    // (case-insensitive), or empty.
    [[nodiscard]] virtual std::string find_endpoint(std::string_view name_substring) = 0;

    // The fallback when set_default() is refused: the platform's own sound
    // settings page. A no-op where there is nothing to open.
    virtual void open_sound_settings() = 0;
};

}  // namespace ac3::crucible
