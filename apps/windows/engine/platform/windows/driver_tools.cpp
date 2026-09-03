#include "driver_tools.hpp"

#include <windows.h>

#include <shellapi.h>
#include <winternl.h>

#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace ac3::windemo {

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

}  // namespace ac3::windemo
