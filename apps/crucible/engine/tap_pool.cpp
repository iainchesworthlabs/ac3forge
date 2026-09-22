#include "tap_pool.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace ac3::crucible {

TapPool::TapPool(std::shared_ptr<AudioDevices> devices, std::uint16_t channels,
                 std::uint32_t sample_rate)
    : devices_(std::move(devices)), channels_(channels), sample_rate_(sample_rate) {}

std::vector<AppId> TapPool::sync(std::span<const AppId> wanted) {
    std::vector<AppId> failed;
    for (auto it = taps_.begin(); it != taps_.end();) {
        if (std::ranges::find(wanted, it->first) == wanted.end()) {
            it->second.source->stop();
            it = taps_.erase(it);
        } else {
            ++it;
        }
    }
    for (const AppId app : wanted) {
        if (taps_.contains(app)) {
            continue;
        }
        Tap tap{.source = devices_->tap(), .scratch = {}};
        if (const auto started = tap.source->start(app, sample_rate_, channels_); !started) {
            failed.push_back(app);
            continue;
        }
        taps_.emplace(app, std::move(tap));
    }
    return failed;
}

void TapPool::flush() {
    std::vector<float> sink(4096);
    for (auto& [app, tap] : taps_) {
        while (tap.source->available() > 0) {
            if (tap.source->read(sink) == 0) {
                break;
            }
        }
    }
}

std::size_t TapPool::backlog_frames() const {
    std::size_t deepest = 0;
    for (const auto& [app, tap] : taps_) {
        deepest = std::max(deepest, tap.source->available() / channels_);
    }
    return deepest;
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
            filled += tap.source->read(std::span{tap.scratch}.subspan(filled, samples - filled));
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

}  // namespace ac3::crucible
