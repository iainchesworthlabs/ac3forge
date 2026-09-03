#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "slots.hpp"

// Who is playing sound: the same session list the Windows Volume Mixer shows,
// read from every active render endpoint's IAudioSessionManager2, grouped
// into applications (docs/platforms/windows-demo.md, "Routing: two problems,
// not one").
//
// Grouping is by process tree, because the session's process is often not
// the application: Chrome's audio comes from a utility process under the
// browser, and a tap on the browser's tree (Capture::start_process_loopback
// in kIncludeProcessTree mode) is what captures all of it. The walk stops at
// the first ancestor whose image name differs, so chrome.exe's tree is one
// application and explorer.exe is nobody's parent.
//
// Polled, not event-driven: refresh() is cheap (a few COM calls per
// endpoint) and the engine calls it a couple of times a second, which is as
// fast as a UI needs a new application to appear. Windows-only, this
// directory only.

namespace ac3::windemo {

struct AppSession {
    AppId app = 0;               // the root process of the tree
    std::string name;            // image name without extension, e.g. "chrome"
    std::string image_path;      // the root process's executable, for an icon
    std::string description;     // the executable's FileDescription, or empty
    std::string endpoint_name;   // where its session lives (the first one seen)
    bool active = false;         // AudioSessionStateActive on at least one session
    // An application, as a person means it: some process in the tree owns
    // a visible top-level window, or the root is a packaged app (whose
    // window belongs to a host process). Neither: a background process
    // with an audio session, such as a VM's backend or the text-input host.
    bool has_window = false;
    bool packaged = false;
    std::vector<std::uint32_t> session_pids;
};

class SessionMonitor {
public:
    // Applications with at least one audio session, active or not, sorted
    // by app id. The system sounds session (pid 0) is left out.
    [[nodiscard]] std::vector<AppSession> refresh();
};

}  // namespace ac3::windemo
