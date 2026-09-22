// Spike S1: does Windows 11 process-loopback capture do what the Desktop Atmos
// Demo plan needs? (docs/platforms/windows-demo.md, "Phase 0: spikes")
//
// Questions, each a flag below:
//   - can N processes be tapped at once, and what format arrives?      --spawn N
//   - does a tap keep delivering when the app's session is muted?      --mute-at S
//   - does it keep delivering when the app renders to another
//     endpoint (e.g. an idle FxSound virtual device)?                   --device SUBSTR
//   - does it keep delivering when we hold the default endpoint in
//     exclusive mode, the way PassthroughSink would?                    --exclusive-at S
//   - does a tap created before the app starts playing pick it up?     --taps-first
//
// tone_player.exe (next to this binary) renders a sine of a known frequency, so
// separation is proven by each tap's estimated frequency matching the process
// it was pointed at. Throwaway code: no reuse intended.

#include <windows.h>

#include <initguid.h>  // PKEY_* / KSDATAFORMAT_* get defined here, not just declared

// WRL first: it reaches cguid.h, whose GUID_NULL declaration breaks if ks.h's
// GUID_NULL macro (via the audio headers below) is already defined.
#include <wrl/client.h>
#include <wrl/implements.h>

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <avrt.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace {

// ---------------------------------------------------------------- utilities

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

std::wstring process_image(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"?";
    wchar_t buf[MAX_PATH * 2];
    DWORD len = static_cast<DWORD>(std::size(buf));
    std::wstring out = L"?";
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
        out = buf;
        const auto slash = out.find_last_of(L"\\/");
        if (slash != std::wstring::npos) out = out.substr(slash + 1);
    }
    CloseHandle(h);
    return out;
}

double now_s() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
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
    f.dwChannelMask = channels == 8   ? KSAUDIO_SPEAKER_7POINT1_SURROUND
                      : channels == 6 ? KSAUDIO_SPEAKER_5POINT1
                      : channels == 1 ? KSAUDIO_SPEAKER_MONO
                                      : KSAUDIO_SPEAKER_STEREO;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return f;
}

// ---------------------------------------------------------------- sessions

struct SessionInfo {
    std::wstring device;
    DWORD pid = 0;
    AudioSessionState state = AudioSessionStateInactive;
    std::wstring display;
    ComPtr<ISimpleAudioVolume> volume;
};

std::vector<SessionInfo> enumerate_sessions(IMMDeviceEnumerator* en) {
    std::vector<SessionInfo> out;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev))) continue;
        ComPtr<IAudioSessionManager2> mgr;
        if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &mgr))) continue;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(mgr->GetSessionEnumerator(&sessions))) continue;
        int n = 0;
        sessions->GetCount(&n);
        const auto dev_name = friendly_name(dev.Get());
        for (int s = 0; s < n; ++s) {
            ComPtr<IAudioSessionControl> ctl;
            if (FAILED(sessions->GetSession(s, &ctl))) continue;
            ComPtr<IAudioSessionControl2> ctl2;
            if (FAILED(ctl.As(&ctl2))) continue;
            SessionInfo info;
            info.device = dev_name;
            ctl2->GetProcessId(&info.pid);
            ctl->GetState(&info.state);
            LPWSTR name = nullptr;
            if (SUCCEEDED(ctl->GetDisplayName(&name)) && name) {
                info.display = name;
                CoTaskMemFree(name);
            }
            ctl.As(&info.volume);
            out.push_back(std::move(info));
        }
    }
    return out;
}

const char* state_name(AudioSessionState s) {
    switch (s) {
        case AudioSessionStateActive: return "active";
        case AudioSessionStateInactive: return "inactive";
        case AudioSessionStateExpired: return "expired";
    }
    return "?";
}

void list_sessions(IMMDeviceEnumerator* en) {
    for (const auto& s : enumerate_sessions(en)) {
        std::printf("  %-8s pid=%-6lu %-28ls %-40ls \"%ls\"\n", state_name(s.state), s.pid, process_image(s.pid).c_str(),
                    s.device.c_str(), s.display.c_str());
    }
}

bool set_session_mute(IMMDeviceEnumerator* en, DWORD pid, bool mute) {
    bool any = false;
    for (auto& s : enumerate_sessions(en)) {
        if (s.pid != pid || !s.volume) continue;
        if (SUCCEEDED(s.volume->SetMute(mute ? TRUE : FALSE, nullptr))) any = true;
    }
    return any;
}

// ---------------------------------------------------------------- taps

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

struct Tap {
    DWORD pid = 0;
    double expected_hz = 0.0;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE event = nullptr;
    std::thread thread;
    std::atomic<bool> stop{false};

    // cumulative, written by the capture thread, read by the reporter
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> silent_frames{0};
    std::atomic<uint64_t> packets{0};
    std::atomic<uint64_t> crossings{0};
    std::atomic<double> sum_sq{0.0};
    std::atomic<double> first_packet_s{-1.0};
    float last_sample = 0.0f;
    HRESULT init_hr = E_PENDING;
    unsigned channels = 2;

    HRESULT open(unsigned request_channels) {
        channels = request_channels;
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
        if (FAILED(hr)) return init_hr = hr;
        if (WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0) return init_hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (FAILED(handler->result)) return init_hr = handler->result;
        client = handler->client;

        auto fmt = make_float_format(channels, 48000);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                0, 0, &fmt.Format, nullptr);
        if (FAILED(hr)) return init_hr = hr;
        if (FAILED(hr = client->SetEventHandle(event))) return init_hr = hr;
        if (FAILED(hr = client->GetService(IID_PPV_ARGS(&capture)))) return init_hr = hr;
        if (FAILED(hr = client->Start())) return init_hr = hr;
        thread = std::thread([this] { run(); });
        return init_hr = S_OK;
    }

    void run() {
        DWORD task_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
        while (!stop.load()) {
            if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) continue;
            UINT32 next = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&next)) && next > 0) {
                BYTE* data = nullptr;
                UINT32 n = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &n, &flags, nullptr, nullptr))) break;
                if (first_packet_s.load() < 0.0) first_packet_s.store(now_s());
                packets.fetch_add(1);
                frames.fetch_add(n);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    silent_frames.fetch_add(n);
                } else {
                    const float* f = reinterpret_cast<const float*>(data);
                    double sq = 0.0;
                    uint64_t zc = 0;
                    float prev = last_sample;
                    for (UINT32 i = 0; i < n; ++i) {
                        const float v = f[i * channels];
                        sq += static_cast<double>(v) * v;
                        if ((prev < 0.0f && v >= 0.0f) || (prev >= 0.0f && v < 0.0f)) ++zc;
                        prev = v;
                    }
                    last_sample = prev;
                    crossings.fetch_add(zc);
                    double old = sum_sq.load();
                    while (!sum_sq.compare_exchange_weak(old, old + sq)) {
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

// ---------------------------------------------------------------- exclusive holder

struct ExclusiveHolder {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    HANDLE event = nullptr;
    std::thread thread;
    std::atomic<bool> stop{false};
    UINT32 buffer_frames = 0;
    WORD block_align = 4;
    std::wstring device_name;

    HRESULT start(IMMDeviceEnumerator* en) {
        ComPtr<IMMDevice> dev;
        HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (FAILED(hr)) return hr;
        device_name = friendly_name(dev.Get());
        if (FAILED(hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return hr;

        // A plain PCM shape most endpoints accept exclusively; try a few.
        struct Shape {
            unsigned rate;
            unsigned bits;
        };
        const Shape shapes[] = {{48000, 16}, {48000, 24}, {44100, 16}, {48000, 32}};
        WAVEFORMATEXTENSIBLE fmt{};
        bool found = false;
        for (const auto& s : shapes) {
            fmt = {};
            fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            fmt.Format.nChannels = 2;
            fmt.Format.nSamplesPerSec = s.rate;
            fmt.Format.wBitsPerSample = static_cast<WORD>(s.bits == 24 ? 32 : s.bits);
            fmt.Format.nBlockAlign = static_cast<WORD>(2 * fmt.Format.wBitsPerSample / 8);
            fmt.Format.nAvgBytesPerSec = s.rate * fmt.Format.nBlockAlign;
            fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            fmt.Samples.wValidBitsPerSample = static_cast<WORD>(s.bits);
            fmt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
            fmt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &fmt.Format, nullptr) == S_OK) {
                found = true;
                break;
            }
        }
        if (!found) return AUDCLNT_E_UNSUPPORTED_FORMAT;
        block_align = fmt.Format.nBlockAlign;

        REFERENCE_TIME def = 0, min = 0;
        client->GetDevicePeriod(&def, &min);
        hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, def, def, &fmt.Format,
                                nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 frames = 0;
            client->GetBufferSize(&frames);
            const auto aligned = static_cast<REFERENCE_TIME>(
                10000.0 * 1000.0 * static_cast<double>(frames) / static_cast<double>(fmt.Format.nSamplesPerSec) + 0.5);
            client.Reset();
            if (FAILED(hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return hr;
            hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, aligned, aligned,
                                    &fmt.Format, nullptr);
        }
        if (FAILED(hr)) return hr;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(hr = client->SetEventHandle(event))) return hr;
        if (FAILED(hr = client->GetBufferSize(&buffer_frames))) return hr;
        if (FAILED(hr = client->GetService(IID_PPV_ARGS(&render)))) return hr;
        BYTE* data = nullptr;
        if (SUCCEEDED(render->GetBuffer(buffer_frames, &data))) render->ReleaseBuffer(buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(hr = client->Start())) return hr;
        thread = std::thread([this] {
            while (!stop.load()) {
                if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) continue;
                BYTE* p = nullptr;
                if (SUCCEEDED(render->GetBuffer(buffer_frames, &p))) render->ReleaseBuffer(buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        });
        return S_OK;
    }

    void finish() {
        stop.store(true);
        if (thread.joinable()) thread.join();
        if (client) client->Stop();
        if (event) CloseHandle(event);
    }
};

// Just the capability probe enumerate_render_devices() does, on the default
// endpoint, with no Initialize: is *asking* enough to disturb live streams?
std::string probe_exclusive(IMMDeviceEnumerator* en) {
    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return "no default";
    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return "no client";
    std::string out;
    const unsigned rates[] = {48000, 44100};
    const unsigned bits[] = {16, 24, 32};
    for (unsigned rate : rates) {
        for (unsigned b : bits) {
            WAVEFORMATEXTENSIBLE fmt{};
            fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            fmt.Format.nChannels = 2;
            fmt.Format.nSamplesPerSec = rate;
            fmt.Format.wBitsPerSample = static_cast<WORD>(b == 24 ? 32 : b);
            fmt.Format.nBlockAlign = static_cast<WORD>(2 * fmt.Format.wBitsPerSample / 8);
            fmt.Format.nAvgBytesPerSec = rate * fmt.Format.nBlockAlign;
            fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            fmt.Samples.wValidBitsPerSample = static_cast<WORD>(b);
            fmt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
            fmt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            const HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &fmt.Format, nullptr);
            char buf[48];
            std::snprintf(buf, sizeof buf, "%u/%u=%s ", rate, b, hr == S_OK ? "yes" : "no");
            out += buf;
        }
    }
    return out;
}

// ---------------------------------------------------------------- spawning

struct Child {
    PROCESS_INFORMATION pi{};
    double freq = 0.0;
};

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring s = buf;
    return s.substr(0, s.find_last_of(L"\\/"));
}

bool spawn_tone(Child& child, double freq, const std::wstring& device, double seconds) {
    std::wstring cmd = L"\"" + exe_dir() + L"\\tone_player.exe\" " + std::to_wstring(freq) + L" \"" + device + L"\" " +
                       std::to_wstring(seconds);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    child.freq = freq;
    return CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &child.pi) != 0;
}

// ---------------------------------------------------------------- main

struct Options {
    bool list = false;
    int spawn = 0;
    std::vector<DWORD> pids;
    std::wstring device;
    double seconds = 8.0;
    double mute_at = -1.0;
    double unmute_at = -1.0;
    double exclusive_at = -1.0;
    double probe_at = -1.0;
    bool taps_first = false;
    unsigned channels = 2;
};

void usage() {
    std::puts(
        "s1_taps --list\n"
        "s1_taps [--spawn N] [--pid P]... [--device SUBSTR] [--seconds S] [--channels C]\n"
        "        [--mute-at S] [--unmute-at S] [--exclusive-at S] [--taps-first]\n"
        "  --spawn N        launch N tone_player processes (250 Hz, 500 Hz, ...) and tap each\n"
        "  --pid P          tap an existing process (repeatable)\n"
        "  --device SUBSTR  spawned players render to the endpoint whose name contains SUBSTR\n"
        "  --channels C     request C channels from each tap (2, 6 or 8)\n"
        "  --mute-at S      mute tap 0's audio session at S seconds\n"
        "  --unmute-at S    unmute it again\n"
        "  --exclusive-at S hold the default endpoint exclusively from S seconds on\n"
        "  --taps-first     create taps before spawning the players");
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--list") opt.list = true;
        else if (a == "--spawn") opt.spawn = std::atoi(next("--spawn"));
        else if (a == "--pid") opt.pids.push_back(static_cast<DWORD>(std::strtoul(next("--pid"), nullptr, 10)));
        else if (a == "--device") opt.device = widen(next("--device"));
        else if (a == "--seconds") opt.seconds = std::atof(next("--seconds"));
        else if (a == "--channels") opt.channels = static_cast<unsigned>(std::atoi(next("--channels")));
        else if (a == "--mute-at") opt.mute_at = std::atof(next("--mute-at"));
        else if (a == "--unmute-at") opt.unmute_at = std::atof(next("--unmute-at"));
        else if (a == "--exclusive-at") opt.exclusive_at = std::atof(next("--exclusive-at"));
        else if (a == "--probe-at") opt.probe_at = std::atof(next("--probe-at"));
        else if (a == "--taps-first") opt.taps_first = true;
        else {
            usage();
            return 2;
        }
    }
    if (!opt.list && opt.spawn == 0 && opt.pids.empty()) {
        usage();
        return 2;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return 2;

    if (opt.list) {
        std::puts("audio sessions on active render endpoints:");
        list_sessions(en.Get());
        return 0;
    }

    ComPtr<IMMDevice> def;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def)))
        std::printf("default render endpoint: \"%ls\"\n", friendly_name(def.Get()).c_str());

    std::vector<Child> children;
    std::vector<Tap> taps(static_cast<size_t>(opt.spawn) + opt.pids.size());
    size_t t = 0;
    for (DWORD pid : opt.pids) taps[t++].pid = pid;

    auto spawn_all = [&] {
        for (int k = 0; k < opt.spawn; ++k) {
            Child c;
            const double freq = 250.0 * (k + 1);
            if (!spawn_tone(c, freq, opt.device, opt.seconds + 5.0)) {
                std::fprintf(stderr, "spawn %d failed (%lu)\n", k, GetLastError());
                std::exit(2);
            }
            children.push_back(c);
        }
    };
    auto assign_children = [&] {
        for (size_t k = 0; k < children.size(); ++k) {
            taps[opt.pids.size() + k].pid = children[k].pi.dwProcessId;
            taps[opt.pids.size() + k].expected_hz = children[k].freq;
        }
    };
    auto open_all = [&] {
        for (auto& tap : taps) {
            const HRESULT hr = tap.open(opt.channels);
            std::printf("tap pid=%-6lu %-24ls expect=%6.0f Hz  open=0x%08lx %s\n", tap.pid, process_image(tap.pid).c_str(),
                        tap.expected_hz, static_cast<unsigned long>(hr), SUCCEEDED(hr) ? "ok" : "FAILED");
        }
    };

    if (opt.taps_first && opt.spawn > 0) {
        // The PIDs don't exist yet, so spawn suspended, tap, then resume.
        for (int k = 0; k < opt.spawn; ++k) {
            Child c;
            const double freq = 250.0 * (k + 1);
            std::wstring cmd = L"\"" + exe_dir() + L"\\tone_player.exe\" " + std::to_wstring(freq) + L" \"" + opt.device +
                               L"\" " + std::to_wstring(opt.seconds + 5.0);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            c.freq = freq;
            if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                nullptr, &si, &c.pi)) {
                std::fprintf(stderr, "spawn %d failed (%lu)\n", k, GetLastError());
                return 2;
            }
            children.push_back(c);
        }
        assign_children();
        open_all();
        for (auto& c : children) ResumeThread(c.pi.hThread);
        std::puts("players resumed after taps were opened");
    } else {
        spawn_all();
        assign_children();
        if (!children.empty()) Sleep(1500);
        open_all();
    }
    std::fflush(stdout);

    ExclusiveHolder holder;
    bool muted = false, unmuted = false, exclusive = false;
    HRESULT exclusive_hr = E_PENDING;

    struct Snapshot {
        uint64_t frames = 0, silent = 0, crossings = 0;
        double sum_sq = 0.0;
        double at = 0.0;
    };
    std::vector<Snapshot> prev(taps.size());
    const double start = now_s();
    for (auto& p : prev) p.at = start;

    std::printf("\n%6s  %-6s  %9s  %8s  %8s  %7s  %s\n", "t(s)", "pid", "frames/s", "silent/s", "rms dBFS", "est Hz", "note");
    int second = 0;
    while (now_s() - start < opt.seconds) {
        Sleep(1000);
        ++second;
        const double t_rel = now_s() - start;
        std::string note;
        if (opt.mute_at >= 0 && !muted && t_rel >= opt.mute_at) {
            muted = true;
            const bool ok = set_session_mute(en.Get(), taps[0].pid, true);
            note += ok ? "[muted tap0 session] " : "[mute FAILED: no session for tap0] ";
        }
        if (opt.unmute_at >= 0 && muted && !unmuted && t_rel >= opt.unmute_at) {
            unmuted = true;
            set_session_mute(en.Get(), taps[0].pid, false);
            note += "[unmuted tap0 session] ";
        }
        static bool probed = false;
        if (opt.probe_at >= 0 && !probed && t_rel >= opt.probe_at) {
            probed = true;
            note += "[probe-only: " + probe_exclusive(en.Get()) + "] ";
        }
        if (opt.exclusive_at >= 0 && !exclusive && t_rel >= opt.exclusive_at) {
            exclusive = true;
            exclusive_hr = holder.start(en.Get());
            char buf[160];
            std::snprintf(buf, sizeof buf, "[exclusive on \"%ls\": 0x%08lx %s] ", holder.device_name.c_str(),
                          static_cast<unsigned long>(exclusive_hr), SUCCEEDED(exclusive_hr) ? "HELD" : "refused");
            note += buf;
        }
        for (size_t k = 0; k < taps.size(); ++k) {
            auto& tap = taps[k];
            Snapshot cur{tap.frames.load(), tap.silent_frames.load(), tap.crossings.load(), tap.sum_sq.load(), now_s()};
            const auto& p = prev[k];
            const double dt = cur.at - p.at;
            const uint64_t df = cur.frames - p.frames;
            const uint64_t ds = cur.silent - p.silent;
            const uint64_t audible = df - ds;
            const double rms = audible ? std::sqrt((cur.sum_sq - p.sum_sq) / static_cast<double>(audible)) : 0.0;
            const double dbfs = rms > 0.0 ? 20.0 * std::log10(rms) : -120.0;
            const double hz = audible ? static_cast<double>(cur.crossings - p.crossings) / 2.0 / dt : 0.0;
            std::printf("%6.1f  %-6lu  %9.0f  %8.0f  %8.1f  %7.0f  %s\n", t_rel, tap.pid, df / dt, ds / dt, dbfs, hz,
                        k == 0 ? note.c_str() : "");
            prev[k] = cur;
        }
        std::fflush(stdout);
    }

    std::puts("\nsummary:");
    for (auto& tap : taps) {
        const uint64_t f = tap.frames.load();
        std::printf("  pid=%-6lu open=0x%08lx frames=%llu silent=%llu packets=%llu first-packet=%.2fs%s\n", tap.pid,
                    static_cast<unsigned long>(tap.init_hr), static_cast<unsigned long long>(f),
                    static_cast<unsigned long long>(tap.silent_frames.load()),
                    static_cast<unsigned long long>(tap.packets.load()),
                    tap.first_packet_s.load() < 0 ? -1.0 : tap.first_packet_s.load() - start,
                    f == 0 ? "  <-- NOTHING ARRIVED" : "");
    }
    if (exclusive)
        std::printf("  exclusive hold on default endpoint: 0x%08lx (%s)\n", static_cast<unsigned long>(exclusive_hr),
                    SUCCEEDED(exclusive_hr) ? "held" : "refused");

    for (auto& tap : taps) tap.close();
    holder.finish();
    if (muted && !unmuted) set_session_mute(en.Get(), taps[0].pid, false);
    for (auto& c : children) {
        DWORD code = 0;
        GetExitCodeProcess(c.pi.hProcess, &code);
        std::printf("  player pid=%-6lu %s\n", c.pi.dwProcessId,
                    code == STILL_ACTIVE ? "still running" : "EXITED before the run ended");
    }
    for (auto& c : children) {
        TerminateProcess(c.pi.hProcess, 0);
        CloseHandle(c.pi.hThread);
        CloseHandle(c.pi.hProcess);
    }
    CoUninitialize();
    return 0;
}
