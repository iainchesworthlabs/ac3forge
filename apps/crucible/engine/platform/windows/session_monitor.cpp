#include "session_monitor.hpp"

#include "platform_services.hpp"

#include <windows.h>
// windows.h must precede the audio headers.
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <appmodel.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>
#include <cwctype>
#include <map>
#include <string>
#include <unordered_map>

namespace ac3::crucible {

namespace {

using Microsoft::WRL::ComPtr;

// Spelled out for the reason src/audio's Windows backend gives: no import
// library defines these, and __uuidof is an MSVC extension.
constexpr CLSID kClsidMmDeviceEnumerator = {  // {bcde0395-e52f-467c-8e3d-c4579291692e}
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
constexpr IID kIidMmDeviceEnumerator = {  // {a95664d2-9614-4f35-a746-de8db63617e6}
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
constexpr IID kIidAudioSessionManager2 = {  // {77aa99a0-1bd6-484f-8bc7-2c654c9a9b6f}
    0x77aa99a0, 0x1bd6, 0x484f, {0x8b, 0xc7, 0x2c, 0x65, 0x4c, 0x9a, 0x9b, 0x6f}};
constexpr IID kIidAudioSessionControl2 = {  // {bfb7ff88-7239-4fc9-8fa2-07c950be9c6d}
    0xbfb7ff88, 0x7239, 0x4fc9, {0x8f, 0xa2, 0x07, 0xc9, 0x50, 0xbe, 0x9c, 0x6d}};
constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

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

std::wstring lower(std::wstring s) {
    for (auto& c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
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

struct ProcessInfo {
    std::uint32_t parent = 0;
    std::wstring image;  // lower-cased file name, e.g. L"chrome.exe"
};

// One snapshot of every process's parent and image name; the walk below
// reads it rather than opening handles per process.
std::unordered_map<std::uint32_t, ProcessInfo> snapshot_processes() {
    std::unordered_map<std::uint32_t, ProcessInfo> out;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return out;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            out[entry.th32ProcessID] = {.parent = entry.th32ParentProcessID,
                                        .image = lower(entry.szExeFile)};
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return out;
}

// The root of `pid`'s tree: the highest ancestor with the same image name.
// Bounded, because parent ids are recycled and a stale chain can loop.
std::uint32_t root_of(std::uint32_t pid, const std::unordered_map<std::uint32_t, ProcessInfo>& procs) {
    std::uint32_t current = pid;
    for (int hops = 0; hops < 16; ++hops) {
        const auto it = procs.find(current);
        if (it == procs.end()) {
            break;
        }
        const auto parent = procs.find(it->second.parent);
        if (parent == procs.end() || parent->second.image != it->second.image ||
            it->second.parent == current) {
            break;
        }
        current = it->second.parent;
    }
    return current;
}

std::string image_path_of(std::uint32_t pid) {
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle == nullptr) {
        return {};
    }
    wchar_t buffer[MAX_PATH * 2];
    DWORD length = static_cast<DWORD>(std::size(buffer));
    std::string path;
    if (QueryFullProcessImageNameW(handle, 0, buffer, &length)) {
        path = to_utf8(buffer);
    }
    CloseHandle(handle);
    return path;
}

// The executable's FileDescription from its version resource: "Steam",
// "Zoom", "VMware Workstation VMX", rather than the image's stem.
std::string description_of(const std::string& image_path) {
    if (image_path.empty()) {
        return {};
    }
    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, image_path.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wide_len > 0 ? wide_len - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, image_path.c_str(), -1, wide.data(), wide_len);
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(wide.c_str(), &handle);
    if (size == 0) {
        return {};
    }
    std::vector<std::byte> block(size);
    if (!GetFileVersionInfoW(wide.c_str(), 0, size, block.data())) {
        return {};
    }
    struct LangCodePage {
        WORD language;
        WORD code_page;
    };
    LangCodePage* translations = nullptr;
    UINT translations_size = 0;
    if (!VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations),
                        &translations_size) ||
        translations_size < sizeof(LangCodePage)) {
        return {};
    }
    // The first translation, then a neutral fallback.
    const LangCodePage candidates[] = {translations[0], {0x0409, 0x04B0}, {0x0409, 0x04E4}};
    for (const auto& c : candidates) {
        wchar_t key[64];
        swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", c.language, c.code_page);
        wchar_t* value = nullptr;
        UINT value_len = 0;
        if (VerQueryValueW(block.data(), key, reinterpret_cast<void**>(&value), &value_len) && value != nullptr &&
            value_len > 1) {
            std::string out = to_utf8(value);
            while (!out.empty() && (out.back() == ' ' || out.back() == '\0')) {
                out.pop_back();
            }
            return out;
        }
    }
    return {};
}

// Windows\SystemApps holds the shell's own packaged components.
bool is_system_app(const std::string& image_path) {
    std::string lower = image_path;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("\\windows\\systemapps\\") != std::string::npos;
}

// Windows' own windowed hosts (the frame host for packaged apps, Settings,
// anything else under System32): windows, but not applications a person
// would place. Only the silent listing asks; a process here that opens an
// audio session is listed like any other, so no source is hidden.
bool is_shell_host(const std::string& image_path) {
    std::string lower = image_path;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* dir : {"\\windows\\system32\\", "\\windows\\syswow64\\", "\\windows\\immersivecontrolpanel\\"}) {
        if (lower.find(dir) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Whether a process is a packaged app (Store/UWP): its windows belong to a
// host process, so the window test below would miss it.
bool packaged_of(std::uint32_t pid) {
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle == nullptr) {
        return false;
    }
    UINT32 length = 0;
    const LONG rc = GetPackageFullName(handle, &length, nullptr);
    CloseHandle(handle);
    return rc == ERROR_INSUFFICIENT_BUFFER;  // a name exists; APPMODEL_ERROR_NO_PACKAGE otherwise
}

// Every process that owns a visible, titled, top-level window that is not
// a tool window: one EnumWindows pass per refresh.
std::unordered_map<std::uint32_t, bool> windowed_processes() {
    std::unordered_map<std::uint32_t, bool> out;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* map = reinterpret_cast<std::unordered_map<std::uint32_t, bool>*>(param);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
                return TRUE;
            }
            const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if ((ex & WS_EX_TOOLWINDOW) != 0) {
                return TRUE;
            }
            if (GetWindowTextLengthW(hwnd) == 0) {
                return TRUE;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != 0) {
                (*map)[pid] = true;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

std::string stem_of(const std::wstring& image) {
    const auto dot = image.find_last_of(L'.');
    return to_utf8(image.substr(0, dot).c_str());
}

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

// The Windows answer to the engine's SessionMonitor seam
// (engine/session_monitor.hpp): IAudioSessionManager2 on every render
// endpoint, grouped by process tree. Everything COM and every Win32 walk is
// below this line and nothing above it knows they happened.
class WindowsSessionMonitor final : public SessionMonitor {
public:
    std::vector<AppSession> refresh(const std::vector<std::uint32_t>& keep) override;

private:
    // What is read once per process and kept while it lives: the image
    // path, the executable's description, whether it is packaged. Reading
    // a version resource for every process on every refresh was most of
    // the refresh's cost (windows-demo.md, "The application review").
    struct Facts {
        std::string image_path;
        std::string description;
        bool packaged = false;
        bool system_app = false;
    };
    std::unordered_map<std::uint32_t, Facts> facts_;
};

std::vector<AppSession> WindowsSessionMonitor::refresh(const std::vector<std::uint32_t>& keep) {
    std::vector<AppSession> out;
    ComScope com;
    if (!com.ok()) {
        return out;
    }
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(kClsidMmDeviceEnumerator, nullptr, CLSCTX_ALL,
                                kIidMmDeviceEnumerator, &enumerator))) {
        return out;
    }
    ComPtr<IMMDeviceCollection> endpoints;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints))) {
        return out;
    }
    UINT count = 0;
    endpoints->GetCount(&count);

    const auto procs = snapshot_processes();
    // Our own output (a monitor or spatial sink) is an audio session too.
    // Listing it would tap it, and tapping it would mix our output back
    // into the bed - found on the first smoke run, where the engine showed
    // up in its own list at -24 dBFS.
    const std::uint32_t self_root = root_of(GetCurrentProcessId(), procs);
    const auto self_info = procs.find(self_root);
    const std::wstring self_image = self_info == procs.end() ? std::wstring{} : self_info->second.image;
    std::map<std::uint32_t, AppSession> apps;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(endpoints->Item(i, &device))) {
            continue;
        }
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(kIidAudioSessionManager2, CLSCTX_ALL, nullptr, &manager))) {
            continue;
        }
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(&sessions))) {
            continue;
        }
        int session_count = 0;
        sessions->GetCount(&session_count);
        std::string endpoint_name;
        for (int s = 0; s < session_count; ++s) {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(sessions->GetSession(s, &control))) {
                continue;
            }
            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(control->QueryInterface(kIidAudioSessionControl2, &control2))) {
                continue;
            }
            DWORD pid = 0;
            control2->GetProcessId(&pid);
            if (pid == 0) {
                continue;  // the system sounds session
            }
            AudioSessionState state = AudioSessionStateInactive;
            control->GetState(&state);
            if (state == AudioSessionStateExpired) {
                continue;
            }
            const std::uint32_t root = root_of(pid, procs);
            if (root == self_root) {
                continue;
            }
            // Another instance of this same program (a capture run beside a
            // live one, say): its output is our own stream, not an application.
            if (const auto info = procs.find(root); info != procs.end() && info->second.image == self_image) {
                continue;
            }
            auto& app = apps[root];
            if (app.app == 0) {
                app.app = root;
                const auto info = procs.find(root);
                app.name = info == procs.end() ? std::to_string(root) : stem_of(info->second.image);
                auto facts = facts_.find(root);
                if (facts == facts_.end()) {
                    Facts f;
                    f.image_path = image_path_of(root);
                    f.description = description_of(f.image_path);
                    f.system_app = is_system_app(f.image_path);
                    // Windows' own shell components (the text-input host,
                    // the search and start hosts) are packaged apps, but
                    // nobody means them when they say "application": their
                    // images live under Windows\SystemApps.
                    f.packaged = packaged_of(root) && !f.system_app;
                    facts = facts_.emplace(root, std::move(f)).first;
                }
                app.image_path = facts->second.image_path;
                app.description = facts->second.description;
                app.packaged = facts->second.packaged;
                if (endpoint_name.empty()) {
                    endpoint_name = friendly_name(device.Get());
                }
                app.endpoint_name = endpoint_name;
            }
            app.active = app.active || state == AudioSessionStateActive;
            if (std::ranges::find(app.session_pids, pid) == app.session_pids.end()) {
                app.session_pids.push_back(pid);
            }
        }
    }
    // A window anywhere in the tree counts for the root: a game's launcher
    // may own the session while its child owns the window, or the reverse.
    const auto windowed = windowed_processes();
    // Every windowed process tree, and every kept id whose process lives,
    // is an application too: listed without a session, so a person can
    // place it before it plays, and so a placed one that falls silent
    // stays where it was put rather than vanishing.
    auto add_silent = [&](std::uint32_t root) {
        if (root == 0 || root == self_root || apps.contains(root)) {
            return;
        }
        const auto info = procs.find(root);
        if (info == procs.end() || info->second.image == self_image) {
            return;
        }
        AppSession app;
        app.app = root;
        app.name = stem_of(info->second.image);
        app.has_session = false;
        auto facts = facts_.find(root);
        if (facts == facts_.end()) {
            Facts f;
            f.image_path = image_path_of(root);
            f.description = description_of(f.image_path);
            f.system_app = is_system_app(f.image_path);
            f.packaged = packaged_of(root) && !f.system_app;
            facts = facts_.emplace(root, std::move(f)).first;
        }
        if (facts->second.system_app || is_shell_host(facts->second.image_path)) {
            return;
        }
        app.image_path = facts->second.image_path;
        app.description = facts->second.description;
        app.packaged = facts->second.packaged;
        apps.emplace(root, std::move(app));
    };
    for (const auto& [pid, is_windowed] : windowed) {
        if (is_windowed) {
            add_silent(root_of(pid, procs));
        }
    }
    for (const std::uint32_t id : keep) {
        add_silent(id);
    }
    // Facts for processes that have gone.
    for (auto it = facts_.begin(); it != facts_.end();) {
        it = apps.contains(it->first) ? std::next(it) : facts_.erase(it);
    }
    for (auto& [root, app] : apps) {
        // Windows' own shell components own windows and are packaged, and
        // are still not what a person means by an application.
        if (const auto f = facts_.find(root); f != facts_.end() && f->second.system_app) {
            app.has_window = false;
            app.packaged = false;
            continue;
        }
        for (const auto& [pid, info] : procs) {
            if (windowed.contains(pid) && root_of(pid, procs) == root) {
                app.has_window = true;
                break;
            }
        }
    }
    out.reserve(apps.size());
    for (auto& [root, app] : apps) {
        out.push_back(std::move(app));
    }
    return out;
}

}  // namespace

std::shared_ptr<SessionMonitor> platform_session_monitor() {
    return std::make_shared<WindowsSessionMonitor>();
}

}  // namespace ac3::crucible
