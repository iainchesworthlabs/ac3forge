package com.ac3forge.shield

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Device-free coverage for NativeBridge's JNI surface (roadmap VX18b).
 * docs/platforms/android.md's tests/backend/android/ note covers only the
 * pure C++-side logic (burst sizing, carrier rate, render-device
 * construction) on the desktop ctest suite; this is the other half - the
 * actual Kotlin<->JNI<->C++ round trip, on a real Android runtime
 * (connectedDebugAndroidTest, see _build.yml's build-android job), exercised
 * the same way MainActivity itself does but with no receiver attached and
 * no encode loop ever actually streaming.
 *
 * Every case here holds on an emulator with no HDMI/S-PDIF receiver:
 * live_cursor.cpp's LiveCursorState is a function-local static, so every
 * query below is safe before nativeStartLiveCursor has ever run, and
 * PassthroughSink::start() fails fast (see
 * NativeBridge.nativeIsLiveCursorRunning's own doc comment) rather than
 * hanging when nothing accepts the format.
 */
@RunWith(AndroidJUnit4::class)
class NativeBridgeInstrumentedTest {

    private val bridge = PassthroughBridge()

    @Before
    fun registerBridge() {
        // Required before nativeProbePassthroughCapabilities/
        // nativeStartLiveCursor/nativePlayEac3File - see NativeBridge's own
        // doc comment, which also says it is safe to call again per test.
        NativeBridge.registerPassthroughBridge(bridge)
    }

    @After
    fun stopLiveCursor() {
        // Always joins the worker thread if one was started this test -
        // nativeStopLiveCursor is documented safe to call while not
        // running - so no test here can leak a native thread into the next.
        NativeBridge.nativeStopLiveCursor()
    }

    @Test
    fun versionStringProvesTheRealLibraryLinked() {
        val version = NativeBridge.nativeVersionString()
        assertTrue("expected a non-empty version string, got '$version'", version.isNotEmpty())
    }

    @Test
    fun probeCapabilitiesRoundTripsWithoutThrowing() {
        val report = NativeBridge.nativeProbePassthroughCapabilities()
        assertTrue(report.contains("passthrough backend compiled in"))
    }

    @Test
    fun objectStateStartsWithOnlyTheLeadObjectSelected() {
        // kObjects (3: 1 interactive + 2 ambient - live_cursor.cpp) * 4
        // floats each (x, y, z, isSelected).
        val state = NativeBridge.nativeGetObjectState()
        assertEquals(12, state.size)
        assertEquals(1.0f, state[3], 0.0f)   // object 0 (the lead) starts selected
        assertEquals(0.0f, state[7], 0.0f)   // ambient object 1
        assertEquals(0.0f, state[11], 0.0f)  // ambient object 2
    }

    @Test
    fun cyclingSelectionStaysOnTheOnlyInteractiveObject() {
        // kInteractiveObjects is 1 (live_cursor.cpp) - cycle_selected()
        // computes modulo that, so this always lands back on 0 rather than
        // advancing - a real, slightly surprising contract worth pinning
        // down explicitly rather than assuming "cycle" means "advance."
        assertEquals(0, NativeBridge.nativeCycleSelectedObject())
        assertEquals(0, NativeBridge.nativeCycleSelectedObject())
    }

    @Test
    fun deflectAndSnapDoNotThrowWithNoEncodeLoopRunning() {
        NativeBridge.nativeDeflectSelectedObject(0.1f, -0.1f, 0.0f)
        NativeBridge.nativeSnapSelectedToCourse()
    }

    @Test
    fun channelLevelsAreSixValuesClampedToUnitRange() {
        val levels = NativeBridge.nativeGetChannelLevels()
        assertEquals(6, levels.size)  // L, C, R, Ls, Rs, LFE (AC-3 coded order)
        for (level in levels) {
            assertTrue("level $level out of [0,1]", level in 0.0f..1.0f)
        }
    }

    @Test
    fun futureTrajectoryReturnsTheRequestedSampleCount() {
        val samples = 5
        val trajectory = NativeBridge.nativeGetFutureLeadTrajectory(1.0f, samples)
        assertEquals(samples * 3, trajectory.size)  // (x, y, z) per sample
    }

    @Test
    fun ambientMuteToggleDoesNotThrow() {
        NativeBridge.nativeSetAmbientMuted(true)
        NativeBridge.nativeSetAmbientMuted(false)
    }

    @Test
    fun underrunCountIsReadableAndNeverNegative() {
        // Not asserted at exactly 0: StreamStats::underruns is a native
        // process-wide static (function-local static, like
        // LiveCursorState), so JUnit4's unspecified method execution order
        // means a run/stop test elsewhere in this class may have already
        // touched it - this only pins down that the counter itself reads
        // back sanely, not a fresh-process value.
        assertTrue(NativeBridge.nativeGetUnderrunCount() >= 0L)
    }

    @Test
    fun setAssetManagerDoesNotThrow() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        NativeBridge.nativeSetAssetManager(context.assets)
    }

    @Test
    fun startAndStopLiveCursorRoundTripCleanlyWithNoReceiver() {
        // No HDMI/S-PDIF receiver on an emulator, so PassthroughSink::start()
        // is expected to fail fast on its own - this asserts the round trip
        // completes and leaves the worker thread properly joined (bounded by
        // this test's own execution, so an actual hang fails it), not the
        // exact fail-fast timing, which nothing here can observe directly.
        assertTrue(NativeBridge.nativeStartLiveCursor())
        NativeBridge.nativeStopLiveCursor()
        assertFalse(NativeBridge.nativeIsLiveCursorRunning())
    }
}
