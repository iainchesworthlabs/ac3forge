#pragma once

#include <libproc.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "bundle_facts.hpp"
#include "session_monitor.hpp"

// What the macOS SessionMonitor knows about a process beyond what Core Audio
// tells it, where it reads it, and the bookkeeping it does with it.
// session_monitor.cpp's own header is where the Core Audio half is explained -
// which property list is walked and what a person will see in it; none of that
// is repeated here.
//
// **NOTHING IN THIS FILE HAS BEEN RUN.** Written 2026-09-06; the only compiler
// that will read it is the macOS CI leg, and no Mac has ever executed any part
// of this application (docs/crucible/promotion.md, "What cannot be verified,
// and why"). Everything below is written against Apple's documented behaviour
// for proc_pidpath(2), not against behaviour anybody has watched.
//
// Split out of session_monitor.cpp the same way platform/linux/proc_facts.hpp
// is split out of its own: that file includes <CoreAudio/CoreAudio.h> and
// nothing in it compiles anywhere but a Mac, while this needs only libproc,
// which is part of libSystem and needs no link line of its own. Unlike the
// Linux file, no test drives this yet - tests/CMakeLists.txt has an `if(LINUX)`
// block for that platform's pure halves and no `if(APPLE)` one, and adding a
// case here that has never been run on a Mac would only assert what this file
// already says. The split is what makes writing one cheap when somebody has a
// machine; bundle_facts.hpp beside it is the part with rules worth asserting.
//
// The one read here is the executable path, because macOS gives no other
// answer this application needs:
//
//   - proc_pidpath() is the documented way to turn a pid into the path of the
//     binary behind it. There is no /proc to read and no version resource to
//     open; the bundle at the head of that path is the identity
//     (bundle_facts.hpp), and the icon provider takes the same path back
//     through the image id.
//   - It fails with EPERM for a process the caller does not own. Every
//     application a person is playing sound from is their own, so the failure
//     case here is a system process rather than something to work around, and
//     an empty path is what leaves it out of the list.
//
// There is no parent walk. The Windows monitor walks to the root of a process
// tree and the Linux one collects same-executable ancestors, both so that the
// engine's full-screen rule can match a browser's window process against its
// audio process (engine.cpp, refresh_sessions). session_pids is the ONLY
// consumer of either walk, and this platform's Foreground never answers with a
// pid at all - macOS gives an application no way to learn whether another
// application's window fills the screen, which foreground.mm states in full -
// so a walk here would be written for a caller that cannot call it. Should
// that change, the grouping to walk on is the outermost .app bundle
// (bundle_facts.hpp's app_bundle_path), not the executable name.

namespace ac3::crucible {

// The executable behind a pid, or empty when it cannot be read: the process
// has gone, or it belongs to another user. PROC_PIDPATHINFO_MAXSIZE is the
// buffer size <libproc.h> itself documents for this call, and the return is
// the byte count written rather than a status, so a non-positive result is
// the failure.
[[nodiscard]] inline std::string process_exe(std::uint32_t pid) {
    char buffer[PROC_PIDPATHINFO_MAXSIZE] = {};
    const int written = proc_pidpath(static_cast<int>(pid), buffer,
                                     static_cast<std::uint32_t>(sizeof(buffer)));
    if (written <= 0) {
        return {};
    }
    return std::string{buffer, static_cast<std::size_t>(written)};
}

// Whether a process is still there, asked the only way this file already has:
// a path comes back for a live process the caller owns and nothing comes back
// for one that has gone. Not kill(pid, 0), which answers "yes" for a process
// this application could never read anything else about, and would then put a
// row in the room with no name and no icon.
[[nodiscard]] inline bool process_alive(std::uint32_t pid) {
    return !process_exe(pid).empty();
}

// What Core Audio says about a process. Its own type rather than a CoreAudio
// one so that this header - and anything written over it later - never sees a
// CoreAudio type; session_monitor.cpp fills one per AudioObjectID.
struct ProcessAudio {
    std::string bundle_id;  // kAudioProcessPropertyBundleID, or empty
    bool running_output = false;  // kAudioProcessPropertyIsRunningOutput
};

// Everything the monitor knows about one process.
struct ProcessFacts {
    std::string exe;        // the binary, from proc_pidpath
    std::string bundle;     // the outermost .app it lies in, or empty
    std::string name;       // what to show a person
    std::string bundle_id;  // Core Audio's, kept for the icon lookup
};

// Read once per process and kept while it lives, for the reason the Windows
// and Linux monitors each cache: the per-process read is the expensive part of
// a refresh and none of it changes. The bundle identifier from Core Audio is
// kept with it and back-filled, because a process object can appear before its
// bundle identifier property answers and a later refresh should not have to
// pay for the path read again to pick it up.
class ProcessFactsCache {
public:
    // `audio` is what Core Audio said about this process on this refresh.
    // On a hit it back-fills the bundle identifier and proc_pidpath is not
    // called again - which is the point of the cache, and is also why a
    // process that replaces its own binary keeps the path it was first seen
    // under.
    const ProcessFacts& facts_for(std::uint32_t pid, const ProcessAudio& audio) {
        if (const auto it = known_.find(pid); it != known_.end()) {
            back_fill(it->second, audio);
            return it->second;
        }
        std::string exe = process_exe(pid);
        std::string bundle = app_bundle_path(exe);
        std::string name = display_name(exe, audio.bundle_id);
        // Every member named, in declaration order: GCC's
        // -Werror=missing-field-initializers rejects a partial designated
        // initialiser and the order must follow the struct. Kept the way
        // platform/linux/proc_facts.hpp keeps it even though only Clang
        // compiles this file, so the two read alike.
        return known_
            .emplace(pid, ProcessFacts{.exe = std::move(exe),
                                       .bundle = std::move(bundle),
                                       .name = std::move(name),
                                       .bundle_id = audio.bundle_id})
            .first->second;
    }

    // Drop the facts for processes that are no longer listed. `keep(pid)`
    // answers whether one still is; everything else goes, so a machine left
    // running for a day does not carry a fact for every process that ever made
    // a sound.
    template <class Keep>
    void forget_unless(Keep keep) {
        std::erase_if(known_, [&keep](const auto& entry) { return !keep(entry.first); });
    }

    [[nodiscard]] std::size_t size() const { return known_.size(); }

private:
    // Whatever this refresh carries and the cached facts do not. Never the
    // other way round: a refresh whose bundle-identifier read failed must not
    // blank one an earlier refresh got, and the name derived from it must not
    // be recomputed backwards into something worse.
    static void back_fill(ProcessFacts& facts, const ProcessAudio& audio) {
        if (facts.bundle_id.empty() && !audio.bundle_id.empty()) {
            facts.bundle_id = audio.bundle_id;
            if (facts.name.empty()) {
                facts.name = name_from_bundle_id(audio.bundle_id);
            }
        }
    }

    std::unordered_map<std::uint32_t, ProcessFacts> known_;
};

// The path the icon provider is handed. The bundle where there is one, so
// ui/platform/macos/app_icon_provider.mm can ask NSWorkspace for the picture
// that belongs to the application rather than the one that belongs to a helper
// binary buried inside it; the executable itself otherwise, which yields no
// icon and leaves the monogram, and that is the right answer for a
// command-line program.
[[nodiscard]] inline std::string icon_path_of(const ProcessFacts& facts) {
    return facts.bundle.empty() ? facts.exe : facts.bundle;
}

// The record a process Core Audio reports gets.
[[nodiscard]] inline AppSession sounding_session(std::uint32_t pid, const ProcessFacts& facts,
                                                 bool running_output) {
    AppSession app;
    app.app = pid;
    app.name = facts.name;
    app.image_path = icon_path_of(facts);
    app.description = facts.bundle_id.empty() ? facts.name : facts.bundle_id;
    // No freedesktop icon-theme name on this platform; the path above and the
    // bundle identifier below are what the icon provider reads
    // (ui/app_icon_provider.hpp documents the one id grammar all three share).
    app.icon_name.clear();
    app.app_id = facts.bundle_id;
    // Core Audio's own word for whether sound is coming out of it now.
    app.active = running_output;
    // Every application on this list has an audio process object, which a
    // process gets by talking to the HAL, and a person's own applications are
    // windowed. There is no windowed/background test here as Windows has: the
    // Behaviour setting that hides background applications therefore has
    // nothing to hide, exactly as on Linux.
    app.has_window = true;
    app.packaged = !facts.bundle.empty();
    app.has_session = true;
    // The pid alone. The Windows and Linux monitors put a whole process tree
    // here for the engine's full-screen match; this platform's Foreground
    // never returns a pid to match against (foreground.mm), so there is
    // nothing for a walk to serve. process_facts.hpp's header says what to
    // walk on if that ever changes.
    app.session_pids = {pid};
    return app;
}

// The record a process the engine asked to keep gets: listed while its process
// lives even with no audio process object, so an application that was placed
// in the room survives a silent spell instead of vanishing from it. Not active
// and with no session, which is what greys it.
[[nodiscard]] inline AppSession kept_session(std::uint32_t pid, const ProcessFacts& facts) {
    AppSession app = sounding_session(pid, facts, /*running_output=*/false);
    app.has_session = false;
    return app;
}

}  // namespace ac3::crucible
