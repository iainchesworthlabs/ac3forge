// ac3crucible-run: the AC3Forge Crucible's engine with a console instead of a
// window - Phase 2's exit criterion (docs/platforms/windows-demo.md). Lists
// the applications the engine sees, takes positions on stdin, and reports
// which output it chose and why.
//
//   ac3crucible-run [--null-sink SUBSTR] [--key PATH] [--pin MODE] [--low-latency]
//              [--bitrate KBPS] [--set-default SUBSTR]
//
//   --null-sink SUBSTR         the silent endpoint applications render into ("Desktop Atmos")
//   --key PATH                 the signing key file; else the AC3FORGE_SIGNING_KEY* variables
//   --pin MODE                 start pinned to a mode (the pin verb's list), not the policy's choice
//   --low-latency              one-block frames, the PCM sink at the engine's smallest period
//   --bitrate KBPS             a fixed bitrate instead of 448 kb/s (1536 in low latency)
//   --set-default SUBSTR       move the system default output there first, restore it on quit
//
//   list                       the applications and their slots (<app> below is the id it prints)
//   pos <app> <x> <y> <z>      position an application (x,y in [0,1], z in [-1,1])
//   bed <app>                  send it back to the bed
//   pin <mode>|off             atmos ddplus dd pcm headphones stereo
//   key <path>|none            load or clear the signing key
//   bypass on|off              PCM-side modes take the engine's own slots and bed, not a decode
//   split <app> on|off         a stereo application as a pair of objects, or one mono fold
//   size <app> 0..1            the object's extent, a point to the whole room
//   default <substr>|restore   move the system default output, or put it back
//   probe                      re-run the output probe
//   status                     one line of engine state
//   quit | exit

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include "engine.hpp"
#include "platform_services.hpp"

namespace {

using ac3::crucible::OutputMode;

std::optional<OutputMode> parse_mode(const std::string& word) {
    if (word == "atmos") return OutputMode::kAtmos;
    if (word == "ddplus") return OutputMode::kDdPlus51;
    if (word == "dd") return OutputMode::kDd51;
    if (word == "pcm") return OutputMode::kPcmSurround;
    if (word == "headphones") return OutputMode::kHeadphones;
    if (word == "stereo") return OutputMode::kStereo;
    return std::nullopt;
}

void print_status(const ac3::crucible::EngineStatus& s) {
    std::printf("[%s on \"%s\"] frames=%llu last=%.2fms worst=%.2fms starved=%llu underruns=%llu taps=%uch backlog=%.0fms sink=%.0fms catchups=%llu objects=%s bypass=%s\n",
                std::string(ac3::crucible::describe(s.mode)).c_str(), s.endpoint_name.c_str(),
                static_cast<unsigned long long>(s.frames_encoded), s.last_frame_ms, s.worst_frame_ms,
                static_cast<unsigned long long>(s.starved_reads),
                static_cast<unsigned long long>(s.underruns), static_cast<unsigned>(s.tap_channels), s.tap_backlog_ms, s.sink_queue_ms, static_cast<unsigned long long>(s.catchups), s.objects_enabled ? "on" : "off", s.codec_bypassed ? "on" : "off");
    std::printf("  output: %s\n  signing: %s\n", s.output_reason.c_str(), s.signing.c_str());
    std::printf("  full-screen rule: %s\n",
                s.fullscreen_rule_available ? "on" : s.fullscreen_rule_reason.c_str());
    if (!s.last_error.empty()) {
        std::printf("  last error: %s\n", s.last_error.c_str());
    }
}

void print_apps(const ac3::crucible::EngineStatus& s) {
    if (s.apps.empty()) {
        std::puts("  (no applications with an audio session)");
        return;
    }
    for (const auto& app : s.apps) {
        std::printf("  %-6u %-20s %-8s %-6s %s%s  %6.1f dBFS", app.app, app.name.c_str(),
                    app.active ? "active" : "idle", app.tapped ? "tapped" : "NO TAP",
                    app.slot ? ("slot " + std::to_string(*app.slot) + " @ (" +
                                std::to_string(app.position.x).substr(0, 4) + ", " +
                                std::to_string(app.position.y).substr(0, 4) + ", " +
                                std::to_string(app.position.z).substr(0, 5) + ")")
                                   .c_str()
                             : "bed",
                    app.fullscreen ? " [full-screen]" : "", static_cast<double>(app.level_dbfs));
        std::putchar('\n');
    }
}

}  // namespace

int main(int argc, char** argv) {
    ac3::crucible::EngineConfig config;
    std::string set_default;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--null-sink") config.null_sink_substring = next();
        else if (a == "--key") config.signing_key_path = next();
        else if (a == "--pin") config.pinned = parse_mode(next());
        else if (a == "--low-latency") config.low_latency = true;
        else if (a == "--bitrate") config.bitrate_kbps = static_cast<std::uint32_t>(std::atoi(next().c_str()));
        else if (a == "--set-default") set_default = next();
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }

    const auto default_device = ac3::crucible::platform_default_device();
    const std::string previous_default = default_device->default_id();
    if (!set_default.empty()) {
        const auto id = default_device->find_endpoint(set_default);
        if (id.empty()) {
            std::fprintf(stderr, "no render endpoint named like \"%s\"\n", set_default.c_str());
            return 2;
        }
        if (const auto ok = default_device->set_default(id); !ok) {
            std::fprintf(stderr, "%s\n", ok.error().c_str());
            default_device->open_sound_settings();
        } else {
            std::printf("default output moved to \"%s\"\n", set_default.c_str());
        }
    }

    ac3::crucible::Engine engine(config);
    if (const auto started = engine.start(); !started) {
        std::fprintf(stderr, "engine: %s\n", started.error().c_str());
        return 1;
    }
    std::puts("engine running; type 'list', 'pos <app> x y z', 'bed <app>', 'pin <mode>', 'bypass on|off', 'split <app> on|off', 'size <app> 0..1', 'status', 'quit'");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    print_status(engine.status());
    print_apps(engine.status());

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string verb;
        in >> verb;
        if (verb == "quit" || verb == "exit") {
            break;
        }
        if (verb == "list") {
            print_apps(engine.status());
        } else if (verb == "status") {
            print_status(engine.status());
        } else if (verb == "pos") {
            unsigned app = 0;
            double x = 0.5, y = 0.5, z = 0.0;
            in >> app >> x >> y >> z;
            engine.position(app, {x, y, z});
        } else if (verb == "side") {
            // side <id> l|r x y z: one object of a split pair on its own.
            unsigned app = 0;
            std::string which;
            double x = 0.5, y = 0.5, z = 0.0;
            in >> app >> which >> x >> y >> z;
            engine.position_side(app, which == "r" ? 1 : 0, {x, y, z});
        } else if (verb == "pair") {
            // pair <id> reset: back to the standard spread.
            unsigned app = 0;
            std::string what;
            in >> app >> what;
            engine.reset_pair(app);
        } else if (verb == "bed") {
            unsigned app = 0;
            in >> app;
            engine.unposition(app);
        } else if (verb == "pin") {
            std::string mode;
            in >> mode;
            engine.pin(mode == "off" ? std::nullopt : parse_mode(mode));
        } else if (verb == "key") {
            std::string path;
            in >> path;
            if (path == "none") {
                engine.clear_signing_key();
            } else {
                engine.load_signing_key(path);
            }
        } else if (verb == "size") {
            unsigned app = 0;
            double size = 0.0;
            in >> app >> size;
            engine.set_size(app, size);
        } else if (verb == "split") {
            unsigned app = 0;
            std::string on;
            in >> app >> on;
            engine.set_split(app, on == "on");
        } else if (verb == "bypass") {
            std::string on;
            in >> on;
            engine.set_bypass(on == "on");
        } else if (verb == "probe") {
            engine.reprobe();
        } else if (verb == "default") {
            std::string which;
            in >> which;
            const std::string id = which == "restore" ? previous_default
                                                      : default_device->find_endpoint(which);
            if (id.empty()) {
                std::puts("  no such endpoint");
            } else if (const auto ok = default_device->set_default(id); !ok) {
                std::printf("  %s\n", ok.error().c_str());
                default_device->open_sound_settings();
            } else {
                std::puts("  done");
                engine.reprobe();
            }
        } else if (!verb.empty()) {
            std::puts("  ? list | pos <app> x y z | bed <app> | pin <mode>|off | key <path>|none | default <substr>|restore | probe | status | quit");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (verb == "pos" || verb == "side" || verb == "pair" || verb == "bed" || verb == "pin" || verb == "key" || verb == "probe") {
            print_status(engine.status());
        }
    }

    engine.stop();
    if (!set_default.empty() && !previous_default.empty()) {
        if (const auto ok = default_device->set_default(previous_default)) {
            std::puts("default output restored");
        }
    }
    return 0;
}
