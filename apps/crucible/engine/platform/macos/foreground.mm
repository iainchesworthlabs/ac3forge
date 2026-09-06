#include "foreground.hpp"

#import <AppKit/AppKit.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "platform_services.hpp"

// The macOS Foreground: what NSWorkspace can say, and the reason it is not
// enough (docs/crucible/promotion.md, Phase 5).
//
// **THIS HAS NEVER BEEN RUN.** Written 2026-09-06; the only thing that will
// read it before somebody has a Mac is the macOS CI compiler
// (docs/crucible/promotion.md, "What cannot be verified, and why").
//
// Objective-C++ because it has to be. NSWorkspace is an AppKit class with no C
// entry point, which is the same wall src/audio/src/backend/macos/capture.cpp
// records for CATapDescription; this is the first .mm in the tree and
// apps/crucible/CMakeLists.txt's APPLE arm is where OBJCXX is turned on. The
// file's path is what says "macOS" - there is no #ifdef here, and
// tools/checks/check_platform_macros.ps1 holds that rule for the whole of
// apps/ (its extension list gained .mm on 2026-09-06, with this file and the
// icon provider).
//
// platform_services.hpp is included after the AppKit import, so the engine's
// whole header set is read with Apple's <AssertMacros.h> already in scope.
// That header defines macros named check(), verify() and require() unless
// __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES is 0, which this
// target's CMake sets - see the reasoning in apps/crucible/CMakeLists.txt's
// APPLE arm, where the collision with Qt's own verify() is written out. No
// engine header uses those three names today; the define is what keeps that
// from being a thing anybody has to remember.
//
// ---------------------------------------------------------------------------
// What this seam is asked, and what macOS answers
// ---------------------------------------------------------------------------
// The engine asks one question: which process owns a FULL-SCREEN window right
// now, so that a full-screen game rendering 7.1 is pinned to the bed whatever
// the person asked for (engine/foreground.hpp).
//
// macOS answers a neighbouring question and not that one.
// -[NSWorkspace frontmostApplication] names the application the user is
// working in - its pid, its bundle identifier, whether it is active. It says
// nothing about that application's windows, and AppKit gives one application
// no way to ask about another's: NSWindow, NSScreen and
// NSApplicationPresentationOptions all describe the process asking. There is
// no AppKit equivalent of _NET_WM_STATE_FULLSCREEN or of Windows'
// SHQueryUserNotificationState.
//
// So this reports no pid, and support() says which of two things is in the
// way, decided by an actual NSWorkspace call rather than assumed:
//
//   - There is a window session and something is in front of it, but macOS
//     will not say whether it fills the screen.
//   - There is no window session at all - Crucible launched from a shell over
//     ssh, or ac3crucible-run under launchd - so there is nothing in front to
//     name either.
//
// That two-reason shape is the Linux file's (platform/linux/foreground.cpp
// separates Wayland from no-display for the same reason): the sentence a
// person is shown should say what is actually true of their machine.
//
// **Returning the frontmost pid instead would be a different claim, and a
// wrong one.** The engine takes what this returns and pins that application
// to the bed (engine.cpp, refresh_sessions -> Slots::set_fullscreen). Handing
// it whatever window has focus would silently move a person's mixer around as
// they clicked between applications, and they would be told nothing, because
// there would be nothing to tell: support() would be reporting availability.
// engine/foreground.hpp states the rule this obeys - "nothing is full-screen"
// and "this platform cannot tell" are different answers and the UI shows the
// second one.
//
// ---------------------------------------------------------------------------
// What would answer it, for whoever has a machine
// ---------------------------------------------------------------------------
// CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID)
// hands back every on-screen window's owner pid, layer and bounds, and
// CGDisplayBounds() gives each display's rectangle in the same coordinate
// space, so "the frontmost application owns a layer-0 window whose bounds are
// a whole display's" is the macOS spelling of the X11 test. It is deliberately
// not written here, on 2026-09-06, for two reasons that are about this
// machine rather than that API: the window-list calls sit in the family Apple
// has been deprecating in favour of ScreenCaptureKit, and a deprecation
// warning is a failed build under this project's -Werror - which would break
// the one thing this file can be held to, that it compiles. Nobody here can
// try it and read the result. Whoever runs Crucible on a Mac first should
// write it, check the deprecation state against the SDK in front of them, and
// then this seam's support() becomes available and its reason goes.
//
// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
// fullscreen_pid() is called from the engine's session-monitor thread at its
// 500 ms cadence, and support() from the frame thread and from the controller
// (engine.cpp; crucible_controller.cpp's diagnostics). So the reason is one
// atomic pointer to a string literal, exactly as X11Foreground keeps its own:
// a literal never goes away, so there is nothing to own and nothing to lock.
//
// -[NSWorkspace sharedWorkspace] is documented as usable from any thread and
// this reads one property of the object it returns. It is NOT AppKit drawing,
// which is main-thread-only; ui/platform/macos/app_icon_provider.mm is the
// file in this application that has that constraint, and it says so.

namespace ac3::crucible {

namespace {

constexpr const char* kNoFullScreenAnswer =
    "macOS names the application in front but tells another application nothing about "
    "whether its window fills the screen, so the full-screen rule is off here";
constexpr const char* kNoWindowSession =
    "no window session: nothing is in front to name, so the full-screen rule is off";
// Before the first poll. Not yet a claim either way, the way
// X11Foreground::kNotYetConnected is not.
constexpr const char* kNotYetAsked = "the foreground check has not run yet";

class MacosForeground final : public Foreground {
public:
    // The engine's session-monitor thread. Nothing is returned; the call is
    // made for the reason it publishes, which is the one thing about this
    // machine that a person can act on.
    std::optional<std::uint32_t> fullscreen_pid() override {
        bool in_front = false;
        @autoreleasepool {
            // Only whether there IS a frontmost application, never its
            // processIdentifier. The pid is deliberately not taken: it is the
            // wrong answer to the question this seam is asked (see the file
            // header), while its mere existence is what separates the two
            // reasons this platform can give.
            NSRunningApplication* front = [[NSWorkspace sharedWorkspace] frontmostApplication];
            in_front = front != nil;
        }
        reason_.store(in_front ? kNoFullScreenAnswer : kNoWindowSession,
                      std::memory_order_relaxed);
        return std::nullopt;
    }

    // Any thread.
    [[nodiscard]] ForegroundSupport support() const override {
        const char* reason = reason_.load(std::memory_order_relaxed);
        return {.available = false, .reason = std::string_view{reason}};
    }

private:
    std::atomic<const char*> reason_{kNotYetAsked};
};

}  // namespace

std::shared_ptr<Foreground> platform_foreground() {
    return std::make_shared<MacosForeground>();
}

}  // namespace ac3::crucible
