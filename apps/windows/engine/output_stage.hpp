#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
// than bypass it. A switch stops the old sink and starts the new one; the
// encoder upstream never notices.

namespace ac3::windemo {

struct OutputStageConfig {
    // The null sink is recognised by name (the driver's own, or a stand-in
    // such as FxSound's idle endpoint during development).
    std::string null_sink_substring = "Desktop Atmos";
    std::optional<OutputMode> pinned;
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
    void set_null_sink_substring(std::string substring);

    // One E-AC-3 access unit from the encoder, and the encoder's own 5.1
    // bed for the same frame (AC-3 coded order: L C R Ls Rs LFE), which
    // only the DD 5.1 leg reads.
    void submit(std::span<const std::byte> unit, std::span<const std::span<const float>> bed);

    void stop();
    [[nodiscard]] const OutputStatus& status() const { return status_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    OutputStageConfig config_;
    OutputStatus status_;
};

}  // namespace ac3::windemo
