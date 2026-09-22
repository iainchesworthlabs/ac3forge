#include <catch2/catch_test_macros.hpp>

#include <windows.h>
// windows.h must precede the audio headers.
#include <audioclient.h>

#include "windows_support.hpp"

// The Windows backend's pure half, tested with no live audio device: a plain
// Windows CI runner has WASAPI/COM available (they are core OS components,
// not drivers), so the only thing this file cannot exercise is what
// ComScope/make_enumerator actually touch - COM itself, and whatever
// endpoints happen to be attached. What is left, and worth testing on its
// own, is stream_gone(): a pure HRESULT classifier, the same shape
// tests/backend/android/test_android_support.cpp's track_is_dead() is for
// AudioTrack.write()'s return value.
//
// CMake adds this file to the suite only when it selected the windows/
// platform directory, and puts that directory on the include path - the same
// selection tests/backend/alsa, tests/backend/android and tests/backend/macos
// already use for their own backend's internal header.

using ac3::windows_audio::stream_gone;

TEST_CASE("an invalidated or stopped device ends the stream") {
    // The two answers Microsoft's own "Recovering from an Invalid-Device
    // Error" names: the endpoint went away, or the audio service that ran it
    // did. Getting either wrong means a render thread spins forever against
    // a device that is never coming back.
    CHECK(stream_gone(AUDCLNT_E_DEVICE_INVALIDATED));
    CHECK(stream_gone(AUDCLNT_E_SERVICE_NOT_RUNNING));
}

TEST_CASE("success is never mistaken for the stream having gone") {
    CHECK_FALSE(stream_gone(S_OK));
}

TEST_CASE("a driver merely declining the call is not the stream ending") {
    // The distinction this predicate exists to draw: passthrough.cpp calls it
    // around GetCurrentPadding specifically because real exclusive-mode
    // hardware has been seen to refuse that call for reasons that are not
    // "the device is gone" - see windows_support.hpp's own comment. Any other
    // FAILED() HRESULT has to read as "answer withheld", not "stream over",
    // or a driver hiccup would end a stream that is still there.
    CHECK_FALSE(stream_gone(E_FAIL));
    CHECK_FALSE(stream_gone(AUDCLNT_E_BUFFER_TOO_LARGE));
    CHECK_FALSE(stream_gone(AUDCLNT_E_BUFFER_ERROR));
}
