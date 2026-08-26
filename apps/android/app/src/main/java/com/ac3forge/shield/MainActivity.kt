package com.ac3forge.shield

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.media.AudioManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.text.Spannable
import android.text.SpannableString
import android.text.style.AbsoluteSizeSpan
import android.text.style.ForegroundColorSpan
import android.text.style.StyleSpan
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.content.ContextCompat
import kotlin.concurrent.thread

private const val TAG = "ShieldAtmosDemo"

// How long the first-launch orientation cue stays up before auto-dismissing
// if nobody touches a control - long enough to read twice, short enough not
// to feel stuck. Dismissed immediately on first real input either way (see
// MainActivity.onUserInputActivity).
private const val ORIENTATION_CUE_MS = 5000L

// How long with no input before the idle/attract prompt appears - a demo
// left alone (between visitors at a booth, say) should invite the next
// person rather than just sit there having already made its point.
private const val IDLE_PROMPT_MS = 14000L
private const val IDLE_CHECK_INTERVAL_MS = 2000L

// How long the overlay cue's fade in/out takes - fast enough not to feel
// laggy, slow enough to read as an intentional transition rather than a
// jarring pop.
private const val OVERLAY_FADE_MS = 220L

// Fallback poll interval for reconcileReceiverState() - ACTION_HDMI_AUDIO_PLUG
// (registered in onResume) is the fast path, but it isn't guaranteed on
// every real AVR power-off/on (some receivers don't change their reported
// EDID/HPD state on standby, which is exactly the gap that used to require
// force-restarting the app). A slow periodic re-check closes that gap
// without needing to trust any one receiver's own broadcast behavior.
private const val RECEIVER_CHECK_INTERVAL_MS = 2500L

// How long reconcileReceiverState() waits after calling nativeStartLiveCursor()
// before it's willing to probe passthroughBridge.isDirectPlaybackSupported()
// again - that call blocks indefinitely (confirmed on real hardware) not
// only against an already-playing direct AudioTrack but also against ONE
// STILL BEING OPENED (AudioTrack.Builder().build().play() runs on the
// encode loop's own background thread, asynchronously - nativeStartLiveCursor()
// returns as soon as that thread is merely SPAWNED, well before its
// AudioTrack actually opens). Long enough for that open (or a fast in-thread
// failure) to resolve one way or the other; see reconcileReceiverState's own
// comment for the rest of this state machine.
private const val START_ATTEMPT_GRACE_MS = 3000L

// How long a first BACK press stays "armed" - long enough to read the
// confirmation and press again deliberately, short enough that a BACK press
// minutes later isn't silently treated as the second half of a pair.
private const val BACK_CONFIRM_MS = 3000L

private const val WAITING_HEADLINE = "Waiting for receiver…"
private const val WAITING_DETAIL =
    "Turn on your AVR/receiver and select this HDMI input.\n" +
        "The demo starts on its own once it's detected - no need to restart the app."

// Shown between asking for the encode loop and the loop confirming it is up.
// Short-lived on a healthy route; if it persists, the AudioTrack is failing to
// open even though the route said it would accept the format - a materially
// different problem from "no receiver", and one that should not look the same.
private const val STARTING_HEADLINE = "Starting…"
private const val STARTING_DETAIL =
    "The receiver accepted the format - opening the audio stream…"

// The E-AC3 IEC61937 carrier rate for this app's fixed 48kHz content rate
// (live_cursor.cpp's kSampleRate) - carrier = 4x content for E-AC3, per
// android_audio::carrier_rate() on the native side (see
// PassthroughBridge.isDirectPlaybackSupported's own doc comment for why
// that relationship is computed once in C++ and just duplicated as a
// constant here rather than exposed over JNI for a single call site).
private const val EAC3_CARRIER_RATE_HZ = 192000

/**
 * Loads the native library, registers the [PassthroughBridge], runs the
 * HDMI capability probe, starts the encode loop (live_cursor.cpp), wires
 * Shield Controller/remote input through [InputController], and shows the
 * live object positions via [RoomView] - the startup/capability report stays
 * as a one-shot logcat entry plus a small on-screen control-hints overlay
 * rather than replacing the room view, since it doesn't change frame to
 * frame the way the room view does.
 *
 * Visual chrome (title/hints bars, the overlay cue) is built by hand here
 * rather than from an XML layout/theme - see [Theme]'s own comment for why
 * one shared palette file exists to keep it all consistent with RoomView's
 * and ChannelMeterView's own Canvas-drawn cards.
 *
 * Diagnostic mode: `am start ... --es play_file /sdcard/Download/whatever.ec3`
 * skips the live cursor/object demo entirely and instead streams that real,
 * already-encoded file through file_replay.cpp's PassthroughSink path - see
 * NativeBridge.nativePlayEac3File's doc comment for why this exists.
 */
class MainActivity : Activity() {
    private val passthroughBridge = PassthroughBridge()
    private val inputController = InputController()
    private val mainHandler = Handler(Looper.getMainLooper())

    // The single overlay banner shared by the first-launch orientation cue
    // and the idle/attract prompt (items 3/5 of the post-feedback demo
    // punch list) - the two are mutually exclusive by construction
    // (orientationCueShowing gates the idle checker below), so one TextView
    // is enough rather than two views fighting over the same screen space.
    private lateinit var overlayCue: TextView
    private var orientationCueShowing = false
    private var lastInputAtMs = 0L

    private val orientationCueTimeout = Runnable { hideOrientationCue() }

    // Persistent (no auto-dismiss) full-screen interstitial shown whenever
    // the current HDMI route doesn't accept E-AC3 passthrough right now -
    // see reconcileReceiverState() for what drives it. Not built at all in
    // diagnostic (play_file) mode - guard on ::waitingOverlay.isInitialized
    // wherever this feature's other members get touched, same pattern
    // already used for overlayCue.
    private lateinit var waitingOverlay: View
    private lateinit var waitingHeadline: TextView
    private lateinit var waitingDetail: TextView
    private lateinit var waitingCapability: TextView

    /**
     * Three states, not two.
     *
     * The overlay used to clear on [PassthroughBridge.isDirectPlaybackSupported]
     * alone - "this route could accept E-AC-3" - which is a different claim
     * from "audio is flowing". If `PassthroughSink::start()` then failed, the
     * loop's thread exited immediately, the next reconcile still found the
     * route capable, the old setReceiverReady(true) short-circuited as
     * no-change, and the user got a fully-drawn dashboard over permanent
     * silence. Worse, before the first encode frame every object is at its
     * default position, so all three dots sit stacked at the origin: a
     * plausible-looking picture, not an obviously broken one.
     *
     * READY now means `nativeIsLiveCursorRunning()`, which becomes true only
     * after the sink actually opened. STARTING covers the gap between asking
     * and knowing, so a slow AVR handshake reads as progress rather than as
     * either a lie or a stall.
     */
    private enum class ReceiverState { WAITING, STARTING, READY }

    private var receiverState = ReceiverState.WAITING
    // The first-launch orientation cue used to show unconditionally at the
    // end of onCreate; now it shows the first time the receiver actually
    // becomes ready (which may be immediately, or after some waiting) - this
    // flag is what keeps it a ONE-TIME thing rather than replaying on every
    // waiting->ready recovery.
    private var hasShownOrientationCueOnce = false
    // The underrun count as of the last reconcileReceiverState() call while
    // the encode loop was running - see that function's own comment on why
    // this, not a capability re-probe, is what detects a receiver
    // disappearing mid-stream.
    private var lastSeenUnderrunCount = 0L
    // Set right after calling nativeStartLiveCursor(), cleared once
    // nativeIsLiveCursorRunning() confirms it (or START_ATTEMPT_GRACE_MS
    // elapses without that happening) - see reconcileReceiverState() and
    // START_ATTEMPT_GRACE_MS's own comments for why this exists at all.
    private var startAttemptPending = false
    private var startAttemptAtMs = 0L

    // Set immediately before launching one of this app's OWN Activities, so
    // onStop can tell "the user left the demo" from "the About screen came
    // forward" and avoid tearing down a locked AVR connection for the latter.
    // Cleared by the onStop it was set for.
    private var launchingOwnActivity = false

    // BACK-to-exit confirmation. The hints bar promises "Press any button to
    // take control", and BACK is the one button that instead ends the demo
    // mid-sentence - on a Shield remote it sits directly under the D-pad. A
    // second press within this window still exits, so nobody is trapped.
    private var backPressedAtMs = 0L
    // Gates the idle checker off the shared overlay while the BACK
    // confirmation owns it, exactly as orientationCueShowing already does -
    // otherwise the next idle tick (<=2s away) either hides the confirmation
    // or overwrites it with the attract prompt.
    private var backConfirmShowing = false

    // Whether startFileReplay's worker is in flight - see onDestroy.
    @Volatile
    private var fileReplayRunning = false

    private val backConfirmTimeout = Runnable {
        backConfirmShowing = false
        backPressedAtMs = 0L
        hideOverlayCue()
    }

    // Fast path: the system broadcasts this whenever the HDMI audio route's
    // capabilities change (receiver on/off, input switched, EDID
    // renegotiated) - see AudioManager.ACTION_HDMI_AUDIO_PLUG's own
    // documentation. Registered in onResume, unregistered in onPause.
    private val hdmiPlugReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            Log.i(TAG, "ACTION_HDMI_AUDIO_PLUG received - re-checking receiver capability")
            reconcileReceiverState()
            // The broadcast means the route's capabilities changed, which is
            // exactly when the advertised-capability line is stale.
            refreshCapabilityLine()
        }
    }

    // Slow fallback path - see RECEIVER_CHECK_INTERVAL_MS's own comment for
    // why the broadcast above can't be trusted alone.
    private val receiverChecker = object : Runnable {
        override fun run() {
            reconcileReceiverState()
            mainHandler.postDelayed(this, RECEIVER_CHECK_INTERVAL_MS)
        }
    }

    // Polls rather than reacts to "input stopped" (there is no such event -
    // see InputController's own comment on why release-to-decay works the
    // same way): checks elapsed idle time on a slow, cheap timer and
    // shows/hides the attract prompt accordingly. Reposts itself for as long
    // as the Activity is resumed (started in onResume, cancelled in
    // onPause).
    private val idleChecker = object : Runnable {
        override fun run() {
            // Diagnostic-mode (play_file) returns from onCreate before
            // overlayCue is ever built - guard rather than crash, since
            // onResume/onPause still run normally in that mode too.
            if (!::overlayCue.isInitialized) return
            if (!orientationCueShowing && !backConfirmShowing) {
                val idleMs = SystemClock.elapsedRealtime() - lastInputAtMs
                if (idleMs >= IDLE_PROMPT_MS && overlayCue.visibility != View.VISIBLE) {
                    setOverlayCueText("Press any button to take control")
                    showOverlayCue()
                } else if (idleMs < IDLE_PROMPT_MS && overlayCue.visibility == View.VISIBLE) {
                    hideOverlayCue()
                }
            }
            mainHandler.postDelayed(this, IDLE_CHECK_INTERVAL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // A live demo that is only interesting while it is on screen, on a TV
        // that will otherwise dim and sleep underneath it - and this app
        // deliberately fires an attract prompt after 14s of no input (see
        // IDLE_PROMPT_MS), which is exactly the state the screen saver would
        // otherwise interrupt.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        // Nothing below this point works without the native library, and
        // limping on produces a fully-drawn dashboard reading zeroes rather
        // than an explanation - see NativeBridge.available's own comment for
        // why the failure is caught there rather than thrown from a static
        // initializer.
        if (!NativeBridge.available) {
            Log.e(TAG, "native library unavailable - showing the failure screen instead of the dashboard")
            setContentView(buildNativeFailureView())
            return
        }

        val version = NativeBridge.nativeVersionString()
        Log.i(TAG, "ac3::forge version reported by native library: $version")

        NativeBridge.registerPassthroughBridge(passthroughBridge)
        val capabilities = NativeBridge.nativeProbePassthroughCapabilities()
        Log.i(TAG, "passthrough capability probe:\n$capabilities")

        val playFilePath = intent.getStringExtra("play_file")
        if (playFilePath != null) {
            startFileReplay(playFilePath)
            return
        }

        // Must run before nativeStartLiveCursor - the encode loop loads the
        // bundled lead-voice sample once at startup, on its own thread; a
        // late call would just miss it (see NativeBridge.nativeSetAssetManager's
        // own doc comment - missing this is a graceful fallback, not a crash).
        NativeBridge.nativeSetAssetManager(assets)

        // nativeStartLiveCursor() is NOT called unconditionally here anymore
        // - it's gated on the receiver actually being ready, via
        // reconcileReceiverState() at the end of this method and repeatedly
        // thereafter (HDMI hotplug broadcast + periodic fallback, see
        // onResume). Calling it before confirming that just meant a thread
        // that spawned, failed fast inside PassthroughSink::start(), and
        // sat there having silently done nothing - exactly the "receiver
        // was off at launch, had to force-restart the app" case hands-on
        // feedback flagged.

        val titleBar = buildTitleBar()
        val roomView = RoomView(this@MainActivity, inputController)
        val channelMeter = ChannelMeterView(this@MainActivity)
        val hintsBar = buildHintsBar(channelMeter)

        // overlayCue sits ON TOP of roomView only (a FrameLayout scoped to
        // just this one row), not overlaid across the whole screen the way
        // the old title/hints bug did - it never competes for space with
        // channelMeter/hints below, only with roomView's own content, and
        // roomView keeps drawing normally underneath it since this is a
        // transient banner, not permanently-reserved chrome.
        overlayCue = TextView(this).apply {
            textSize = 26f
            gravity = Gravity.CENTER
            setTextColor(Theme.colorTextPrimary)
            background = GradientDrawable().apply {
                cornerRadius = Theme.cornerRadiusLarge
                setColor(Theme.colorSurface)
                setStroke(3, Theme.colorAccent)
            }
            setPadding(56, 40, 56, 40)
            alpha = 0f
            visibility = View.GONE
            // RoomView's own left ("3D track") column is now a square capped
            // at roughly half this row's width (see its own leftSize split,
            // narrowed considerably from its original ~70-75% share once
            // top-down/elevation moved beside it rather than being squeezed
            // into a narrow stacked column) - capped comfortably under that
            // so this cue, centered across the WHOLE row, can never grow
            // wide enough to visually reach into the top-down/elevation
            // cards on the right, regardless of device aspect ratio or how
            // long a future cue's text gets.
            maxWidth = (resources.displayMetrics.widthPixels * 0.42f).toInt()
        }
        val roomStack = FrameLayout(this).apply {
            addView(
                roomView,
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT,
                ),
            )
            addView(
                overlayCue,
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.CENTER,
                ),
            )
        }

        // Title/hints bars run edge-to-edge (real AV-receiver chrome does
        // too), but the room content sits inset from the screen edges on a
        // plain dark background - contentColumn owns that padding so
        // RoomView's own rounded cards read as panels floating on the
        // background rather than touching the bezel. ChannelMeterView no
        // longer lives in this column at all - see buildHintsBar.
        val pad = Theme.spacingUnit.toInt()
        val contentColumn = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            addView(
                roomStack,
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.MATCH_PARENT),
            )
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Theme.colorBackground)
            addView(titleBar, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT,
            ))
            addView(contentColumn, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f,
            ))
            addView(hintsBar, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT,
            ))
        }

        waitingOverlay = buildWaitingOverlay()
        // A sibling of `root`, not a child - it fully covers the dashboard
        // (including the title bar) while waiting, rather than requiring
        // any coordination with root's own internal row heights. RoomView/
        // ChannelMeterView underneath keep polling harmlessly (they just
        // show static/zeroed content, fully hidden behind this opaque
        // overlay) - simpler than swapping setContentView back and forth.
        setContentView(
            FrameLayout(this).apply {
                addView(root, FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT,
                ))
                addView(waitingOverlay, FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT,
                ))
            },
        )

        inputController.onInputActivity = { onUserInputActivity() }
        lastInputAtMs = SystemClock.elapsedRealtime()
        // Establishes the initial waiting/ready state and starts the encode
        // loop immediately if a receiver is already there - see this
        // function's own comment. The first-launch orientation cue now
        // shows from inside here (on first reaching "ready"), not
        // unconditionally at this point - showing it while still waiting
        // for a receiver would just be confusing.
        reconcileReceiverState()
        // setReceiverState(WAITING) short-circuits on the very first call -
        // WAITING is already receiverState's starting value - so the initial
        // read has to be kicked off explicitly rather than relying on a
        // transition that never happens.
        refreshCapabilityLine()
    }

    // Shown instead of the dashboard when ac3forge_jni did not load at all.
    // Everything this app does is on the other side of that library, so the
    // honest failure is a screen saying so - not a dashboard of zeroes that
    // looks like a receiver problem.
    private fun buildNativeFailureView(): View = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER
        setBackgroundColor(Theme.colorBackground)
        addView(TextView(this@MainActivity).apply {
            text = "ac3forge — Shield Atmos Demo"
            textSize = 18f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Theme.colorTextSecondary)
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(0, 0, 0, 32)
        })
        addView(TextView(this@MainActivity).apply {
            text = "Native library failed to load"
            textSize = 30f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Theme.colorWarn)
            gravity = Gravity.CENTER_HORIZONTAL
        })
        addView(TextView(this@MainActivity).apply {
            text = "libac3forge_jni.so could not be loaded on this device.\n" +
                "Check that the APK's ABI matches (release builds are arm64-v8a only) " +
                "and see logcat, tag $TAG, for the loader's own error."
            textSize = 16f
            setTextColor(Theme.colorTextSecondary)
            gravity = Gravity.CENTER_HORIZONTAL
            setLineSpacing(6f, 1f)
            setPadding(64, 28, 64, 0)
        })
    }

    // Shown whenever the current HDMI route doesn't accept E-AC3 passthrough
    // - AVR off, wrong input selected, or HDMI not yet negotiated. See
    // reconcileReceiverState() for what shows/hides this.
    private fun buildWaitingOverlay(): View = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        gravity = Gravity.CENTER
        setBackgroundColor(Theme.colorBackground)
        addView(TextView(this@MainActivity).apply {
            text = "ac3forge — Shield Atmos Demo"
            textSize = 18f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Theme.colorTextSecondary)
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(0, 0, 0, 32)
        })
        waitingHeadline = TextView(this@MainActivity).apply {
            text = WAITING_HEADLINE
            textSize = 30f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Theme.colorTextPrimary)
            gravity = Gravity.CENTER_HORIZONTAL
        }
        addView(waitingHeadline)
        waitingDetail = TextView(this@MainActivity).apply {
            text = WAITING_DETAIL
            textSize = 16f
            setTextColor(Theme.colorTextSecondary)
            gravity = Gravity.CENTER_HORIZONTAL
            setLineSpacing(6f, 1f)
            setPadding(64, 28, 64, 0)
        }
        addView(waitingDetail)
        // What the route itself advertises, as opposed to the single bit
        // isDirectPlaybackSupported returns - see CapabilityProbe. This is the
        // line that tells "the AVR is off" apart from "the AVR is on and does
        // not do E-AC-3", which the old screen could not distinguish.
        waitingCapability = TextView(this@MainActivity).apply {
            text = ""
            textSize = 15f
            setTextColor(Theme.colorAccent)
            gravity = Gravity.CENTER_HORIZONTAL
            setLineSpacing(5f, 1f)
            setPadding(64, 26, 64, 0)
        }
        addView(waitingCapability)
        // VISIBLE by default, matching receiverState's own default (WAITING)
        // - setReceiverState() only touches this view on an actual CHANGE
        // (`state == receiverState` short-circuits otherwise), so if this
        // started GONE while receiverState started WAITING, a capability
        // check that finds "not ready" on the very first call (WAITING ->
        // WAITING, no change) would never flip it on at all - confirmed on a
        // real device: the full dashboard rendered instead of the waiting
        // screen despite no receiver being capable.
        visibility = View.VISIBLE
    }

    // The core of this feature: reconciles "does the current HDMI route
    // accept E-AC3 passthrough right now" against "is the encode loop
    // actually running," and corrects any mismatch - starting the loop once
    // a receiver shows up, stopping it cleanly if one goes away mid-session,
    // and retrying a start that silently failed the first time (see
    // NativeBridge.nativeIsLiveCursorRunning's own comment on why that's a
    // real, distinct case). Called once at the end of onCreate, again in
    // onResume, on every ACTION_HDMI_AUDIO_PLUG broadcast, and on
    // receiverChecker's slow periodic fallback.
    private fun reconcileReceiverState() {
        if (!::waitingOverlay.isInitialized) return  // diagnostic (play_file) mode never builds this

        val running = try {
            NativeBridge.nativeIsLiveCursorRunning()
        } catch (e: UnsatisfiedLinkError) {
            false
        }

        if (running) {
            // Already streaming - deliberately does NOT call
            // passthroughBridge.isDirectPlaybackSupported() here.
            // AudioTrack.isDirectPlaybackSupported() BLOCKS INDEFINITELY
            // when called while a direct/exclusive AudioTrack is already
            // open and playing on the same route - confirmed hanging on
            // real hardware (the whole Activity got stuck on its splash
            // screen forever, main thread idle, no exception, while the
            // encode loop kept streaming happily underneath) - almost
            // certainly audio-policy-manager lock contention with the
            // encode loop's own AudioTrack, not a probe bug. A rising
            // underrun count is used instead - see
            // NativeBridge.nativeGetUnderrunCount's own comment - a purely
            // numeric signal that never has to ask Android the same
            // question the actively-playing track is already answering by
            // existing.
            startAttemptPending = false
            val underruns = try {
                NativeBridge.nativeGetUnderrunCount()
            } catch (e: UnsatisfiedLinkError) {
                0L
            }
            if (underruns > lastSeenUnderrunCount) {
                Log.w(TAG, "underruns climbing ($lastSeenUnderrunCount -> $underruns) - " +
                    "receiver likely gone, stopping the encode loop")
                NativeBridge.nativeStopLiveCursor()
                lastSeenUnderrunCount = 0L
                setReceiverState(ReceiverState.WAITING)
                return
            }
            lastSeenUnderrunCount = underruns
            setReceiverState(ReceiverState.READY)
            return
        }

        // Not confirmed running yet - but if a start attempt is still
        // within its grace period, the encode loop's own background thread
        // may right now be in the middle of AudioTrack.Builder().build().play()
        // for THIS attempt - and that also blocks a concurrent
        // isDirectPlaybackSupported() call, same as an already-playing
        // track (confirmed on real hardware: calling reconcileReceiverState()
        // again from onResume, moments after onCreate's own start attempt,
        // hung the exact same way). Just wait for `running` to catch up
        // instead of probing again.
        if (startAttemptPending) {
            if (SystemClock.elapsedRealtime() - startAttemptAtMs < START_ATTEMPT_GRACE_MS) {
                // Asked, not yet confirmed. Deliberately NOT reported as
                // ready: nativeStartLiveCursor returns JNI_TRUE
                // unconditionally by design - it returns as soon as the worker
                // is spawned - and making it wait for that worker's outcome is
                // exactly the main-thread hang this grace period exists to
                // avoid.
                setReceiverState(ReceiverState.STARTING)
                return
            }
            // Grace period elapsed and still not running - the attempt
            // genuinely failed (not just slow); fall through and probe
            // fresh rather than waiting forever.
            startAttemptPending = false
        }

        // Not running, and no start attempt currently in flight - no
        // AudioTrack of ours is open or opening right now, so this is the
        // one place in this function it's actually safe to probe capability.
        val capable = passthroughBridge.isDirectPlaybackSupported(EAC3_CARRIER_RATE_HZ, true)
        if (capable) {
            NativeBridge.nativeStartLiveCursor()
            startAttemptPending = true
            startAttemptAtMs = SystemClock.elapsedRealtime()
            lastSeenUnderrunCount = 0L
            // Capable, and asked - but nothing is flowing until the worker
            // says so. This is the case that used to clear the overlay
            // outright and leave a silent dashboard behind it.
            setReceiverState(ReceiverState.STARTING)
        } else {
            setReceiverState(ReceiverState.WAITING)
        }
    }

    /**
     * Refreshes the advertised-capability line. Off the main thread (see
     * [CapabilityProbe]) with the result posted back, and only while the
     * overlay is actually up: nothing reads it while streaming, and the fewer
     * questions asked of the audio route during a live direct track, the
     * better.
     */
    private fun refreshCapabilityLine() {
        if (!::waitingCapability.isInitialized) return
        CapabilityProbe.probeAsync(applicationContext) { report ->
            mainHandler.post {
                if (::waitingCapability.isInitialized && receiverState != ReceiverState.READY) {
                    waitingCapability.text = report.describe()
                }
            }
        }
    }

    private fun setReceiverState(state: ReceiverState) {
        if (state == receiverState) return
        val wasReady = receiverState == ReceiverState.READY
        receiverState = state
        Log.i(TAG, "receiver state changed: $state")

        val ready = state == ReceiverState.READY
        waitingOverlay.visibility = if (ready) View.GONE else View.VISIBLE
        if (!ready) {
            val starting = state == ReceiverState.STARTING
            waitingHeadline.text = if (starting) STARTING_HEADLINE else WAITING_HEADLINE
            waitingDetail.text = if (starting) STARTING_DETAIL else WAITING_DETAIL
            // Only re-read the route on a real transition into a non-ready
            // state, not on every reconcile tick (one every 2.5s).
            if (wasReady || !starting) {
                refreshCapabilityLine()
            }
        }
        if (ready && !hasShownOrientationCueOnce) {
            hasShownOrientationCueOnce = true
            lastInputAtMs = SystemClock.elapsedRealtime()
            setOverlayCueText(
                "This is the front wall",
                "Up on the stick/D-pad = toward the screen",
            )
            orientationCueShowing = true
            showOverlayCue()
            mainHandler.postDelayed(orientationCueTimeout, ORIENTATION_CUE_MS)
        }
    }

    // Edge-to-edge top bar: a bold primary line plus a small accent-colored
    // caption underneath (the kind of two-tier title a real AVR's own front
    // panel display uses), separated from the content below it by a hairline
    // divider rather than relying on a flat color change alone to read as
    // "chrome, not content."
    private fun buildTitleBar(): View = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setBackgroundColor(Theme.colorSurface)
        addView(TextView(this@MainActivity).apply {
            text = "ac3forge — Shield Atmos Demo"
            textSize = 24f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Theme.colorTextPrimary)
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(24, 20, 24, 2)
        })
        addView(TextView(this@MainActivity).apply {
            text = "LIVE DOLBY ATMOS OBJECT DEMO"
            textSize = 14f
            letterSpacing = 0.14f
            setTextColor(Theme.colorAccent)
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(24, 0, 24, 16)
        })
        addView(dividerView(), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 2))
    }

    // Edge-to-edge bottom bar: the speaker-activity meter moved down here,
    // beside a shrunk control-hints column, rather than its own full-width
    // row above this one - per hands-on feedback, that row's whole purpose
    // was to give the 3D track (and now the side-by-side top-down/elevation
    // panels) back the vertical space the meter used to take, while keeping
    // the meter itself visible, just smaller and to the side.
    private fun buildHintsBar(channelMeter: View): View = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setBackgroundColor(Theme.colorSurface)
        addView(dividerView(), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 2))
        addView(
            LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(
                    channelMeter,
                    LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 2f),
                )
                addView(TextView(this@MainActivity).apply {
                    text = "Stick/D-pad: push the lead object off its course, it drifts back " +
                        "when you let go   •   Right stick / L1+R1: height   •   Press A/center: " +
                        "D-pad up/down toggles between depth and height   •   Pause: isolate the " +
                        "lead   •   Play: bring the ambient tones back   •   Info: About\n" +
                        "● lead (yours to push around)   ● ● two ambient tones, always on their " +
                        "own course"
                    textSize = 12f
                    gravity = Gravity.CENTER
                    setTextColor(Theme.colorTextSecondary)
                    setLineSpacing(5f, 1f)
                    setPadding(20, 14, 24, 14)
                }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 3f))
            },
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT),
        )
    }

    private fun dividerView(): View = View(this).apply {
        setBackgroundColor(Theme.colorSurfaceBorder)
    }

    // Builds overlayCue's text as a two-tier Spannable (a larger bold
    // headline, an optional smaller dimmer subtitle) rather than plain text
    // - matches the title bar's own headline/caption pairing so the cue
    // reads as part of the same design language, not a plain system toast.
    private fun setOverlayCueText(headline: String, subtitle: String? = null) {
        val full = if (subtitle != null) "$headline\n$subtitle" else headline
        val spannable = SpannableString(full)
        spannable.setSpan(StyleSpan(Typeface.BOLD), 0, headline.length, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE)
        spannable.setSpan(AbsoluteSizeSpan(30, true), 0, headline.length, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE)
        if (subtitle != null) {
            val start = headline.length + 1
            spannable.setSpan(ForegroundColorSpan(Theme.colorTextSecondary), start, full.length, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE)
            spannable.setSpan(AbsoluteSizeSpan(22, true), start, full.length, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        overlayCue.text = spannable
    }

    private fun showOverlayCue() {
        overlayCue.visibility = View.VISIBLE
        overlayCue.animate().cancel()
        overlayCue.animate().alpha(1f).setDuration(OVERLAY_FADE_MS).start()
    }

    private fun hideOverlayCue() {
        overlayCue.animate().cancel()
        overlayCue.animate().alpha(0f).setDuration(OVERLAY_FADE_MS).withEndAction {
            overlayCue.visibility = View.GONE
        }.start()
    }

    // Dismisses whichever of the two overlayCue uses is currently up (first
    // real input always wins over the auto-dismiss timer) and resets the
    // idle clock the attract prompt watches. Called from
    // InputController.onInputActivity - see that field's own comment for why
    // this class, not InputController, owns the actual UI reaction.
    private fun onUserInputActivity() {
        lastInputAtMs = SystemClock.elapsedRealtime()
        if (orientationCueShowing) {
            hideOrientationCue()
        } else if (overlayCue.visibility == View.VISIBLE) {
            hideOverlayCue()
        }
    }

    private fun hideOrientationCue() {
        orientationCueShowing = false
        hideOverlayCue()
        mainHandler.removeCallbacks(orientationCueTimeout)
    }

    // Diagnostic-mode path: no live cursor, no room view, no input handling -
    // just a status line while file_replay.cpp streams the file on a
    // background thread (nativePlayEac3File blocks until the whole file has
    // drained, so it must never run on the main/UI thread).
    private fun startFileReplay(path: String) {
        val status = TextView(this).apply {
            text = "Diagnostic file replay\n\n$path\n\nstreaming… (see logcat " +
                "tag ac3forge.shield.file_replay)"
            textSize = 20f
            gravity = Gravity.CENTER
            setTextColor(Theme.colorTextPrimary)
            setBackgroundColor(Theme.colorBackground)
        }
        setContentView(status)
        fileReplayRunning = true
        thread(name = "file-replay") {
            val ok = try {
                NativeBridge.nativePlayEac3File(path)
            } catch (e: UnsatisfiedLinkError) {
                Log.e(TAG, "file replay failed to start", e)
                false
            } finally {
                fileReplayRunning = false
            }
            Log.i(TAG, "file replay finished: ok=$ok")
            runOnUiThread {
                status.text = "Diagnostic file replay\n\n$path\n\n" +
                    (if (ok) "done - see logcat for burst stats" else "FAILED - see logcat")
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // The native-failure screen has no room view, no encode loop and no
        // JNI to deflect anything in - starting the input ticker there would
        // drive nativeDeflectSelectedObject straight into the
        // UnsatisfiedLinkError the failure screen exists to avoid.
        if (!NativeBridge.available) return
        inputController.start()
        lastInputAtMs = SystemClock.elapsedRealtime()
        mainHandler.postDelayed(idleChecker, IDLE_CHECK_INTERVAL_MS)

        if (::waitingOverlay.isInitialized) {
            // RECEIVER_NOT_EXPORTED: only the system sends this broadcast,
            // no other app has any legitimate reason to - matches this
            // app's other exported=false posture (see AndroidManifest.xml).
            ContextCompat.registerReceiver(
                this,
                hdmiPlugReceiver,
                IntentFilter(AudioManager.ACTION_HDMI_AUDIO_PLUG),
                ContextCompat.RECEIVER_NOT_EXPORTED,
            )
            // Catches anything that changed while paused (screen off,
            // launcher in front, etc.) immediately on return, rather than
            // waiting for the next periodic tick.
            reconcileReceiverState()
            mainHandler.postDelayed(receiverChecker, RECEIVER_CHECK_INTERVAL_MS)
        }
    }

    override fun onPause() {
        if (!NativeBridge.available) {
            super.onPause()
            return
        }
        inputController.stop()
        mainHandler.removeCallbacks(idleChecker)
        if (::waitingOverlay.isInitialized) {
            mainHandler.removeCallbacks(receiverChecker)
            unregisterReceiver(hdmiPlugReceiver)
        }
        super.onPause()
    }

    /**
     * Where the encode loop actually stops when the demo leaves the screen.
     *
     * It used to stop only in [onDestroy], so pressing HOME left a cached
     * process pushing E-AC-3 bursts into the AVR indefinitely with no UI, no
     * notification and no way to stop it short of force-stopping the app -
     * the receiver stays locked to a bitstream nothing on screen accounts for.
     *
     * `onStop`, not `onPause`: [AboutActivity] is a full-screen Activity of
     * this same app, and pausing for it should not tear down the AudioTrack
     * and force the receiver to re-lock (a visible dropout, and a bounce back
     * through the "Waiting for receiver…" interstitial) just because someone
     * read the About screen for four seconds. `launchingOwnActivity` carries
     * that intent across the stop, and `isChangingConfigurations` covers the
     * recreation case for the same reason.
     */
    override fun onStop() {
        if (::waitingOverlay.isInitialized && !isChangingConfigurations && !launchingOwnActivity) {
            Log.i(TAG, "leaving the foreground - stopping the encode loop")
            NativeBridge.nativeStopLiveCursor()
            // The next reconcileReceiverState() (onResume) probes fresh and
            // restarts. Reset the state machine so it does, rather than
            // leaving it believing a receiver is still streaming.
            startAttemptPending = false
            lastSeenUnderrunCount = 0L
            setReceiverState(ReceiverState.WAITING)
        }
        launchingOwnActivity = false
        super.onStop()
    }

    override fun onDestroy() {
        if (NativeBridge.available) {
            NativeBridge.nativeStopLiveCursor()
            // Diagnostic (play_file) mode: the replay runs on its own thread
            // and, before it had a stop flag, outlived the Activity that
            // started it. Non-blocking - it ends at its next wait point.
            if (fileReplayRunning) {
                NativeBridge.nativeStopFileReplay()
            }
        }
        passthroughBridge.close()
        super.onDestroy()
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (inputController.onGenericMotionEvent(event)) {
            return true
        }
        return super.onGenericMotionEvent(event)
    }

    override fun onKeyDown(keyCode: Int, keyEvent: KeyEvent?): Boolean {
        // The TV remote's dedicated "info" button - checked before
        // inputController (which doesn't recognize this code anyway) since
        // this is a MainActivity-level UI reaction, the same reasoning
        // onUserInputActivity's own comment gives for keeping UI reactions
        // here rather than inside InputController.
        if (keyCode == KeyEvent.KEYCODE_INFO) {
            launchingOwnActivity = true
            startActivity(Intent(this, AboutActivity::class.java))
            return true
        }
        // See backPressedAtMs. Deliberately a confirmation rather than a
        // block: a demo nobody can leave is worse than one that exits by
        // accident.
        if (keyCode == KeyEvent.KEYCODE_BACK && ::overlayCue.isInitialized) {
            val now = SystemClock.elapsedRealtime()
            if (now - backPressedAtMs > BACK_CONFIRM_MS) {
                backPressedAtMs = now
                backConfirmShowing = true
                setOverlayCueText("Press BACK again to exit the demo")
                showOverlayCue()
                mainHandler.removeCallbacks(backConfirmTimeout)
                mainHandler.postDelayed(backConfirmTimeout, BACK_CONFIRM_MS)
                return true
            }
            mainHandler.removeCallbacks(backConfirmTimeout)
            backConfirmShowing = false
        }
        if (keyEvent != null && inputController.onKeyDown(keyCode, keyEvent)) {
            return true
        }
        return super.onKeyDown(keyCode, keyEvent)
    }

    override fun onKeyUp(keyCode: Int, keyEvent: KeyEvent?): Boolean {
        if (inputController.onKeyUp(keyCode)) {
            return true
        }
        return super.onKeyUp(keyCode, keyEvent)
    }

    override fun onKeyLongPress(keyCode: Int, keyEvent: KeyEvent?): Boolean {
        if (inputController.onKeyLongPress(keyCode)) {
            return true
        }
        return super.onKeyLongPress(keyCode, keyEvent)
    }
}
