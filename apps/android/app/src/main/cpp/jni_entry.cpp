// This app's own native entry points. Everything platform-specific that
// iclforge::audio needs from JNI (the PassthroughBridge plumbing, and JNI_OnLoad
// itself) lives in that library's own
// src/audio/src/backend/android/passthrough.cpp, not here - see that
// file's header comment for why JNI_OnLoad belongs there (capturing the
// JavaVM is iclforge::audio's own concern) rather than being duplicated in this
// translation unit, which would be an ODR violation at link time (a shared
// object may define JNI_OnLoad exactly once).
//
// Two smoke-test entry points, checkable from Kotlin/logcat with
// progressively more of the pipeline exercised, matching
// docs/platforms/android.md's build-order notes:
//  - nativeVersionString: proof iclforge_jni.so actually linked the real
//    iclforge::ac3 static library rather than an empty stub. No audio
//    hardware or JNI passthrough bridge involved at all.
//  - nativeProbePassthroughCapabilities: the real HDMI capability probe
//    end to end - native calls iclforge::audio::enumerate_render_devices(),
//    which calls back into the just-registered PassthroughBridge over
//    JNI, which asks AudioTrack.isDirectPlaybackSupported() what the
//    Shield's current audio route (and the receiver on the other end of
//    the HDMI cable) actually accepts. Exercises the whole
//    NativeBridge<->PassthroughBridge<->passthrough.cpp round trip with no
//    audio content required - the encode loop (a later pass) is what
//    actually streams bursts through it.

#include <jni.h>

#include <android/log.h>

#include <string>

#include "iclforge/audio/audio_backend.hpp"
#include "iclforge/audio/passthrough.hpp"
#include "iclforge/ac3/version.hpp"

namespace {
constexpr char kLogTag[] = "iclforge.shield";
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_iclforge_shield_NativeBridge_nativeVersionString(JNIEnv* env, jclass /*clazz*/) {
    const std::string version(iclforge::version_full);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "iclforge::ac3 %s linked into iclforge_jni.so",
                        version.c_str());
    return env->NewStringUTF(version.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_iclforge_shield_NativeBridge_nativeProbePassthroughCapabilities(JNIEnv* env,
                                                                          jclass /*clazz*/) {
    const auto& backend = iclforge::audio::audio_backend();
    std::string report = "passthrough backend compiled in: ";
    report += backend.passthrough.available ? "yes" : "no";
    if (!backend.passthrough.available) {
        report += " (";
        report += backend.passthrough.reason;
        report += ")";
    }

    const auto devices = iclforge::audio::enumerate_render_devices(48000);
    if (!devices) {
        report += "\nenumerate_render_devices failed: ";
        report += iclforge::audio::describe(devices.error());
    } else if (devices->empty()) {
        report += "\nenumerate_render_devices returned no devices";
    } else {
        for (const auto& device : *devices) {
            report += "\n";
            report += device.name;
            report += ": AC-3=";
            report += device.supports_ac3_passthrough ? "yes" : "no";
            report += " E-AC-3=";
            report += device.supports_eac3_passthrough ? "yes" : "no";
            report += " PCM=";
            report += device.supports_exclusive_pcm ? "yes" : "no";
        }
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "passthrough capability probe:\n%s",
                        report.c_str());
    return env->NewStringUTF(report.c_str());
}
