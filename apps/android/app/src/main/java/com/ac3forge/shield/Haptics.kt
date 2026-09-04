package com.ac3forge.shield

import android.os.VibrationEffect
import android.os.Vibrator
import android.util.Log
import android.view.InputDevice
import kotlin.math.sqrt

private const val TAG = "ShieldAtmosDemo"

/**
 * Controller rumble tied to where the object actually is.
 *
 * Height is the hardest thing about this demo to be sure of by ear. Everything
 * else has a second channel of evidence — you can see the dot move left, and
 * you hear it move left. Elevation has only the sound, and a listener who is
 * not certain what they are supposed to be hearing tends to conclude they are
 * not hearing it. A pulse in the hand at the moment the object crosses over
 * the seat is a second channel for exactly the cue that lacks one.
 *
 * Deliberately event-driven rather than continuous: a permanently buzzing
 * controller stops meaning anything within about ten seconds, and it is the
 * fastest way to flatten a wireless pad's battery during a demo.
 */
class Haptics {

    private var lastDeviceId = -1
    private var vibrator: Vibrator? = null
    private var resolvedForDeviceId = -1

    // Previous frame's sampled values, for edge detection. NaN means "no
    // previous frame" — the first sample after a start must not fire a pulse
    // for a threshold the object was already past when we began looking.
    private var lastHeight = Float.NaN
    private var lastDistance = Float.NaN

    /** Called from the input path so rumble goes to the pad actually in use. */
    fun noteDevice(deviceId: Int) {
        if (deviceId != lastDeviceId) {
            lastDeviceId = deviceId
            resolvedForDeviceId = -1  // re-resolve lazily on the next pulse
        }
    }

    fun reset() {
        lastHeight = Float.NaN
        lastDistance = Float.NaN
    }

    /**
     * Samples the lead object's position for this frame and fires a pulse on
     * the crossings worth feeling. `x`/`y` are room fractions in [0,1] about a
     * listener at (0.5, 0.5); `z` is [-1,1] with 0 at ear height.
     */
    fun onLeadPosition(x: Float, y: Float, z: Float) {
        val dx = x - 0.5f
        val dy = y - 0.5f
        val distance = sqrt(dx * dx + dy * dy + z * z)

        // Passing overhead: the cue the ear is least sure of.
        if (!lastHeight.isNaN() && lastHeight < OVERHEAD_Z && z >= OVERHEAD_Z) {
            pulse(OVERHEAD_MS, OVERHEAD_AMPLITUDE)
        }
        // Passing through the listening position, at any height — the other
        // moment worth marking, and the one that makes a flyover land.
        if (!lastDistance.isNaN() && lastDistance > CLOSE_RADIUS && distance <= CLOSE_RADIUS) {
            pulse(CLOSE_MS, CLOSE_AMPLITUDE)
        }
        lastHeight = z
        lastDistance = distance
    }

    private fun pulse(durationMs: Long, amplitude: Int) {
        val vib = resolveVibrator() ?: return
        try {
            vib.vibrate(VibrationEffect.createOneShot(durationMs, amplitude))
        } catch (t: Throwable) {
            // A pad whose vibrator disappears mid-demo must not take the demo
            // with it. Rumble is the most optional thing in this app.
            Log.w(TAG, "vibrate failed", t)
        }
    }

    /**
     * The vibrator belonging to the pad in use, resolved lazily and cached.
     *
     * Deliberately the INPUT DEVICE's vibrator, not the system one: on a TV
     * box the system vibrator is absent or meaningless, and the thing the user
     * is holding is the controller.
     */
    private fun resolveVibrator(): Vibrator? {
        if (resolvedForDeviceId == lastDeviceId) return vibrator
        resolvedForDeviceId = lastDeviceId
        vibrator = try {
            val device = InputDevice.getDevice(lastDeviceId)
            // getVibrator() is the API available at minSdk 26 - getVibratorManager()
            // needs API 31 (see PassthroughBridge's class doc for the same shape of
            // trade-off). The codeql[] array entry is what actually registers with
            // CodeQL's AlertSuppressionAnnotations query - a suppression comment
            // above this line does not, see PassthroughBridge.kt.
            @Suppress("DEPRECATION", "codeql[java/deprecated-call]")
            val v = device?.vibrator
            if (v != null && v.hasVibrator()) v else null
        } catch (t: Throwable) {
            null
        }
        Log.i(TAG, "haptics: device $lastDeviceId vibrator=${vibrator != null}")
        return vibrator
    }

    companion object {
        // VibrationEffect.createOneShot with an explicit amplitude needs API
        // 26, which is this app's own minSdk - so no version guard is needed
        // here, unlike the API-29 passthrough query in PassthroughBridge.

        // Room z is [-1, 1]; 0.55 is comfortably "above you" without firing on
        // the ordinary height bob every scene has.
        private const val OVERHEAD_Z = 0.55f
        private const val OVERHEAD_MS = 55L
        private const val OVERHEAD_AMPLITUDE = 200

        // Close enough to the seat to read as "it went past me", in the same
        // room-fraction units. Lighter than the overhead pulse: this one fires
        // more often, and it is a nudge rather than an announcement.
        private const val CLOSE_RADIUS = 0.22f
        private const val CLOSE_MS = 30L
        private const val CLOSE_AMPLITUDE = 110
    }
}
