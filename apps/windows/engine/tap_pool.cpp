#include "tap_pool.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace ac3::windemo {

TapPool::TapPool(std::uint16_t channels, std::uint32_t sample_rate)
    : channels_(channels), sample_rate_(sample_rate) {}

std::vector<AppId> TapPool::sync(std::span<const AppId> wanted) {
    std::vector<AppId> failed;
    for (auto it = taps_.begin(); it != taps_.end();) {
        if (std::ranges::find(wanted, it->first) == wanted.end()) {
            it->second.capture->stop();
            it = taps_.erase(it);
        } else {
            ++it;
        }
    }
    for (const AppId app : wanted) {
        if (taps_.contains(app)) {
            continue;
        }
        Tap tap{.capture = std::make_unique<ac3::audio::Capture>()};
        const auto started = tap.capture->start_process_loopback(
            app, ac3::audio::ProcessLoopbackMode::kIncludeProcessTree,
            {.sample_rate = sample_rate_, .channels = channels_});
        if (!started) {
            failed.push_back(app);
            continue;
        }
        taps_.emplace(app, std::move(tap));
    }
    return failed;
}

const std::vector<TapRead>& TapPool::read(std::size_t frames, int wait_ms) {
    reads_.clear();
    const std::size_t samples = frames * channels_;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    for (auto& [app, tap] : taps_) {
        tap.scratch.resize(samples);
        std::size_t filled = 0;
        bool starved = false;
        while (filled < samples) {
            const std::size_t got = tap.capture->buffer()->read(
                std::span{tap.scratch}.subspan(filled, samples - filled));
            filled += got;
            if (filled < samples) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    starved = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (starved) {
            std::fill(tap.scratch.begin() + static_cast<std::ptrdiff_t>(filled), tap.scratch.end(),
                      0.0F);
        }
        reads_.push_back({.app = app, .interleaved = tap.scratch, .starved = starved});
    }
    return reads_;
}

}  // namespace ac3::windemo
