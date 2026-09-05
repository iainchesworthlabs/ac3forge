#include "foreground.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "display_session.hpp"
#include "platform_services.hpp"
#include "x11_foreground.hpp"

// The Linux Foreground: an answer under X11, and under everything else the
// reason there is none (docs/crucible/promotion.md, Phase 4 and its X11
// addendum).
//
// This is the one seam a platform can be unable to implement, and the two
// Linux display servers differ in kind:
//
// **Wayland cannot answer, ever.** A client is given no way to ask about
// another client's windows - that is the security model - and no
// xdg-desktop-portal exposes it either. No amount of work here changes that.
//
// **X11 answers**, by reading _NET_WM_STATE_FULLSCREEN on the window named by
// _NET_ACTIVE_WINDOW and taking its _NET_WM_PID (x11_foreground.hpp, over
// libxcb when the build has it). The engine matches that pid against each
// application's process tree on its session-monitor thread.
//
// **No display at all** - an ssh login, the CI container - has no window
// manager to ask, which is a third reason and a different one from the
// other two.
//
// Reporting "nothing is full-screen" in any case that cannot answer would be
// a different claim and a wrong one: the engine would silently stop pinning
// a full-screen game to the bed, and nobody would be told why. support()
// says so instead, and the Room page prints it. Which case this is comes
// from display_session.hpp, from the seat's own word first.

namespace ac3::crucible {

namespace {

constexpr const char* kWaylandReason =
    "Wayland gives a client no way to ask which window is full-screen, "
    "so the full-screen rule cannot apply here";
constexpr const char* kNoDisplayReason =
    "no graphical session: neither DISPLAY nor a Wayland display is set, so there is no "
    "window manager to ask; the full-screen rule is off";

[[nodiscard]] std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

// A compositor socket in the runtime directory: there whoever is asking,
// which is what makes it the resort when the variables are silent
// (display_session.hpp says why it does not beat an explicit
// XDG_SESSION_TYPE).
[[nodiscard]] bool wayland_socket_present() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || runtime[0] == '\0') {
        return false;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(runtime, ec)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("wayland-") && !name.ends_with(".lock")) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] SessionFacts read_session_facts() {
    return {.xdg_session_type = env_or_empty("XDG_SESSION_TYPE"),
            .wayland_display = env_or_empty("WAYLAND_DISPLAY"),
            .display = env_or_empty("DISPLAY"),
            .wayland_socket = wayland_socket_present()};
}

// No answer, and the reason: a view of a literal, which never goes away.
class UnavailableForeground final : public Foreground {
public:
    explicit UnavailableForeground(const char* reason) : reason_(reason) {}

    std::optional<std::uint32_t> fullscreen_pid() override { return std::nullopt; }

    ForegroundSupport support() const override {
        return {.available = false, .reason = reason_};
    }

private:
    std::string_view reason_;
};

}  // namespace

std::shared_ptr<Foreground> platform_foreground() {
    switch (classify_session(read_session_facts())) {
        case DisplaySession::kWayland:
            return std::make_shared<UnavailableForeground>(kWaylandReason);
        case DisplaySession::kX11:
            return std::make_shared<X11Foreground>(make_x11_window_reader());
        case DisplaySession::kNone:
            break;
    }
    return std::make_shared<UnavailableForeground>(kNoDisplayReason);
}

}  // namespace ac3::crucible
