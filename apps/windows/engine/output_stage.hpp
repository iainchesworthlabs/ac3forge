#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/oba/atmos.hpp"
#include "audio_devices.hpp"
#include "output_policy.hpp"

// The output end of the engine (docs/platforms/windows-demo.md, "Output modes
// and hot switching"): probe what the machine has, let the policy choose,
// own whichever sink that means, and route each encoded access unit to it.
//
//   Atmos / DD+ 5.1  ->  Eac3BurstPacker -> PassthroughSink (E-AC-3)
//   DD 5.1           ->  FrameEncoder over the encoder's own bed -> wrap_frame
//                        -> PassthroughSink (AC-3)
//   PCM surround     ->  Eac3Decoder -> MonitorSink, 5.1
//   headphones       ->  Eac3Decoder -> SpatialObjectSink (decoded objects)
//   stereo           ->  Eac3Decoder (Lo/Ro fold) -> MonitorSink, 2 channels
//
// The three decoded modes decode what was encoded, on purpose: they are
// what makes the demo's headphone and TV paths exercise the codec rather
// than bypass it. The bypass switch is the exception, there so the
// difference can be demonstrated: with it on, those modes take the engine's
// own objects and bed (RawFrame) and the decoder is out of the loop. A mode
// switch stops the old sink and starts the new one; the encoder upstream
// never notices.

namespace ac3::windemo {

struct OutputStageConfig {
    // Where endpoints and sinks come from; null means WASAPI.
    std::shared_ptr<AudioDevices> devices;
    // Headphones, PCM surround and stereo play the raw frame instead of a
    // decode of the stream.
    bool bypass_codec = false;
    // One-block frames: the PCM sinks ask for the platform's smallest
    // render period, since they are fed 5.3 ms at a time.
    bool low_latency = false;
    // The null sink is recognised by name (the driver's own, or a stand-in
    // such as FxSound's idle endpoint during development).
    std::string null_sink_substring = "Desktop Atmos";
    std::optional<OutputMode> pinned;
    std::string preferred_endpoint_id{};  // the user's choice of endpoint; empty: automatic
    std::uint32_t sample_rate = 48000;
    std::uint32_t ac3_bitrate_kbps = 448;  // the DD 5.1 leg only
};

struct OutputStatus {
    OutputMode mode = OutputMode::kNone;
    std::string endpoint_id;
    std::string endpoint_name;
    std::string reason;
    bool running = false;
    std::uint64_t units_submitted = 0;
    std::uint64_t underruns = 0;
    std::vector<EndpointFacts> endpoints;  // what the last probe saw
    bool bypassed = false;                 // the last unit took the raw path
    // The PCM sink's queue depth after the last submit (0 for bitstream
    // and headphone modes): latency the output stage is adding.
    std::size_t sink_queue_frames = 0;
};

// The engine's own signal for one frame: every object slot's PCM with its
// placement, and the encoder's 5.1 bed in AC-3 coded order (L C R Ls Rs
// LFE). The DD 5.1 leg reads the bed; the bypass path reads all of it.
struct RawFrame {
    std::span<const std::span<const float>> objects;
    std::span<const ac3::oba::ObjectPlacement> placements;
    std::span<const std::span<const float>> bed;
};

class OutputStage {
public:
    explicit OutputStage(OutputStageConfig config);
    ~OutputStage();
    OutputStage(const OutputStage&) = delete;
    OutputStage& operator=(const OutputStage&) = delete;

    // Enumerates and probes every render endpoint, runs the policy, and if
    // the answer differs from what is running, switches. Cheap enough to
    // call on every device event and on demand.
    const OutputStatus& reprobe(bool signing_key_loaded);
    void set_pinned(std::optional<OutputMode> pinned);
    void set_preferred_endpoint(std::string id);
    void set_null_sink_substring(std::string substring);
    void set_bypass(bool on);

    // One E-AC-3 access unit from the encoder, with the raw frame it was
    // encoded from.
    void submit(std::span<const std::byte> unit, const RawFrame& raw);

    void stop();
    [[nodiscard]] const OutputStatus& status() const { return status_; }

private:
    struct Impl;
    [[nodiscard]] bool ensure_spatial(bool has_lfe, std::size_t objects);
    void submit_raw(const RawFrame& raw);
    std::unique_ptr<Impl> impl_;
    OutputStageConfig config_;
    OutputStatus status_;
};

}  // namespace ac3::windemo
