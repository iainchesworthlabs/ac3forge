#pragma once

#include <windows.h>
// windows.h must precede the audio headers.
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <expected>

// The handful of things capture.cpp, device_watcher.cpp, monitor.cpp and
// passthrough.cpp all need from WASAPI/COM, kept in one place so the four
// cannot drift - the same role alsa_support.hpp plays for the ALSA backend
// (see its own header comment), coreaudio_support.hpp for CoreAudio, and
// pipewire_support.hpp for PipeWire.
//
// Before this existed, ComScope and the CLSID_MMDeviceEnumerator/IID byte
// arrays below were hand-copied into each Windows backend file that needed
// them, capture.cpp's own copies being the ones the others' comments pointed
// back to ("same as capture.cpp's") rather than actually sharing. That the
// copies can drift is not hypothetical: monitor.cpp and passthrough.cpp each
// grew their own "the render device went away under me" detection
// independently, on their own schedules, and disagreed on which HRESULT
// codes end the stream - see stream_gone()'s own comment below for what
// that disagreement is and why part of it is intentional rather than a bug.
//
// spatial.cpp is not wired to this header yet. It duplicates ComScope and
// the two enumerator constants the same way the other four once did, and its
// render loop does not check for device loss at all today - see
// stream_gone()'s own comment for the shape that would need.

namespace ac3::windows_audio {

// COM lifetime for one thread. WASAPI is apartment-sensitive, so every thread
// that touches an interface initialises and uninitialises its own.
class ComScope {
public:
    ComScope() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    [[nodiscard]] bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

// The class and interface identifiers spelled out rather than taken from
// __uuidof, for the reason every file that used to define these locally
// already noted: the SDK declares these but ships no import library that
// defines them as linkable symbols, and __uuidof is an MSVC extension clang
// rejects under -Wpedantic. Values are the DECLSPEC_UUID / MIDL_INTERFACE
// strings in mmdeviceapi.h and audioclient.h.
//
// Only the ones genuinely identical across two or more of this directory's
// files live here. kIidAudioCaptureClient, kIidActivateCompletionHandler and
// kIidAgileObject (capture.cpp's process-loopback activation),
// kIidMmNotificationClient (device_watcher.cpp's listener registration) and
// spatial.cpp's ISpatialAudioClient/ISpatialAudioObjectRenderStream ids each
// have exactly one caller and stay there.
constexpr CLSID kClsidMmDeviceEnumerator = {  // {bcde0395-e52f-467c-8e3d-c4579291692e}
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
constexpr IID kIidMmDeviceEnumerator = {  // {a95664d2-9614-4f35-a746-de8db63617e6}
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
constexpr IID kIidAudioClient = {  // {1cb9ad4c-dbfa-4c32-b178-c2f568a703b2}
    0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
constexpr IID kIidAudioRenderClient = {  // {f294acfc-3146-4483-a7bf-addca7c260e2}
    0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
constexpr IID kIidUnknown = {  // {00000000-0000-0000-c000-000000000046}
    0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// CoCreateInstance for the one COM class every backend file starts from.
// Generic over the caller's own error enum so a call site stays a plain
// substitution - make_enumerator(XError::kComFailure) - rather than
// translating a new shared error type back into its own; every caller's
// error enum already has the kComFailure member this asks for.
template <typename Error>
[[nodiscard]] std::expected<Microsoft::WRL::ComPtr<IMMDeviceEnumerator>, Error> make_enumerator(
    Error on_failure) {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(kClsidMmDeviceEnumerator, nullptr, CLSCTX_ALL,
                                kIidMmDeviceEnumerator, &enumerator))) {
        return std::unexpected(on_failure);
    }
    return enumerator;
}

// The answers that mean a render stream has gone for good: its endpoint
// unplugged, disabled or reconfigured under it, or the audio service stopped
// (Microsoft's "Recovering from an Invalid-Device Error"). Distinct from a
// call a driver merely declines to answer, which is what the callers below
// use this narrow allowlist to tell apart from an actual loss - anything
// else FAILED() reports is treated as the driver being uncooperative rather
// than gone.
//
// passthrough.cpp (exclusive mode) is this predicate's original and current
// caller, around GetCurrentPadding: real hardware has been seen to decline
// that call without the device having gone - see docs/platforms/windows.md -
// so only these two codes end the stream there.
//
// monitor.cpp's shared-mode GetCurrentPadding checks deliberately do NOT
// call this, and that is not an oversight this header should paper over: a
// shared-mode stream always answers for its padding (monitor.cpp's own
// comment on the point), so there a refusal for ANY reason already means the
// stream is over, and narrowing that check to this allowlist would silently
// stop noticing a real loss that happens to fail a different way. The two
// backends are answering different questions that happen to look like the
// same call.
//
// spatial.cpp does not check for device loss at all yet: its render loop
// has no equivalent of monitor.cpp's/passthrough.cpp's `lost` flag today.
// Its ISpatialAudioClient is activated off the same IMMDevice/audio engine
// as the other two, and Microsoft's own reference pages for
// BeginUpdatingAudioObjects/GetAvailableDynamicObjectCount list the same
// AUDCLNT_E_DEVICE_INVALIDATED endpoint-removal code this predicate already
// matches, so it is the plausible starting point once that loop gains one -
// not necessarily the final answer, the same way monitor.cpp's shared-mode
// loop needed a different one from passthrough.cpp's exclusive-mode loop.
[[nodiscard]] inline bool stream_gone(HRESULT result) {
    return result == AUDCLNT_E_DEVICE_INVALIDATED || result == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

}  // namespace ac3::windows_audio
