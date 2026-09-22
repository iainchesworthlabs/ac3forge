#pragma once

#include <string>

// Which display server the seat is running, from what the environment says
// about it: the one question the Linux Foreground has to answer before it
// knows whether it can answer anything else (foreground.cpp).
//
// XDG_SESSION_TYPE is what logind sets and WAYLAND_DISPLAY what a compositor
// exports; between them they answer for an application the desktop launched,
// which is how this one is meant to run. Neither is set in a session that is
// not the graphical one - an ssh login, most obviously - and the environment
// of the shell this happens to be running in says nothing about the seat.
// So when both are silent, the next resort is a compositor socket in the
// runtime directory, which is there whoever is asking, and after that DISPLAY
// alone. Getting this wrong is not academic: it decides which of three
// reasons the UI prints, or whether an X server is asked at all, and
// reporting "no X server" on a Wayland desktop sends somebody looking for a
// feature that cannot exist.
//
// The seat's own word wins in both directions. An explicit x11 beats a
// wayland-* socket because the socket can be stale (a compositor that
// crashed leaves it behind) or belong to a seat this session is nested in:
// a Xephyr on a Wayland desktop, which is how the X11 path is exercised on
// the Raspberry Pi, is told XDG_SESSION_TYPE=x11 and DISPLAY=:2 and is then
// an X11 session whatever the outer seat runs. And Wayland keeps its answer
// even with Xwayland's DISPLAY set, since an Xwayland root sees only X
// clients, and "nothing is full-screen" for a native Wayland window would be
// the wrong claim foreground.hpp warns about.
//
// Pure, so it is tested with facts built by hand rather than with a process
// environment (tests/crucible/platform/linux/test_x11_foreground.cpp).

namespace ac3::crucible {

enum class DisplaySession { kWayland, kX11, kNone };

struct SessionFacts {
    std::string xdg_session_type;  // logind's word for the seat, or empty
    std::string wayland_display;   // WAYLAND_DISPLAY, or empty
    std::string display;           // DISPLAY, or empty
    bool wayland_socket = false;   // a wayland-* socket in XDG_RUNTIME_DIR
};

[[nodiscard]] inline DisplaySession classify_session(const SessionFacts& facts) {
    if (facts.xdg_session_type == "wayland") {
        return DisplaySession::kWayland;
    }
    if (facts.xdg_session_type == "x11") {
        return facts.display.empty() ? DisplaySession::kNone : DisplaySession::kX11;
    }
    if (!facts.wayland_display.empty() || facts.wayland_socket) {
        return DisplaySession::kWayland;
    }
    if (!facts.display.empty()) {
        return DisplaySession::kX11;
    }
    return DisplaySession::kNone;
}

}  // namespace ac3::crucible
