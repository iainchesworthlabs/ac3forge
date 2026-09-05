// Spike S1 helper: a process that renders one sine tone through WASAPI shared
// mode, so s1_taps has something with a known frequency to tap by PID.
//
//   tone_player <freq_hz> [device-name-substring|""] [seconds]
//
// Prints one line describing what it opened, then plays until the deadline or
// until it is terminated by the parent. Throwaway code: no reuse intended.

#include <windows.h>

#include <initguid.h>  // PKEY_* / KSDATAFORMAT_* get defined here, not just declared

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <numbers>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

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
    ComPtr<IMMDevice> dev;
    if (substr.empty()) {
        en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        return dev;
    }
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

bool is_float(const WAVEFORMATEX* fmt) {
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const double freq = argc > 1 ? std::atof(argv[1]) : 440.0;
    const std::wstring device_substr = argc > 2 ? widen(argv[2]) : L"";
    const double seconds = argc > 3 ? std::atof(argv[3]) : 3600.0;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;

    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        std::fprintf(stderr, "tone_player: no device enumerator\n");
        return 2;
    }
    auto dev = pick_device(en.Get(), device_substr);
    if (!dev) {
        std::fprintf(stderr, "tone_player: no render device matching \"%ls\"\n", device_substr.c_str());
        return 2;
    }

    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
        std::fprintf(stderr, "tone_player: IAudioClient activation failed\n");
        return 2;
    }
    WAVEFORMATEX* fmt = nullptr;
    if (FAILED(client->GetMixFormat(&fmt))) return 2;

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, fmt, nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "tone_player: Initialize failed 0x%08lx\n", static_cast<unsigned long>(hr));
        return 2;
    }
    client->SetEventHandle(ev);
    UINT32 buffer_frames = 0;
    client->GetBufferSize(&buffer_frames);
    ComPtr<IAudioRenderClient> render;
    if (FAILED(client->GetService(IID_PPV_ARGS(&render)))) return 2;

    const bool floating = is_float(fmt);
    const unsigned channels = fmt->nChannels;
    std::printf("pid=%lu device=\"%ls\" rate=%lu ch=%u bits=%u float=%d freq=%.1f\n", GetCurrentProcessId(),
                friendly_name(dev.Get()).c_str(), fmt->nSamplesPerSec, channels, fmt->wBitsPerSample, floating ? 1 : 0,
                freq);
    std::fflush(stdout);

    double phase = 0.0;
    const double inc = 2.0 * std::numbers::pi * freq / static_cast<double>(fmt->nSamplesPerSec);
    constexpr double kAmplitude = 0.25;

    client->Start();
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.0);
    while (GetTickCount64() < deadline) {
        WaitForSingleObject(ev, 2000);
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        const UINT32 avail = buffer_frames - padding;
        if (avail == 0) continue;
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(avail, &data))) break;
        for (UINT32 i = 0; i < avail; ++i) {
            const double v = kAmplitude * std::sin(phase);
            phase += inc;
            if (phase > 2.0 * std::numbers::pi) phase -= 2.0 * std::numbers::pi;
            for (unsigned c = 0; c < channels; ++c) {
                if (floating) {
                    reinterpret_cast<float*>(data)[i * channels + c] = static_cast<float>(v);
                } else {
                    reinterpret_cast<int16_t*>(data)[i * channels + c] = static_cast<int16_t>(v * 32767.0);
                }
            }
        }
        render->ReleaseBuffer(avail, 0);
    }
    client->Stop();
    CoTaskMemFree(fmt);
    CloseHandle(ev);
    CoUninitialize();
    return 0;
}
