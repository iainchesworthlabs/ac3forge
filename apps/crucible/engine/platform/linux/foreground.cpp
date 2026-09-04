#include "foreground.hpp"

#include <cstdlib>
#include <memory>
#include <string_view>

#include "platform_services.hpp"

// The Linux Foreground: it says it cannot tell, and which of the two reasons
// applies (docs/crucible/promotion.md, Phase 4).
//
// This is the one seam a platform can be genuinely unable to implement, and
// the two Linux display servers fail differently:
//
// **Wayland cannot answer, ever.** A client is given no way to ask about
// another client's windows - that is the security model, not a gap - and no
// xdg-desktop-portal exposes it either. No amount of work here changes that.
//
// **X11 could answer**, by reading _NET_WM_STATE_FULLSCREEN on the window
// named by _NET_ACTIVE_WINDOW and taking its _NET_WM_PID. That is a libX11
// dependency for a rule that the other display server will never satisfy, on
// a desktop that is moving to Wayland. It is left as a follow-up rather than
// taken now, so this file has no dependency at all.
//
// Reporting "nothing is full-screen" instead would be a different claim and
// a wrong one: the engine would silently stop pinning a full-screen game to
// the bed, and nobody would be told why. support() says so instead, and the
// UI can print it.

namespace ac3::crucible {

namespace {

// XDG_SESSION_TYPE is what logind sets and what every desktop toolkit reads;
// WAYLAND_DISPLAY is the fallback for a session that did not set it.
[[nodiscard]] bool session_is_wayland() {
    if (const char* type = std::getenv("XDG_SESSION_TYPE");
        type != nullptr && std::string_view{type} == "wayland") {
        return true;
    }
    const char* display = std::getenv("WAYLAND_DISPLAY");
    return display != nullptr && display[0] != '\0';
}

class LinuxForeground final : public Foreground {
public:
    LinuxForeground() : wayland_(session_is_wayland()) {}

    std::optional<std::uint32_t> fullscreen_pid() override { return std::nullopt; }

    ForegroundSupport support() const override {
        if (wayland_) {
            return {.available = false,
                    .reason = "Wayland gives a client no way to ask which window is full-screen, "
                              "so the full-screen rule cannot apply here"};
        }
        return {.available = false,
                .reason = "the full-screen check is not implemented on X11 yet; the rule that "
                          "makes a full-screen application the bed is off"};
    }

private:
    bool wayland_ = false;
};

}  // namespace

std::shared_ptr<Foreground> platform_foreground() {
    return std::make_shared<LinuxForeground>();
}

}  // namespace ac3::crucible
