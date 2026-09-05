#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "display_session.hpp"
#include "process_tree.hpp"
#include "x11_foreground.hpp"

// The Linux Foreground's X11 half, with no X server in the room
// (docs/crucible/promotion.md, "X11 full-screen detection"). Three things
// live here and none of them needs a display:
//
// X11Foreground's policy over its reader - a pid only while the active
// window is full-screen; a refused connection or a lost one as a reason
// support() shows, rather than as "nothing is full-screen"; and the retry
// cadence, which is what keeps a server that is not there from being asked
// twice a second for ever.
//
// classify_session(), which decides whether the seat is Wayland, X11 or no
// display at all, with the seat's own word winning in both directions.
//
// same_image_ancestors(), the walk that lets a browser's window process
// match its audio process and keeps a terminal from being its child.
//
// Linux only, by tests/CMakeLists.txt, the way tests/backend/pipewire rides
// its backend: the files under test live in a platform directory.

using namespace ac3::crucible;

namespace {

class FakeX11WindowReader final : public X11WindowReader {
public:
    // What connect() answers: nullptr connects, a literal refuses.
    void refuse(const char* reason) { refusal_ = reason; }
    // What active_window() answers; nullopt is a lost connection.
    void answer(std::optional<X11ActiveWindow> window) { window_ = std::move(window); }
    [[nodiscard]] int connects() const { return connects_; }
    [[nodiscard]] int reads() const { return reads_; }

    const char* connect() override {
        ++connects_;
        return refusal_;
    }
    std::optional<X11ActiveWindow> active_window() override {
        ++reads_;
        return window_;
    }

private:
    const char* refusal_ = nullptr;
    std::optional<X11ActiveWindow> window_{
        X11ActiveWindow{.window = 0, .fullscreen = false, .pid = std::nullopt}};
    int connects_ = 0;
    int reads_ = 0;
};

}  // namespace

TEST_CASE("x11 foreground reports the active window's pid only while it is full-screen",
          "[crucible][x11]") {
    auto owned = std::make_unique<FakeX11WindowReader>();
    FakeX11WindowReader* fake = owned.get();
    X11Foreground foreground(std::move(owned));

    fake->answer(X11ActiveWindow{.window = 7, .fullscreen = true, .pid = 4242U});
    REQUIRE(foreground.fullscreen_pid() == 4242U);
    REQUIRE(foreground.support().available);
    REQUIRE(foreground.support().reason.empty());

    // An ordinary window in front: no pid, and the rule stays available -
    // this is the one case where "nothing is full-screen" is the right claim.
    fake->answer(X11ActiveWindow{.window = 7, .fullscreen = false, .pid = 4242U});
    REQUIRE_FALSE(foreground.fullscreen_pid().has_value());
    REQUIRE(foreground.support().available);

    // Full-screen but no _NET_WM_PID: no pid is no claim.
    fake->answer(X11ActiveWindow{.window = 7, .fullscreen = true, .pid = std::nullopt});
    REQUIRE_FALSE(foreground.fullscreen_pid().has_value());

    // No active window at all.
    fake->answer(X11ActiveWindow{.window = 0, .fullscreen = false, .pid = std::nullopt});
    REQUIRE_FALSE(foreground.fullscreen_pid().has_value());
    REQUIRE(foreground.support().available);

    // One connection, one read per call.
    REQUIRE(fake->connects() == 1);
    REQUIRE(fake->reads() == 4);

    // The rule on its own.
    REQUIRE(fullscreen_pid_of({.window = 1, .fullscreen = true, .pid = 5U}) == 5U);
    REQUIRE_FALSE(fullscreen_pid_of({.window = 1, .fullscreen = false, .pid = 5U}).has_value());
    REQUIRE_FALSE(
        fullscreen_pid_of({.window = 1, .fullscreen = true, .pid = std::nullopt}).has_value());
}

TEST_CASE("x11 foreground connects on first use and a refused connection is a reason",
          "[crucible][x11]") {
    auto owned = std::make_unique<FakeX11WindowReader>();
    FakeX11WindowReader* fake = owned.get();
    X11Foreground foreground(std::move(owned));

    // Before any read: not connected, and not a claim of availability.
    REQUIRE(fake->connects() == 0);
    REQUIRE_FALSE(foreground.support().available);
    REQUIRE(foreground.support().reason == X11Foreground::kNotYetConnected);

    static constexpr const char* kRefusal = "DISPLAY is set but no X server answered on it";
    fake->refuse(kRefusal);

    // The first read connects once, is refused, and says so verbatim.
    REQUIRE_FALSE(foreground.fullscreen_pid().has_value());
    REQUIRE(fake->connects() == 1);
    REQUIRE(fake->reads() == 0);
    REQUIRE_FALSE(foreground.support().available);
    REQUIRE(foreground.support().reason == kRefusal);

    // The next kReconnectEvery - 1 reads do not retry ...
    for (int i = 1; i < X11Foreground::kReconnectEvery; ++i) {
        CHECK_FALSE(foreground.fullscreen_pid().has_value());
    }
    REQUIRE(fake->connects() == 1);
    // ... and the one after does, still refused.
    REQUIRE_FALSE(foreground.fullscreen_pid().has_value());
    REQUIRE(fake->connects() == 2);
    REQUIRE(foreground.support().reason == kRefusal);

    // Once the server answers, the retry connects and reads in the same call.
    fake->refuse(nullptr);
    fake->answer(X11ActiveWindow{.window = 9, .fullscreen = true, .pid = 77U});
    for (int i = 1; i < X11Foreground::kReconnectEvery; ++i) {
        CHECK_FALSE(foreground.fullscreen_pid().has_value());
    }
    REQUIRE(fake->connects() == 2);
    REQUIRE(foreground.fullscreen_pid() == 77U);
    REQUIRE(fake->connects() == 3);
    REQUIRE(fake->reads() == 1);
    REQUIRE(foreground.support().available);
    REQUIRE(foreground.support().reason.empty());
}

TEST_CASE("x11 foreground loses the display and says so", "[crucible][x11]") {
    auto owned = std::make_unique<FakeX11WindowReader>();
    FakeX11WindowReader* fake = owned.get();
    X11Foreground foreground(std::move(owned));

    fake->answer(X11ActiveWindow{.window = 7, .fullscreen = true, .pid = 4242U});
    REQUIRE(foreground.fullscreen_pid() == 4242U);
    REQUIRE(foreground.support().available);

    // The server went away: no pid, and a reason rather than silence.
    fake->answer(std::nullopt);
    REQUIRE_FALSE(foreground.fullscreen_pid().has_value());
    REQUIRE_FALSE(foreground.support().available);
    REQUIRE(foreground.support().reason == X11Foreground::kConnectionLost);

    // It comes back: the reconnect cadence applies, then the pid is back too.
    fake->answer(X11ActiveWindow{.window = 7, .fullscreen = true, .pid = 4242U});
    for (int i = 1; i < X11Foreground::kReconnectEvery; ++i) {
        CHECK_FALSE(foreground.fullscreen_pid().has_value());
        CHECK_FALSE(foreground.support().available);
    }
    REQUIRE(fake->connects() == 1);
    REQUIRE(foreground.fullscreen_pid() == 4242U);
    REQUIRE(fake->connects() == 2);
    REQUIRE(foreground.support().available);
    REQUIRE(foreground.support().reason.empty());
}

TEST_CASE("display session classification follows the seat first", "[crucible][x11]") {
    // The seat's word wins in both directions: Wayland even with Xwayland's
    // DISPLAY set, and x11 even with a compositor socket lying about.
    REQUIRE(classify_session({.xdg_session_type = "wayland",
                              .wayland_display = {},
                              .display = ":0",
                              .wayland_socket = false}) == DisplaySession::kWayland);
    REQUIRE(classify_session({.xdg_session_type = "x11",
                              .wayland_display = {},
                              .display = ":2",
                              .wayland_socket = true}) == DisplaySession::kX11);
    // x11 with no DISPLAY is no display: nothing to ask.
    REQUIRE(classify_session({.xdg_session_type = "x11",
                              .wayland_display = {},
                              .display = {},
                              .wayland_socket = false}) == DisplaySession::kNone);
    // Without the seat's word, compositor evidence, then DISPLAY.
    REQUIRE(classify_session({.xdg_session_type = {},
                              .wayland_display = "wayland-0",
                              .display = ":0",
                              .wayland_socket = false}) == DisplaySession::kWayland);
    REQUIRE(classify_session({.xdg_session_type = {},
                              .wayland_display = {},
                              .display = ":0",
                              .wayland_socket = true}) == DisplaySession::kWayland);
    REQUIRE(classify_session({.xdg_session_type = {},
                              .wayland_display = {},
                              .display = ":0",
                              .wayland_socket = false}) == DisplaySession::kX11);
    // An ssh login, or the CI container.
    REQUIRE(classify_session({.xdg_session_type = {},
                              .wayland_display = {},
                              .display = {},
                              .wayland_socket = false}) == DisplaySession::kNone);
    // A seat type this code does not know (tty, mir) falls through to the evidence.
    REQUIRE(classify_session({.xdg_session_type = "tty",
                              .wayland_display = {},
                              .display = ":0",
                              .wayland_socket = false}) == DisplaySession::kX11);
}

TEST_CASE("same-image ancestors stop where the executable changes", "[crucible][x11]") {
    struct Proc {
        std::uint32_t parent;
        std::string exe;
    };
    // 300 (chrome utility) <- 200 (chrome) <- 100 (bash) <- 1; a 50 <-> 60
    // cycle; 400 whose parent's executable cannot be read; 500 unreadable.
    const std::map<std::uint32_t, Proc> procs{
        {300U, {.parent = 200U, .exe = "/opt/google/chrome/chrome"}},
        {200U, {.parent = 100U, .exe = "/opt/google/chrome/chrome"}},
        {100U, {.parent = 1U, .exe = "/bin/bash"}},
        {1U, {.parent = 0U, .exe = "/sbin/init"}},
        {50U, {.parent = 60U, .exe = "/opt/loop"}},
        {60U, {.parent = 50U, .exe = "/opt/loop"}},
        {400U, {.parent = 401U, .exe = "/opt/game"}},
        {401U, {.parent = 1U, .exe = ""}},
        {500U, {.parent = 1U, .exe = ""}},
    };
    const auto ppid_of = [&procs](std::uint32_t pid) -> std::optional<std::uint32_t> {
        const auto it = procs.find(pid);
        if (it == procs.end()) {
            return std::nullopt;
        }
        return it->second.parent;
    };
    const auto exe_of = [&procs](std::uint32_t pid) -> std::string {
        const auto it = procs.find(pid);
        return it == procs.end() ? std::string{} : it->second.exe;
    };

    // The utility process climbs to the browser and stops at the shell.
    REQUIRE(same_image_ancestors(300U, ppid_of, exe_of) == std::vector<std::uint32_t>{300U, 200U});
    REQUIRE(same_image_ancestors(200U, ppid_of, exe_of) == std::vector<std::uint32_t>{200U});
    // The shell that launched it is not it: a full-screen terminal never pins its child.
    REQUIRE(same_image_ancestors(100U, ppid_of, exe_of) == std::vector<std::uint32_t>{100U});
    // A ppid cycle ends at the hop bound.
    const auto looped = same_image_ancestors(50U, ppid_of, exe_of);
    REQUIRE(looped.size() == static_cast<std::size_t>(kMaxAncestorHops) + 1);
    REQUIRE(looped.front() == 50U);
    // An unreadable executable for the start pid is just that pid ...
    REQUIRE(same_image_ancestors(500U, ppid_of, exe_of) == std::vector<std::uint32_t>{500U});
    // ... and for a parent it ends the walk.
    REQUIRE(same_image_ancestors(400U, ppid_of, exe_of) == std::vector<std::uint32_t>{400U});
    // A pid /proc has never heard of.
    REQUIRE(same_image_ancestors(999U, ppid_of, exe_of) == std::vector<std::uint32_t>{999U});
}
