// Spike S5: end-to-end latency of the AC3Forge Crucible, tap to speaker
// (docs/platforms/windows-demo.md, "End-to-end latency, measured").
//
//   s5_latency <runner-pid> [null-sink-substring] [seconds]
//
// This process is an "application": it renders short tone bursts into the
// null sink at known times, the way any game or player would. The demo's
// runner (ac3crucible-run, the given pid) taps it, encodes, and plays its output
// on a real endpoint. A process-loopback tap on the RUNNER captures what it
// rendered, at the mix, with the QPC timestamp WASAPI attaches to every
// capture packet. Both sides are on the same QPC clock: the render side
// through IAudioClock, which maps the frames this process has written to
// the moment they play, and the capture side through the packet position.
// The delay of each burst is the difference; the report is their mean,
// median, spread and count. Burst spacing is pseudo-random (180 to 320 ms)
// so a burst cannot be paired with the wrong one.
//
// What it measures: from the sample leaving this process's render buffer
// to the same sample leaving the runner's render buffer. It does not
// include the endpoint's own DAC delay (the same on both ends) or a
// receiver's decode delay; those are the HDMI run's business.
//
// Throwaway code, like the other spikes: raw WASAPI, no reuse intended.

#include <windows.h>

#include <initguid.h>

#include <wrl/client.h>
#include <wrl/implements.h>

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace {

constexpr unsigned kRate = 48000;
constexpr unsigned kBurstFrames = 240;        // 5 ms of 1 kHz
constexpr double kBurstHz = 1000.0;
constexpr float kBurstAmplitude = 0.8F;
constexpr double kMinGapS = 0.180;
constexpr double kMaxGapS = 0.320;
constexpr float kOnsetThreshold = 0.05F;      // envelope, after a quiet stretch
constexpr double kQuietBeforeOnsetS = 0.100;

std::wstring widen(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    out.resize(out.size() - 1);
    return out;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring friendly_name(IMMDevice* dev) {
    ComPtr<IPropertyStore> store;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &store))) return L"?";
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring name = L"?";
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) name = pv.pwszVal;
    PropVariantClear(&pv);
    return name;
}

ComPtr<IMMDevice> pick_device(IMMDeviceEnumerator* en, const std::wstring& substr) {
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT count = 0;
    col->GetCount(&count);
    const auto needle = lower(substr);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> candidate;
        if (FAILED(col->Item(i, &candidate))) continue;
        if (lower(friendly_name(candidate.Get())).find(needle) != std::wstring::npos) return candidate;
    }
    return nullptr;
}

WAVEFORMATEXTENSIBLE make_float_format(unsigned channels, unsigned rate) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = static_cast<WORD>(channels);
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = static_cast<WORD>(channels * 4);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = channels == 1 ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return f;
}

double qpc_to_s(std::uint64_t hundred_ns) { return static_cast<double>(hundred_ns) * 1e-7; }

// ---------------------------------------------------------------- the burst schedule

struct Schedule {
    std::vector<std::uint64_t> starts;  // frame index of each burst in this process's stream
    explicit Schedule(double seconds) {
        std::uint32_t lcg = 0x5EED1234U;
        double t = 0.5;
        while (t < seconds) {
            starts.push_back(static_cast<std::uint64_t>(t * kRate));
            lcg = lcg * 1664525U + 1013904223U;
            t += kMinGapS + (kMaxGapS - kMinGapS) * (static_cast<double>(lcg >> 8) / 16777216.0);
        }
    }
};

// ---------------------------------------------------------------- render side

struct Renderer {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioClock> clock;
    HANDLE event = nullptr;
    UINT32 buffer_frames = 0;
    unsigned channels = 2;
    bool floating = true;
    std::wstring name;
    std::thread thread;
    std::atomic<bool> stop{false};
    const Schedule* schedule = nullptr;

    // QPC (seconds) at which stream frame `frame` plays, derived from the
    // device clock: taken repeatedly and averaged, since one reading
    // carries the call's own jitter.
    std::mutex map_mutex;
    double map_qpc_s = 0.0;       // QPC seconds ...
    double map_frame = 0.0;       // ... at which this stream frame played
    bool mapped = false;

    HRESULT open(IMMDevice* dev) {
        name = friendly_name(dev);
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return E_FAIL;
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix))) return E_FAIL;
        channels = mix->nChannels;
        floating = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                   (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        if (mix->nSamplesPerSec != kRate) {
            std::fprintf(stderr, "s5_latency: the null sink's mix rate is %lu, not 48000\n", mix->nSamplesPerSec);
        }
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) return hr;
        if (FAILED(hr = client->SetEventHandle(event))) return hr;
        if (FAILED(hr = client->GetBufferSize(&buffer_frames))) return hr;
        if (FAILED(hr = client->GetService(IID_PPV_ARGS(&render)))) return hr;
        if (FAILED(hr = client->GetService(IID_PPV_ARGS(&clock)))) return hr;
        return S_OK;
    }

    void start() {
        thread = std::thread([this] { run(); });
    }

    void run() {
        DWORD task_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
        std::uint64_t written = 0;
        std::size_t next_burst = 0;
        UINT64 clock_freq = 1;
        clock->GetFrequency(&clock_freq);
        // Pre-fill with silence so the first event has a stable buffer.
        BYTE* data = nullptr;
        if (SUCCEEDED(render->GetBuffer(buffer_frames, &data))) {
            render->ReleaseBuffer(buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
            written += buffer_frames;
        }
        client->Start();
        int readings = 0;
        double sum_qpc = 0.0, sum_frame = 0.0;
        while (!stop.load()) {
            if (WaitForSingleObject(event, 2000) != WAIT_OBJECT_0) continue;
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;
            const UINT32 avail = buffer_frames - padding;
            if (avail == 0) continue;
            if (FAILED(render->GetBuffer(avail, &data))) break;
            for (UINT32 i = 0; i < avail; ++i) {
                const std::uint64_t frame = written + i;
                float v = 0.0F;
                while (next_burst < schedule->starts.size() && frame >= schedule->starts[next_burst] + kBurstFrames) {
                    ++next_burst;
                }
                if (next_burst < schedule->starts.size() && frame >= schedule->starts[next_burst]) {
                    const auto k = static_cast<double>(frame - schedule->starts[next_burst]);
                    v = kBurstAmplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * kBurstHz * k / kRate));
                }
                for (unsigned c = 0; c < channels; ++c) {
                    if (floating) {
                        reinterpret_cast<float*>(data)[i * channels + c] = v;
                    } else {
                        reinterpret_cast<std::int16_t*>(data)[i * channels + c] = static_cast<std::int16_t>(v * 32767.0F);
                    }
                }
            }
            render->ReleaseBuffer(avail, 0);
            written += avail;

            // The device clock: position (in clock units) and the QPC at
            // which it was read, i.e. "stream frame P is playing now".
            UINT64 pos = 0, qpc = 0;
            if (SUCCEEDED(clock->GetPosition(&pos, &qpc)) && clock_freq > 0) {
                const double frames_played = static_cast<double>(pos) * static_cast<double>(kRate) / static_cast<double>(clock_freq);
                if (frames_played > 0.0) {
                    sum_qpc += qpc_to_s(qpc);
                    sum_frame += frames_played;
                    ++readings;
                    const std::lock_guard lock(map_mutex);
                    map_qpc_s = sum_qpc / readings;
                    map_frame = sum_frame / readings;
                    mapped = true;
                }
            }
        }
        client->Stop();
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }

    // When did stream frame `frame` play, in QPC seconds?
    bool play_time(std::uint64_t frame, double& out) {
        const std::lock_guard lock(map_mutex);
        if (!mapped) return false;
        out = map_qpc_s + (static_cast<double>(frame) - map_frame) / kRate;
        return true;
    }

    void close() {
        stop.store(true);
        if (thread.joinable()) thread.join();
        if (event) CloseHandle(event);
    }
};

// ---------------------------------------------------------------- capture side

struct ActivationHandler
    : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Microsoft::WRL::FtmBase,
                                   IActivateAudioInterfaceCompletionHandler> {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;
    ~ActivationHandler() override { CloseHandle(done); }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT activate_hr = E_FAIL;
        ComPtr<IUnknown> unk;
        op->GetActivateResult(&activate_hr, &unk);
        result = activate_hr;
        if (SUCCEEDED(result) && unk) result = unk.As(&client);
        SetEvent(done);
        return S_OK;
    }
};

struct Onset {
    double qpc_s;
};

struct Tap {
    DWORD pid = 0;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE event = nullptr;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::mutex onsets_mutex;
    std::vector<Onset> onsets;
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> silent_frames{0};
    // envelope state
    float envelope = 0.0F;
    double quiet_frames = 0.0;

    HRESULT open() {
        AUDIOCLIENT_ACTIVATION_PARAMS params{};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        params.ProcessLoopbackParams.TargetProcessId = pid;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(params);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
        auto handler = Make<ActivationHandler>();
        ComPtr<IActivateAudioInterfaceAsyncOperation> op;
        HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv,
                                                 handler.Get(), &op);
        if (FAILED(hr)) return hr;
        if (WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (FAILED(handler->result)) return handler->result;
        client = handler->client;
        auto fmt = make_float_format(1, kRate);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                0, 0, &fmt.Format, nullptr);
        if (FAILED(hr)) return hr;
        if (FAILED(hr = client->SetEventHandle(event))) return hr;
        if (FAILED(hr = client->GetService(IID_PPV_ARGS(&capture)))) return hr;
        if (FAILED(hr = client->Start())) return hr;
        thread = std::thread([this] { run(); });
        return S_OK;
    }

    void run() {
        DWORD task_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
        constexpr float kAttack = 0.3F;
        constexpr float kRelease = 0.002F;
        while (!stop.load()) {
            if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) continue;
            UINT32 next = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&next)) && next > 0) {
                BYTE* data = nullptr;
                UINT32 n = 0;
                DWORD flags = 0;
                UINT64 device_position = 0, qpc = 0;
                if (FAILED(capture->GetBuffer(&data, &n, &flags, &device_position, &qpc))) break;
                frames.fetch_add(n);
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                if (silent) silent_frames.fetch_add(n);
                const float* f = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < n; ++i) {
                    const float v = silent ? 0.0F : std::fabs(f[i]);
                    envelope += (v > envelope ? kAttack : kRelease) * (v - envelope);
                    if (envelope > kOnsetThreshold) {
                        if (quiet_frames >= kQuietBeforeOnsetS * kRate) {
                            const std::lock_guard lock(onsets_mutex);
                            onsets.push_back({.qpc_s = qpc_to_s(qpc) + static_cast<double>(i) / kRate});
                        }
                        quiet_frames = 0.0;
                    } else {
                        quiet_frames += 1.0;
                    }
                }
                capture->ReleaseBuffer(n);
            }
        }
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }

    void close() {
        stop.store(true);
        if (thread.joinable()) thread.join();
        if (client) client->Stop();
        if (event) CloseHandle(event);
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: s5_latency <runner-pid> [null-sink-substring] [seconds]\n");
        return 2;
    }
    // "self": tap this process instead of a runner, which measures the
    // render-to-loopback path alone and calibrates the rest.
    const DWORD runner_pid = std::string(argv[1]) == "self" ? GetCurrentProcessId()
                                                             : static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10));
    const std::wstring sink = argc > 2 ? widen(argv[2]) : L"FxSound";
    const double seconds = argc > 3 ? std::atof(argv[3]) : 20.0;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return 2;
    auto dev = pick_device(en.Get(), sink);
    if (!dev) {
        std::fprintf(stderr, "s5_latency: no render endpoint matching \"%ls\"\n", sink.c_str());
        return 2;
    }

    const Schedule schedule(seconds);
    Renderer renderer;
    renderer.schedule = &schedule;
    if (HRESULT hr = renderer.open(dev.Get()); FAILED(hr)) {
        std::fprintf(stderr, "s5_latency: render open failed 0x%08lx\n", static_cast<unsigned long>(hr));
        return 2;
    }
    Tap tap;
    tap.pid = runner_pid;
    if (HRESULT hr = tap.open(); FAILED(hr)) {
        std::fprintf(stderr, "s5_latency: tap on pid %lu failed 0x%08lx\n", static_cast<unsigned long>(runner_pid),
                     static_cast<unsigned long>(hr));
        return 2;
    }
    std::printf("s5_latency: pid=%lu bursts into \"%ls\", tapping runner pid %lu, %.0f s, %zu bursts\n",
                GetCurrentProcessId(), renderer.name.c_str(), static_cast<unsigned long>(runner_pid), seconds,
                schedule.starts.size());
    std::fflush(stdout);
    renderer.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0) + 1500));
    renderer.close();
    tap.close();

    // Pair every observed onset with the burst that played most recently
    // before it (delays are positive and under a second by construction).
    std::vector<double> delays_ms;
    {
        const std::lock_guard lock(tap.onsets_mutex);
        for (const auto& onset : tap.onsets) {
            double best = -1.0;
            for (const auto start : schedule.starts) {
                double played = 0.0;
                if (!renderer.play_time(start, played)) break;
                const double d = onset.qpc_s - played;
                if (d >= 0.0 && d < 1.0 && (best < 0.0 || d < best)) best = d;
            }
            if (best >= 0.0) delays_ms.push_back(best * 1000.0);
        }
    }
    std::printf("captured frames=%llu silent=%llu onsets=%zu paired=%zu\n",
                static_cast<unsigned long long>(tap.frames.load()), static_cast<unsigned long long>(tap.silent_frames.load()),
                tap.onsets.size(), delays_ms.size());
    if (delays_ms.empty()) {
        std::printf("no bursts observed: is the runner tapping this process and playing PCM?\n");
        CoUninitialize();
        return 1;
    }
    std::sort(delays_ms.begin(), delays_ms.end());
    double sum = 0.0;
    for (const double d : delays_ms) sum += d;
    const double mean = sum / static_cast<double>(delays_ms.size());
    double var = 0.0;
    for (const double d : delays_ms) var += (d - mean) * (d - mean);
    const double sd = std::sqrt(var / static_cast<double>(delays_ms.size()));
    const double median = delays_ms[delays_ms.size() / 2];
    std::printf("RESULT bursts=%zu mean=%.1fms median=%.1fms min=%.1fms max=%.1fms sd=%.1fms\n", delays_ms.size(), mean,
                median, delays_ms.front(), delays_ms.back(), sd);
    CoUninitialize();
    return 0;
}
