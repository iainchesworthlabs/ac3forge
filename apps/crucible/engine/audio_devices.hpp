#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/audio/spatial.hpp"

// The seam between the engine and the machine's audio devices
// (docs/platforms/windows-demo.md, "Test remediation"). Everything the
// output stage and the tap pool ask of the library's WASAPI classes is
// behind these interfaces: enumerate and probe render endpoints, open one
// of three kinds of sink on an endpoint, open a per-process tap. The
// production implementation for each platform (platform/<os>/, behind
// platform_audio_devices() in platform_services.hpp) forwards
// to ac3::audio; tests/crucible/fake_devices.hpp scripts endpoints and
// records what was submitted, so the frame loop, the five routes, the
// bypass fold and a mode switch mid-stream run in a plain Catch2 process
// with no audio hardware.
//
// The shapes are the library's own, deliberately: submit() returns false
// when the sink is ahead of real time and the caller waits, exactly as
// MonitorSink/PassthroughSink/SpatialObjectSink document, so the stage's
// submit_with_patience() behaves identically over a fake and over WASAPI.

namespace ac3::crucible {

// What a render endpoint probe finds, before the policy turns it into
// EndpointFacts (output_policy.hpp adds the null-sink judgement).
struct DeviceFacts {
    std::string id;
    std::string name;
    bool is_default = false;
    bool accepts_eac3 = false;
    bool accepts_ac3 = false;
    std::uint16_t shared_channels = 0;
    bool spatial = false;
    std::uint32_t spatial_max_objects = 0;
};

// IEC 61937 bursts to an exclusive-mode endpoint (Atmos, DD+ 5.1, DD 5.1).
class BurstSink {
public:
    virtual ~BurstSink() = default;
    [[nodiscard]] virtual std::expected<void, std::string> start(const std::string& device_id,
                                                                 std::uint32_t sample_rate,
                                                                 bool eac3) = 0;
    virtual bool submit(std::span<const std::byte> burst) = 0;
    virtual void stop() = 0;
};

// Interleaved PCM to a shared-mode endpoint (PCM surround, stereo).
class PcmSink {
public:
    virtual ~PcmSink() = default;
    // `low_latency` asks for the platform's smallest render period.
    [[nodiscard]] virtual std::expected<void, std::string> start(const std::string& device_id,
                                                                 std::uint32_t sample_rate,
                                                                 std::uint16_t channels,
                                                                 std::uint32_t channel_mask,
                                                                 bool low_latency) = 0;
    virtual bool submit(std::span<const float> interleaved) = 0;
    // Sample-frames submitted but not yet rendered: the sink's queue depth,
    // which is latency once the sink has started consuming.
    [[nodiscard]] virtual std::size_t queued_frames() const = 0;
    virtual void stop() = 0;
};

// Per-object PCM with positions to a spatial-sound endpoint (headphones).
class ObjectSink {
public:
    virtual ~ObjectSink() = default;
    [[nodiscard]] virtual std::expected<void, std::string> start(const std::string& device_id,
                                                                 std::uint32_t sample_rate,
                                                                 std::uint32_t static_channels,
                                                                 std::uint32_t max_dynamic_objects) = 0;
    virtual bool submit(std::span<const ac3::audio::DynamicObjectUpdate> dynamic,
                        std::span<const ac3::audio::StaticObjectUpdate> static_objects) = 0;
    virtual void stop() = 0;
};

// One application's process-loopback tap: interleaved float at the format
// asked for, silence-filled at wall-clock rate by the producer, read by
// the engine's frame thread.
class TapSource {
public:
    virtual ~TapSource() = default;
    [[nodiscard]] virtual std::expected<void, std::string> start(std::uint32_t process_id,
                                                                 std::uint32_t sample_rate,
                                                                 std::uint16_t channels) = 0;
    // Up to out.size() samples, as many as are available now.
    virtual std::size_t read(std::span<float> out) = 0;
    // Samples waiting to be read: the tap's backlog, which is latency.
    [[nodiscard]] virtual std::size_t available() const = 0;
    virtual void stop() = 0;
};

// The factory the engine is built over.
class AudioDevices {
public:
    virtual ~AudioDevices() = default;
    [[nodiscard]] virtual std::vector<DeviceFacts> render_devices(std::uint32_t sample_rate) = 0;
    [[nodiscard]] virtual std::unique_ptr<BurstSink> burst_sink() = 0;
    [[nodiscard]] virtual std::unique_ptr<PcmSink> pcm_sink() = 0;
    [[nodiscard]] virtual std::unique_ptr<ObjectSink> object_sink() = 0;
    [[nodiscard]] virtual std::unique_ptr<TapSource> tap() = 0;
};

}  // namespace ac3::crucible
