package com.ac3forge.shield

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.util.Log

private const val TAG = "ShieldAtmosDemo"

// AudioDeviceInfo.TYPE_HDMI_EARC is API 31 and AudioFormat.ENCODING_E_AC3_JOC
// is API 28, both above this app's minSdk of 26. They are `static final int`
// constants, so the compiler inlines the value and comparing against them is
// safe on any API level - but referencing the platform symbols directly still
// trips lint's NewApi check on a build that has to stay green. Naming the
// values here, with the constant each one mirrors, keeps both properties.
private const val TYPE_HDMI_EARC = 29          // AudioDeviceInfo.TYPE_HDMI_EARC
private const val ENCODING_E_AC3_JOC = 18      // AudioFormat.ENCODING_E_AC3_JOC

/**
 * What the currently-connected HDMI sink actually advertises.
 *
 * The app has always had exactly one question it could ask about the route -
 * `AudioTrack.isDirectPlaybackSupported(...)`, a yes/no for one format - and
 * that call is unusable for anything richer here: it blocks indefinitely
 * against a direct `AudioTrack` that is open or opening on the same route,
 * which is a confirmed-on-hardware Activity hang, not a hypothetical (see
 * `MainActivity.reconcileReceiverState` and `docs/platforms/android.md`).
 *
 * `AudioManager.getDevices` is a different API against a different object: it
 * reports what the *route* enumerates rather than asking the policy manager to
 * validate a track configuration. That is what makes it usable while
 * streaming, and it is what lets the waiting screen say "your receiver
 * advertises DD+ but not JOC" instead of the single bit "not ready".
 *
 * **Still called off the main thread anyway.** Whether `getDevices` contends
 * the same audio-policy lock that `isDirectPlaybackSupported` deadlocks
 * against is unproven - nothing in this project has established it either way
 * - so this is treated as guilty until a real device says otherwise. See
 * [probeAsync].
 */
object CapabilityProbe {

    /**
     * One HDMI output route's advertised capability.
     *
     * [encodingsKnown] is the distinction that matters and the one easy to get
     * wrong: an empty `getEncodings()` array means "this device does not
     * publish a fixed list", **not** "this device supports nothing". Reporting
     * the second as though it were the first would turn a perfectly working
     * receiver into an on-screen accusation.
     */
    data class SinkReport(
        val found: Boolean,
        val routeName: String,
        val encodingNames: List<String>,
        val encodingsKnown: Boolean,
        val maxChannels: Int,
        val supportsEac3: Boolean,
        val supportsJoc: Boolean,
    ) {
        /** One line for the waiting screen. Never blames the receiver for an unknown. */
        fun describe(): String = when {
            !found -> "No HDMI audio route is connected right now."
            !encodingsKnown ->
                "$routeName is connected, but publishes no encoding list - " +
                    "capability can only be told by trying."
            supportsJoc ->
                "$routeName advertises Dolby Atmos (E-AC-3 JOC)" +
                    channelSuffix() + "."
            supportsEac3 ->
                "$routeName advertises Dolby Digital Plus, but not the JOC " +
                    "(Atmos) profile" + channelSuffix() + "."
            else ->
                "$routeName is connected but does not advertise E-AC-3: " +
                    encodingNames.joinToString(", ").ifEmpty { "no recognised encodings" } + "."
        }

        private fun channelSuffix(): String =
            if (maxChannels > 0) ", up to $maxChannels channels" else ""
    }

    val UNKNOWN = SinkReport(
        found = false,
        routeName = "",
        encodingNames = emptyList(),
        encodingsKnown = false,
        maxChannels = 0,
        supportsEac3 = false,
        supportsJoc = false,
    )

    /**
     * Runs [probe] on a short-lived worker and delivers the result to
     * [onResult]. The callback runs on that worker, not the main thread -
     * callers post it where they need it.
     */
    fun probeAsync(context: Context, onResult: (SinkReport) -> Unit) {
        Thread({
            val report = try {
                probe(context)
            } catch (t: Throwable) {
                // A capability read is decoration. It must never be the reason
                // the demo stops - hence Throwable, not a narrower catch.
                Log.w(TAG, "HDMI capability probe failed", t)
                UNKNOWN
            }
            onResult(report)
        }, "hdmi-capability-probe").start()
    }

    /** Blocking. Call from [probeAsync], or from a thread you already own. */
    fun probe(context: Context): SinkReport {
        val audio = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
            ?: return UNKNOWN

        val hdmi = audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull {
            it.type == AudioDeviceInfo.TYPE_HDMI ||
                it.type == AudioDeviceInfo.TYPE_HDMI_ARC ||
                it.type == TYPE_HDMI_EARC
        } ?: return UNKNOWN

        val encodings = hdmi.encodings
        val channelCounts = hdmi.channelCounts
        return SinkReport(
            found = true,
            routeName = routeName(hdmi),
            encodingNames = encodings.map(::encodingName),
            encodingsKnown = encodings.isNotEmpty(),
            maxChannels = channelCounts.maxOrNull() ?: 0,
            supportsEac3 = encodings.any {
                it == AudioFormat.ENCODING_E_AC3 || it == ENCODING_E_AC3_JOC
            },
            supportsJoc = encodings.any { it == ENCODING_E_AC3_JOC },
        )
    }

    private fun routeName(device: AudioDeviceInfo): String {
        val kind = when (device.type) {
            AudioDeviceInfo.TYPE_HDMI_ARC -> "HDMI ARC"
            TYPE_HDMI_EARC -> "HDMI eARC"
            else -> "HDMI"
        }
        val product = device.productName?.toString()?.trim().orEmpty()
        return if (product.isEmpty()) kind else "$product ($kind)"
    }

    // Only the encodings this demo could plausibly care about are named; the
    // rest are reported by number rather than silently dropped, so an
    // unexpected sink still shows something a person can look up.
    private fun encodingName(encoding: Int): String = when (encoding) {
        AudioFormat.ENCODING_PCM_16BIT -> "PCM 16"
        AudioFormat.ENCODING_AC3 -> "AC-3"
        AudioFormat.ENCODING_E_AC3 -> "DD+"
        ENCODING_E_AC3_JOC -> "DD+ JOC (Atmos)"
        AudioFormat.ENCODING_DTS -> "DTS"
        AudioFormat.ENCODING_DTS_HD -> "DTS-HD"
        AudioFormat.ENCODING_DOLBY_TRUEHD -> "TrueHD"
        AudioFormat.ENCODING_IEC61937 -> "IEC 61937"
        else -> "encoding $encoding"
    }
}
