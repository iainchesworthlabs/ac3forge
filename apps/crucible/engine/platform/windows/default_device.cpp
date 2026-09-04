#include "default_device.hpp"

#include <memory>

#include "platform_services.hpp"

#include <windows.h>
// windows.h must precede the audio headers; mmreg.h (WAVEFORMATEX, which
// IPolicyConfig's declaration below names) is not pulled in under
// WIN32_LEAN_AND_MEAN, so it is asked for by name.
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <cwctype>
#include <string>

namespace ac3::crucible {

namespace {

using Microsoft::WRL::ComPtr;

constexpr CLSID kClsidMmDeviceEnumerator = {  // {bcde0395-e52f-467c-8e3d-c4579291692e}
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
constexpr IID kIidMmDeviceEnumerator = {  // {a95664d2-9614-4f35-a746-de8db63617e6}
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// PolicyConfigClient {870af99c-171d-4f9e-af0d-e63df40c2bc9} and its
// IPolicyConfig {f8679f50-850a-41cf-9c72-430f290290c8}: undocumented, and
// declared here to the vtable layout the Windows 7+ implementation has kept
// ever since. Only SetDefaultEndpoint is called; the rest are here so the
// slot numbers line up.
constexpr CLSID kClsidPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
constexpr IID kIidPolicyConfig = {
    0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

struct IPolicyConfig : IUnknown {
    // A COM interface: the vtable must be exactly the methods below, so no
    // virtual destructor; protected keeps -Wnon-virtual-dtor honest and
    // nothing here ever deletes one (Release() does).
protected:
    ~IPolicyConfig() = default;

public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(LPCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(LPCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(LPCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(LPCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(LPCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(LPCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(LPCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(LPCWSTR, const PROPERTYKEY&,
                                                       PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(LPCWSTR, const PROPERTYKEY&,
                                                       PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR device_id, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(LPCWSTR, INT) = 0;
};

std::string to_utf8(const wchar_t* wide) {
    if (wide == nullptr || *wide == L'\0') {
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

std::wstring to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                           nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
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

std::string friendly_name(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) {
        return {};
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name;
    if (SUCCEEDED(store->GetValue(kPkeyDeviceFriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = to_utf8(value.pwszVal);
    }
    PropVariantClear(&value);
    return name;
}

std::string id_of(IMMDevice* device) {
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id))) {
        return {};
    }
    std::string out = to_utf8(id);
    CoTaskMemFree(id);
    return out;
}

ComPtr<IMMDeviceEnumerator> make_enumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    CoCreateInstance(kClsidMmDeviceEnumerator, nullptr, CLSCTX_ALL, kIidMmDeviceEnumerator,
                     &enumerator);
    return enumerator;
}

// Forward declarations: these used to live in this directory's own header,
// which the engine-level seam replaced. render_endpoints() calls
// default_render_id() and is defined first.
std::vector<RenderEndpoint> render_endpoints();
std::string default_render_id();
std::expected<void, std::string> set_default_render(std::string_view endpoint_id);
std::string find_render_endpoint(std::string_view name_substring);
void open_sound_settings();

std::vector<RenderEndpoint> render_endpoints() {
    std::vector<RenderEndpoint> out;
    ComScope com;
    if (!com.ok()) {
        return out;
    }
    auto enumerator = make_enumerator();
    if (!enumerator) {
        return out;
    }
    const std::string default_id = default_render_id();
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        return out;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) {
            continue;
        }
        RenderEndpoint endpoint{.id = id_of(device.Get()), .name = friendly_name(device.Get())};
        endpoint.is_default = !endpoint.id.empty() && endpoint.id == default_id;
        if (endpoint.is_default) {
            out.insert(out.begin(), std::move(endpoint));
        } else {
            out.push_back(std::move(endpoint));
        }
    }
    return out;
}

std::string default_render_id() {
    ComScope com;
    if (!com.ok()) {
        return {};
    }
    auto enumerator = make_enumerator();
    ComPtr<IMMDevice> device;
    if (!enumerator || FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        return {};
    }
    return id_of(device.Get());
}

std::expected<void, std::string> set_default_render(std::string_view endpoint_id) {
    ComScope com;
    if (!com.ok()) {
        return std::unexpected("COM could not be initialised");
    }
    ComPtr<IPolicyConfig> policy;
    const HRESULT created = CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL,
                                             kIidPolicyConfig, &policy);
    if (FAILED(created)) {
        return std::unexpected("this Windows build does not expose the policy-config "
                               "interface; set the default output by hand");
    }
    const std::wstring wide = to_wide(endpoint_id);
    for (const ERole role : {eConsole, eMultimedia, eCommunications}) {
        const HRESULT hr = policy->SetDefaultEndpoint(wide.c_str(), role);
        if (FAILED(hr)) {
            return std::unexpected("Windows refused to change the default output (0x" +
                                   [hr] {
                                       char buf[16];
                                       std::snprintf(buf, sizeof buf, "%08lx",
                                                     static_cast<unsigned long>(hr));
                                       return std::string(buf);
                                   }() +
                                   "); set it by hand");
        }
    }
    return {};
}

std::string find_render_endpoint(std::string_view name_substring) {
    if (name_substring.empty()) {
        return {};
    }
    const std::string needle = lower(std::string(name_substring));
    for (const auto& endpoint : render_endpoints()) {
        if (lower(endpoint.name).find(needle) != std::string::npos) {
            return endpoint.id;
        }
    }
    return {};
}

void open_sound_settings() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
}

// The Windows answer to the engine's DefaultDevice seam
// (engine/default_device.hpp). Windows documents no API for setting the
// default; IPolicyConfig is what every default-switcher uses and is declared
// by hand above, so a refusal is ordinary and open_sound_settings() is the
// fallback.
class WindowsDefaultDevice final : public DefaultDevice {
public:
    std::vector<RenderEndpoint> endpoints() override { return render_endpoints(); }
    std::string default_id() override { return default_render_id(); }
    std::expected<void, std::string> set_default(std::string_view id) override {
        return set_default_render(id);
    }
    bool moves_default() const override { return true; }
    std::string find_endpoint(std::string_view name_substring) override {
        return find_render_endpoint(name_substring);
    }
    void open_sound_settings() override { ac3::crucible::open_sound_settings(); }
};

}  // namespace

std::shared_ptr<DefaultDevice> platform_default_device() {
    return std::make_shared<WindowsDefaultDevice>();
}

}  // namespace ac3::crucible
