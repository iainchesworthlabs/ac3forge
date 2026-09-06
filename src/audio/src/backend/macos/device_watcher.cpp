#include "ac3/audio/device_watcher.hpp"

// The Core Audio device watcher: HAL property listeners on the system object.
// CMake compiles this directory's device_watcher.cpp under APPLE and another
// platform directory's everywhere else, so there is no #ifdef here - the
// file's path is what says "macOS".
//
// ---------------------------------------------------------------------------
// Three listeners, four events
// ---------------------------------------------------------------------------
// Everything this file needs is a property of kAudioObjectSystemObject, so
// there is no per-device registration to keep in step with a device list the
// way a naive reading of the header's DeviceChange enum might suggest:
//
//   kAudioHardwarePropertyDevices             -> kAdded / kRemoved
//   kAudioHardwarePropertyDefaultOutputDevice -> kDefaultRenderChanged
//   kAudioHardwarePropertyDefaultInputDevice  -> kDefaultCaptureChanged
//
// The first of those is the one with work behind it. IMMNotificationClient
// hands Windows separate OnDeviceAdded/OnDeviceRemoved calls and PipeWire's
// registry hands out global/global_remove; Core Audio says only "the device
// list changed" and leaves the caller to work out what changed, so this
// keeps the previous list of device UIDs and diffs against it - the same
// bookkeeping the PipeWire watcher's `nodes` map does, for the opposite
// reason (there, to recover an id the removal event does not carry).
//
// kStateChanged is never raised here, and that is a difference rather than
// an omission. It exists because a Windows endpoint can stay in the
// enumerator while becoming disabled, unplugged or not-present; a HAL device
// that goes away leaves kAudioHardwarePropertyDevices, which is already
// reported as kRemoved. A caller that re-probes on kAdded/kRemoved sees
// everything this platform has to say.
//
// The default-INPUT listener is a third registration where roadmap UX12's
// plan for this backend named two. It is here because DeviceChange has a
// kDefaultCaptureChanged case that both other backends raise - Windows from
// OnDefaultDeviceChanged with eCapture, PipeWire from the metadata key
// default.audio.source - and a watcher that could never raise it would be the
// odd one of the three, for the cost of one more address in the array below.
//
// ---------------------------------------------------------------------------
// Which thread a callback arrives on
// ---------------------------------------------------------------------------
// The HAL's, not this library's: there is no jthread field below the way the
// Windows watcher has one, for the same reason capture.cpp has none (see
// coreaudio_support.hpp's "No worker thread"). Unlike the IOProc that file
// registers, this is NOT a realtime thread - it is the HAL's notification
// thread, so the property reads snapshot() makes and the std::string it
// allocates are allowed here, which is what lets the diff happen where the
// event does.
//
// Nothing here writes kAudioHardwarePropertyRunLoop, and that is a decision
// rather than an oversight. The HAL's older listener API delivered on the
// process's MAIN run loop, so a program that never runs one - ac3cli, a test
// binary - would register and then never hear anything, and setting that
// property to a null CFRunLoopRef is the lever that asks for a dedicated
// notification thread instead. Three reasons it is not pulled here. It is
// process-wide, so a library reaching for it decides the question for
// whatever else is in the process. AudioObjectAddPropertyListener, the API
// used below, is the newer of the two and is documented as delivering on a
// thread the HAL owns, so on the deployment target this project builds for
// (13.3, cmake/toolchains/macos.llvm.toolchain.cmake) the lever should have
// nothing to change. And that selector carries a deprecation annotation in
// recent SDKs - which nobody on this project can check, having no Mac (DR9) -
// where -Wdeprecated-declarations under this project's -Werror would turn a
// best-effort call that nothing depends on into a red build on both macOS
// legs. If notifications turn out not to arrive in a process with no run
// loop, that property is the thing to set, and this paragraph is why it was
// left out first. Decided 2026-09-06.
//
// Delivery is under `mutex`, and the whole handler runs under it rather than
// only the callback invocation, because the diff reads and rewrites the
// device list that stop() is entitled to tear down from another thread. That
// single lock is also what lets stop() promise the header's guarantee: it
// unregisters first, so the HAL stops calling, then takes the lock and clears
// the callback, so anything already inside the handler has finished and
// anything that was waiting on the lock finds nothing to call. Exactly the
// shape platform/windows/device_watcher.cpp's Listener::disarm() has.
//
// Be precise about what that buys, because the header's sentence is easy to
// over-read. What is guaranteed is that the CALLER'S callback is not invoked
// again once stop() has returned: it is cleared under the lock, and every
// path to it checks. What is not guaranteed, because the API gives no way to
// ask for it, is that no HAL thread is still somewhere inside handle() taking
// that lock - AudioObjectRemovePropertyListener stops future dispatches and
// says nothing about one already dispatched. Impl outlives stop() in every
// case this class allows (the destructor calls stop() before ~Impl runs), so
// the mutex it is blocking on is still there; a caller that could destroy the
// watcher from a second thread the instant stop() returned on a first is the
// case this shape does not cover, and the header already tells a caller not
// to drive it that way.
//
// Nothing in this file has been run. No Mac has ever run this backend
// (ROADMAP.md DR9); what is claimed here is that it compiles. Written
// 2026-09-06.

#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "coreaudio_support.hpp"

namespace ac3::audio {

namespace {

// The three system-object properties this watcher listens to, in the order
// they are registered and the reverse of the order they are removed.
[[nodiscard]] std::array<AudioObjectPropertyAddress, 3> watched_addresses() {
    return {coreaudio::address(kAudioHardwarePropertyDevices),
            coreaudio::address(kAudioHardwarePropertyDefaultOutputDevice),
            coreaudio::address(kAudioHardwarePropertyDefaultInputDevice)};
}

// Every device UID the machine currently has, sorted so that two snapshots
// can be diffed with std::set_difference rather than a nested scan.
// A device whose UID cannot be read is skipped, matching capture.cpp's own
// enumerate_devices(): an endpoint with no persistent id is one no caller
// could match against a list it already holds, which is the whole purpose of
// DeviceChangeEvent::device_id.
[[nodiscard]] std::vector<std::string> snapshot() {
    std::vector<std::string> uids;
    for (const auto device : coreaudio::device_list()) {
        std::string uid = coreaudio::device_uid(device);
        if (!uid.empty()) {
            uids.push_back(std::move(uid));
        }
    }
    // std::sort rather than std::ranges::sort, for the reason diff_devices()
    // gives below for preferring the iterator-pair std::set_difference: the
    // ranges overloads return a value this has no use for, some standard
    // libraries mark that return [[nodiscard]], and -Werror turns a discarded
    // one into a build failure on a leg nobody here can try first. The
    // classic algorithms return void or an iterator nobody objects to
    // dropping.
    std::sort(uids.begin(), uids.end());
    return uids;
}

}  // namespace

std::string_view describe(DeviceWatchError error) {
    switch (error) {
        case DeviceWatchError::kNoBackend: return "no device-notification backend on this platform";
        case DeviceWatchError::kComFailure:
            return "a Core Audio HAL call failed while registering for property notifications";
        case DeviceWatchError::kAlreadyRunning: return "the device watcher is already running";
    }
    return "unknown device watch error";
}

struct DeviceWatcher::Impl {
    std::mutex mutex;
    Callback callback;
    // Sorted device UIDs as of the last notification (or of start(), before
    // the first one). Read and written only under `mutex`.
    std::vector<std::string> known;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> events{0};
    // How many of watched_addresses() were registered, so a partial failure
    // removes exactly what it added.
    std::size_t registered = 0;

    // Called with `mutex` held and `callback` known non-empty.
    void emit(DeviceChange change, std::string device_id) {
        events.fetch_add(1, std::memory_order_relaxed);
        callback(DeviceChangeEvent{.change = change, .device_id = std::move(device_id)});
    }

    // Called with `mutex` held. Core Audio says only that the list changed,
    // so the difference in both directions is the event.
    void diff_devices() {
        std::vector<std::string> current = snapshot();
        // The iterator-pair form rather than std::ranges::set_difference: the
        // ranges overload returns a result aggregate this has no use for, and
        // a discarded return is a warning waiting to happen under a standard
        // library that marks it [[nodiscard]] - which -Werror would turn into
        // a build failure on a leg nobody here can try first.
        std::vector<std::string> gone;
        std::set_difference(known.begin(), known.end(), current.begin(), current.end(),
                            std::back_inserter(gone));
        std::vector<std::string> arrived;
        std::set_difference(current.begin(), current.end(), known.begin(), known.end(),
                            std::back_inserter(arrived));
        known = std::move(current);
        // Removals first: a caller re-probing its list wants what went away
        // before what replaced it, and a device that was swapped for another
        // on the same physical port reads more sensibly in that order.
        for (auto& uid : gone) {
            emit(DeviceChange::kRemoved, std::move(uid));
        }
        for (auto& uid : arrived) {
            emit(DeviceChange::kAdded, std::move(uid));
        }
    }

    void handle(const AudioObjectPropertyAddress& addr) {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!callback) {
            return;
        }
        switch (addr.mSelector) {
            case kAudioHardwarePropertyDevices:
                diff_devices();
                break;
            case kAudioHardwarePropertyDefaultOutputDevice:
                // An empty id where there is no default at all - the last
                // output went away - which is exactly what
                // device_watcher.hpp says to report for that case.
                emit(DeviceChange::kDefaultRenderChanged,
                     coreaudio::device_uid(coreaudio::default_device(/*input=*/false)));
                break;
            case kAudioHardwarePropertyDefaultInputDevice:
                emit(DeviceChange::kDefaultCaptureChanged,
                     coreaudio::device_uid(coreaudio::default_device(/*input=*/true)));
                break;
            default:
                // Not one of ours. The HAL may batch several addresses into
                // one call and there is nothing to say every one of them was
                // asked for by this listener.
                break;
        }
    }

    // A static member for the reason capture.cpp's Impl::io_proc is one:
    // AudioObjectPropertyListenerProc is a plain C function pointer, and
    // `Impl` is private to DeviceWatcher, so nothing outside the class can
    // name it. The PipeWire backend's Impl::on_global does the same job for
    // the same pair of constraints.
    static OSStatus listener(AudioObjectID object, UInt32 address_count,
                             const AudioObjectPropertyAddress* addresses, void* client_data);
};

OSStatus DeviceWatcher::Impl::listener(AudioObjectID /*object*/, UInt32 address_count,
                                       const AudioObjectPropertyAddress* addresses,
                                       void* client_data) {
    auto* impl = static_cast<Impl*>(client_data);
    if (impl == nullptr || addresses == nullptr) {
        return noErr;
    }
    for (UInt32 i = 0; i < address_count; ++i) {
        impl->handle(addresses[i]);
    }
    return noErr;
}

DeviceWatcher::DeviceWatcher() : impl_(std::make_unique<Impl>()) {}

DeviceWatcher::~DeviceWatcher() {
    stop();
}

bool DeviceWatcher::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

DeviceWatchStats DeviceWatcher::stats() const {
    return DeviceWatchStats{.events_delivered = impl_->events.load(std::memory_order_relaxed)};
}

std::expected<void, DeviceWatchError> DeviceWatcher::start(Callback callback) {
    if (running()) {
        return std::unexpected(DeviceWatchError::kAlreadyRunning);
    }

    // Both of these before the first listener is registered, so a
    // notification that arrives during registration finds a callback to call
    // and a list to diff against rather than an empty one - which would
    // report every device on the machine as newly added.
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->callback = std::move(callback);
        impl_->known = snapshot();
    }
    impl_->events.store(0, std::memory_order_relaxed);
    impl_->registered = 0;

    const auto addresses = watched_addresses();
    for (const auto& addr : addresses) {
        if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr, &Impl::listener,
                                           impl_.get()) != noErr) {
            // Unwind exactly what was added: all three listeners or none. A
            // watcher that reported success while missing one of its events
            // would be worse than one that refused.
            for (std::size_t i = 0; i < impl_->registered; ++i) {
                AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &addresses[i],
                                                  &Impl::listener, impl_.get());
            }
            impl_->registered = 0;
            const std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->callback = nullptr;
            impl_->known.clear();
            return std::unexpected(DeviceWatchError::kComFailure);
        }
        ++impl_->registered;
    }

    impl_->running.store(true, std::memory_order_release);
    return {};
}

void DeviceWatcher::stop() {
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // Order matters, and is the Windows watcher's: unregister first so the
    // HAL stops calling, then take the lock and clear the callback so that
    // anything already inside handle() has finished and anything blocked on
    // the lock finds nothing left to invoke. Once this returns the caller's
    // callback cannot be invoked again - see this file's header comment for
    // what that does and does not say about a HAL thread still unwinding.
    const auto addresses = watched_addresses();
    for (std::size_t i = 0; i < impl_->registered; ++i) {
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &addresses[i], &Impl::listener,
                                          impl_.get());
    }
    impl_->registered = 0;
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->callback = nullptr;
    impl_->known.clear();
}

}  // namespace ac3::audio
