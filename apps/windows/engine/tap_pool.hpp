#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "audio_devices.hpp"
#include "slots.hpp"

// One process-loopback tap per application, kept in step with the session
// list (docs/platforms/windows-demo.md, "Routing: two problems, not one").
//
// Every tap is opened at the same shape - 48 kHz, `channels` wide - so the
// engine can read the same number of frames from each per encode frame. The
// taps are wall-clock silence-filled by the library, so a quiet application
// still advances; one that has stalled (its process gone, and the OS still
// delivering zeros, or a tap that simply has not caught up) is read as
// silence rather than holding the frame. The taps come from an
// AudioDevices (audio_devices.hpp): WASAPI in the app, fakes in the tests.

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
    explicit TapPool(std::shared_ptr<AudioDevices> devices, std::uint16_t channels = 2,
                     std::uint32_t sample_rate = 48000);

    // Opens taps for applications in `wanted` that have none, closes taps
    // whose application is gone. Returns the ids that could not be opened,
    // for the UI.
    std::vector<AppId> sync(std::span<const AppId> wanted);

    // Pulls `frames` frames from every tap into internal scratch and returns
    // a view per application. Waits up to `wait_ms` for a lagging tap.
    [[nodiscard]] const std::vector<TapRead>& read(std::size_t frames, int wait_ms);

    // Discards everything the taps hold, so the next read starts at "now".
    // Called when the output starts or switches: the backlog that built up
    // while a sink was opening would otherwise sit in the sink's queue for
    // the rest of the session, as latency (spike S5 measured it).
    void flush();

    // The deepest tap backlog, in frames: the latency the taps are adding
    // right now.
    [[nodiscard]] std::size_t backlog_frames() const;

    [[nodiscard]] std::uint16_t channels() const { return channels_; }
    [[nodiscard]] std::size_t size() const { return taps_.size(); }
    [[nodiscard]] bool has(AppId app) const { return taps_.contains(app); }

private:
    struct Tap {
        std::unique_ptr<TapSource> source;
        std::vector<float> scratch;
    };
    std::shared_ptr<AudioDevices> devices_;
    std::uint16_t channels_;
    std::uint32_t sample_rate_;
    std::unordered_map<AppId, Tap> taps_;
    std::vector<TapRead> reads_;
};

}  // namespace ac3::windemo
