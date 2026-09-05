// Spike S1, part two: the same tap through the LIBRARY's own entry points
// (Phase 1 of docs/platforms/windows-demo.md) rather than the raw WASAPI of
// s1_taps.cpp - Capture::start_process_loopback and DeviceWatcher. Spawns
// one tone_player, taps it, reads the ring for a few seconds and reports
// level and estimated frequency, then starts and stops a device watcher.
// Throwaway code: no reuse intended.

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ac3/audio/audio_backend.hpp"
#include "ac3/audio/capture.hpp"
#include "ac3/audio/device_watcher.hpp"

namespace {

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring s = buf;
    return s.substr(0, s.find_last_of(L"\\/"));
}

}  // namespace

int main(int argc, char** argv) {
    const double freq = argc > 1 ? std::atof(argv[1]) : 440.0;
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 4;

    const auto& backend = ac3::audio::audio_backend();
    std::printf("process_loopback: %s%s\n", backend.process_loopback.available ? "available" : "UNAVAILABLE: ",
                backend.process_loopback.available ? "" : std::string(backend.process_loopback.reason).c_str());
    std::printf("device_watch:     %s\n", backend.device_watch.available ? "available" : "UNAVAILABLE");

    // A process that does not exist must be refused before anything is opened.
    {
        ac3::audio::Capture probe;
        const auto refused = probe.start_process_loopback(999999);
        std::printf("pid 999999 -> %s\n",
                    refused ? "ACCEPTED (wrong)" : std::string(ac3::audio::describe(refused.error())).c_str());
    }

    std::wstring cmd = L"\"" + exe_dir() + L"\\tone_player.exe\" " + std::to_wstring(freq) + L" \"\" " +
                       std::to_wstring(seconds + 5);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "spawn failed (%lu)\n", GetLastError());
        return 2;
    }
    Sleep(1000);

    ac3::audio::Capture capture;
    const auto started = capture.start_process_loopback(pi.dwProcessId);
    if (!started) {
        std::printf("start_process_loopback(pid %lu) -> %s\n", pi.dwProcessId,
                    std::string(ac3::audio::describe(started.error())).c_str());
        TerminateProcess(pi.hProcess, 0);
        return 2;
    }
    std::printf("tapping pid %lu at %u Hz x %u ch, expect %.0f Hz\n", pi.dwProcessId, capture.sample_rate(),
                capture.channels(), freq);

    std::vector<float> chunk(4096);
    const auto t0 = std::chrono::steady_clock::now();
    double sum_sq = 0.0;
    std::uint64_t n = 0, crossings = 0;
    float prev = 0.0f;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        const std::size_t got = capture.buffer()->read(std::span<float>(chunk));
        if (got == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        const unsigned ch = capture.channels();
        for (std::size_t i = 0; i + ch <= got; i += ch) {
            const float v = chunk[i];
            sum_sq += static_cast<double>(v) * v;
            if ((prev < 0.0f && v >= 0.0f) || (prev >= 0.0f && v < 0.0f)) ++crossings;
            prev = v;
            ++n;
        }
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double rms = n ? std::sqrt(sum_sq / static_cast<double>(n)) : 0.0;
    const auto stats = capture.stats();
    std::printf("read %llu frames in %.1fs (%.0f/s): rms %.1f dBFS, est %.0f Hz; captured=%llu silence=%llu dropped=%llu\n",
                static_cast<unsigned long long>(n), elapsed, n / elapsed, rms > 0 ? 20.0 * std::log10(rms) : -120.0,
                static_cast<double>(crossings) / 2.0 / elapsed, static_cast<unsigned long long>(stats.frames_captured),
                static_cast<unsigned long long>(stats.frames_silence_filled),
                static_cast<unsigned long long>(stats.frames_dropped));
    capture.stop();
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::atomic<int> events{0};
    ac3::audio::DeviceWatcher watcher;
    const auto watching = watcher.start([&](const ac3::audio::DeviceChangeEvent& e) {
        ++events;
        std::printf("  device event %d on \"%s\"\n", static_cast<int>(e.change), e.device_id.c_str());
    });
    std::printf("DeviceWatcher::start -> %s, running=%d\n",
                watching ? "ok" : std::string(ac3::audio::describe(watching.error())).c_str(), watcher.running() ? 1 : 0);
    if (watching) {
        std::puts("  (plug or unplug something in the next 3 s to see an event; none is expected otherwise)");
        Sleep(3000);
        watcher.stop();
        std::printf("DeviceWatcher stopped, running=%d, events delivered=%llu\n", watcher.running() ? 1 : 0,
                    static_cast<unsigned long long>(watcher.stats().events_delivered));
    }
    return 0;
}
