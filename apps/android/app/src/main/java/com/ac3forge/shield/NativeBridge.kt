package com.ac3forge.shield

/**
 * The JNI surface `ac3forge_jni.so` exposes to Kotlin, and the registration
 * call that goes the other way.
 *
 * `registerPassthroughBridge` is declared here now (implemented in
 * src/audio/src/backend/android/passthrough.cpp) even though
 * [PassthroughBridge] itself does not exist yet - the native symbol name is
 * part of the JNI contract fixed by mangling
 * (`Java_com_ac3forge_shield_NativeBridge_registerPassthroughBridge`), so the
 * Kotlin-side declaration and the native `extern "C"` definition have to
 * agree on the package/class name from the start, not be introduced
 * together later.
 */
object NativeBridge {
    /**
     * Whether `ac3forge_jni` actually loaded. Callers must check this before
     * any `native*` call below; every one of them throws
     * [UnsatisfiedLinkError] when it is false.
     *
     * The load is caught here rather than left to throw out of the static
     * initializer, because a throwing initializer does not just fail once -
     * it marks the class **erroneous** for the life of the process, and every
     * subsequent access raises [NoClassDefFoundError] rather than the
     * [UnsatisfiedLinkError] the call sites are written to catch. That turned
     * the carefully-written "(native link failed - see logcat)" degraded mode
     * into a crash on the very next line of `MainActivity.onCreate`: the
     * `catch (e: UnsatisfiedLinkError)` around the first call did its job, and
     * the *second* touch of this object took the process down.
     *
     * Catching [Throwable], not [UnsatisfiedLinkError]: `System.loadLibrary`
     * also raises [SecurityException] and, on a mismatched/corrupt .so,
     * [UnsatisfiedLinkError]'s siblings - and the whole point here is that
     * nothing this method can do should be able to poison the class.
     */
    @JvmStatic
    val available: Boolean = try {
        System.loadLibrary("ac3forge_jni")
        true
    } catch (t: Throwable) {
        android.util.Log.e("ShieldAtmosDemo", "System.loadLibrary(ac3forge_jni) failed", t)
        false
    }

    /** Smoke test only - see jni_entry.cpp. Proves the native link worked. */
    external fun nativeVersionString(): String

    /**
     * Registers the process-lifetime [PassthroughBridge] singleton with the
     * native passthrough backend. Must be called once before any
     * PassthroughSink::start() on the native side; safe to call again (e.g.
     * on Activity recreation).
     */
    external fun registerPassthroughBridge(bridge: Any)

    /**
     * Smoke test: runs ac3::audio::enumerate_render_devices() end to end
     * (native -> PassthroughBridge -> AudioTrack.isDirectPlaybackSupported)
     * and returns a human-readable report. Requires
     * [registerPassthroughBridge] to have run first. See jni_entry.cpp.
     */
    external fun nativeProbePassthroughCapabilities(): String

    /**
     * Starts/stops the encode loop (live_cursor.cpp) on its own native
     * thread. Requires [registerPassthroughBridge] to have run first. Safe
     * to call nativeStartLiveCursor while already running (no-op); safe to
     * call nativeStopLiveCursor while not running (no-op).
     */
    external fun nativeStartLiveCursor(): Boolean
    external fun nativeStopLiveCursor()

    /**
     * Whether the encode loop's worker thread is actually up, distinct from
     * "was nativeStartLiveCursor called" - PassthroughSink::start() fails
     * fast (and the thread exits immediately, before this ever becomes true)
     * if no receiver currently accepts E-AC3/Atmos, e.g. an AVR that's off
     * or not yet HDMI-negotiated at the moment nativeStartLiveCursor() was
     * called. MainActivity polls this (see its own reconcileReceiverState())
     * to notice that case and retry once the receiver actually shows up,
     * instead of requiring the user to force-restart the app.
     */
    external fun nativeIsLiveCursorRunning(): Boolean

    /**
     * The running-total count of failed AudioTrack writes since the encode
     * loop last started - how reconcileReceiverState() notices a receiver
     * disappearing WHILE already streaming, without calling
     * [PassthroughBridge.isDirectPlaybackSupported] again while a direct
     * AudioTrack is open: that call BLOCKS INDEFINITELY against an
     * actively-playing direct track on the same route (confirmed hanging on
     * real hardware, not a hypothetical) - a rising underrun count is a
     * safe, purely-numeric alternative signal.
     */
    external fun nativeGetUnderrunCount(): Long

    /**
     * Hands the app's [android.content.res.AssetManager] to native so the
     * encode loop can load the bundled lead-object voice sample
     * (assets/lead_voice_48k_mono_s16le.raw) from its own thread. Must be
     * called before [nativeStartLiveCursor] - the encode loop reads the
     * asset once at startup, not lazily. Missing this call is not fatal:
     * live_cursor.cpp falls back to its live-synthesized voice.
     */
    external fun nativeSetAssetManager(assetManager: android.content.res.AssetManager)

    /**
     * Biases the currently-selected object's position by (dx, dy, dz) away
     * from its pre-planned trajectory, clamped to a bounding box on the
     * native side. Called from [InputController]'s animation ticker roughly
     * once per frame, already scaled by stick magnitude/speed/elapsed-time
     * or a held D-pad direction x speed x elapsed-time - never called once
     * per raw input event. The bias decays back to zero on its own, every
     * encode frame, whether or not this is called again - see
     * live_cursor.cpp's LiveCursorState::advance/deflect_selected.
     */
    external fun nativeDeflectSelectedObject(dx: Float, dy: Float, dz: Float)

    /** Moves the selection to the next object; returns the new selected index. */
    external fun nativeCycleSelectedObject(): Int

    /**
     * Instantly zeroes the selected object's deflection, rather than
     * waiting out the usual ~1.5s spring-back decay - a presenter's "and...
     * reset" button. See live_cursor.cpp's LiveCursorState::snap_selected.
     */
    external fun nativeSnapSelectedToCourse()

    /**
     * kObjects*4 floats: (x, y, z, isSelected) per object, in native's fixed
     * object order. For the room visualization.
     */
    external fun nativeGetObjectState(): FloatArray

    /**
     * Mutes/unmutes the two ambient objects' audio (their trajectory keeps
     * advancing regardless) - wired to the remote's pause/play keys so a
     * listener can isolate the lead object's sound. See live_cursor.cpp's
     * StreamStats::ambient_muted.
     */
    external fun nativeSetAmbientMuted(muted: Boolean)

    /** One formatted line of live encode-loop stats, for the on-screen overlay. */
    external fun nativeGetStreamStatsText(): String

    /**
     * 6 floats, AC-3 coded order (L, C, R, Ls, Rs, LFE): RMS level of each
     * real bed channel the last frame actually encoded
     * (AtmosEncoder::bed()), already scaled and clamped to [0,1] for direct
     * use as a meter bar height. For the speaker-activity meter.
     */
    external fun nativeGetChannelLevels(): FloatArray

    /**
     * `samples` (x,y,z) triples along the lead object's own base
     * trajectory - no deflection, since future deflection can't be known -
     * starting now and running `secondsAhead` seconds into the future. For
     * the 3D trail view's "path ahead"; see live_cursor.cpp's
     * trajectory_position().
     */
    external fun nativeGetFutureLeadTrajectory(secondsAhead: Float, samples: Int): FloatArray

    /**
     * The demo scene: which path through the room every object is following,
     * and what the demo is asking the listener to notice. See live_cursor.cpp's
     * `kScenes`. Wraps in both directions, so "next" from the last scene is
     * the first.
     *
     * Changing scene starts a short blend rather than jumping - a 32ms step
     * from one side of the room to the other is an abrupt pan, not a move.
     */
    external fun nativeSetScene(scene: Int)
    external fun nativeGetScene(): Int
    external fun nativeGetSceneCount(): Int

    /**
     * One scene's name and its "listen for this" line, tab-separated. One call
     * rather than two because they are only ever wanted together.
     */
    external fun nativeGetSceneText(scene: Int): String

    /**
     * OBJECTS OFF: strips the object layer out of every access unit before it
     * is wrapped for output, live, leaving everything else about the stream
     * alone (`ac3::io::strip_objects`). The bed decodes identically - it is
     * the same coded bed either way - so what changes is that a licensed
     * decoder stops seeing an object programme and drops to plain DD+.
     *
     * Only does anything on a build carrying the signing key: without one the
     * encoder emits no object container at all, so there is nothing to strip
     * and the toggle is a visible no-op. See shield_signing_hook.hpp.
     */
    external fun nativeSetObjectsOff(off: Boolean)
    external fun nativeGetObjectsOff(): Boolean

    /**
     * Two floats: the energy vector's azimuth (degrees counterclockwise from
     * front) and its magnitude in [0,1], over the REAL encoded 5.1 bed
     * (`ac3::analysis::energy_vector`).
     *
     * Distinct from the object positions [nativeGetObjectState] reports: those
     * are where the demo asked the object to go, this is where a 5.1 decoder's
     * own speakers will actually put the energy. Seeing the two agree is the
     * point.
     */
    external fun nativeGetSoundfieldVector(): FloatArray

    /**
     * The bed's measured BS.1770 integrated loudness and the dialnorm it
     * implies, preformatted. Empty until the meter's first gated 400ms block
     * has passed - and empty is the correct thing to show for silence, not a
     * fabricated number.
     */
    external fun nativeGetLoudnessText(): String

    /**
     * Diagnostic-only: streams a real, already-encoded AC-3/E-AC-3 file
     * (e.g. an audio track pulled from a commercial Dolby Atmos demo MKV,
     * unmodified) through the same PassthroughSink path the live cursor
     * uses - see file_replay.cpp. Blocks until the whole file has been
     * submitted and drained; call off the main thread. Independent of
     * [nativeStartLiveCursor] - does not touch LiveCursorState. Requires
     * [registerPassthroughBridge] to have run first, same as the live
     * cursor. Returns false on read/parse/sink-start failure (see logcat
     * tag ac3forge.shield.file_replay for why).
     */
    external fun nativePlayEac3File(path: String): Boolean

    /**
     * Asks an in-flight [nativePlayEac3File] to end at its next wait point.
     * Non-blocking and safe to call when nothing is playing.
     *
     * Exists because both of that function's wait loops used to have no exit
     * but success: a receiver that stopped accepting or draining bursts left
     * the replay thread sleeping in 4ms increments for the life of the
     * process, with the sink still open. See file_replay.cpp.
     */
    external fun nativeStopFileReplay()
}
