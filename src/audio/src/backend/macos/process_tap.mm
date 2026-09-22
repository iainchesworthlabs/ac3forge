#include "process_tap.hpp"

// The Objective-C++ half of the macOS process tap. See process_tap.hpp for
// why this file exists at all, for what is and is not claimed about it, and
// for the C++ interface it implements.
//
// ---------------------------------------------------------------------------
// The shape of a Core Audio process tap
// ---------------------------------------------------------------------------
// Three objects, created in this order and destroyed in the reverse of it:
//
//   1. An audio PROCESS OBJECT. The HAL gives every audio client an
//      AudioObjectID of its own; kAudioHardwarePropertyTranslatePIDToProcessObject
//      turns a unix process id into one. This is the only place in the
//      backend that passes qualifier data to AudioObjectGetPropertyData (the
//      pid is the qualifier, the object id is the answer), which is why it is
//      written out here rather than reached through coreaudio_support.hpp's
//      get_property<T>.
//
//   2. The TAP itself, AudioHardwareCreateProcessTap over a CATapDescription.
//      The description is the Objective-C object this whole translation unit
//      exists for. It carries the process list, the mixdown shape, a UUID we
//      choose (so that step 3 can name this tap), and the mute behaviour.
//
//   3. A private AGGREGATE DEVICE that carries the tap. A tap is not a device
//      and has no IOProc of its own; the way audio comes out of one is to
//      build an aggregate device whose kAudioAggregateDeviceTapListKey names
//      it, and then register an ordinary AudioDeviceIOProcID on that device -
//      which is exactly what capture.cpp already does for a real input
//      device, so the read side needs nothing new.
//
// The aggregate's main sub-device is the current default output device, and
// that device also appears in its sub-device list. That is the arrangement
// every published implementation of this API surveyed while writing this file
// uses (Apple's own audio-tap sample and the widely-copied AudioCap project
// both build the dictionary this way), and the reason is clocking: a tap
// carries no clock, so the aggregate is timed by the device the tapped
// application was playing to in the first place. kAudioAggregateDeviceIsPrivateKey
// keeps the device out of every other process's device list, including the
// user's Sound preferences, so tapping an application does not leave a
// stray "Aggregate Device" behind for someone to wonder about.
//
// ---------------------------------------------------------------------------
// muteBehavior, and why this platform needs no silent device
// ---------------------------------------------------------------------------
// CATapMutedWhenTapped mutes the tapped application at the point the tap
// takes its audio: the application keeps rendering, this process receives the
// samples, and nothing reaches the speakers by the ordinary path. On Windows
// and Linux the equivalent effect needs a silent device to move applications
// on to - an installed driver on one, a PipeWire node created at run time on
// the other - and moving the default output is a visible change to the user's
// machine that has to be explained and undone. Here the mute rides on the tap
// itself, so nothing in the sound settings changes and there is nothing to
// restore on quit. What it mutes is whatever the description covers, which
// for TapScope::kEverythingExceptListedProcess is every other application on
// the machine - see create_process_tap()'s comment in process_tap.hpp, where
// that consequence is spelled out for the caller who chooses the scope.
//
// ---------------------------------------------------------------------------
// Memory management
// ---------------------------------------------------------------------------
// This file is compiled with -fobjc-arc (see the APPLE block of
// src/audio/CMakeLists.txt), so the Objective-C objects below are released by
// the compiler and the toll-free bridge to CFDictionaryRef is spelled
// __bridge. The @autoreleasepool is not decoration: create_process_tap() is
// called from whatever thread the caller happens to be on, which on this
// project's callers is never the main thread and therefore never has a run
// loop draining a pool for it.

#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>

#include <sys/types.h>

#include <string>

#include "coreaudio_support.hpp"

namespace ac3::coreaudio {

namespace {

// The unix process id a caller names, as the AudioObjectID the HAL knows it
// by. kAudioObjectUnknown for a process the HAL has no audio object for -
// which is a process that does not exist AND a process that exists but has
// never opened an audio client, two cases this cannot tell apart and does not
// try to (process_tap.hpp's kProcessNotFound says so, and capture.cpp's
// describe() repeats it where a caller will read it).
API_AVAILABLE(macos(14.2))
AudioObjectID process_object_for_pid(std::uint32_t process_id) {
    const auto addr = address(kAudioHardwarePropertyTranslatePIDToProcessObject);
    const pid_t pid = static_cast<pid_t>(process_id);
    AudioObjectID object = kAudioObjectUnknown;
    UInt32 size = static_cast<UInt32>(sizeof(object));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                                   static_cast<UInt32>(sizeof(pid)), &pid, &size,
                                   &object) != noErr) {
        return kAudioObjectUnknown;
    }
    return object;
}

// What the tap says it will deliver. Read from the tap object rather than
// inferred from the mixdown shape: the channel count does follow from mono
// versus stereo, but the sample rate does not - it is the rate of the device
// the tap mixes down to, which is the user's, not ours. Returning false
// rather than a rate of zero so a driver that answers with a zeroed
// descriptor is refused rather than treated as 0 Hz.
API_AVAILABLE(macos(14.2))
bool read_tap_format(AudioObjectID tap, AudioStreamBasicDescription& out) {
    const auto addr = address(kAudioTapPropertyFormat);
    AudioStreamBasicDescription asbd{};
    UInt32 size = static_cast<UInt32>(sizeof(asbd));
    if (AudioObjectGetPropertyData(tap, &addr, 0, nullptr, &size, &asbd) != noErr) {
        return false;
    }
    if (asbd.mSampleRate <= 0.0 || asbd.mChannelsPerFrame == 0) {
        return false;
    }
    out = asbd;
    return true;
}

API_AVAILABLE(macos(14.2))
CATapDescription* make_description(AudioObjectID process_object, TapMixdown mixdown,
                                   TapScope scope) {
    NSArray<NSNumber*>* processes = @[ @(process_object) ];
    CATapDescription* description = nil;
    if (scope == TapScope::kOnlyListedProcess) {
        description = mixdown == TapMixdown::kMono
                          ? [[CATapDescription alloc] initMonoMixdownOfProcesses:processes]
                          : [[CATapDescription alloc] initStereoMixdownOfProcesses:processes];
    } else {
        description =
            mixdown == TapMixdown::kMono
                ? [[CATapDescription alloc] initMonoGlobalTapButExcludeProcesses:processes]
                : [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:processes];
    }
    if (description == nil) {
        return nil;
    }
    // A UUID of our own choosing rather than the one the initialiser
    // generates, because step 3 has to name this tap in the aggregate
    // device's tap list and there is no way to ask a created tap for the
    // description it came from.
    description.UUID = [NSUUID UUID];
    description.name = @"AC3Forge process tap";
    // The whole reason this platform needs no silent device - see this
    // file's header comment, and process_tap.hpp for what it means when
    // `scope` is the global-exclude one and the covered set is everything
    // else running.
    description.muteBehavior = CATapMutedWhenTapped;
    return description;
}

// The aggregate device dictionary. Built here rather than in capture.cpp
// only because it is one literal in Objective-C and three CFDictionary
// constructions in C; nothing in it needs Objective-C the way
// CATapDescription does.
API_AVAILABLE(macos(14.2))
NSDictionary* make_aggregate_description(NSString* output_uid, NSString* tap_uid,
                                         std::uint32_t process_id) {
    return @{
        @kAudioAggregateDeviceNameKey :
            [NSString stringWithFormat:@"AC3Forge tap (pid %u)", process_id],
        @kAudioAggregateDeviceUIDKey : [[NSUUID UUID] UUIDString],
        // The device that clocks the aggregate, named twice: once as the
        // main sub-device and once as the single entry of the sub-device
        // list, which is the form the surveyed implementations use.
        @kAudioAggregateDeviceMainSubDeviceKey : output_uid,
        @kAudioAggregateDeviceSubDeviceListKey : @[ @{@kAudioSubDeviceUIDKey : output_uid} ],
        // Invisible to every other process, including the Sound settings.
        @kAudioAggregateDeviceIsPrivateKey : @YES,
        // Not a stacked (multi-output) device: this aggregate exists to read
        // a tap, not to play the same audio out of several devices at once.
        @kAudioAggregateDeviceIsStackedKey : @NO,
        // The tap starts with the device rather than needing a separate
        // start of its own.
        @kAudioAggregateDeviceTapAutoStartKey : @YES,
        @kAudioAggregateDeviceTapListKey : @[ @{
            @kAudioSubTapUIDKey : tap_uid,
            // The tap and the clocking device are two different timebases
            // whenever the tapped application renders to something other
            // than the default output; drift compensation is what keeps the
            // aggregate from slowly over- or under-running in that case.
            @kAudioSubTapDriftCompensationKey : @YES,
        } ],
    };
}

API_AVAILABLE(macos(14.2))
TapStatus create_on_supported_os(std::uint32_t process_id, TapMixdown mixdown, TapScope scope,
                                 ProcessTap& out) {
    const AudioObjectID process_object = process_object_for_pid(process_id);
    if (process_object == kAudioObjectUnknown) {
        return TapStatus::kProcessNotFound;
    }

    // Read before the tap is created, so a Mac with no output at all is
    // turned away without having asked the user for consent to something
    // that could not have worked anyway.
    const std::string output_uid = device_uid(default_device(/*input=*/false));
    if (output_uid.empty()) {
        return TapStatus::kNoDefaultOutputDevice;
    }

    CATapDescription* description = make_description(process_object, mixdown, scope);
    if (description == nil) {
        return TapStatus::kTapRefused;
    }

    AudioObjectID tap = kAudioObjectUnknown;
    if (AudioHardwareCreateProcessTap(description, &tap) != noErr ||
        tap == kAudioObjectUnknown) {
        // The consent refusal arrives here, and so does an unsigned binary
        // whose prompt never appeared - neither is distinguishable from any
        // other refusal through this API, which is one of the reasons the
        // permission flow is named in docs/platforms/macos.md as something
        // only a real machine can settle.
        return TapStatus::kTapRefused;
    }

    NSDictionary* aggregate_description = make_aggregate_description(
        [NSString stringWithUTF8String:output_uid.c_str()], [description.UUID UUIDString],
        process_id);
    AudioObjectID aggregate = kAudioObjectUnknown;
    if (AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate_description,
                                           &aggregate) != noErr ||
        aggregate == kAudioObjectUnknown) {
        AudioHardwareDestroyProcessTap(tap);
        return TapStatus::kAggregateRefused;
    }

    AudioStreamBasicDescription format{};
    if (!read_tap_format(tap, format)) {
        AudioHardwareDestroyAggregateDevice(aggregate);
        AudioHardwareDestroyProcessTap(tap);
        return TapStatus::kFormatUnreadable;
    }

    out.tap = tap;
    out.aggregate_device = aggregate;
    out.format = format;
    return TapStatus::kOk;
}

}  // namespace

TapStatus create_process_tap(std::uint32_t process_id, TapMixdown mixdown, TapScope scope,
                             ProcessTap& out) {
    @autoreleasepool {
        if (@available(macOS 14.2, *)) {
            return create_on_supported_os(process_id, mixdown, scope, out);
        }
    }
    // The same answer system_audio_tap_api_available() gives, stated a second
    // time because @available will only accept a literal version here - a
    // call to that function is not a form the compiler will take as an
    // availability guard, and without a guard every line above would be an
    // unguarded-availability error against this project's 13.3 deployment
    // target (cmake/toolchains/macos.llvm.toolchain.cmake).
    return TapStatus::kOsTooOld;
}

void destroy_process_tap(ProcessTap& tap) {
    if (@available(macOS 14.2, *)) {
        // The aggregate device names the tap, so the device goes first: a tap
        // destroyed while a live aggregate still refers to it is the one
        // ordering this API gives no account of, and the reverse of creation
        // order is the ordering that needs no account.
        if (tap.aggregate_device != kAudioObjectUnknown) {
            AudioHardwareDestroyAggregateDevice(tap.aggregate_device);
        }
        if (tap.tap != kAudioObjectUnknown) {
            AudioHardwareDestroyProcessTap(tap.tap);
        }
    }
    // Cleared outside the guard as well as inside it: on an OS with no tap
    // API these were never anything but kAudioObjectUnknown, and a caller
    // that can call this twice (Capture::stop(), which is also the
    // destructor's path) must not see stale ids either way.
    tap.aggregate_device = kAudioObjectUnknown;
    tap.tap = kAudioObjectUnknown;
    tap.format = AudioStreamBasicDescription{};
}

}  // namespace ac3::coreaudio
