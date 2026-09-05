#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

// Which application is full-screen in the foreground, if any: the seam
// between the engine and the platform's window manager
// (docs/crucible/promotion.md, "The seams to extract").
//
// The rule it serves is the demo's: a full-screen application is the bed
// whatever the user asked, because a full-screen game rendering 7.1 is the
// bed (windows-demo.md, "Objects and the bed").
//
// This is the one seam a platform can be unable to implement rather than
// merely implement differently. Windows asks the shell
// (SHQueryUserNotificationState) and then takes the foreground window's
// process. X11 reads _NET_WM_STATE_FULLSCREEN on the active window. macOS
// asks NSWorkspace for the frontmost application. **Wayland cannot answer at
// all**: a client is given no way to ask about another client's windows, by
// design, and no portal exposes it. So `support()` exists beside the query,
// the way ac3::audio::Capability does, and an implementation that cannot
// answer says so in one line the UI can show - rather than silently
// reporting "nothing is full-screen", which is a different claim and a wrong
// one.

namespace ac3::crucible {

struct ForegroundSupport {
    bool available = false;
    // Empty when available. Otherwise one line saying what is missing and
    // why, shown by the UI beside the full-screen rule it disables.
    std::string_view reason;
};

class Foreground {
public:
    virtual ~Foreground() = default;

    // The foreground window's process id when the platform reports a
    // full-screen state, else nullopt - which also covers "this platform
    // cannot tell", so callers that care about the difference read
    // support() first. The engine maps the pid to an application by process
    // tree, since the window's process may not be the one with the audio
    // session.
    [[nodiscard]] virtual std::optional<std::uint32_t> fullscreen_pid() = 0;

    [[nodiscard]] virtual ForegroundSupport support() const = 0;
};

}  // namespace ac3::crucible
