#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the Settings screen's driver buttons need from Windows: whether a
// test-signed kernel driver could load at all (test signing on, memory
// integrity off), and a way to run the package's install and remove
// scripts elevated and learn how they ended
// (docs/platforms/windows-demo.md, "Phase 4"). Windows-only, this
// directory only.

namespace ac3::crucible {

struct CodeIntegrityState {
    bool test_signing = false;   // bcdedit /set testsigning on, after a reboot
    bool hvci = false;           // memory integrity (hypervisor-enforced code integrity)
    bool known = false;          // false when the query itself failed
};

// The kernel's own view, through SystemCodeIntegrityInformation; needs no
// privilege.
[[nodiscard]] CodeIntegrityState code_integrity_state();

// A process started with the "runas" verb (a UAC prompt) that can be
// polled for completion. Moving only.
class ElevatedProcess {
public:
    // Starts `program` with `arguments` (one command line, already quoted).
    // The window is hidden; the caller passes a log path to the script
    // instead. A declined UAC prompt is a refusal, not a crash.
    [[nodiscard]] static std::expected<ElevatedProcess, std::string> start(std::wstring_view program,
                                                                            std::wstring_view arguments);

    ElevatedProcess() = default;
    ElevatedProcess(ElevatedProcess&& other) noexcept;
    ElevatedProcess& operator=(ElevatedProcess&& other) noexcept;
    ElevatedProcess(const ElevatedProcess&) = delete;
    ElevatedProcess& operator=(const ElevatedProcess&) = delete;
    ~ElevatedProcess();

    [[nodiscard]] bool running() const { return handle_ != nullptr; }

    // The exit code once the process has ended, else nullopt. After it
    // returns a value the handle is closed and running() is false.
    [[nodiscard]] std::optional<int> poll();

private:
    void* handle_ = nullptr;
};

// The last `max_lines` non-empty lines of a PowerShell transcript, with
// the header and footer banners removed; empty when the file is missing.
[[nodiscard]] std::vector<std::string> transcript_tail(std::wstring_view path, std::size_t max_lines);

}  // namespace ac3::crucible
