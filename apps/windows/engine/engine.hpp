#pragma once

#include <cstdint>
#include <expected>
#include <memory>

#include "audio_devices.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ac3/oba/oamd.hpp"
#include "output_policy.hpp"
#include "slots.hpp"

// The Desktop Atmos Demo's engine: everything except the window
// (docs/platforms/windows-demo.md). One thread runs the frame loop - refresh
// the session list, tap every application, fold each into its slot, encode,
// sign, hand the unit to the output stage - and the UI talks to it through
// the thread-safe commands below and reads back a status snapshot.

namespace ac3::windemo {

struct EngineConfig {
    std::string null_sink_substring = "Desktop Atmos";
    std::string signing_key_path;  // empty: the environment, then none
    bool low_latency = false;      // 1-block frames at a bitrate that carries the metadata
    std::uint32_t bitrate_kbps = 0;  // 0: 448 normal, 1536 low-latency
    std::optional<OutputMode> pinned;
    bool bypass_codec = false;  // headphones/PCM/stereo play the raw frame, not a decode
    // The audio devices the engine is built over; null means WASAPI
    // (audio_devices.hpp). Tests pass fakes.
    std::shared_ptr<AudioDevices> devices;
    // The width taps open at until the first probe; after each probe the
    // engine follows the null sink's shared-mode channel count (2, 6 or 8),
    // so a surround-capable application rendering 7.1 into the driver
    // reaches the bed by channel rather than as a stereo fold.
    std::uint16_t tap_channels = 2;
};

struct AppStatus {
    AppId app = 0;
    std::string name;
    std::string image_path;
    bool active = false;
    bool tapped = false;
    bool fullscreen = false;
    std::optional<int> slot;  // positioned slot, or nullopt: in the bed
    ac3::oba::Position position{0.5, 0.5, 0.0};
    float level_dbfs = -120.0F;
};

struct EngineStatus {
    bool running = false;
    std::vector<AppStatus> apps;
    OutputMode mode = OutputMode::kNone;
    std::string endpoint_name;
    std::string output_reason;
    std::vector<EndpointFacts> endpoints;  // what the output stage last probed
    std::string signing;
    bool objects_enabled = false;
    std::uint64_t frames_encoded = 0;
    std::uint64_t starved_reads = 0;
    std::uint64_t underruns = 0;
    double last_frame_ms = 0.0;
    double worst_frame_ms = 0.0;
    std::string last_error;
    std::uint16_t tap_channels = 0;  // the width every tap is open at
    bool codec_bypassed = false;     // the last unit took the raw path
    double tap_backlog_ms = 0.0;     // the deepest tap's unread audio: latency the taps add
    double sink_queue_ms = 0.0;      // the PCM sink's queue: latency the output adds
    std::uint64_t catchups = 0;      // frames dropped to keep the sink queue bounded
};

class Engine {
public:
    explicit Engine(EngineConfig config);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] std::expected<void, std::string> start();
    void stop();

    // Commands, safe from any thread; applied at the next frame boundary.
    void position(AppId app, ac3::oba::Position where);
    void unposition(AppId app);
    void pin(std::optional<OutputMode> mode);
    void set_bypass(bool on);
    void reprobe();
    // Loads (or, with an empty path, re-resolves from the environment) the
    // signing key and rebuilds the encoder, since objects-or-nothing is
    // decided at construction.
    void load_signing_key(std::string path);
    void clear_signing_key();

    [[nodiscard]] EngineStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::windemo
