#include "session_monitor.hpp"

#include <CoreAudio/CoreAudio.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bundle_facts.hpp"
#include "coreaudio_support.hpp"
#include "platform_services.hpp"
#include "process_facts.hpp"

// The macOS SessionMonitor: who is playing sound, from Core Audio's own list
// of the processes using the HAL (docs/crucible/promotion.md, Phase 5).
//
// **THIS HAS NEVER BEEN RUN.** Written 2026-09-06 against Apple's
// documentation for the AudioProcess object class; the only thing that will
// read it before somebody has a Mac in front of them is the macOS CI
// compiler. Nothing below is a report of observed behaviour, and the
// sentences a person sees are written to be true of the API rather than
// convenient (docs/crucible/promotion.md, "What cannot be verified, and why").
//
// The mechanism. macOS 14.0 added an object class to the HAL for a process:
// kAudioHardwarePropertyProcessObjectList on the system object hands back an
// AudioObjectID per process that Core Audio is holding audio state for, and
// each of those answers three properties this needs -
// kAudioProcessPropertyPID, kAudioProcessPropertyBundleID and
// kAudioProcessPropertyIsRunningOutput. That is the whole of it: no client
// enumeration per endpoint the way Windows walks IAudioSessionManager2 on
// every render device, and no graph walk with a client join the way PipeWire
// needs, because the HAL keeps this list itself.
//
// The three properties are read through the library's own CoreAudio helpers
// (src/audio/src/backend/macos/coreaudio_support.hpp: address(),
// get_property<T>(), CFOwned, to_utf8), reused rather than reimplemented for
// the same reason the Linux half reuses ac3::pipewire's - a second copy of
// the two-call property idiom is a second place for it to be wrong. That is
// what the private include directory in apps/crucible/CMakeLists.txt's APPLE
// arm is for.
//
// **The version floor is checked at runtime, not assumed.** The process object
// class does not exist before macOS 14.0, and CMAKE_OSX_DEPLOYMENT_TARGET for
// this project is 13.3 (cmake/toolchains/macos.llvm.toolchain.cmake), so this
// binary can be launched on a system that has never had it. __builtin_available
// is the same runtime gate coreaudio_names.hpp's system_audio_tap_api_available()
// is written with, for the same reason: it is what silences
// -Wunguarded-availability under this project's -Werror, and it is a version
// test rather than a device round trip. Below the floor the list is empty and
// listing_rule() says why, rather than an empty room leaving somebody to guess.
//
// One thing about that precedent, checked on 2026-09-06 before leaning on it:
// system_audio_tap_api_available() is an inline function in a header and no
// caller anywhere in src/ names it, so no build has ever EMITTED a
// __builtin_available on this platform. Clang lowers one to a call into
// compiler-rt (__isPlatformVersionAtLeast), and whether the Homebrew clang the
// toolchain file picks links its own builtins archive here is untried. The
// shape is borrowed; the link is not evidence.
//
// How this differs from the other two, which a person can see.
//
// Windows keeps an audio session for as long as an application holds the
// device open, so a paused media player stays in the list and greys. PipeWire
// has a stream only while there is sound, so an application appears when it
// starts playing and leaves when it stops. macOS sits between the two and the
// exact placement is the thing that cannot be checked from here: the HAL holds
// a process object while a process is using audio, and
// kAudioProcessPropertyIsRunningOutput is a separate question from whether the
// object exists at all. So the rule below says both halves - listed while it
// is using the sound hardware, greyed when it is not currently playing - and
// claims nothing about how long a paused player lingers, because that is
// exactly what one launch on a Mac would settle and no launch has happened.
//
// Grouping. An entry here is one process, as on Linux, and the identity a
// person reads comes from the OUTERMOST .app bundle the process's executable
// lies in (bundle_facts.hpp says why). That is what puts a browser's audio
// helper in the room as the browser, with the browser's icon, without any
// process-tree walk: the helper's binary is a different file with a different
// name, so Windows' same-image walk would find nothing here, while the bundle
// is shared. Two helpers of one browser stay two entries, as two utility
// processes do on Linux.
//
// What the taps do. Nothing yet: ac3::audio's macOS backend reports
// process_loopback unavailable (src/audio/src/backend/macos/audio_backend.cpp)
// because the Core Audio process tap it would need is not written - it needs
// an Objective-C class, a consent prompt keyed to a code-signing identity, and
// a machine to see either on (docs/platforms/macos.md, "Loopback capture").
// So this list is what the room shows and the engine cannot yet capture any of
// it. That is a gap in the library half, stated here because it is what a
// reader of this file will ask next.

namespace ac3::crucible {

namespace {

// Whether this OS build has the process object class at all. The same shape,
// and the same reasoning, as coreaudio_names.hpp's
// system_audio_tap_api_available(): __builtin_available compiles the runtime
// check @available uses in Objective-C, and Clang allows it only as an
// if-condition, which is why this wraps it instead of returning the
// expression. Apple shipped kAudioHardwarePropertyProcessObjectList in
// macOS 14.0 (Sonoma).
[[nodiscard]] bool process_object_list_available() {
    if (__builtin_available(macOS 14.0, *)) {
        return true;
    }
    return false;
}

// Every function below that names a macOS 14.0 selector carries its OWN
// __builtin_available gate rather than trusting the one at the call site.
// Clang's -Wunguarded-availability-new is on by default and is analysed per
// function: a guard in a caller does not cover a callee's body, so a single
// gate around the loop would leave these two flagged - and under this
// project's -Werror a warning is a failed build, on the one leg that is the
// only thing here that ever gets compiled at all. The repetition is the
// price of that, and each gate costs a version compare on a poll that runs
// twice a second.

// One process object's two properties. A property a process does not answer
// for - a command-line program has no bundle identifier - leaves its field
// empty rather than dropping the process.
[[nodiscard]] ProcessAudio audio_facts(AudioObjectID process) {
    ProcessAudio facts;
    if (__builtin_available(macOS 14.0, *)) {
        if (const auto bundle = coreaudio::get_property<CFStringRef>(
                process, coreaudio::address(kAudioProcessPropertyBundleID));
            bundle && *bundle != nullptr) {
            // Owned by this scope: a CFString property read hands the caller a
            // reference, the same contract device_uid()/device_name() are
            // written against next door.
            const coreaudio::CFOwned<CFStringRef> owned{*bundle};
            facts.bundle_id = coreaudio::to_utf8(owned.get());
        }
        facts.running_output =
            coreaudio::get_property<UInt32>(
                process, coreaudio::address(kAudioProcessPropertyIsRunningOutput))
                .value_or(0U) != 0U;
    }
    return facts;
}

// The pid behind a process object, or 0 when it cannot be read. pid_t is
// signed and every pid a caller can use is positive, so a negative answer is
// treated as no answer rather than cast into a very large unsigned one.
[[nodiscard]] std::uint32_t pid_of(AudioObjectID process) {
    if (__builtin_available(macOS 14.0, *)) {
        const auto pid =
            coreaudio::get_property<pid_t>(process, coreaudio::address(kAudioProcessPropertyPID));
        if (pid && *pid > 0) {
            return static_cast<std::uint32_t>(*pid);
        }
    }
    return 0;
}

// The HAL's list of process objects, empty below the version floor.
[[nodiscard]] std::vector<AudioObjectID> process_objects() {
    if (__builtin_available(macOS 14.0, *)) {
        return coreaudio::get_property_array<AudioObjectID>(
            kAudioObjectSystemObject,
            coreaudio::address(kAudioHardwarePropertyProcessObjectList));
    }
    return {};
}

class MacosSessionMonitor final : public SessionMonitor {
public:
    // See SessionMonitor::listing_rule, and this file's header comment for why
    // this says two things where Windows and Linux each say one. Below the
    // version floor there is no list to describe, and saying so is better than
    // a room that is empty for a reason nobody is told.
    [[nodiscard]] std::string listing_rule() const override {
        if (!process_object_list_available()) {
            // The placed-application clause holds here too, and is not a
            // consolation: the keep list is served outside the version gate in
            // refresh(), so an application already in the room stays in it
            // while its process lives even on a machine that can list nothing
            // new. tst_platform.qml asserts that clause on every platform.
            return "This machine's macOS is older than 14.0, which is where Core Audio "
                   "started reporting which processes are using sound, so no application "
                   "can be listed here. A placed application keeps its place while it runs.";
        }
        return "An application is listed while macOS is holding sound open for it, and is "
               "greyed while nothing is coming out of it; the list follows what is using "
               "the sound hardware rather than what has a window. A placed application "
               "keeps its place while it runs.";
    }

    std::vector<AppSession> refresh(const std::vector<std::uint32_t>& keep) override {
        std::unordered_map<std::uint32_t, AppSession> apps;
        collect(apps);

        // Applications the engine asked to keep: listed while their process
        // lives even with no audio process object, so one that was placed in
        // the room survives a silent spell instead of vanishing from it. This
        // loop is deliberately outside the version gate - it reads libproc and
        // not the HAL, and proc_pidpath carries no version floor, so nothing
        // here depends on the OS being 14.0 or later. listing_rule()'s
        // below-the-floor sentence keeps its placed-application clause on the
        // strength of that.
        for (const std::uint32_t pid : keep) {
            if (pid == 0 || apps.contains(pid) || !process_alive(pid)) {
                continue;
            }
            apps.emplace(pid, kept_session(pid, facts_.facts_for(pid, ProcessAudio{})));
        }

        // Facts for processes that have gone.
        facts_.forget_unless([&apps](std::uint32_t pid) { return apps.contains(pid); });

        std::vector<AppSession> out;
        out.reserve(apps.size());
        // The pair, not a structured binding whose key half nothing reads:
        // Clang has warned on an unused binding before now and this file is
        // compiled under -Werror on the one leg that compiles it at all.
        for (auto& entry : apps) {
            out.push_back(std::move(entry.second));
        }
        std::ranges::sort(out, {}, &AppSession::app);
        return out;
    }

private:
    // The HAL's list, turned into one entry per process. Empty below the
    // version floor, because process_objects() is (each of the three readers
    // above carries its own gate; the note beside them says why).
    void collect(std::unordered_map<std::uint32_t, AppSession>& apps) {
        const std::vector<AudioObjectID> processes = process_objects();
        const auto self = static_cast<std::uint32_t>(::getpid());
        for (const AudioObjectID process : processes) {
            const std::uint32_t pid = pid_of(process);
            if (pid == 0) {
                continue;  // the HAL did not attribute it; nothing to name
            }
            if (pid == self) {
                // Crucible's own output opens the HAL like any other client and
                // would otherwise list this application in its own room - the
                // mistake the first Linux screenshot made with its probe
                // streams. Windows never lists another instance of this
                // program; the same rule, by pid.
                continue;
            }
            const ProcessAudio audio = audio_facts(process);
            const ProcessFacts& facts = facts_.facts_for(pid, audio);
            if (facts.name.empty()) {
                // Nothing was readable about it: no executable path (another
                // user's process, or one that went between the list and the
                // read) and no bundle identifier. A row with no name and no
                // icon is worse than no row.
                continue;
            }
            if (auto [entry, fresh] = apps.try_emplace(pid); fresh) {
                entry->second = sounding_session(pid, facts, audio.running_output);
            } else if (audio.running_output) {
                // A second process object for one process. One entry, and the
                // louder answer wins: playing anywhere is playing.
                entry->second.active = true;
            }
        }
    }

    // The per-process reads and the eviction of them. process_facts.hpp holds
    // both, and the reasoning for each, because neither needs CoreAudio.
    ProcessFactsCache facts_;
};

}  // namespace

std::shared_ptr<SessionMonitor> platform_session_monitor() {
    return std::make_shared<MacosSessionMonitor>();
}

}  // namespace ac3::crucible
