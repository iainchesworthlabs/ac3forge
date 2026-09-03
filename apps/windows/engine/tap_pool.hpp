#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "ac3/audio/capture.hpp"
#include "slots.hpp"

// One process-loopback tap per application, kept in step with the session
// list (docs/platforms/windows-demo.md, "Routing: two problems, not one").
//
// Every tap is opened at the same shape - 48 kHz, `channels` wide - so the
// engine can read the same number of frames from each per encode frame. The
// taps are wall-clock silence-filled by the library, so a quiet application
// still advances; one that has stalled (its process gone, and the OS still
// delivering zeros, or a tap that simply has not caught up) is read as
// silence rather than holding the frame.

namespace ac3::windemo {

struct TapRead {
    AppId app = 0;
    // `frames` x channels interleaved floats, or all zero when the tap had
    // nothing to give this frame.
    std::span<const float> interleaved;
    bool starved = false;
};

class TapPool {
public:
    explicit TapPool(std::uint16_t channels = 2, std::uint32_t sample_rate = 48000);

    // Opens taps for applications in `wanted` that have none, closes taps
    // whose application is gone. Returns the ids that could not be opened,
    // for the UI.
    std::vector<AppId> sync(std::span<const AppId> wanted);

    // Pulls `frames` frames from every tap into internal scratch and returns
    // a view per application. Waits up to `wait_ms` for a lagging tap.
    [[nodiscard]] const std::vector<TapRead>& read(std::size_t frames, int wait_ms);

    [[nodiscard]] std::uint16_t channels() const { return channels_; }
    [[nodiscard]] std::size_t size() const { return taps_.size(); }
    [[nodiscard]] bool has(AppId app) const { return taps_.contains(app); }

private:
    struct Tap {
        std::unique_ptr<ac3::audio::Capture> capture;
        std::vector<float> scratch;
    };
    std::uint16_t channels_;
    std::uint32_t sample_rate_;
    std::unordered_map<AppId, Tap> taps_;
    std::vector<TapRead> reads_;
};

}  // namespace ac3::windemo
