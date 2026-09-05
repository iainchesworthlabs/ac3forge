#include "foreground.hpp"

#include <memory>

#include "platform_services.hpp"

#include <windows.h>
// windows.h must precede shellapi.h.
#include <shellapi.h>

namespace ac3::crucible {

namespace {

// The Windows answer to the engine's Foreground seam
// (engine/foreground.hpp). The shell's own notion is asked first, and only
// then is the foreground window's process taken, so an ordinary maximised
// window never counts.
class WindowsForeground final : public Foreground {
public:
    std::optional<std::uint32_t> fullscreen_pid() override;
    ForegroundSupport support() const override { return {.available = true, .reason = {}}; }
};

std::optional<std::uint32_t> WindowsForeground::fullscreen_pid() {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state))) {
        return std::nullopt;
    }
    // QUNS_BUSY: a full-screen application (a video player, a game in
    // borderless full-screen). QUNS_RUNNING_D3D_FULL_SCREEN: exclusive
    // full-screen Direct3D. QUNS_PRESENTATION_MODE: presentation settings.
    // Everything else (QUNS_NOT_PRESENT, QUNS_ACCEPTS_NOTIFICATIONS,
    // QUNS_QUIET_TIME, QUNS_APP) is an ordinary desktop.
    if (state != QUNS_BUSY && state != QUNS_RUNNING_D3D_FULL_SCREEN &&
        state != QUNS_PRESENTATION_MODE) {
        return std::nullopt;
    }
    const HWND window = GetForegroundWindow();
    if (window == nullptr) {
        return std::nullopt;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0) {
        return std::nullopt;
    }
    return pid;
}

}  // namespace

std::shared_ptr<Foreground> platform_foreground() {
    return std::make_shared<WindowsForeground>();
}

}  // namespace ac3::crucible
