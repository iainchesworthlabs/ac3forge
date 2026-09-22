package com.ac3forge.shield

import android.media.AudioTrack
import androidx.test.ext.junit.runners.AndroidJUnit4
import java.nio.ByteBuffer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Device-free coverage for PassthroughBridge (roadmap VX18b) - a plain
 * Kotlin class, not a JNI singleton (see its own header comment), so this
 * drives real android.media.AudioTrack/AudioFormat calls directly rather
 * than through NativeBridge. NativeBridgeInstrumentedTest covers the JNI
 * round trip; this covers the Java audio API surface underneath it.
 *
 * An emulator's virtual audio HAL has no compressed/direct-passthrough
 * route, so isDirectPlaybackSupported is expected false throughout - every
 * assertion below holds regardless of what it actually returns on a given
 * emulator image, which is the point: these are the "no receiver attached"
 * contract checks (open()/submit()/close() behaving safely with nothing on
 * the other end), not the "the receiver said yes" happy path, which
 * genuinely needs real hardware (see docs/platforms/android.md).
 */
@RunWith(AndroidJUnit4::class)
class PassthroughBridgeInstrumentedTest {

    private val bridge = PassthroughBridge()

    @After
    fun closeBridge() {
        bridge.close()
    }

    @Test
    fun directPlaybackQueryIsDeterministicAndDoesNotThrow() {
        val first = bridge.isDirectPlaybackSupported(192000, eac3 = true)
        val second = bridge.isDirectPlaybackSupported(192000, eac3 = true)
        assertEquals(first, second)
    }

    @Test
    fun pcmSupportQueryDoesNotThrow() {
        bridge.isPcmSupported(48000)
    }

    @Test
    fun openFailsSafelyWithNoDirectRoute() {
        // open() checks isDirectPlaybackSupported before ever building an
        // AudioTrack (its own guard) - on an emulator this is expected
        // false, so open() returns false without throwing.
        val opened = bridge.open(48000, eac3 = false)
        if (!opened) {
            return  // the expected emulator path: nothing to submit to
        }
        // If some emulator image genuinely does support it, the bridge must
        // still behave: submit and close round-trip without throwing.
        val buffer = ByteBuffer.allocateDirect(256)
        bridge.submit(buffer, 256)
    }

    @Test
    fun closeOnAFreshBridgeDoesNotThrow() {
        // No open() call at all - exercises close()'s own early-return guard
        // for "nothing was ever opened."
        bridge.close()
    }

    @Test
    fun submitOnAnUnopenedBridgeReportsInvalidOperation() {
        val buffer = ByteBuffer.allocateDirect(256)
        assertEquals(AudioTrack.ERROR_INVALID_OPERATION, bridge.submit(buffer, 256))
    }
}
