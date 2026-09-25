#include "ac3/audio/passthrough.hpp"

// The Windows passthrough backend. CMake compiles this directory's
// passthrough.cpp on Windows and another platform directory's everywhere
// else, so there is no #ifdef - the file's path is what says "Windows".
//
// WIN32_LEAN_AND_MEAN and NOMINMAX are set by the WIN32 block of
// src/forge/CMakeLists.txt, not by #defines here: they configure <windows.h> for
// every translation unit that pulls it in, and one setting in one place cannot
// disagree with itself the way per-file guards can.

#include <windows.h>
// windows.h must precede the audio headers.
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>

#include "ac3/audio/playback_counter.hpp"
#include "ac3/audio/ring_buffer.hpp"
#include "ac3/audio/speakers.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "windows_support.hpp"

namespace ac3::audio {

namespace {

using Microsoft::WRL::ComPtr;
using windows_audio::ComScope;
using windows_audio::kClsidMmDeviceEnumerator;
using windows_audio::kIidAudioClient;
using windows_audio::kIidAudioRenderClient;
using windows_audio::kIidMmDeviceEnumerator;
using windows_audio::stream_gone;

// PKEY_Device_FriendlyName, spelled out for the same reason as in the capture
// backend: functiondiscoverykeys_devpkey.h needs a fragile include ordering.
constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
// PKEY_Device_DeviceDesc - same fmtid, property id 2: the endpoint's own
// short description ("Speakers") without the adapter suffix. The fallback
// when an endpoint has no friendly name at all, which real machines produce
// (a virtual endpoint whose driver never filled the property in).
constexpr PROPERTYKEY kPkeyDeviceDescription = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};

// KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL from ksmedia.h:
// {00000092-0000-0010-8000-00aa00389b71}. The low 16 bits of Data1 are the
// wFormatTag (0x0092 = WAVE_FORMAT_DOLBY_AC3_SPDIF).
constexpr GUID kSubtypeIec61937DolbyDigital = {
    0x00000092, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS from ksmedia.h:
// {0000000a-0cea-0010-8000-00aa00389b71}. A different GUID family from AC-3's
// (Data2 0x0cea rather than 0x0000) - confirmed against a Windows SDK
// ksmedia.h mirror and against Microsoft's own "Representing Formats for IEC
// 61937 Transmissions" documentation, which gives this exact value in a
// worked Dolby Digital Plus example.
constexpr GUID kSubtypeIec61937DolbyDigitalPlus = {
    0x0000000a, 0x0cea, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// KSDATAFORMAT_SUBTYPE_PCM: {00000001-0000-0010-8000-00aa00389b71}, the
// WAVE_FORMAT_PCM tag in the same GUID family. Needed for the rate probes,
// which ask about ordinary PCM rather than a bitstream.
constexpr GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// The IEC 61937 carrier is a 2-channel 16-bit stream; one AC-3 frame occupies
// one 6144-byte burst = 1536 stereo frames, matching the AC-3 frame duration.
constexpr WORD kCarrierChannels = 2;
constexpr WORD kCarrierBits = 16;
constexpr DWORD kSpeakerStereo = 0x3;  // KSAUDIO_SPEAKER_STEREO

// WAVEFORMATEXTENSIBLE_IEC61937 (ksmedia.h): a WAVEFORMATEXTENSIBLE followed
// by three fields describing the *encoded* stream inside the carrier.
struct WaveFormatIec61937 {
    WAVEFORMATEXTENSIBLE FormatExt;
    DWORD dwEncodedSamplesPerSec;
    DWORD dwEncodedChannelCount;
    DWORD dwAverageBytesPerSec;
};

WaveFormatIec61937 make_ac3_format(std::uint32_t sample_rate, DWORD encoded_channels) {
    WaveFormatIec61937 format{};
    auto& wf = format.FormatExt.Format;
    wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.nChannels = kCarrierChannels;
    wf.nSamplesPerSec = sample_rate;
    wf.wBitsPerSample = kCarrierBits;
    wf.nBlockAlign = static_cast<WORD>(kCarrierChannels * kCarrierBits / 8);
    wf.nAvgBytesPerSec = sample_rate * wf.nBlockAlign;
    wf.cbSize = sizeof(WaveFormatIec61937) - sizeof(WAVEFORMATEX);

    format.FormatExt.Samples.wValidBitsPerSample = kCarrierBits;
    format.FormatExt.dwChannelMask = kSpeakerStereo;
    format.FormatExt.SubFormat = kSubtypeIec61937DolbyDigital;

    // Describes the AC-3 payload, not the carrier: the decoded rate and how
    // many channels the receiver will reproduce.
    format.dwEncodedSamplesPerSec = sample_rate;
    format.dwEncodedChannelCount = encoded_channels;
    format.dwAverageBytesPerSec = 0;  // 0 is permitted and means "unspecified"
    return format;
}

// Dolby Digital Plus over IEC 60958/61937: per Microsoft's "Representing
// Formats for IEC 61937 Transmissions", "the link-sampling rate must be four
// times the sampling rate of the content" - so unlike AC-3, the carrier
// itself (nSamplesPerSec/nAvgBytesPerSec) runs at 4x `sample_rate`, which
// stays the CONTENT rate throughout. Field values otherwise mirror
// Microsoft's own worked 48 kHz DD+ example verbatim.
WaveFormatIec61937 make_eac3_format(std::uint32_t sample_rate, DWORD encoded_channels) {
    WaveFormatIec61937 format{};
    auto& wf = format.FormatExt.Format;
    wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.nChannels = kCarrierChannels;
    wf.nSamplesPerSec = sample_rate * 4;
    wf.wBitsPerSample = kCarrierBits;
    wf.nBlockAlign = static_cast<WORD>(kCarrierChannels * kCarrierBits / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize = sizeof(WaveFormatIec61937) - sizeof(WAVEFORMATEX);

    format.FormatExt.Samples.wValidBitsPerSample = kCarrierBits;
    format.FormatExt.dwChannelMask = kSpeakerStereo;
    format.FormatExt.SubFormat = kSubtypeIec61937DolbyDigitalPlus;

    format.dwEncodedSamplesPerSec = sample_rate;
    format.dwEncodedChannelCount = encoded_channels;
    format.dwAverageBytesPerSec = 0;  // ignored for this format (MS docs)
    return format;
}

// AC-3 or E-AC-3 only: start() refuses AC-4 before it gets here, there being
// no subformat to name it by (see passthrough.hpp's header comment).
WaveFormatIec61937 make_format(BitstreamFormat format, std::uint32_t sample_rate,
                               DWORD encoded_channels) {
    return format == BitstreamFormat::kEac3 ? make_eac3_format(sample_rate, encoded_channels)
                                            : make_ac3_format(sample_rate, encoded_channels);
}

std::string to_utf8(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

// A display name that is never empty: the friendly name if the endpoint has
// one, its short description otherwise, else a stand-in carrying the
// endpoint id. Both front ends put this straight into a device list, where
// a blank row is indistinguishable from a rendering bug - and the id keeps
// two unnamed endpoints tellable apart.
std::string endpoint_display_name(IMMDevice* device, const std::string& id) {
    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
        for (const auto& key : {kPkeyDeviceFriendlyName, kPkeyDeviceDescription}) {
            PROPVARIANT value;
            PropVariantInit(&value);
            std::string name;
            if (SUCCEEDED(properties->GetValue(key, &value)) && value.vt == VT_LPWSTR &&
                value.pwszVal != nullptr && value.pwszVal[0] != L'\0') {
                name = to_utf8(value.pwszVal);
            }
            PropVariantClear(&value);
            if (!name.empty()) {
                return name;
            }
        }
    }
    return id.empty() ? std::string{"Unnamed audio endpoint"} : "Unnamed endpoint " + id;
}

// The endpoint's configured speaker arrangement, which the mix format does not
// always carry: a stereo mix on a 5.1 endpoint has dwChannelMask 0x3, while
// PKEY_AudioEndpoint_PhysicalSpeakers holds what the user set under Sound >
// Configure. Same SPEAKER_* bits either way (ac3::audio::speakers.hpp).
constexpr PROPERTYKEY kPkeyAudioEndpointPhysicalSpeakers = {
    {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 3};

std::uint32_t physical_speakers(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) {
        return 0;
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::uint32_t mask = 0;
    if (SUCCEEDED(properties->GetValue(kPkeyAudioEndpointPhysicalSpeakers, &value)) &&
        value.vt == VT_UI4) {
        mask = value.ulVal;
    }
    PropVariantClear(&value);
    // SPEAKER_ALL says "every speaker" without saying which, which is no more
    // usable than nothing at all.
    return mask & kSpeakerAllPositions;
}

// A 16-bit PCM format of `channels` channels at `sample_rate`, as
// WAVEFORMATEXTENSIBLE, which is the only form exclusive mode takes above two
// channels.
WAVEFORMATEXTENSIBLE make_pcm_format(std::uint32_t sample_rate, WORD channels, DWORD mask) {
    WAVEFORMATEXTENSIBLE format{};
    auto& wf = format.Format;
    wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.nChannels = channels;
    wf.nSamplesPerSec = sample_rate;
    wf.wBitsPerSample = kCarrierBits;
    wf.nBlockAlign = static_cast<WORD>(channels * kCarrierBits / 8);
    wf.nAvgBytesPerSec = sample_rate * wf.nBlockAlign;
    wf.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = kCarrierBits;
    format.dwChannelMask = mask != 0 ? mask : default_speakers(channels);
    format.SubFormat = kSubtypePcm;
    return format;
}

// Which of the rates a consumer endpoint might run at it accepts in exclusive
// mode, at its own width: the rate list RenderDeviceInfo reports. Shared mode
// resamples anything, so asking about it would answer "all of them" and say
// nothing about the device.
std::vector<std::uint32_t> probe_sample_rates(IAudioClient* client, WORD channels, DWORD mask) {
    constexpr std::array<std::uint32_t, 6> kRates{44100, 48000, 88200, 96000, 176400, 192000};
    std::vector<std::uint32_t> rates;
    for (const std::uint32_t rate : kRates) {
        WAVEFORMATEXTENSIBLE format = make_pcm_format(rate, channels, mask);
        if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr) == S_OK) {
            rates.push_back(rate);
        }
    }
    return rates;
}

}  // namespace

std::string_view describe(PassthroughError error) {
    switch (error) {
        case PassthroughError::kNoBackend: return "no passthrough backend on this platform";
        case PassthroughError::kComFailure: return "a Windows audio (WASAPI/COM) call failed";
        case PassthroughError::kDeviceNotFound: return "the requested render device was not found";
        case PassthroughError::kFormatRejected:
            return "the endpoint will not accept this format over IEC 61937 (enable Dolby "
                   "Digital / Dolby Digital Plus passthrough for the device, or use an S/PDIF "
                   "or HDMI output)";
        case PassthroughError::kExclusiveUnavailable:
            return "exclusive access was refused (another app holds the device, or exclusive "
                   "mode is disabled for it in Sound settings)";
        case PassthroughError::kAlreadyRunning: return "passthrough is already running";
        case PassthroughError::kNotRunning: return "passthrough is not running";
        case PassthroughError::kUnsupportedFormat:
            return "Windows defines no IEC 61937 subformat for AC-4 (the SDK's ksmedia.h has "
                   "none), so WASAPI cannot be asked to send it";
    }
    return "unknown passthrough error";
}

std::expected<std::vector<RenderDeviceInfo>, PassthroughError> enumerate_render_devices(
    std::uint32_t sample_rate) {
    ComScope com;
    if (!com.ok()) {
        return std::unexpected(PassthroughError::kComFailure);
    }
    auto enumerator = windows_audio::make_enumerator(PassthroughError::kComFailure);
    if (!enumerator) {
        return std::unexpected(enumerator.error());
    }

    std::string default_id;
    ComPtr<IMMDevice> default_device;
    if (SUCCEEDED((*enumerator)->GetDefaultAudioEndpoint(eRender, eConsole, &default_device))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(default_device->GetId(&id))) {
            default_id = to_utf8(id);
            CoTaskMemFree(id);
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED((*enumerator)->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        return std::unexpected(PassthroughError::kComFailure);
    }
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return std::unexpected(PassthroughError::kComFailure);
    }

    auto ac3_format = make_ac3_format(sample_rate, 6);
    auto eac3_format = make_eac3_format(sample_rate, 6);
    std::vector<RenderDeviceInfo> devices;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) {
            continue;
        }
        RenderDeviceInfo info;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) {
            // An entry without an id names a device nobody can open -
            // start() is handed the id back, so listing it would only offer
            // a choice that cannot work.
            continue;
        }
        info.id = to_utf8(id);
        CoTaskMemFree(id);
        info.is_default = !info.id.empty() && info.id == default_id;
        info.name = endpoint_display_name(device.Get(), info.id);

        ComPtr<IAudioClient> client;
        if (SUCCEEDED(device->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, &client))) {
            // The shared-mode mix format is what the endpoint actually
            // renders, which is the figure a caller needs to know whether a
            // decoded programme has to be folded down before it is played -
            // see RenderDeviceInfo::channels. A failure here is not an error
            // for this function: the field stays 0 ("cannot say") and the
            // passthrough probes below carry on regardless.
            std::uint32_t mix_rate = 0;
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix)) && mix != nullptr) {
                info.channels = mix->nChannels;
                mix_rate = mix->nSamplesPerSec;
                // The mix format carries a mask only in its EXTENSIBLE form,
                // and then only the one the engine is mixing to: a stereo mix
                // on a 5.1 endpoint says 0x3. The endpoint's own arrangement
                // wins where it has one.
                if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
                    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
                    info.speakers = extensible->dwChannelMask & kSpeakerAllPositions;
                }
                CoTaskMemFree(mix);
            }
            if (const std::uint32_t configured = physical_speakers(device.Get());
                configured != 0 && speaker_count(configured) == info.channels) {
                info.speakers = configured;
            }

            info.sample_rates = probe_sample_rates(
                client.Get(), info.channels != 0 ? info.channels : kCarrierChannels, info.speakers);
            if (info.sample_rates.empty() && mix_rate != 0) {
                // Exclusive mode is unavailable, so the device cannot be asked
                // what it takes. The rate the engine is mixing at is one the
                // endpoint is rendering right now, which is the strongest
                // thing left to say (see RenderDeviceInfo::sample_rates).
                info.sample_rates.push_back(mix_rate);
            }

            // IsFormatSupported is the only honest way to ask "can this
            // endpoint bitstream AC-3 / E-AC-3?" - the answer depends on the
            // driver, the physical connector and the user's per-device
            // settings.
            info.supports_ac3_passthrough =
                client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                          &ac3_format.FormatExt.Format, nullptr) == S_OK;
            info.supports_eac3_passthrough =
                client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                          &eac3_format.FormatExt.Format, nullptr) == S_OK;

            // Control probe with an ordinary exclusive-mode PCM format, so a
            // "no" above can be attributed to the device rather than to
            // exclusive mode being unavailable at all.
            WAVEFORMATEX pcm{};
            pcm.wFormatTag = WAVE_FORMAT_PCM;
            pcm.nChannels = kCarrierChannels;
            pcm.nSamplesPerSec = sample_rate;
            pcm.wBitsPerSample = kCarrierBits;
            pcm.nBlockAlign = static_cast<WORD>(kCarrierChannels * kCarrierBits / 8);
            pcm.nAvgBytesPerSec = sample_rate * pcm.nBlockAlign;
            pcm.cbSize = 0;
            info.supports_exclusive_pcm =
                client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &pcm, nullptr) == S_OK;
        }
        devices.push_back(std::move(info));
    }
    return devices;
}

struct PassthroughSink::Impl {
    std::unique_ptr<ByteRingBuffer> queue;
    std::jthread worker;
    // Raised by the render thread once the device is open. Lowered by stop(),
    // or by the render thread itself when the device goes away under it: the
    // loop's end says how, and why that is the only flag it touches.
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    // Accumulated in bytes, not bursts: the exclusive-mode buffer WASAPI
    // grants (driven by the device's own period) has no reason to align to
    // a whole burst, so a per-callback "bytes this cycle / burst_bytes"
    // would truncate to 0 almost every cycle. stats() converts to bursts.
    std::atomic<std::uint64_t> rendered_bytes{0};
    std::atomic<std::uint64_t> underruns{0};
    // Set by start(); submit()/can_submit() validate against whichever burst
    // size the chosen BitstreamFormat uses.
    std::size_t burst_bytes = iec61937::kBurstBytes;
    // The link's frames: four bytes each, and carrier_ratio() of them to a
    // content frame. Set by start() before the render thread runs.
    std::size_t frame_bytes = kCarrierChannels * (kCarrierBits / 8);
    std::uint32_t ratio = 1;
    // What the render thread last saw of the device, in link frames, for
    // position(): the frames handed over against what GetCurrentPadding said
    // it still held. Only the render thread calls WASAPI - the client lives on
    // it, and the AUDIOSES crash start() describes is what calling it from
    // another thread cost - so position() reads the counter.
    PlaybackCounter counter;
    // IAudioClient::GetStreamLatency, in link frames.
    std::atomic<std::uint32_t> latency{0};
    // Set by pause()/resume() and flush(); acted on by the render thread.
    // `flushes` counts the flushes it has completed, which flush() waits for,
    // and `flush_mark` is how far the queue had been written when the flush
    // was asked for: what it drops.
    std::atomic_bool paused{false};
    std::atomic_bool flushing{false};
    std::atomic<std::uint64_t> flushes{0};
    std::atomic<std::size_t> flush_mark{0};
};

PassthroughSink::PassthroughSink() : impl_(std::make_unique<Impl>()) {}

PassthroughSink::~PassthroughSink() {
    stop();
}

bool PassthroughSink::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

PassthroughStats PassthroughSink::stats() const {
    return {.bursts_submitted = impl_->submitted.load(std::memory_order_relaxed),
            .bursts_rendered =
                impl_->rendered_bytes.load(std::memory_order_relaxed) / impl_->burst_bytes,
            .underruns = impl_->underruns.load(std::memory_order_relaxed)};
}

std::optional<MonitorPosition> PassthroughSink::position() const {
    if (!running() || !impl_->queue) {
        return std::nullopt;
    }
    const std::uint64_t queued_here = impl_->queue->available() / impl_->frame_bytes;
    return per_content_frame(
        impl_->counter.position(queued_here, impl_->latency.load(std::memory_order_relaxed)),
        impl_->ratio);
}

void PassthroughSink::flush() {
    if (!running()) {
        return;
    }
    const std::uint64_t done = impl_->flushes.load(std::memory_order_acquire);
    impl_->flush_mark.store(impl_->queue->write_mark(), std::memory_order_release);
    impl_->flushing.store(true, std::memory_order_release);
    // As MonitorSink::flush(): the render thread stops, resets and restarts
    // the device and drops the queue up to the mark. A render period is 10 ms
    // at most, so this waits far longer than it should need to. Past that the
    // device has stopped answering, and the flush is left for the thread to
    // make when it next runs; the mark keeps it from dropping bursts
    // submitted after this call returned. A thread that has ended because
    // its device went away will not run again, and lowers `running` as it
    // goes, which ends the wait at once.
    for (int waited = 0; waited < 200; ++waited) {
        if (impl_->flushes.load(std::memory_order_acquire) != done || !running()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::expected<void, PassthroughError> PassthroughSink::pause() {
    if (!running()) {
        return std::unexpected(PassthroughError::kNotRunning);
    }
    impl_->paused.store(true, std::memory_order_release);
    return {};
}

std::expected<void, PassthroughError> PassthroughSink::resume() {
    if (!running()) {
        return std::unexpected(PassthroughError::kNotRunning);
    }
    impl_->paused.store(false, std::memory_order_release);
    return {};
}

bool PassthroughSink::paused() const {
    // A pause is a property of a running stream, so one whose device has
    // gone is not paused either.
    return running() && impl_->paused.load(std::memory_order_acquire);
}

bool PassthroughSink::can_submit() const {
    // Nothing takes a burst once the stream has ended, whatever room the
    // queue it leaves behind still has.
    if (!running() || !impl_->queue) {
        return false;
    }
    return impl_->queue->capacity() - impl_->queue->available() > impl_->burst_bytes;
}

bool PassthroughSink::submit(std::span<const std::byte> burst) {
    if (!running() || !impl_->queue || burst.size() != impl_->burst_bytes) {
        return false;
    }
    if (!can_submit()) {
        return false;
    }
    const auto wrote = impl_->queue->write(burst);
    if (wrote != burst.size()) {
        return false;
    }
    impl_->submitted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PassthroughSink::stop() {
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    // A pause is a property of a running stream, so it does not outlive one.
    impl_->paused.store(false, std::memory_order_relaxed);
    impl_->flushing.store(false, std::memory_order_relaxed);
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, PassthroughError> PassthroughSink::start(const std::string& device_id,
                                                             std::uint32_t sample_rate,
                                                             BitstreamFormat format_kind) {
    if (running()) {
        return std::unexpected(PassthroughError::kAlreadyRunning);
    }
    if (is_ac4(format_kind)) {
        return std::unexpected(PassthroughError::kUnsupportedFormat);
    }
    // A render thread that ended because its device went away still holds
    // its client until it is joined, and an exclusive hold can refuse the
    // next Initialize until then. stop() joins it; with nothing started it
    // does nothing.
    stop();

    ComScope com;
    if (!com.ok()) {
        return std::unexpected(PassthroughError::kComFailure);
    }
    auto enumerator = windows_audio::make_enumerator(PassthroughError::kComFailure);
    if (!enumerator) {
        return std::unexpected(enumerator.error());
    }

    ComPtr<IMMDevice> device;
    if (device_id.empty()) {
        if (FAILED((*enumerator)->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
            return std::unexpected(PassthroughError::kDeviceNotFound);
        }
    } else {
        const int wide_len = MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(wide_len > 0 ? wide_len - 1 : 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(), -1, wide.data(), wide_len);
        if (FAILED((*enumerator)->GetDevice(wide.c_str(), &device))) {
            return std::unexpected(PassthroughError::kDeviceNotFound);
        }
    }

    auto format = make_format(format_kind, sample_rate, 6);
    const std::size_t frame_bytes = format.FormatExt.Format.nBlockAlign;

    // Room for roughly a second of bursts, so a caller encoding slightly
    // ahead of real time never has to spin.
    impl_->burst_bytes = max_burst_bytes(format_kind);
    impl_->queue = std::make_unique<ByteRingBuffer>(impl_->burst_bytes * 40);
    impl_->frame_bytes = frame_bytes;
    impl_->ratio = carrier_ratio(format_kind);
    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered_bytes.store(0, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);
    impl_->counter.restart();
    impl_->latency.store(0, std::memory_order_relaxed);
    impl_->paused.store(false, std::memory_order_relaxed);
    impl_->flushing.store(false, std::memory_order_relaxed);
    impl_->flushes.store(0, std::memory_order_relaxed);

    // Activate, IsFormatSupported, Initialize, GetService and Start all run
    // on this one worker thread from here on, never on the thread that calls
    // start(). Every real device this backend had been checked against
    // before an actual AV receiver was cabled to a Windows machine was
    // shared-mode/PCM, where handing the IAudioClient/IAudioRenderClient
    // pointers to a second thread (as MonitorSink still does) works fine.
    // The first real exclusive-mode bitstream endpoint to accept AC-3/E-AC-3
    // crashed inside AUDIOSES.DLL the instant a different thread called
    // Start() on a client Initialize()'d elsewhere - see
    // docs/platforms/windows.md. start() blocks on a promise so it still
    // reports the real open/format-support result synchronously.
    std::promise<std::expected<void, PassthroughError>> ready_promise;
    auto ready_future = ready_promise.get_future();

    impl_->worker = std::jthread([this, device, format, frame_bytes,
                                  promise = std::move(ready_promise)](
                                     const std::stop_token& stop) mutable {
        ComScope thread_com;
        if (!thread_com.ok()) {
            promise.set_value(std::unexpected(PassthroughError::kComFailure));
            return;
        }

        ComPtr<IAudioClient> client;
        if (FAILED(device->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, &client))) {
            promise.set_value(std::unexpected(PassthroughError::kComFailure));
            return;
        }

        if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.FormatExt.Format,
                                      nullptr) != S_OK) {
            promise.set_value(std::unexpected(PassthroughError::kFormatRejected));
            return;
        }
        // The carrier (link) rate, not the content rate: identical to
        // `sample_rate` for AC-3, 4x it for E-AC-3 (make_eac3_format already
        // applied that). GetDevicePeriod/GetBufferSize below deal in frames
        // of this carrier, so the realignment math has to use it too.
        const std::uint32_t carrier_rate = format.FormatExt.Format.nSamplesPerSec;

        REFERENCE_TIME default_period = 0;
        REFERENCE_TIME minimum_period = 0;
        if (FAILED(client->GetDevicePeriod(&default_period, &minimum_period))) {
            promise.set_value(std::unexpected(PassthroughError::kComFailure));
            return;
        }
        REFERENCE_TIME period = default_period;

        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                        &format.FormatExt.Format, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            // The documented realignment dance: ask what buffer size the
            // driver actually wants, convert it back to a period, and
            // re-Initialize on a fresh client (an initialised one cannot be
            // reconfigured).
            UINT32 aligned_frames = 0;
            if (FAILED(client->GetBufferSize(&aligned_frames)) || aligned_frames == 0) {
                promise.set_value(std::unexpected(PassthroughError::kComFailure));
                return;
            }
            period = static_cast<REFERENCE_TIME>(
                10000.0 * 1000 * aligned_frames / carrier_rate + 0.5);
            client.Reset();
            if (FAILED(device->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, &client))) {
                promise.set_value(std::unexpected(PassthroughError::kComFailure));
                return;
            }
            hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    period, period, &format.FormatExt.Format, nullptr);
        }
        if (hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) {
            promise.set_value(std::unexpected(PassthroughError::kExclusiveUnavailable));
            return;
        }
        if (FAILED(hr)) {
            promise.set_value(std::unexpected(PassthroughError::kComFailure));
            return;
        }

        UINT32 buffer_frames = 0;
        if (FAILED(client->GetBufferSize(&buffer_frames))) {
            promise.set_value(std::unexpected(PassthroughError::kComFailure));
            return;
        }

        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ready == nullptr || FAILED(client->SetEventHandle(ready))) {
            if (ready != nullptr) {
                CloseHandle(ready);
            }
            promise.set_value(std::unexpected(PassthroughError::kComFailure));
            return;
        }

        ComPtr<IAudioRenderClient> render;
        if (FAILED(client->GetService(kIidAudioRenderClient, &render))) {
            CloseHandle(ready);
            promise.set_value(std::unexpected(PassthroughError::kComFailure));
            return;
        }

        // GetStreamLatency is in 100 ns units: the delay past the buffer this
        // sink fills, which is what MonitorPosition::latency_frames reports.
        REFERENCE_TIME stream_latency = 0;
        if (SUCCEEDED(client->GetStreamLatency(&stream_latency)) && stream_latency > 0) {
            impl_->latency.store(
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(stream_latency) *
                                           carrier_rate / 10'000'000ULL),
                std::memory_order_relaxed);
        }

        impl_->running.store(true, std::memory_order_release);
        promise.set_value({});

        DWORD mmcss_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_index);

        std::vector<std::byte> chunk;
        std::uint64_t handed_over = 0;
        // Set when the loop ends because the device did rather than because
        // stop() asked it to. A device that will not start has gone, or will
        // not play this stream; no render event would ever come for it.
        bool lost = FAILED(client->Start());
        bool device_running = !lost;

        while (!lost && !stop.stop_requested()) {
            // A pause stops the device and leaves everything else standing:
            // the queue keeps its bursts and goes on taking more. No render
            // event arrives while stopped, so the loop sleeps rather than
            // wait for one - and asks the device whether it is still there,
            // since nothing else would say.
            if (impl_->paused.load(std::memory_order_acquire)) {
                if (device_running) {
                    client->Stop();
                    device_running = false;
                }
                if (!impl_->flushing.load(std::memory_order_acquire)) {
                    UINT32 held = 0;
                    if (stream_gone(client->GetCurrentPadding(&held))) {
                        lost = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
            }
            // A flush drops both buffers: Reset() discards what the device
            // holds, and is only legal while stopped. The counts restart with
            // them, as position() promises.
            if (impl_->flushing.exchange(false, std::memory_order_acq_rel)) {
                if (device_running) {
                    client->Stop();
                    device_running = false;
                }
                client->Reset();
                impl_->queue->discard_to(impl_->flush_mark.load(std::memory_order_acquire));
                handed_over = 0;
                impl_->counter.restart();
                impl_->rendered_bytes.store(0, std::memory_order_relaxed);
                impl_->submitted.store(0, std::memory_order_relaxed);
                impl_->flushes.fetch_add(1, std::memory_order_release);
                continue;
            }
            if (!device_running) {
                // An event signalled before the stop is stale: after a pause
                // the device still holds the buffer it had, and would refuse
                // another until it has played that one. A flush's Stop and
                // Reset on a device that has gone fail quietly, so it is this
                // Start that finds out.
                ResetEvent(ready);
                if (FAILED(client->Start())) {
                    lost = true;
                    break;
                }
                device_running = true;
            }
            if (WaitForSingleObject(ready, 200) != WAIT_OBJECT_0) {
                // Many periods without an event. A device can stall and come
                // back - an HDMI display asleep - but one that has been
                // removed need never signal again, so the wait alone would
                // not notice. The padding is the cheapest thing to ask; only
                // an answer that the stream has gone ends it, since some
                // drivers decline the question (see below).
                UINT32 held = 0;
                if (stream_gone(client->GetCurrentPadding(&held))) {
                    lost = true;
                    break;
                }
                continue;
            }
            // Exclusive event-driven streams take a whole buffer each period,
            // so the padding sizes nothing here; it only tells the counter what
            // the device still holds. A driver that will not say is taken to
            // have played its buffer, which is what the event means.
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) {
                padding = 0;
            }
            impl_->counter.report(handed_over, padding);
            BYTE* target = nullptr;
            const HRESULT buffer_result = render->GetBuffer(buffer_frames, &target);
            if (buffer_result == AUDCLNT_E_BUFFER_TOO_LARGE) {
                // The buffer is not free yet, which only a wake from before a
                // restart can say; the next event is a real one. Anything
                // else is the device failing - AUDCLNT_E_DEVICE_INVALIDATED
                // for one unplugged - and ends the thread.
                continue;
            }
            if (FAILED(buffer_result)) {
                lost = true;
                break;
            }
            const std::size_t wanted = static_cast<std::size_t>(buffer_frames) * frame_bytes;
            chunk.resize(wanted);
            const auto got = impl_->queue->read(chunk);
            if (got < wanted) {
                // Nothing queued: emit silence for the remainder. A receiver
                // that sees a gap in the burst stream usually drops lock, so
                // this is counted, not hidden.
                std::fill(chunk.begin() + static_cast<std::ptrdiff_t>(got), chunk.end(),
                          std::byte{0});
                impl_->underruns.fetch_add(1, std::memory_order_relaxed);
            }
            std::memcpy(target, chunk.data(), wanted);
            render->ReleaseBuffer(buffer_frames, 0);
            handed_over += buffer_frames;
            impl_->counter.report(handed_over, static_cast<std::uint64_t>(padding) + buffer_frames);
            impl_->rendered_bytes.fetch_add(got, std::memory_order_relaxed);
        }

        if (lost) {
            // The stream has ended with its device, and says so the way a
            // stop() would: running() false, so position() reports nothing,
            // submit() refuses, and flush(), pause() and resume() return at
            // once - a flush() already waiting sees it on its next look.
            // Only the flag is touched. The queue's read side and the counts
            // are this thread's to the end, and it has finished with them;
            // `paused` and `flushing` are the caller's, and stop() lowers
            // them when it joins this thread, which it still has to do.
            impl_->running.store(false, std::memory_order_release);
        }
        client->Stop();
        if (mmcss != nullptr) {
            AvRevertMmThreadCharacteristics(mmcss);
        }
        CloseHandle(ready);
    });

    auto result = ready_future.get();
    if (!result) {
        impl_->worker.join();
        impl_->queue.reset();
        return std::unexpected(result.error());
    }
    return {};
}

}  // namespace ac3::audio
