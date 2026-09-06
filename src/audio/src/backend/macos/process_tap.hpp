#pragma once

#include <CoreAudio/CoreAudio.h>

#include <cstdint>

// The one Objective-C++ seam in this backend, and its C++ face.
//
// Everything else under src/audio/src/backend/macos/ is plain C++ against
// CoreAudio's C API - AudioObjectGetPropertyData, AudioDeviceCreateIOProcID,
// AudioHardwareCreateAggregateDevice are all ordinary C functions a .cpp can
// call. Core Audio's process tap is the one exception in the whole directory:
// CATapDescription is an Objective-C class with no C entry point at all, so a
// tap cannot be described from a .cpp however the surrounding calls are
// spelled. process_tap.mm is therefore the directory's only .mm, and it is
// kept to exactly the two operations that need Objective-C plus the aggregate
// device those operations exist to produce - so capture.cpp, which does the
// rest, stays a .cpp like its neighbours.
//
// Nothing Objective-C appears below: AudioObjectID and
// AudioStreamBasicDescription are C types from <CoreAudio/CoreAudio.h>, and
// the enums and struct are ordinary C++. That is what lets the same header be
// included from the .mm and from capture.cpp without the second one changing
// language.
//
// ---------------------------------------------------------------------------
// What is claimed about this pair
// ---------------------------------------------------------------------------
// It compiles. That is the whole of the claim, and it is deliberately not
// hedged anywhere else in these files so that it can be stated once, plainly,
// here. No Mac has ever run this backend (ROADMAP.md DR9), and the tap's TCC
// consent prompt is keyed to a code-signing identity this project's binaries
// do not yet carry (DR6), so neither the tap nor the aggregate device
// process_tap.mm builds has been observed to come into existence. Every
// comment in the .mm describing what a call does is describing what Apple's
// headers and the surveyed real-world implementations say it does - never
// something that was watched happening. Written 2026-09-06.

namespace ac3::coreaudio {

// How many channels the tap mixes the processes it covers down to.
//
// These two are the whole of the format choice a mixdown tap gets. WASAPI's
// process-loopback activation lets a caller state any WAVEFORMATEXTENSIBLE it
// likes and has the audio engine convert to it (see the Windows backend's
// start_process_loopback); a CATapDescription has no converter behind it at
// all - the mixdown descriptions Apple provides are mono and stereo, and the
// non-mixdown initialiser (initWithProcesses:andDeviceUID:withStream:) hands
// back whatever the tapped device stream's own format happens to be rather
// than a count the caller asked for. capture.cpp turns anything other than
// one or two channels away for exactly that reason.
enum class TapMixdown : std::uint8_t { kMono, kStereo };

// Which side of the process list the tap keeps - the two shapes
// ProcessLoopbackMode names, as far as Core Audio can express them.
//
// The word "tree" is absent on purpose. Windows' activation takes a target
// process and walks its children (PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_
// PROCESS_TREE); a CATapDescription takes a list of audio process objects and
// covers exactly those, with no descendant relation for it to follow - the
// HAL's process objects are audio clients, not a process hierarchy. So a
// macOS tap built from one process id covers that one process, and
// capture.cpp's own comment says so where a caller can see it.
enum class TapScope : std::uint8_t { kOnlyListedProcess, kEverythingExceptListedProcess };

// Why a tap was not created. Each maps to exactly one CaptureError in
// capture.cpp - the mapping lives there, next to the contract those errors
// belong to, rather than here.
enum class TapStatus : std::uint8_t {
    kOk,
    // This OS predates the tap API. See coreaudio_names.hpp's
    // kSystemAudioTapMinimumOs for which version, and why that one.
    kOsTooOld,
    // No audio process object answers to that process id. Note this is a
    // narrower question than "does that process exist": the HAL creates a
    // process object for a process that has touched Core Audio, so a live
    // process that has never played a sound has none either.
    kProcessNotFound,
    // There is no default output device to clock the aggregate against, and
    // to name as its main sub-device. A Mac with no output at all.
    kNoDefaultOutputDevice,
    // AudioHardwareCreateProcessTap refused. The consent denial lands here
    // too: a user who says no to the system-audio prompt, or a binary whose
    // signature means the prompt never appears, is a tap that was not
    // created rather than one that runs silent.
    kTapRefused,
    // The tap exists but AudioHardwareCreateAggregateDevice refused, so
    // there is no device to register an IOProc on. The tap is destroyed
    // before returning; the caller is left owning nothing.
    kAggregateRefused,
    // Neither the tap's own kAudioTapPropertyFormat nor anything else says
    // what shape it will deliver, so there is no way to size a ring buffer
    // or convert a sample. Both objects are destroyed before returning.
    kFormatUnreadable,
};

// A created tap and the private aggregate device that carries it. Both ids
// are owned by the caller and are released by destroy_process_tap(); nothing
// in this pair keeps a copy.
struct ProcessTap {
    AudioObjectID tap = kAudioObjectUnknown;
    AudioObjectID aggregate_device = kAudioObjectUnknown;
    // What the tap says it will deliver - read from the tap object itself
    // rather than assumed, because a mixdown tap's rate is the rate of the
    // device it mixes down to and neither this code nor its caller chose
    // that. capture.cpp compares it against what the caller asked for.
    AudioStreamBasicDescription format{};
};

// Creates a tap over `process_id` (or over everything except it) and a
// private aggregate device that carries it, returning both plus the tap's
// own format. `out` is left untouched on any status other than kOk, and
// nothing is left behind for the caller to clean up: every failure path
// destroys whatever it had already created.
//
// The tap is created with muteBehavior = CATapMutedWhenTapped. That single
// choice is what makes a mixer on this platform possible at all: the tapped
// application is muted at the point it is tapped, so its sound does not also
// reach the speakers while the tap is carrying it, and no default output
// device has to be moved to a silent one the way Windows and Linux each need
// (see apps/crucible - macOS has no silent device to point at, and with this
// mute behaviour it needs none).
//
// Read that together with `scope`, because the set it mutes is the set it
// taps. With kOnlyListedProcess that is one application. With
// kEverythingExceptListedProcess it is EVERY OTHER APPLICATION ON THE
// MACHINE: a global tap that excludes one process mutes everything it
// covers, so the speakers fall silent apart from the excluded process for as
// long as the tap lives. That follows from the API - one mute behaviour is
// carried by one description, and there is nowhere to say "tap this set,
// mute that one" - and it is the right answer for a mixer that is about to
// render the same audio itself. It is the wrong answer for a caller that
// wanted a global tap only to listen. Nobody has heard either happen: no Mac
// has run this (ROADMAP.md DR9), and the sentence above is what the mute
// behaviour's documented meaning implies, not something observed.
[[nodiscard]] TapStatus create_process_tap(std::uint32_t process_id, TapMixdown mixdown,
                                           TapScope scope, ProcessTap& out);

// Destroys the aggregate device and then the tap, in that order - the device
// refers to the tap, so the reference goes first - and clears `tap` back to
// kAudioObjectUnknown. Safe on a default-constructed ProcessTap and safe to
// call twice, which is what lets Capture::stop() call it unconditionally.
// Best-effort, and deliberately not [[nodiscard]]: like coreaudio_support.hpp's
// own revert paths, a caller here is already unwinding and has nothing left
// to do if the HAL refuses to let go.
void destroy_process_tap(ProcessTap& tap);

}  // namespace ac3::coreaudio
