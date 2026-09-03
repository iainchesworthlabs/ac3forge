// Spike S5 helper: what shared-mode periods the audio engine offers a render
// endpoint for the demo's own stream format (float32, 48 kHz, stereo or 5.1),
// and whether it will open the stream at the smallest one. Answers the
// question the low-latency monitor sink depends on.
//
//   period_probe [device-name-substring] [channels]

#include <windows.h>

#include <initguid.h>

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cwctype>
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

}  // namespace

int main(int argc, char** argv) {
    const std::wstring needle = lower(argc > 1 ? widen(argv[1]) : L"Realtek");
    const unsigned channels = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 2;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return 2;
    ComPtr<IMMDeviceCollection> col;
    en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    UINT count = 0;
    col->GetCount(&count);
    ComPtr<IMMDevice> dev;
    for (UINT i = 0; i < count && !dev; ++i) {
        ComPtr<IMMDevice> c;
        col->Item(i, &c);
        if (lower(friendly_name(c.Get())).find(needle) != std::wstring::npos) dev = c;
    }
    if (!dev) {
        std::printf("no endpoint matching the substring\n");
        return 2;
    }
    std::printf("endpoint: %ls\n", friendly_name(dev.Get()).c_str());

    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return 2;
    WAVEFORMATEX* mix = nullptr;
    client->GetMixFormat(&mix);
    std::printf("mix format: %lu Hz, %u ch, %u bits, tag 0x%x\n", mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample,
                mix->wFormatTag);
    REFERENCE_TIME def = 0, min = 0;
    client->GetDevicePeriod(&def, &min);
    std::printf("IAudioClient periods: default %.2f ms, minimum %.2f ms\n", def / 10000.0, min / 10000.0);

    WAVEFORMATEXTENSIBLE fmt{};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = static_cast<WORD>(channels);
    fmt.Format.nSamplesPerSec = 48000;
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = static_cast<WORD>(channels * 4);
    fmt.Format.nAvgBytesPerSec = 48000 * fmt.Format.nBlockAlign;
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = 32;
    fmt.dwChannelMask = channels == 6 ? KSAUDIO_SPEAKER_5POINT1 : KSAUDIO_SPEAKER_STEREO;
    fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    ComPtr<IAudioClient3> client3;
    HRESULT hr = client.As(&client3);
    std::printf("IAudioClient3: %s\n", SUCCEEDED(hr) ? "available" : "missing");
    if (SUCCEEDED(hr)) {
        UINT32 d = 0, f = 0, mn = 0, mx = 0;
        hr = client3->GetSharedModeEnginePeriod(&fmt.Format, &d, &f, &mn, &mx);
        std::printf("GetSharedModeEnginePeriod(our %u-ch float): hr=0x%08lx default=%u fundamental=%u min=%u max=%u frames (%.2f ms min)\n",
                    channels, static_cast<unsigned long>(hr), d, f, mn, mx, mn / 48.0);
        if (SUCCEEDED(hr)) {
            hr = client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, &fmt.Format, nullptr);
            std::printf("InitializeSharedAudioStream(min): hr=0x%08lx\n", static_cast<unsigned long>(hr));
            if (SUCCEEDED(hr)) {
                UINT32 buffer = 0;
                client->GetBufferSize(&buffer);
                std::printf("buffer: %u frames (%.2f ms)\n", buffer, buffer / 48.0);
            }
        }
        // And the mix format itself, for comparison.
        ComPtr<IAudioClient> client_b;
        dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_b);
        ComPtr<IAudioClient3> client3_b;
        client_b.As(&client3_b);
        UINT32 d2 = 0, f2 = 0, mn2 = 0, mx2 = 0;
        hr = client3_b->GetSharedModeEnginePeriod(mix, &d2, &f2, &mn2, &mx2);
        std::printf("GetSharedModeEnginePeriod(mix format): hr=0x%08lx default=%u min=%u max=%u frames\n",
                    static_cast<unsigned long>(hr), d2, mn2, mx2);
    }
    CoTaskMemFree(mix);
    CoUninitialize();
    return 0;
}
