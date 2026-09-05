// A standing probe for the platform seams, on a machine that has a real
// audio session (docs/crucible/promotion.md, Phase 4's verification).
//
// The engine's own tests run everywhere by using fakes, which is what makes
// them run on a CI leg with no sound card - and is exactly why they cannot
// answer the question this asks: does the platform half actually work when
// there is a session to talk to. Everything here touches the real machine,
// so it is a tool rather than a test, run by hand on hardware and reported
// in the plan.
//
// It is deliberately read-mostly. The one thing it changes is the silent
// device, which it creates and then removes; it never moves the default
// output, because that would interrupt whatever the person at the machine is
// listening to.
//
// Not a CMake target: built by hand on the machine with the session, against
// a PipeWire build of the library and the engine in build-pw/. One command,
// wrapped here; as run on the Raspberry Pi:
//
//   g++ -std=c++23 -O1 -o /tmp/probe tools/checks/crucible_platform_probe.cpp
//       -Isrc/audio/include -Isrc/forge/include -Ibuild-pw/src/forge/generated
//       -Isrc/signing/include -Ibuild-pw/src/signing/generated
//       -Iapps/crucible/engine $(pkg-config --cflags libpipewire-0.3)
//       -DAC3FORGE_STATIC_DEFINE -DAC3SIGNING_STATIC_DEFINE
//       build-pw/apps/crucible/libac3crucible_engine.a
//       build-pw/src/audio/libac3audio.a build-pw/src/forge/libac3forge_static.a
//       build-pw/src/signing/libac3signing_static.a
//       $(pkg-config --libs libpipewire-0.3) $(pkg-config --libs xcb) -lpthread
//
//   ($(pkg-config --libs xcb) is for an engine built with libxcb, the X11
//   full-screen reader; a build configured without it links nothing extra.)
//
//   crucible_platform_probe [seconds-to-watch]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <thread>

#include "ac3/audio/audio_backend.hpp"
#include "ac3/audio/capture.hpp"
#include "ac3/audio/device_watcher.hpp"

#include "default_device.hpp"
#include "foreground.hpp"
#include "platform_services.hpp"
#include "session_monitor.hpp"
#include "virtual_device.hpp"

namespace {

void rule(const char* title) {
    std::printf("\n=== %s ===\n", title);
}

void show(const char* name, const ac3::audio::Capability& capability) {
    std::printf("  %-18s %-3s %s\n", name, capability.available ? "yes" : "NO",
                std::string(capability.reason).c_str());
}

}  // namespace

int main(int argc, char** argv) {
    const int watch_seconds = argc > 1 ? std::atoi(argv[1]) : 5;

    rule("library capabilities");
    const auto& backend = ac3::audio::audio_backend();
    show("capture", backend.capture);
    show("passthrough", backend.passthrough);
    show("monitor", backend.monitor);
    show("spatial", backend.spatial);
    show("process_loopback", backend.process_loopback);
    show("device_watch", backend.device_watch);
    std::printf("  process_loopback_available() = %s\n",
                ac3::audio::process_loopback_available() ? "true" : "false");

    rule("session monitor: who is playing");
    const auto sessions = ac3::crucible::platform_session_monitor();
    const auto apps = sessions->refresh();
    if (apps.empty()) {
        std::printf("  (nothing is playing; start something and run again)\n");
    }
    for (const auto& app : apps) {
        std::printf("  pid %-7u %-20s %-24s active=%d session=%d\n", app.app, app.name.c_str(),
                    app.description.c_str(), app.active ? 1 : 0, app.has_session ? 1 : 0);
        std::printf("                 exe: %s\n", app.image_path.c_str());
    }

    rule("per-application tap");
    if (apps.empty()) {
        std::printf("  skipped: nothing to tap\n");
    } else {
        for (const auto& app : apps) {
            if (!app.has_session) {
                continue;
            }
            ac3::audio::Capture capture;
            const auto started = capture.start_process_loopback(
                app.app, ac3::audio::ProcessLoopbackMode::kIncludeProcessTree, {48000, 2});
            if (!started) {
                std::printf("  pid %-7u REFUSED: %s\n", app.app,
                            std::string(ac3::audio::describe(started.error())).c_str());
                continue;
            }
            // Long enough for the graph to link and a few packets to land.
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            const auto stats = capture.stats();
            std::printf("  pid %-7u tapped: %llu frames, %llu silence-filled, %llu dropped\n",
                        app.app, static_cast<unsigned long long>(stats.frames_captured),
                        static_cast<unsigned long long>(stats.frames_silence_filled),
                        static_cast<unsigned long long>(stats.frames_dropped));
            capture.stop();
        }
    }

    rule("default device");
    const auto devices = ac3::crucible::platform_default_device();
    std::printf("  moves_default: %s\n", devices->moves_default() ? "yes" : "no");
    std::printf("  default id:    '%s'\n", devices->default_id().c_str());
    for (const auto& endpoint : devices->endpoints()) {
        std::printf("  %-1s %-38s %s\n", endpoint.is_default ? "*" : " ", endpoint.id.c_str(),
                    endpoint.name.c_str());
    }

    rule("foreground");
    // The three variables the Linux Foreground classifies the seat from
    // (engine/platform/linux/display_session.hpp), as this process sees them.
    for (const char* name : {"XDG_SESSION_TYPE", "WAYLAND_DISPLAY", "DISPLAY"}) {
        const char* value = std::getenv(name);
        std::printf("  %-17s '%s'\n", name, value == nullptr ? "" : value);
    }
    const auto foreground = ac3::crucible::platform_foreground();
    // The X11 Foreground connects on its first read, so read once before
    // asking support(); the engine does the same on its monitor thread.
    (void)foreground->fullscreen_pid();
    const auto support = foreground->support();
    std::printf("  available: %s\n", support.available ? "yes" : "no");
    std::printf("  reason:    %s\n", std::string(support.reason).c_str());
    if (support.available) {
        // A timeline: the pid every 500 ms for the watch period, printed on
        // change and matched the way the engine matches it (engine.cpp,
        // refresh_sessions): the application's own pid, or any pid in its
        // session tree. Make something full-screen while it runs; wmctrl -r
        // <name> -b add,fullscreen / remove,fullscreen toggles it by hand.
        std::printf("  watching %d s; make an application with sound full-screen\n", watch_seconds);
        const auto watch_from = std::chrono::steady_clock::now();
        std::optional<std::uint32_t> last;
        bool first = true;
        while (std::chrono::steady_clock::now() - watch_from < std::chrono::seconds(watch_seconds)) {
            const auto pid = foreground->fullscreen_pid();
            if (first || pid != last) {
                const double t =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - watch_from).count();
                if (!pid) {
                    std::printf("  t=%.1fs  none\n", t);
                } else {
                    const auto now_apps = sessions->refresh();
                    const auto match = std::ranges::find_if(now_apps, [&pid](const auto& app) {
                        return app.app == *pid ||
                               std::ranges::find(app.session_pids, *pid) != app.session_pids.end();
                    });
                    if (match == now_apps.end()) {
                        std::printf("  t=%.1fs  full-screen pid %u -> no session\n", t, *pid);
                    } else {
                        std::printf("  t=%.1fs  full-screen pid %u -> session pid %u (%s)\n", t, *pid,
                                    match->app, match->name.c_str());
                    }
                }
                last = pid;
                first = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    rule("silent device: create and remove");
    const auto silent = ac3::crucible::platform_virtual_device();
    auto before = silent->state({});
    std::printf("  before: needed=%d present=%d can_install=%d %s\n", before.needed ? 1 : 0,
                before.present ? 1 : 0, before.can_install ? 1 : 0, before.blocker.c_str());
    const auto installed = silent->install();
    if (!installed) {
        std::printf("  install REFUSED: %s\n", installed.error().c_str());
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        auto after = silent->state({});
        std::printf("  after:  needed=%d present=%d can_install=%d\n", after.needed ? 1 : 0,
                    after.present ? 1 : 0, after.can_install ? 1 : 0);
        for (const auto& line : after.detail) {
            std::printf("          %s\n", line.c_str());
        }
        // Whether the rest of the graph can see it is the question that
        // matters, and only another client can answer it - so leave it up
        // long enough for the watcher below to report the arrival.
    }

    rule("device watcher");
    ac3::audio::DeviceWatcher watcher;
    int events = 0;
    const auto watch_started = watcher.start([&events](const ac3::audio::DeviceChangeEvent& event) {
        const char* kind = "?";
        switch (event.change) {
            case ac3::audio::DeviceChange::kAdded: kind = "added"; break;
            case ac3::audio::DeviceChange::kRemoved: kind = "removed"; break;
            case ac3::audio::DeviceChange::kDefaultRenderChanged: kind = "default-render"; break;
            case ac3::audio::DeviceChange::kDefaultCaptureChanged: kind = "default-capture"; break;
            case ac3::audio::DeviceChange::kStateChanged: kind = "state"; break;
        }
        std::printf("  %-16s %s\n", kind, event.device_id.c_str());
        ++events;
    });
    if (!watch_started) {
        std::printf("  start REFUSED: %s\n",
                    std::string(ac3::audio::describe(watch_started.error())).c_str());
    } else {
        std::printf("  running; watching %d s (the silent device is removed during it, so a "
                    "'removed' should appear)\n",
                    watch_seconds);
        std::this_thread::sleep_for(std::chrono::seconds(watch_seconds / 2 + 1));
        if (installed) {
            (void)silent->remove();
        }
        std::this_thread::sleep_for(std::chrono::seconds(watch_seconds / 2 + 1));
        watcher.stop();
        std::printf("  %d event(s); stats says %llu\n", events,
                    static_cast<unsigned long long>(watcher.stats().events_delivered));
    }

    rule("done");
    return 0;
}
