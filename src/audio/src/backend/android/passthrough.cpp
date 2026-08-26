#include "ac3/audio/passthrough.hpp"

// The Android passthrough backend. CMake compiles this directory's
// passthrough.cpp on Android and another platform directory's everywhere
// else, so there is no #ifdef - the file's path is what says "Android".
//
// Deliberately NOT AAudio (see audio_backend.cpp): AAudio has no support for
// compressed/IEC 61937 bitstream passthrough anywhere in its API surface, so
// this is a thin JNI shim to a Kotlin-owned android.media.AudioTrack. See
// docs/platforms/android.md for the full background.
//
// Opened with ENCODING_IEC61937, deliberately NOT ENCODING_E_AC3/
// ENCODING_AC3, and this is not a minor detail: the two mean different
// things to AudioFlinger. ENCODING_E_AC3/ENCODING_AC3 take RAW elementary
// compressed frames and Android does the IEC 61937 burst-wrapping itself;
// ENCODING_IEC61937 takes an ALREADY-WRAPPED burst and passes it straight
// through untouched (confirmed against Kodi's AESinkAUDIOTRACK.cpp, which
// writes raw frames for the ENCODING_AC3/DTS/TRUEHD path but explicitly
// documents ENCODING_IEC61937 data as "already IEC 61937-formatted from
// upstream"). This project's PassthroughSink::submit() contract is fixed
// across every platform - it receives a complete, already-wrapped
// ac3::iec61937::wrap_frame()/Eac3BurstPacker::push() burst - so
// ENCODING_E_AC3 would double-wrap that burst inside another layer of
// Android's own framing and produce noise; ENCODING_IEC61937 is the only
// encoding whose input shape matches what this backend actually has.
//
// Two consequences of ENCODING_IEC61937 specifically, both load-bearing:
//  - The channel mask is CHANNEL_OUT_STEREO, not 5.1/7.1, regardless of the
//    decoded channel count - a burst physically rides a 2-channel 16-bit
//    S/PDIF-shaped carrier no matter how many channels it decodes to on the
//    other end (same Kodi source, E-AC-3 is not exempted the way DTS-HD-MA/
//    TrueHD are).
//  - The declared sample rate is the CARRIER rate, not the content rate -
//    the same "the declared rate describes the wire" fact
//    android_support.hpp's carrier_rate() and the ALSA/WASAPI backends
//    already encode (E-AC-3 carries at 4x content rate; see that function's
//    comment). So this backend computes the carrier rate in C++ and hands
//    Kotlin the already-carrier-rate value - Kotlin never multiplies by 4
//    itself, there is exactly one place that fact lives.
//
// The Kotlin side (app-specific, not part of ac3::audio - see
// apps/android/app/src/main/java/.../NativeBridge.kt and
// PassthroughBridge.kt) is expected to expose exactly this contract, called
// through the method IDs cached in registerPassthroughBridge() below:
//
//   class PassthroughBridge {
//       // Probes AudioTrack.isDirectPlaybackSupported() for ENCODING_
//       // IEC61937 at `carrierRateHz` (already the carrier rate for eac3 -
//       // see above). One format per call, not a batch: AC-3 and E-AC-3
//       // need different carrier rates for the same content rate, so there
//       // is no single rate a batched probe could use for both.
//       fun isDirectPlaybackSupported(carrierRateHz: Int, eac3: Boolean): Boolean
//       // Ordinary (non-IEC61937) direct PCM support at the CONTENT rate -
//       // no carrier-rate concept applies to plain PCM.
//       fun isPcmSupported(sampleRateHz: Int): Boolean
//       // Opens the AudioTrack at `carrierRateHz`; false on any failure
//       // (format rejected, device busy, etc - Android does not
//       // distinguish the reason at this boundary the way WASAPI's
//       // HRESULTs do).
//       fun open(carrierRateHz: Int, eac3: Boolean): Boolean
//       // AudioTrack.write(buffer, sizeBytes, WRITE_NON_BLOCKING): the
//       // number of bytes actually written, or a negative
//       // AudioTrack.ERROR_* code.
//       fun submit(buffer: ByteBuffer, sizeBytes: Int): Int
//       fun close()
//   }
//
// The app registers one bridge instance for the process's lifetime via
// NativeBridge.registerPassthroughBridge(), which calls the JNI-exported
// registerPassthroughBridge() below. JNI_OnLoad here captures the process
// JavaVM so a background thread (there is none today - see the "no separate
// render thread" note on PassthroughSink::submit - but a future one could)
// can attach itself.

#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "ac3/sinks/iec61937.hpp"
#include "android_support.hpp"

namespace ac3::audio {

namespace {

// Visible via `adb logcat -s ac3audio.passthrough` - there is no interactive
// debugger for a background render path talking to Java over JNI, so every
// failure branch below logs rather than only returning an error code.
constexpr char kLogTag[] = "ac3audio.passthrough";

// --- JNI plumbing --------------------------------------------------------
// Everything below is internal to this translation unit. One JavaVM per
// process; the Kotlin bridge is a singleton registered once at app startup,
// before any PassthroughSink can start() - see register_bridge().
JavaVM* g_vm = nullptr;
std::mutex g_bridge_mutex;
jobject g_bridge = nullptr;  // GlobalRef to the Kotlin PassthroughBridge singleton
jmethodID g_mid_is_direct_supported = nullptr;  // boolean isDirectPlaybackSupported(int, boolean)
jmethodID g_mid_is_pcm_supported = nullptr;     // boolean isPcmSupported(int)
jmethodID g_mid_open = nullptr;                 // boolean open(int, boolean)
jmethodID g_mid_submit = nullptr;               // int submit(ByteBuffer, int)
jmethodID g_mid_close = nullptr;                // void close()

// Detaches a thread WE attached, when that thread exits.
//
// A thread that attached to the JVM must detach before it exits or the VM
// aborts, which is what the previous attach-then-detach-per-call shape was
// really guarding against. The cost of that shape landed on the worst
// possible thread: submit() is called once per 32ms audio frame from the
// encode loop's real-time worker, so a deadline thread registered itself with
// ART, walked its thread list, and unregistered again, every single frame,
// for the life of the stream.
//
// A thread_local with a destructor gets the same guarantee for free: it is
// constructed on the call that actually attaches, and its destructor runs
// when the thread exits, however it exits.
struct JvmThreadAttachment {
    bool attached = false;
    ~JvmThreadAttachment() {
        if (attached && g_vm != nullptr) {
            g_vm->DetachCurrentThread();
        }
    }
};

// The JNIEnv for the calling thread, attaching it once if needed. Callers do
// no detach bookkeeping - see JvmThreadAttachment.
JNIEnv* jni_env() {
    if (g_vm == nullptr) {
        return nullptr;
    }
    JNIEnv* env = nullptr;
    // Already attached: either a JVM-owned thread (the one JNI_OnLoad and
    // registerBridge run on), or one this function attached earlier.
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }
    // Constructed on this path only, which is exactly the path that attaches.
    thread_local JvmThreadAttachment attachment;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return nullptr;
    }
    attachment.attached = true;
    return env;
}

// True once register_bridge() has cached every method ID successfully. Every
// entry point below checks this before touching JNI, so a Shield app that
// forgot to register the bridge (or whose Kotlin side crashed on startup)
// fails cleanly with kNoBackend/kComFailure instead of crashing on a null
// method ID.
bool bridge_ready() {
    std::lock_guard lock(g_bridge_mutex);
    return g_bridge != nullptr && g_mid_is_direct_supported != nullptr &&
           g_mid_is_pcm_supported != nullptr && g_mid_open != nullptr &&
           g_mid_submit != nullptr && g_mid_close != nullptr;
}

}  // namespace

std::string_view describe(PassthroughError error) {
    switch (error) {
        case PassthroughError::kNoBackend: return "no passthrough backend on this platform";
        case PassthroughError::kComFailure:
            return "a JNI call into the Kotlin AudioTrack bridge failed, or the bridge was "
                   "never registered (NativeBridge.registerPassthroughBridge was not called)";
        case PassthroughError::kDeviceNotFound:
            return "AudioTrack could not open an output stream";
        case PassthroughError::kFormatRejected:
            return "the connected HDMI sink/AVR does not support this format for direct "
                   "playback (AudioTrack.isDirectPlaybackSupported returned false) - check the "
                   "Shield's own audio output format settings and the receiver's EDID-reported "
                   "encodings";
        case PassthroughError::kExclusiveUnavailable:
            return "the system audio route was unavailable (output busy, or the AudioTrack "
                   "entered an error state) - Android has no separate exclusive-mode concept, "
                   "this maps to AudioTrack write failures instead";
        case PassthroughError::kAlreadyRunning: return "passthrough is already running";
        case PassthroughError::kNotRunning: return "passthrough is not running";
    }
    return "unknown passthrough error";
}

std::expected<std::vector<RenderDeviceInfo>, PassthroughError> enumerate_render_devices(
    std::uint32_t sample_rate) {
    if (!bridge_ready()) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "enumerate_render_devices: bridge not registered - did "
                            "NativeBridge.registerPassthroughBridge run?");
        return std::unexpected(PassthroughError::kNoBackend);
    }
    JNIEnv* env = jni_env();
    if (env == nullptr) {
        return std::unexpected(PassthroughError::kComFailure);
    }

    std::vector<RenderDeviceInfo> devices;
    {
        std::lock_guard lock(g_bridge_mutex);
        // Three separate calls, not one batched probe: AC-3 and E-AC-3 need
        // different carrier rates for the same content rate (see this
        // file's header comment and android_audio::carrier_rate), so there
        // is no single rate a batched probe could pass for both, and plain
        // PCM has no carrier-rate concept at all.
        const jboolean ac3_supported = env->CallBooleanMethod(
            g_bridge, g_mid_is_direct_supported, static_cast<jint>(sample_rate), JNI_FALSE);
        const bool ac3_ok = env->ExceptionCheck() == 0 && ac3_supported != JNI_FALSE;
        if (env->ExceptionCheck() != 0) {
            env->ExceptionClear();
        }

        const auto eac3_carrier = android_audio::carrier_rate(BitstreamFormat::kEac3, sample_rate);
        const jboolean eac3_supported = env->CallBooleanMethod(
            g_bridge, g_mid_is_direct_supported, static_cast<jint>(eac3_carrier), JNI_TRUE);
        const bool eac3_ok = env->ExceptionCheck() == 0 && eac3_supported != JNI_FALSE;
        if (env->ExceptionCheck() != 0) {
            env->ExceptionClear();
        }

        const jboolean pcm_supported =
            env->CallBooleanMethod(g_bridge, g_mid_is_pcm_supported, static_cast<jint>(sample_rate));
        const bool pcm_ok = env->ExceptionCheck() == 0 && pcm_supported != JNI_FALSE;
        if (env->ExceptionCheck() != 0) {
            env->ExceptionClear();
        }

        // Android abstracts device routing away from the app - there is no
        // WASAPI-style enumeration of individually named render endpoints
        // to walk, only "what the system's current output route currently
        // supports". So this reports exactly one synthetic entry describing
        // that route, rather than a real per-device list - see
        // android_support.hpp's make_render_device_info() for the pure
        // (and tested) half of this mapping.
        devices.push_back(android_audio::make_render_device_info(ac3_ok, eac3_ok, pcm_ok));
    }

    return devices;
}

struct PassthroughSink::Impl {
    // A small pool of native buffers, each wrapped exactly once as a direct
    // ByteBuffer (env->NewDirectByteBuffer) and promoted to a GlobalRef -
    // NewDirectByteBuffer's result is only a local ref, not valid to retain
    // across calls, and direct buffers are non-moving (unlike a
    // ByteBuffer.wrap()'d Java heap array, which the GC can relocate). Each
    // submit() round-robins to the next slot instead of allocating a fresh
    // wrapper per frame - see docs/platforms/android.md for why this beats
    // Kodi's per-write heap-array-wrap approach.
    static constexpr int kPoolSize = 3;
    std::vector<std::vector<std::byte>> native_storage{kPoolSize};
    jobject direct_buffers[kPoolSize] = {};
    int next_slot = 0;

    // How many bytes of the burst currently being submitted the AudioTrack
    // has already accepted across earlier, partial attempts. See submit().
    std::size_t pending_offset = 0;
    std::uint64_t partial_writes = 0;

    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> underruns{0};
    std::size_t burst_bytes = iec61937::kBurstBytes;
};

PassthroughSink::PassthroughSink() : impl_(std::make_unique<Impl>()) {}

PassthroughSink::~PassthroughSink() {
    stop();
}

bool PassthroughSink::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

PassthroughStats PassthroughSink::stats() const {
    return {.bursts_submitted = impl_->submitted.load(std::memory_order_relaxed),
            .bursts_rendered = impl_->rendered.load(std::memory_order_relaxed),
            .underruns = impl_->underruns.load(std::memory_order_relaxed)};
}

bool PassthroughSink::can_submit() const {
    // There is no separate ring buffer/render thread to ask here - unlike
    // WASAPI/ALSA, where WE own an explicit queue a background thread
    // drains, the Java AudioTrack itself owns the actual buffering and
    // mixer thread. submit() below writes straight into it
    // (WRITE_NON_BLOCKING) and reports success/failure from that single
    // call, so "room to submit" is simply "are we started".
    return running();
}

bool PassthroughSink::submit(std::span<const std::byte> burst) {
    if (!running() || burst.size() != impl_->burst_bytes || !bridge_ready()) {
        return false;
    }
    JNIEnv* env = jni_env();
    if (env == nullptr) {
        return false;
    }

    bool ok = false;
    {
        std::lock_guard lock(g_bridge_mutex);
        // A short (but non-negative) write means the AudioTrack accepted
        // SOME bytes without accepting the whole burst. That is the
        // documented behaviour of WRITE_NON_BLOCKING - it queues as much as
        // fits and tells you how much - and it is reachable here whenever the
        // track has room for part of a burst but not all of it.
        //
        // This used to be treated as a plain failure and the caller's retry
        // loop resubmitted the WHOLE burst, splicing the already-accepted
        // bytes into the stream a second time: a corrupt IEC 61937 burst, and
        // a receiver that mutes or glitches rather than an error anyone could
        // trace. (The comment that used to sit here excused it on the grounds
        // that "that space is inspected by the Kotlin side before calling
        // write" - PassthroughBridge.submit does no such inspection; it calls
        // write directly.)
        //
        // So a partial write is now resumed from rather than restarted:
        // pending_offset remembers how much of THIS burst is already queued,
        // and only the remainder is staged and offered on the next attempt.
        // The caller keeps passing the same full burst, which is what makes
        // this safe to fix entirely on this side of the interface - no JNI
        // signature change, no change to what a caller has to know.
        const std::size_t offset = impl_->pending_offset;
        const std::size_t remaining = burst.size() - offset;

        const int slot = impl_->next_slot;
        impl_->next_slot = (impl_->next_slot + 1) % Impl::kPoolSize;
        auto& storage = impl_->native_storage[static_cast<std::size_t>(slot)];
        std::memcpy(storage.data(), burst.data() + offset, remaining);

        const jint written = env->CallIntMethod(g_bridge, g_mid_submit,
                                                impl_->direct_buffers[slot],
                                                static_cast<jint>(remaining));
        if (env->ExceptionCheck() != 0) {
            env->ExceptionClear();
        } else if (written == static_cast<jint>(remaining)) {
            ok = true;
            impl_->pending_offset = 0;
        } else if (written > 0) {
            // Partial: keep what was accepted and resume from there. Not an
            // underrun - the track took data, it just could not take all of
            // it this instant.
            impl_->pending_offset = offset + static_cast<std::size_t>(written);
            if (impl_->partial_writes++ == 0) {
                __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                    "AudioTrack accepted a partial burst (%d of %zu bytes) - "
                                    "resuming from the offset rather than resubmitting",
                                    static_cast<int>(written), remaining);
            }
        } else if (written < 0) {
            // A hard AudioTrack error. pending_offset is deliberately NOT
            // reset: if the track recovers, resuming is still correct, and
            // duplicating already-queued bytes is worse than a short burst.
            impl_->underruns.fetch_add(1, std::memory_order_relaxed);
        }
        // written == 0 is the ordinary "buffer full" case: nothing was
        // accepted, so there is nothing to remember and the caller simply
        // retries.
    }

    if (ok) {
        impl_->submitted.fetch_add(1, std::memory_order_relaxed);
        impl_->rendered.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
}

void PassthroughSink::stop() {
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // Whatever was half-queued dies with the track; a later start() must not
    // resume into a burst nothing is waiting for.
    impl_->pending_offset = 0;
    if (!bridge_ready()) {
        return;
    }
    JNIEnv* env = jni_env();
    if (env == nullptr) {
        return;
    }
    {
        std::lock_guard lock(g_bridge_mutex);
        env->CallVoidMethod(g_bridge, g_mid_close);
        if (env->ExceptionCheck() != 0) {
            env->ExceptionClear();
        }
        for (auto& buf : impl_->direct_buffers) {
            if (buf != nullptr) {
                env->DeleteGlobalRef(buf);
                buf = nullptr;
            }
        }
    }
}

std::expected<void, PassthroughError> PassthroughSink::start(const std::string& /*device_id*/,
                                                              std::uint32_t sample_rate,
                                                              BitstreamFormat format) {
    // device_id is accepted (the interface is shared with the WASAPI/ALSA
    // backends) but unused: see enumerate_render_devices() above on why
    // Android has exactly one addressable output route, not a list of
    // endpoint ids to pick from.
    if (running()) {
        return std::unexpected(PassthroughError::kAlreadyRunning);
    }
    if (!bridge_ready()) {
        return std::unexpected(PassthroughError::kNoBackend);
    }
    JNIEnv* env = jni_env();
    if (env == nullptr) {
        return std::unexpected(PassthroughError::kComFailure);
    }

    impl_->burst_bytes = android_audio::burst_bytes_for(format);
    // No burst is half-queued on a track that does not exist yet - see
    // submit()'s partial-write handling for what this tracks.
    impl_->pending_offset = 0;
    impl_->partial_writes = 0;
    bool opened = false;
    {
        std::lock_guard lock(g_bridge_mutex);
        // Allocate and wrap the buffer pool now, sized to this format's
        // burst - see the Impl comment on why this happens once here rather
        // than per submit() call.
        for (int i = 0; i < Impl::kPoolSize; ++i) {
            auto& storage = impl_->native_storage[static_cast<std::size_t>(i)];
            storage.assign(impl_->burst_bytes, std::byte{0});
            jobject direct = env->NewDirectByteBuffer(storage.data(),
                                                       static_cast<jlong>(storage.size()));
            if (direct == nullptr) {
                for (int j = 0; j < i; ++j) {
                    env->DeleteGlobalRef(impl_->direct_buffers[j]);
                    impl_->direct_buffers[j] = nullptr;
                }
                return std::unexpected(PassthroughError::kComFailure);
            }
            impl_->direct_buffers[i] = env->NewGlobalRef(direct);
            env->DeleteLocalRef(direct);
        }

        // The carrier rate, not the content rate - open() is documented to
        // expect it already computed (see this file's header comment on
        // why that fact lives in exactly one place, here, and not in
        // Kotlin).
        const auto carrier = android_audio::carrier_rate(format, sample_rate);
        const jboolean eac3 = format == BitstreamFormat::kEac3 ? JNI_TRUE : JNI_FALSE;
        opened = env->CallBooleanMethod(g_bridge, g_mid_open, static_cast<jint>(carrier),
                                        eac3) != JNI_FALSE;
        if (env->ExceptionCheck() != 0) {
            env->ExceptionClear();
            opened = false;
        }
    }

    if (!opened) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "start: PassthroughBridge.open(%u Hz carrier, eac3=%d) returned "
                            "false - the sink likely does not accept this format for direct "
                            "playback",
                            android_audio::carrier_rate(format, sample_rate),
                            format == BitstreamFormat::kEac3);
        return std::unexpected(PassthroughError::kFormatRejected);
    }

    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered.store(0, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);
    impl_->running.store(true, std::memory_order_release);
    return {};
}

}  // namespace ac3::audio

// --- JNI entry points -----------------------------------------------------
// Deliberately outside namespace ac3::audio: JNI symbol names are fixed by
// the mangling convention (Java_<package>_<Class>_<method>), not by us.
//
// JNI_OnLoad is defined here, not in the app's own native code
// (apps/android/app/src/main/cpp/), because capturing the JavaVM is
// ac3::audio's own concern (jni_env() above needs it) and a
// process may load exactly one JNI_OnLoad per shared object - this is the
// only translation unit in ac3forge_jni.so that needs it.

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    ac3::audio::g_vm = vm;
    __android_log_print(ANDROID_LOG_INFO, ac3::audio::kLogTag,
                        "JNI_OnLoad: ac3forge_jni loaded, JavaVM captured");
    return JNI_VERSION_1_6;
}

// Called once from Kotlin (NativeBridge.registerPassthroughBridge) after
// System.loadLibrary, before any PassthroughSink::start(). Safe to call
// again (e.g. on Activity recreation) - drops the previous GlobalRef first.
extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_registerPassthroughBridge(JNIEnv* env, jclass /*clazz*/,
                                                                 jobject bridge) {
    std::lock_guard lock(ac3::audio::g_bridge_mutex);

    if (ac3::audio::g_bridge != nullptr) {
        env->DeleteGlobalRef(ac3::audio::g_bridge);
        ac3::audio::g_bridge = nullptr;
    }
    if (bridge == nullptr) {
        return;
    }

    jclass local_class = env->GetObjectClass(bridge);
    if (local_class == nullptr) {
        return;
    }
    ac3::audio::g_mid_is_direct_supported =
        env->GetMethodID(local_class, "isDirectPlaybackSupported", "(IZ)Z");
    ac3::audio::g_mid_is_pcm_supported = env->GetMethodID(local_class, "isPcmSupported", "(I)Z");
    ac3::audio::g_mid_open = env->GetMethodID(local_class, "open", "(IZ)Z");
    ac3::audio::g_mid_submit =
        env->GetMethodID(local_class, "submit", "(Ljava/nio/ByteBuffer;I)I");
    ac3::audio::g_mid_close = env->GetMethodID(local_class, "close", "()V");
    if (env->ExceptionCheck() != 0) {
        env->ExceptionClear();
        ac3::audio::g_mid_is_direct_supported = nullptr;
        ac3::audio::g_mid_is_pcm_supported = nullptr;
        ac3::audio::g_mid_open = nullptr;
        ac3::audio::g_mid_submit = nullptr;
        ac3::audio::g_mid_close = nullptr;
        env->DeleteLocalRef(local_class);
        __android_log_print(ANDROID_LOG_ERROR, ac3::audio::kLogTag,
                            "registerPassthroughBridge: GetMethodID failed - does "
                            "PassthroughBridge match the expected "
                            "isDirectPlaybackSupported/isPcmSupported/open/submit/close "
                            "signatures?");
        return;
    }

    ac3::audio::g_bridge = env->NewGlobalRef(bridge);
    env->DeleteLocalRef(local_class);
    __android_log_print(ANDROID_LOG_INFO, ac3::audio::kLogTag,
                        "registerPassthroughBridge: bridge registered");
}
