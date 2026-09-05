#include "x11_foreground.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

// The X11 Foreground's policy (x11_foreground.hpp), with no X in it.
//
// Connecting happens on the first read rather than at construction, so the
// object is made on whatever thread builds the engine and talks to the X
// server only from the thread that polls it: the engine's session-monitor
// thread, at its 500 ms cadence (engine.cpp). A refused connection is
// retried every kReconnectEvery reads - about ten seconds - which covers a
// window manager that starts after Crucible did, and a display that comes
// back, without asking a server that is not there twice a second for ever.
//
// support() is read from the frame thread while fullscreen_pid() runs on the
// monitor's, so the reason is one atomic pointer to a string literal: the
// readers hand back literals (x11_foreground.hpp says so), and a literal
// never goes away.

namespace ac3::crucible {

X11Foreground::X11Foreground(std::unique_ptr<X11WindowReader> reader)
    : reader_(std::move(reader)) {}

std::optional<std::uint32_t> X11Foreground::fullscreen_pid() {
    if (!connected_) {
        if (calls_since_failure_ != 0 && calls_since_failure_ < kReconnectEvery) {
            ++calls_since_failure_;
            return std::nullopt;
        }
        if (const char* refusal = reader_->connect(); refusal != nullptr) {
            reason_.store(refusal);
            calls_since_failure_ = 1;
            return std::nullopt;
        }
        connected_ = true;
        calls_since_failure_ = 0;
        reason_.store(nullptr);
    }
    const std::optional<X11ActiveWindow> window = reader_->active_window();
    if (!window) {
        // The connection has gone. Say so until a later read gets it back;
        // "nothing is full-screen" would be the wrong claim meanwhile.
        connected_ = false;
        calls_since_failure_ = 1;
        reason_.store(kConnectionLost);
        return std::nullopt;
    }
    return fullscreen_pid_of(*window);
}

ForegroundSupport X11Foreground::support() const {
    const char* reason = reason_.load();
    return {.available = reason == nullptr,
            .reason = reason == nullptr ? std::string_view{} : std::string_view{reason}};
}

}  // namespace ac3::crucible
