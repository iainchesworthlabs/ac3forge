#include "ac3/audio/spatial.hpp"

// The Windows spatial backend. CMake compiles this directory's spatial.cpp on
// Windows and one of the "no backend" stubs everywhere else, so there is no
// #ifdef here - the file's path is what says "Windows" (see monitor.cpp).
//
// ISpatialAudioObjectRenderStream is activated straight off IMMDevice, the
// same IMMDeviceEnumerator/IMMDevice::Activate pattern monitor.cpp already
// uses (not ActivateAudioInterfaceAsync) - it needs no async completion
// handler here because, unlike the WinRT-era activation path, IMMDevice
// already has the device in hand. CLSIDs/IIDs are spelled out as constexpr
// byte arrays rather than __uuidof, for the same reason monitor.cpp's are:
// the SDK declares these but ships no import library defining them as
// linkable symbols, and __uuidof is an MSVC extension clang rejects under
// -Wpedantic.
//
// One activated ISpatialAudioObject per static channel and per granted
// dynamic slot, ALL activated once in start() and reused for the life of the
// stream - never reactivated per render period - which is what makes the
// render thread allocation-free: BeginUpdatingAudioObjects/
// EndUpdatingAudioObjects only bracket SetPosition/SetVolume/GetBuffer calls
// on objects that already exist. Each object gets its own ring buffer, sized
// once at start(); submit() and the render thread only push into / pop from
// those existing rings, exactly mirroring monitor.cpp's own queue.

#include <windows.h>
// windows.h must precede the audio headers.
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/audio/ring_buffer.hpp"

namespace ac3::audio {

namespace {

using Microsoft::WRL::ComPtr;

constexpr CLSID kClsidMmDeviceEnumerator = {  // {bcde0395-e52f-467c-8e3d-c4579291692e}
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
constexpr IID kIidMmDeviceEnumerator = {  // {a95664d2-9614-4f35-a746-de8db63617e6}
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
constexpr IID kIidSpatialAudioClient = {  // {BBF8E066-AAAA-49BE-9A4D-FD2A858EA27F}
    0xbbf8e066, 0xaaaa, 0x49be, {0x9a, 0x4d, 0xfd, 0x2a, 0x85, 0x8e, 0xa2, 0x7f}};
constexpr IID kIidSpatialAudioObjectRenderStream = {  // {BAB5F473-B423-477B-85F5-B5A332A04153}
    0xbab5f473, 0xb423, 0x477b, {0x85, 0xf5, 0xb5, 0xa3, 0x32, 0xa0, 0x41, 0x53}};

// WAVEFORMATEXTENSIBLE SPEAKER_* bits (ksmedia.h), spelled out rather than
// pulled in from another header for the same reason monitor.cpp hardcodes
// kSpeaker51/kSpeaker71 instead of including mmreg.h - these values are part
// of the public ABI and have never changed.
constexpr std::uint32_t kSpeakerFrontLeft = 0x1;
constexpr std::uint32_t kSpeakerFrontRight = 0x2;
constexpr std::uint32_t kSpeakerFrontCenter = 0x4;
constexpr std::uint32_t kSpeakerLowFrequency = 0x8;
constexpr std::uint32_t kSpeakerBackLeft = 0x10;
constexpr std::uint32_t kSpeakerBackRight = 0x20;
constexpr std::uint32_t kSpeakerSideLeft = 0x200;
constexpr std::uint32_t kSpeakerSideRight = 0x400;
constexpr std::uint32_t kSpeakerTopFrontLeft = 0x1000;
constexpr std::uint32_t kSpeakerTopFrontRight = 0x4000;
constexpr std::uint32_t kSpeakerTopBackLeft = 0x8000;
constexpr std::uint32_t kSpeakerTopBackRight = 0x20000;

// A requested SPEAKER_* bit with no Windows static-object counterpart -
// oba::BedLabel::kLw/kRw (wide pairs) and kLfe2 (a second LFE) among them -
// returns AudioObjectType_None. The caller drops it rather than miscounting;
// see start()'s own comment on where that happens.
AudioObjectType audio_object_type_for_speaker(std::uint32_t speaker_bit) {
    switch (speaker_bit) {
        case kSpeakerFrontLeft: return AudioObjectType_FrontLeft;
        case kSpeakerFrontRight: return AudioObjectType_FrontRight;
        case kSpeakerFrontCenter: return AudioObjectType_FrontCenter;
        case kSpeakerLowFrequency: return AudioObjectType_LowFrequency;
        case kSpeakerBackLeft: return AudioObjectType_BackLeft;
        case kSpeakerBackRight: return AudioObjectType_BackRight;
        case kSpeakerSideLeft: return AudioObjectType_SideLeft;
        case kSpeakerSideRight: return AudioObjectType_SideRight;
        case kSpeakerTopFrontLeft: return AudioObjectType_TopFrontLeft;
        case kSpeakerTopFrontRight: return AudioObjectType_TopFrontRight;
        case kSpeakerTopBackLeft: return AudioObjectType_TopBackLeft;
        case kSpeakerTopBackRight: return AudioObjectType_TopBackRight;
        default: return AudioObjectType_None;
    }
}

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

std::expected<ComPtr<IMMDeviceEnumerator>, SpatialError> make_enumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(kClsidMmDeviceEnumerator, nullptr, CLSCTX_ALL,
                                kIidMmDeviceEnumerator, &enumerator))) {
        return std::unexpected(SpatialError::kComFailure);
    }
    return enumerator;
}

std::expected<ComPtr<IMMDevice>, SpatialError> resolve_device(
    IMMDeviceEnumerator& enumerator, const std::string& device_id) {
    ComPtr<IMMDevice> device;
    if (device_id.empty()) {
        if (FAILED(enumerator.GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
            return std::unexpected(SpatialError::kDeviceNotFound);
        }
    } else {
        const int wide_len = MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(wide_len > 0 ? wide_len - 1 : 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, wide.data(), wide_len);
        if (FAILED(enumerator.GetDevice(wide.c_str(), &device))) {
            return std::unexpected(SpatialError::kDeviceNotFound);
        }
    }
    return device;
}

std::expected<ComPtr<ISpatialAudioClient>, SpatialError> activate_spatial_client(
    const std::string& device_id) {
    auto enumerator = make_enumerator();
    if (!enumerator) {
        return std::unexpected(enumerator.error());
    }
    auto device = resolve_device(**enumerator, device_id);
    if (!device) {
        return std::unexpected(device.error());
    }
    ComPtr<ISpatialAudioClient> client;
    if (FAILED((*device)->Activate(kIidSpatialAudioClient, CLSCTX_ALL, nullptr, &client))) {
        return std::unexpected(SpatialError::kComFailure);
    }
    return client;
}

}  // namespace

std::string_view describe(SpatialError error) {
    switch (error) {
        case SpatialError::kNoBackend: return "no spatial backend on this platform";
        case SpatialError::kComFailure: return "a Windows audio (WASAPI/COM) call failed";
        case SpatialError::kDeviceNotFound: return "the requested render device was not found";
        case SpatialError::kNoSpatialFormat:
            return "no spatial sound format is enabled on this endpoint - enable Windows Sonic "
                   "for Headphones or Dolby Atmos for Home Theater/Headphones in Settings > "
                   "System > Sound";
        case SpatialError::kFormatRejected:
            return "the endpoint rejected the negotiated audio format";
        case SpatialError::kAlreadyRunning: return "spatial rendering is already running";
        case SpatialError::kNotRunning: return "spatial rendering is not running";
    }
    return "unknown spatial error";
}

std::expected<SpatialDeviceCapability, SpatialError> probe_spatial_capability(
    const std::string& device_id) {
    ComScope com;
    if (!com.ok()) {
        return std::unexpected(SpatialError::kComFailure);
    }
    auto client = activate_spatial_client(device_id);
    if (!client) {
        return std::unexpected(client.error());
    }
    UINT32 max_objects = 0;
    if (FAILED((*client)->GetMaxDynamicObjectCount(&max_objects))) {
        return std::unexpected(SpatialError::kComFailure);
    }
    SpatialDeviceCapability cap{.available = true, .max_dynamic_objects = max_objects};
    if (max_objects == 0) {
        cap.reason = std::string{describe(SpatialError::kNoSpatialFormat)};
    }
    return cap;
}

struct SpatialObjectSink::Impl {
    // Holds four std::atomic<float> - not copyable or movable, per
    // std::atomic's own contract - so instances live behind a unique_ptr and
    // only the pointer relocates when the owning vector grows. The atomics
    // themselves are latest-value-wins with no interpolation between
    // updates, the same convention ac3::oba::SceneCursor documents for a
    // live position source arriving slower than render rate (submit() here
    // is called once per decode block, ~32 ms; the render period is much
    // shorter). Four independent atomics rather than one struct: lock-free
    // on every target this project builds for, and a render period reading
    // x from one submit() and y from the next is imperceptible at this
    // control rate - exactly the tolerance SceneCursor's own doc accepts.
    struct ObjectSlot {
        ObjectSlot(ComPtr<ISpatialAudioObject> obj, std::unique_ptr<RingBuffer> r,
                   std::uint32_t ch = 0)
            : object(std::move(obj)), ring(std::move(r)), channel(ch) {}

        ComPtr<ISpatialAudioObject> object;
        std::unique_ptr<RingBuffer> ring;
        std::atomic<float> x{0.0F};
        std::atomic<float> y{0.0F};
        std::atomic<float> z{0.0F};
        std::atomic<float> gain{1.0F};
        std::uint32_t channel = 0;  // SPEAKER_* bit; unused for dynamic slots
    };

    ComPtr<ISpatialAudioObjectRenderStream> stream;
    std::vector<std::unique_ptr<ObjectSlot>> statics;
    std::vector<std::unique_ptr<ObjectSlot>> dynamics;
    std::jthread worker;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> underruns{0};

    ObjectSlot* find_static(std::uint32_t channel) {
        for (auto& slot : statics) {
            if (slot->channel == channel) {
                return slot.get();
            }
        }
        return nullptr;
    }
};

SpatialObjectSink::SpatialObjectSink() : impl_(std::make_unique<Impl>()) {}

SpatialObjectSink::~SpatialObjectSink() {
    stop();
}

bool SpatialObjectSink::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

SpatialObjectStats SpatialObjectSink::stats() const {
    return {.updates_submitted = impl_->submitted.load(std::memory_order_relaxed),
            .updates_rendered = impl_->rendered.load(std::memory_order_relaxed),
            .underruns = impl_->underruns.load(std::memory_order_relaxed),
            .active_dynamic_objects = static_cast<std::uint32_t>(impl_->dynamics.size())};
}

bool SpatialObjectSink::can_submit() const {
    if (!running()) {
        return false;
    }
    // Room for roughly 20 ms on every active ring - a hint, not a guarantee;
    // submit() itself is what actually gates each write, same split as
    // MonitorSink::can_submit()/submit().
    const auto has_room = [](const Impl::ObjectSlot& slot) {
        return slot.ring->capacity() - slot.ring->available() > slot.ring->capacity() / 50;
    };
    for (const auto& slot : impl_->dynamics) {
        if (!has_room(*slot)) {
            return false;
        }
    }
    for (const auto& slot : impl_->statics) {
        if (!has_room(*slot)) {
            return false;
        }
    }
    return true;
}

bool SpatialObjectSink::submit(std::span<const DynamicObjectUpdate> dynamic,
                               std::span<const StaticObjectUpdate> static_objects) {
    if (!running() || dynamic.size() > impl_->dynamics.size()) {
        return false;
    }
    // Check every ring has room BEFORE writing to any of them - a partial
    // commit here would desync one object's audio from the others' by a
    // block, which is worse than refusing the whole submit() and letting the
    // caller retry, exactly the reasoning MonitorSink::submit() documents
    // for its own single check-then-write.
    for (std::size_t i = 0; i < dynamic.size(); ++i) {
        auto& ring = *impl_->dynamics[i]->ring;
        if (ring.capacity() - ring.available() <= dynamic[i].pcm.size()) {
            return false;
        }
    }
    for (const auto& update : static_objects) {
        const auto* slot = impl_->find_static(update.channel);
        if (slot == nullptr) {
            return false;
        }
        if (slot->ring->capacity() - slot->ring->available() <= update.pcm.size()) {
            return false;
        }
    }

    for (std::size_t i = 0; i < dynamic.size(); ++i) {
        auto& slot = *impl_->dynamics[i];
        slot.ring->write(dynamic[i].pcm);
        slot.x.store(dynamic[i].x, std::memory_order_relaxed);
        slot.y.store(dynamic[i].y, std::memory_order_relaxed);
        slot.z.store(dynamic[i].z, std::memory_order_relaxed);
        slot.gain.store(dynamic[i].gain, std::memory_order_relaxed);
    }
    for (const auto& update : static_objects) {
        impl_->find_static(update.channel)->ring->write(update.pcm);
    }
    impl_->submitted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void SpatialObjectSink::stop() {
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    impl_->running.store(false, std::memory_order_release);
    impl_->statics.clear();
    impl_->dynamics.clear();
    impl_->stream.Reset();
}

std::expected<void, SpatialError> SpatialObjectSink::start(const std::string& device_id,
                                                            std::uint32_t sample_rate,
                                                            std::uint32_t static_channels,
                                                            std::uint32_t max_dynamic_objects) {
    if (running()) {
        return std::unexpected(SpatialError::kAlreadyRunning);
    }

    ComScope com;
    if (!com.ok()) {
        return std::unexpected(SpatialError::kComFailure);
    }
    auto client = activate_spatial_client(device_id);
    if (!client) {
        return std::unexpected(client.error());
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 1;  // every spatial object, static or dynamic, is mono
    format.nSamplesPerSec = sample_rate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    if (FAILED((*client)->IsAudioObjectFormatSupported(&format))) {
        return std::unexpected(SpatialError::kFormatRejected);
    }

    UINT32 endpoint_max_dynamic = 0;
    if (FAILED((*client)->GetMaxDynamicObjectCount(&endpoint_max_dynamic))) {
        return std::unexpected(SpatialError::kComFailure);
    }
    // The clean refusal the roadmap calls for: a caller that wants at least
    // one dynamic object, on an endpoint with none enabled, is told which
    // Settings toggle would fix it rather than getting a generic failure.
    if (max_dynamic_objects > 0 && endpoint_max_dynamic == 0) {
        return std::unexpected(SpatialError::kNoSpatialFormat);
    }
    const UINT32 granted_dynamic = std::min(max_dynamic_objects, endpoint_max_dynamic);

    AudioObjectType native_mask = AudioObjectType_None;
    if (FAILED((*client)->GetNativeStaticObjectTypeMask(&native_mask))) {
        return std::unexpected(SpatialError::kComFailure);
    }
    // Bits requested with no Windows static-object counterpart, or no native
    // support on this endpoint, are dropped rather than failing the whole
    // session - see audio_object_type_for_speaker's own comment. The caller
    // (run_spatial) reads back which channels were actually granted via
    // find_static() succeeding or not; there is no separate accessor because
    // a dropped channel simply never appears as a valid submit() target.
    AudioObjectType static_mask = AudioObjectType_None;
    std::vector<std::uint32_t> granted_static_channels;
    for (std::uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((static_channels & bit) == 0) {
            continue;
        }
        const auto object_type = audio_object_type_for_speaker(bit);
        if (object_type == AudioObjectType_None ||
            (native_mask & object_type) == AudioObjectType_None) {
            continue;
        }
        static_mask = static_mask | object_type;
        granted_static_channels.push_back(bit);
    }

    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ready == nullptr) {
        return std::unexpected(SpatialError::kComFailure);
    }

    SpatialAudioObjectRenderStreamActivationParams params{};
    params.ObjectFormat = &format;
    params.StaticObjectTypeMask = static_mask;
    params.MinDynamicObjectCount = 0;
    params.MaxDynamicObjectCount = granted_dynamic;
    params.Category = AudioCategory_Movie;
    params.EventHandle = ready;
    params.NotifyObject = nullptr;

    PROPVARIANT activation{};
    PropVariantInit(&activation);
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    ComPtr<ISpatialAudioObjectRenderStream> stream;
    if (FAILED((*client)->ActivateSpatialAudioStream(&activation, kIidSpatialAudioObjectRenderStream,
                                                     &stream))) {
        CloseHandle(ready);
        return std::unexpected(SpatialError::kComFailure);
    }

    // Every object this session will ever use, activated ONCE here - see
    // this file's header comment on why that is what keeps the render
    // thread allocation-free.
    std::vector<std::unique_ptr<Impl::ObjectSlot>> statics;
    for (const auto channel : granted_static_channels) {
        ComPtr<ISpatialAudioObject> object;
        if (FAILED(stream->ActivateSpatialAudioObject(audio_object_type_for_speaker(channel),
                                                       &object))) {
            continue;  // this one endpoint refused; the rest still render
        }
        statics.push_back(std::make_unique<Impl::ObjectSlot>(
            std::move(object), std::make_unique<RingBuffer>(sample_rate), channel));
    }
    std::vector<std::unique_ptr<Impl::ObjectSlot>> dynamics;
    for (UINT32 i = 0; i < granted_dynamic; ++i) {
        ComPtr<ISpatialAudioObject> object;
        if (FAILED(stream->ActivateSpatialAudioObject(AudioObjectType_Dynamic, &object))) {
            break;  // fewer than advertised; use what activated
        }
        dynamics.push_back(std::make_unique<Impl::ObjectSlot>(
            std::move(object), std::make_unique<RingBuffer>(sample_rate)));
    }

    impl_->stream = stream;
    impl_->statics = std::move(statics);
    impl_->dynamics = std::move(dynamics);
    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered.store(0, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);
    impl_->running.store(true, std::memory_order_release);

    impl_->worker = std::jthread([this, stream, ready](const std::stop_token& stop) mutable {
        ComScope thread_com;
        std::vector<float> chunk;
        stream->Start();

        while (!stop.stop_requested()) {
            if (WaitForSingleObject(ready, 200) != WAIT_OBJECT_0) {
                continue;
            }
            UINT32 available_dynamic = 0;
            UINT32 frame_count = 0;
            if (FAILED(stream->BeginUpdatingAudioObjects(&available_dynamic, &frame_count))) {
                break;
            }

            const auto render_one = [&](Impl::ObjectSlot& slot) {
                BYTE* buffer = nullptr;
                UINT32 buffer_bytes = 0;
                if (FAILED(slot.object->GetBuffer(&buffer, &buffer_bytes))) {
                    return;
                }
                const std::size_t wanted = buffer_bytes / sizeof(float);
                chunk.resize(wanted);
                const auto got = slot.ring->read(chunk);
                if (got < wanted) {
                    // Silence for the remainder, counted rather than
                    // hidden - MonitorSink's own underrun discipline.
                    std::fill(chunk.begin() + static_cast<std::ptrdiff_t>(got), chunk.end(), 0.0F);
                    impl_->underruns.fetch_add(1, std::memory_order_relaxed);
                }
                std::memcpy(buffer, chunk.data(), wanted * sizeof(float));
            };

            for (auto& slot : impl_->statics) {
                render_one(*slot);
            }
            // Only the first `available_dynamic` slots render this period -
            // the endpoint can throttle the count under load, and a slot
            // beyond it is simply skipped, not torn down.
            const auto active =
                std::min<std::size_t>(impl_->dynamics.size(), available_dynamic);
            for (std::size_t i = 0; i < active; ++i) {
                auto& slot = *impl_->dynamics[i];
                slot.object->SetPosition(slot.x.load(std::memory_order_relaxed),
                                         slot.y.load(std::memory_order_relaxed),
                                         slot.z.load(std::memory_order_relaxed));
                slot.object->SetVolume(slot.gain.load(std::memory_order_relaxed));
                render_one(slot);
            }

            stream->EndUpdatingAudioObjects();
            impl_->rendered.fetch_add(1, std::memory_order_relaxed);
        }

        stream->Stop();
        CloseHandle(ready);
    });

    return {};
}

}  // namespace ac3::audio
