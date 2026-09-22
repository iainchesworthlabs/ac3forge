#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "foreground.hpp"

// The X11 half of the Linux Foreground (foreground.cpp), split in two so the
// policy is tested without a display: X11WindowReader answers one question -
// what is the active window, is it full-screen, whose is it - and
// X11Foreground decides when to connect, what a lost display means, and what
// support() says meanwhile. Exactly one reader is compiled
// (apps/crucible/CMakeLists.txt): xcb_window_reader.cpp over libxcb, or
// no_xcb_window_reader.cpp when the build was configured without it, whose
// connect() says so. No X header is named here; only the xcb reader has one.

namespace ac3::crucible {

struct X11ActiveWindow {
    std::uint32_t window = 0;          // the XID _NET_ACTIVE_WINDOW names; 0: none
    bool fullscreen = false;           // _NET_WM_STATE contains _NET_WM_STATE_FULLSCREEN
    std::optional<std::uint32_t> pid;  // _NET_WM_PID, when the client set it for this host
};

class X11WindowReader {
public:
    virtual ~X11WindowReader() = default;

    // Connects to the display DISPLAY names. nullptr on success, else a
    // string literal saying what is missing, which support() shows verbatim.
    [[nodiscard]] virtual const char* connect() = 0;

    // One read. nullopt when the connection has gone (the poller then
    // reports it and retries connect() later); an absent active window is
    // {0, false, nullopt}.
    [[nodiscard]] virtual std::optional<X11ActiveWindow> active_window() = 0;
};

// The rule, on its own so it is trivially testable: a pid only while the
// window is full-screen. A full-screen window with no pid is no claim.
[[nodiscard]] constexpr std::optional<std::uint32_t> fullscreen_pid_of(const X11ActiveWindow& w) {
    if (!w.fullscreen) {
        return std::nullopt;
    }
    return w.pid;
}

class X11Foreground final : public Foreground {
public:
    // A refused or lost connection is retried every this many reads: about
    // ten seconds at the engine's session cadence.
    static constexpr int kReconnectEvery = 20;
    // support() before the first read: not yet a claim of availability.
    static constexpr const char* kNotYetConnected = "the X11 check has not connected yet";
    static constexpr const char* kConnectionLost =
        "the X server connection was lost; the full-screen rule is off until it is back";

    explicit X11Foreground(std::unique_ptr<X11WindowReader> reader);

    // One caller thread at a time: the engine's session-monitor thread.
    std::optional<std::uint32_t> fullscreen_pid() override;
    // Any thread.
    ForegroundSupport support() const override;

private:
    std::unique_ptr<X11WindowReader> reader_;
    bool connected_ = false;
    int calls_since_failure_ = 0;
    std::atomic<const char*> reason_{kNotYetConnected};  // nullptr: available
};

// Exactly one definition is compiled, chosen by CMake.
[[nodiscard]] std::unique_ptr<X11WindowReader> make_x11_window_reader();

}  // namespace ac3::crucible
