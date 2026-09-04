#include "ac3/audio/device_watcher.hpp"

// The Windows device watcher: IMMNotificationClient, registered with the
// MMDevice enumerator. CMake compiles this directory's device_watcher.cpp on
// Windows and another platform directory's everywhere else, so there is no
// #ifdef here - the file's path is what says "Windows".
//
// The shape follows capture.cpp: a dedicated thread owns the COM apartment
// the enumerator lives in, and the listener object is hand-rolled IUnknown
// rather than a WRL RuntimeClass, so this file needs no more of WRL than
// ComPtr and none of <wrl/implements.h>'s include-order demands.

#include <windows.h>
// windows.h must precede the audio headers.
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

namespace ac3::audio {

namespace {

using Microsoft::WRL::ComPtr;

// Spelled out rather than taken from __uuidof, for the reason capture.cpp
// gives: the SDK declares these but ships no import library defining them,
// and __uuidof is an MSVC extension clang rejects under -Wpedantic.
constexpr CLSID kClsidMmDeviceEnumerator = {  // {bcde0395-e52f-467c-8e3d-c4579291692e}
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
constexpr IID kIidMmDeviceEnumerator = {  // {a95664d2-9614-4f35-a746-de8db63617e6}
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
constexpr IID kIidMmNotificationClient = {  // {7991eec9-7e89-4d85-8390-6c703cec60c0}
    0x7991eec9, 0x7e89, 0x4d85, {0x83, 0x90, 0x6c, 0x70, 0x3c, 0xec, 0x60, 0xc0}};
constexpr IID kIidUnknown = {  // {00000000-0000-0000-c000-000000000046}
    0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

std::string to_utf8(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

// COM lifetime for one thread, same as capture.cpp's.
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

// What the audio subsystem calls back into. Every notification is delivered
// under mutex_, which is what lets disarm() promise that once it returns no
// callback is running and none will start - the guarantee stop() documents.
class Listener final : public IMMNotificationClient {
public:
    Listener(DeviceWatcher::Callback callback, std::atomic<std::uint64_t>& delivered)
        : callback_(std::move(callback)), delivered_(delivered) {}
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // IUnknown
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --refs_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, kIidUnknown) || IsEqualIID(riid, kIidMmNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR id, DWORD) override {
        deliver(DeviceChange::kStateChanged, id);
        return S_OK;
    }
    STDMETHODIMP OnDeviceAdded(LPCWSTR id) override {
        deliver(DeviceChange::kAdded, id);
        return S_OK;
    }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR id) override {
        deliver(DeviceChange::kRemoved, id);
        return S_OK;
    }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        // Windows raises this once per role - console, multimedia,
        // communications - for the same physical change. The sinks in this
        // library open the console default (every GetDefaultAudioEndpoint
        // call in this tree passes eConsole), so that is the one role whose
        // change means anything to a caller here; the other two would only
        // deliver the same event twice more.
        if (role != eConsole) {
            return S_OK;
        }
        deliver(flow == eRender ? DeviceChange::kDefaultRenderChanged
                                : DeviceChange::kDefaultCaptureChanged,
                id);
        return S_OK;
    }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        // A volume nudge, a rename, a driver writing a property: none of it
        // changes which endpoints exist or which one is the default, and a
        // sink-follower that re-probed on every one would never sit still.
        return S_OK;
    }

    // After this returns, no callback is running and none will run again.
    void disarm() {
        const std::lock_guard<std::mutex> lock(mutex_);
        callback_ = nullptr;
    }

private:
    ~Listener() = default;

    void deliver(DeviceChange change, LPCWSTR id) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!callback_) {
            return;
        }
        delivered_.fetch_add(1, std::memory_order_relaxed);
        callback_(DeviceChangeEvent{.change = change, .device_id = to_utf8(id)});
    }

    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    DeviceWatcher::Callback callback_;
    std::atomic<std::uint64_t>& delivered_;
};

}  // namespace

std::string_view describe(DeviceWatchError error) {
    switch (error) {
        case DeviceWatchError::kNoBackend: return "no device-notification backend on this platform";
        case DeviceWatchError::kComFailure:
            return "a Windows audio (WASAPI/COM) call failed while registering for notifications";
        case DeviceWatchError::kAlreadyRunning: return "the device watcher is already running";
    }
    return "unknown device watch error";
}

struct DeviceWatcher::Impl {
    std::jthread worker;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> delivered{0};
    // Manual-reset: signalled once by stop(), waited on by the worker.
    HANDLE stop_event = nullptr;
};

DeviceWatcher::DeviceWatcher() : impl_(std::make_unique<Impl>()) {}

DeviceWatcher::~DeviceWatcher() {
    stop();
}

bool DeviceWatcher::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

DeviceWatchStats DeviceWatcher::stats() const {
    return {.events_delivered = impl_->delivered.load(std::memory_order_relaxed)};
}

std::expected<void, DeviceWatchError> DeviceWatcher::start(Callback callback) {
    if (running()) {
        return std::unexpected(DeviceWatchError::kAlreadyRunning);
    }

    // The enumerator that holds the registration has to live in a COM
    // apartment that outlives this call, so a worker thread owns both: it
    // initialises COM, registers, parks on stop_event, then unregisters and
    // uninitialises in that order. Registration's result is handed back
    // through `registered` so failure is reported here, synchronously.
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE registered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr || registered == nullptr) {
        if (stop_event != nullptr) {
            CloseHandle(stop_event);
        }
        if (registered != nullptr) {
            CloseHandle(registered);
        }
        return std::unexpected(DeviceWatchError::kComFailure);
    }
    impl_->stop_event = stop_event;
    impl_->delivered.store(0, std::memory_order_relaxed);

    std::atomic<HRESULT> outcome{E_PENDING};
    impl_->worker = std::jthread([this, callback = std::move(callback), stop_event, registered,
                                  &outcome]() mutable {
        ComScope com;
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = com.ok() ? CoCreateInstance(kClsidMmDeviceEnumerator, nullptr, CLSCTX_ALL,
                                                 kIidMmDeviceEnumerator, &enumerator)
                              : E_FAIL;
        ComPtr<Listener> listener;
        if (SUCCEEDED(hr)) {
            listener.Attach(new Listener(std::move(callback), impl_->delivered));
            hr = enumerator->RegisterEndpointNotificationCallback(listener.Get());
        }
        outcome.store(hr, std::memory_order_release);
        SetEvent(registered);
        if (FAILED(hr)) {
            return;
        }

        WaitForSingleObject(stop_event, INFINITE);

        // Order matters: unregister first so the subsystem stops calling,
        // then disarm so anything already inside deliver() has finished
        // before the listener's last reference goes.
        enumerator->UnregisterEndpointNotificationCallback(listener.Get());
        listener->disarm();
    });

    WaitForSingleObject(registered, INFINITE);
    CloseHandle(registered);
    if (FAILED(outcome.load(std::memory_order_acquire))) {
        impl_->worker.join();
        CloseHandle(stop_event);
        impl_->stop_event = nullptr;
        return std::unexpected(DeviceWatchError::kComFailure);
    }
    impl_->running.store(true, std::memory_order_release);
    return {};
}

void DeviceWatcher::stop() {
    if (!running()) {
        return;
    }
    SetEvent(impl_->stop_event);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    CloseHandle(impl_->stop_event);
    impl_->stop_event = nullptr;
    impl_->running.store(false, std::memory_order_release);
}

}  // namespace ac3::audio
