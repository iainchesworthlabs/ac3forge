#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "slots.hpp"

// Who is playing sound, as applications rather than as streams: the seam
// between the engine and whatever the platform uses to answer that
// (docs/crucible/promotion.md, "The seams to extract").
//
// Every platform has some notion of it and no two agree on the shape.
// Windows reads IAudioSessionManager2 on each render endpoint and groups the
// sessions by process tree, because the session's process is often not the
// application - Chrome's audio comes from a utility process under the
// browser. Linux reads the PipeWire graph: a stream's Client carries the
// process id, and the stream node's info carries application.name and the
// icon name the application gave itself. macOS reads
// kAudioHardwarePropertyProcessObjectList. What the engine needs from all
// three is the same list, so that is what this interface is: the list, and
// nothing about how it was obtained.
//
// Polled, not event-driven, on every platform: refresh() is cheap enough to
// call a couple of times a second, which is as fast as a UI needs a new
// application to appear. The Windows implementation runs it on its own
// thread because enumerating processes and reading version resources cost
// 48 ms on the frame thread (windows-demo.md, "The application review").

namespace ac3::crucible {

struct AppSession {
    AppId app = 0;               // the root process of the tree
    std::string name;            // image name without extension, e.g. "chrome"
    std::string image_path;      // the root process's executable, for an icon
    std::string description;     // the executable's FileDescription, or empty
    // The platform's own icon name for it, when it gives one (PipeWire's
    // application.icon-name): a freedesktop icon-theme name, never a path;
    // empty on Windows, where the executable's path is the icon's identity.
    std::string icon_name;
    // The platform's application id when sandboxed (a Flatpak app id, which
    // is also its .desktop file id); empty otherwise.
    std::string app_id;
    std::string endpoint_name;   // where its session lives (the first one seen)
    bool active = false;         // playing on at least one session
    // An application, as a person means it: some process in the tree owns
    // a visible top-level window, or the root is a packaged app (whose
    // window belongs to a host process). Neither: a background process
    // with an audio session, such as a VM's backend or the text-input host.
    bool has_window = false;
    bool packaged = false;
    // False for an application listed because it runs with a window (or
    // because the engine asked for it to be kept) while it has no audio
    // session: nothing to tap, shown greyed.
    bool has_session = true;
    std::vector<std::uint32_t> session_pids;
};

class SessionMonitor {
public:
    virtual ~SessionMonitor() = default;

    // Applications with at least one audio session, active or not, sorted
    // by app id. The system sounds session is left out.
    // `keep`: application ids to list while their process lives even with
    // no session and no window (the engine passes what is placed).
    [[nodiscard]] virtual std::vector<AppSession> refresh(
        const std::vector<std::uint32_t>& keep = {}) = 0;
};

}  // namespace ac3::crucible
