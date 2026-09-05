#include "driver_tools.hpp"

#include "platform_services.hpp"

#include <windows.h>

#include <shellapi.h>
#include <winternl.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ac3::crucible {

namespace {

// SystemCodeIntegrityInformation (winternl.h names the class but not the
// struct or the option bits; they are stable since Vista and what
// msinfo32 reads).
constexpr int kSystemCodeIntegrityInformation = 103;
constexpr ULONG kCodeIntegrityOptionTestSign = 0x02;
constexpr ULONG kCodeIntegrityOptionHvciKmciEnabled = 0x400;

struct SystemCodeIntegrityInformationBlock {
    ULONG length;
    ULONG options;
};

using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(int, PVOID, ULONG, PULONG);

std::string last_error_text(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (n > 0 && buffer != nullptr) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
        text.resize(static_cast<std::size_t>(bytes));
        WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(n), text.data(), bytes, nullptr, nullptr);
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    return text.empty() ? "error " + std::to_string(code) : text;
}

}  // namespace

CodeIntegrityState code_integrity_state() {
    CodeIntegrityState state;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return state;
    }
    const auto query = reinterpret_cast<NtQuerySystemInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQuerySystemInformation")));
    if (query == nullptr) {
        return state;
    }
    SystemCodeIntegrityInformationBlock info{sizeof(info), 0};
    ULONG returned = 0;
    if (query(kSystemCodeIntegrityInformation, &info, sizeof(info), &returned) < 0) {
        return state;
    }
    state.known = true;
    state.test_signing = (info.options & kCodeIntegrityOptionTestSign) != 0;
    state.hvci = (info.options & kCodeIntegrityOptionHvciKmciEnabled) != 0;
    return state;
}

std::expected<ElevatedProcess, std::string> ElevatedProcess::start(std::wstring_view program,
                                                                    std::wstring_view arguments) {
    const std::wstring program_z(program);
    const std::wstring arguments_z(arguments);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = program_z.c_str();
    info.lpParameters = arguments_z.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        if (code == ERROR_CANCELLED) {
            return std::unexpected("the elevation prompt was declined");
        }
        return std::unexpected("could not start the elevated process: " + last_error_text(code));
    }
    if (info.hProcess == nullptr) {
        return std::unexpected("the elevated process started without a handle to wait on");
    }
    ElevatedProcess process;
    process.handle_ = info.hProcess;
    return process;
}

ElevatedProcess::ElevatedProcess(ElevatedProcess&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

ElevatedProcess& ElevatedProcess::operator=(ElevatedProcess&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            CloseHandle(static_cast<HANDLE>(handle_));
        }
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

ElevatedProcess::~ElevatedProcess() {
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
    }
}

std::optional<int> ElevatedProcess::poll() {
    if (handle_ == nullptr) {
        return std::nullopt;
    }
    if (WaitForSingleObject(static_cast<HANDLE>(handle_), 0) != WAIT_OBJECT_0) {
        return std::nullopt;
    }
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    return static_cast<int>(code);
}

std::vector<std::string> transcript_tail(std::wstring_view path, std::size_t max_lines) {
    std::ifstream in(std::wstring(path), std::ios::binary);
    std::vector<std::string> kept;
    if (!in) {
        return kept;
    }
    // Start-Transcript writes UTF-16 or UTF-8 depending on the host; both
    // are read as bytes and NULs dropped, which is enough for a status
    // line.
    std::string line;
    while (std::getline(in, line)) {
        std::string clean;
        for (const char c : line) {
            if (c != '\0' && c != '\r') {
                clean.push_back(c);
            }
        }
        if (clean.size() >= 3 && clean.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            clean.erase(0, 3);
        }
        if (clean.size() >= 2 && (clean.compare(0, 2, "\xFF\xFE") == 0)) {
            clean.erase(0, 2);
        }
        if (clean.empty() || clean.find("****") == 0 || clean.find("Windows PowerShell transcript") == 0 ||
            clean.find("Start time:") == 0 || clean.find("End time:") == 0 || clean.find("Username:") == 0 ||
            clean.find("RunAs User:") == 0 || clean.find("Configuration Name:") == 0 || clean.find("Machine:") == 0 ||
            clean.find("Host Application:") == 0 || clean.find("Process ID:") == 0 || clean.find("PSVersion:") == 0 ||
            clean.find("PSEdition:") == 0 || clean.find("PSCompatibleVersions:") == 0 || clean.find("BuildVersion:") == 0 ||
            clean.find("CLRVersion:") == 0 || clean.find("WSManStackVersion:") == 0 || clean.find("PSRemotingProtocolVersion:") == 0 ||
            clean.find("SerializationVersion:") == 0 || clean.find("Transcript started") == 0) {
            continue;
        }
        kept.push_back(clean);
        if (kept.size() > max_lines) {
            kept.erase(kept.begin());
        }
    }
    return kept;
}

namespace {

// The Windows answer to the engine's VirtualDevice seam
// (engine/virtual_device.hpp): the silent device is a kernel driver, and
// everything about test signing, memory integrity, elevated PowerShell and
// the built package is here rather than in the window. What reaches the UI
// is SilentDeviceState's plain text, which the other platforms fill in with
// their own facts - a module load on Linux, nothing at all on macOS.
class WindowsVirtualDevice final : public VirtualDevice {
public:
    void set_package_dir(std::string_view dir) override { package_dir_ = dir; }

    // The driver's endpoint. It changes to "Crucible" in the same change
    // that renames the INF, once signing is paid for.
    std::string device_name() const override { return "Desktop Atmos"; }
    bool from_package() const override { return true; }
    std::string how_to_get_one() const override {
        return "install the Desktop Atmos driver (Settings)";
    }

    SilentDeviceState state(const SilentDeviceQuery& query) override {
        SilentDeviceState out;
        out.needed = true;
        out.present = query.endpoint_present;
        out.in_use = query.endpoint_is_default;
        out.can_install = package_complete();
        const auto integrity = code_integrity_state();
        if (!out.present) {
            // Only worth saying while there is no device: once it is there it
            // plainly loaded, whatever the kernel reports.
            if (!integrity.known) {
                out.blocker = "could not read the kernel's code-integrity state";
            } else if (!integrity.test_signing || integrity.hvci) {
                out.blocker = "this machine will not load a test-signed driver: ";
                if (!integrity.test_signing) {
                    out.blocker += "turn test signing on (bcdedit /set testsigning on, then restart)";
                }
                if (!integrity.test_signing && integrity.hvci) {
                    out.blocker += " and ";
                }
                if (integrity.hvci) {
                    out.blocker +=
                        "turn memory integrity off (Windows Security, Core isolation, then restart)";
                }
            } else if (!out.can_install) {
                out.blocker = "no built driver package to install";
            }
        }
        out.detail.push_back(out.can_install ? "a built package is in " + package_dir_
                                             : "no built package in " + package_dir_);
        return out;
    }

    std::expected<void, std::string> install() override { return run("install.ps1", "install"); }
    std::expected<void, std::string> remove() override { return run("remove.ps1", "remove"); }

    DeviceActionStatus action_status() override {
        DeviceActionStatus out;
        if (!process_.running()) {
            out.running = false;
            return out;
        }
        const auto code = process_.poll();
        if (!code) {
            out.running = true;
            return out;
        }
        out.running = false;
        out.exit_code = code;
        out.log_tail = transcript_tail(log_path(), 3);
        return out;
    }

private:
    [[nodiscard]] bool package_complete() const {
        if (package_dir_.empty()) {
            return false;
        }
        const std::filesystem::path dir(package_dir_);
        std::error_code ec;
        return std::filesystem::exists(dir / "install.ps1", ec) &&
               std::filesystem::exists(dir / "remove.ps1", ec) &&
               std::filesystem::exists(
                   dir / "Package" / "x64" / "Release" / "package" / "Ac3ForgeNullSink.inf", ec);
    }

    // %LOCALAPPDATA%\ac3forge\driver-<verb>.log. The engine library has no Qt,
    // so this is the environment rather than QStandardPaths.
    [[nodiscard]] std::wstring log_path() const {
        wchar_t base[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        std::filesystem::path dir = (n > 0 && n < MAX_PATH) ? std::filesystem::path(base)
                                                            : std::filesystem::temp_directory_path();
        dir /= L"ac3forge";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return (dir / (L"driver-" + std::wstring(verb_.begin(), verb_.end()) + L".log")).wstring();
    }

    std::expected<void, std::string> run(std::string_view script, std::string_view verb) {
        if (process_.running()) {
            return std::unexpected("an install or remove is already running");
        }
        if (!package_complete()) {
            return std::unexpected("no driver package under " + package_dir_);
        }
        verb_ = verb;
        const std::filesystem::path dir(package_dir_);
        const auto script_path = (dir / script).wstring();
        const auto package = (dir / "Package" / "x64" / "Release" / "package").wstring();
        const auto log = log_path();
        std::error_code ec;
        std::filesystem::remove(log, ec);
        // One -Command so the script's output lands in a transcript this can
        // read back; the window itself is hidden. Paths are single-quoted for
        // PowerShell; the whole command is one argument to powershell.exe.
        std::wstring command = L"Start-Transcript -Path '" + log + L"' | Out-Null; try { & '" +
                               script_path + L"' -PackageDir '" + package +
                               L"'; $code = 0 } catch { Write-Host $_; $code = 1 }; "
                               L"Stop-Transcript | Out-Null; exit $code";
        const std::wstring arguments =
            L"-NoProfile -ExecutionPolicy Bypass -Command \"" + command + L"\"";
        auto started = ElevatedProcess::start(L"powershell.exe", arguments);
        if (!started) {
            return std::unexpected(started.error());
        }
        process_ = std::move(*started);
        return {};
    }

    std::string package_dir_;
    std::string verb_ = "install";
    ElevatedProcess process_;
};

}  // namespace

std::shared_ptr<VirtualDevice> platform_virtual_device() {
    return std::make_shared<WindowsVirtualDevice>();
}

}  // namespace ac3::crucible
