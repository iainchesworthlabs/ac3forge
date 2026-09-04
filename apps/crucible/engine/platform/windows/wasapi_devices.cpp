#include "audio_devices.hpp"

#include "platform_services.hpp"

#include <memory>
#include <string>
#include <utility>

#include "ac3/audio/capture.hpp"
#include "ac3/audio/monitor.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/spatial.hpp"

// The production AudioDevices: each interface forwards to the ac3::audio
// class of the same shape, and error enums become the library's own
// one-line descriptions. Windows-only, this directory only.

namespace ac3::crucible {

namespace {

class WasapiBurstSink final : public BurstSink {
public:
    std::expected<void, std::string> start(const std::string& device_id, std::uint32_t sample_rate,
                                           bool eac3) override {
        const auto started = sink_.start(device_id, sample_rate,
                                         eac3 ? ac3::audio::BitstreamFormat::kEac3
                                              : ac3::audio::BitstreamFormat::kAc3);
        if (!started) {
            return std::unexpected(std::string(ac3::audio::describe(started.error())));
        }
        return {};
    }
    bool submit(std::span<const std::byte> burst) override { return sink_.submit(burst); }
    void stop() override { sink_.stop(); }

private:
    ac3::audio::PassthroughSink sink_;
};

class WasapiPcmSink final : public PcmSink {
public:
    std::expected<void, std::string> start(const std::string& device_id, std::uint32_t sample_rate,
                                           std::uint16_t channels, std::uint32_t channel_mask,
                                           bool low_latency) override {
        const auto started = sink_.start(device_id, sample_rate, channels, channel_mask, low_latency);
        if (!started) {
            return std::unexpected(std::string(ac3::audio::describe(started.error())));
        }
        return {};
    }
    bool submit(std::span<const float> interleaved) override { return sink_.submit(interleaved); }
    std::size_t queued_frames() const override {
        const auto stats = sink_.stats();
        return stats.frames_submitted > stats.frames_rendered
                   ? static_cast<std::size_t>(stats.frames_submitted - stats.frames_rendered)
                   : 0;
    }
    void stop() override { sink_.stop(); }

private:
    ac3::audio::MonitorSink sink_;
};

class WasapiObjectSink final : public ObjectSink {
public:
    std::expected<void, std::string> start(const std::string& device_id, std::uint32_t sample_rate,
                                           std::uint32_t static_channels,
                                           std::uint32_t max_dynamic_objects) override {
        const auto started = sink_.start(device_id, sample_rate, static_channels, max_dynamic_objects);
        if (!started) {
            return std::unexpected(std::string(ac3::audio::describe(started.error())));
        }
        return {};
    }
    bool submit(std::span<const ac3::audio::DynamicObjectUpdate> dynamic,
                std::span<const ac3::audio::StaticObjectUpdate> static_objects) override {
        return sink_.submit(dynamic, static_objects);
    }
    void stop() override { sink_.stop(); }

private:
    ac3::audio::SpatialObjectSink sink_;
};

class WasapiTap final : public TapSource {
public:
    std::expected<void, std::string> start(std::uint32_t process_id, std::uint32_t sample_rate,
                                           std::uint16_t channels) override {
        const auto started = capture_.start_process_loopback(
            process_id, ac3::audio::ProcessLoopbackMode::kIncludeProcessTree,
            {.sample_rate = sample_rate, .channels = channels});
        if (!started) {
            return std::unexpected(std::string(ac3::audio::describe(started.error())));
        }
        return {};
    }
    std::size_t read(std::span<float> out) override {
        auto* ring = capture_.buffer();
        return ring != nullptr ? ring->read(out) : 0;
    }
    std::size_t available() const override {
        auto* ring = const_cast<ac3::audio::Capture&>(capture_).buffer();
        return ring != nullptr ? ring->available() : 0;
    }
    void stop() override { capture_.stop(); }

private:
    ac3::audio::Capture capture_;
};

class WasapiDevices final : public AudioDevices {
public:
    std::vector<DeviceFacts> render_devices(std::uint32_t sample_rate) override {
        std::vector<DeviceFacts> facts;
        const auto devices = ac3::audio::enumerate_render_devices(sample_rate);
        if (!devices) {
            return facts;
        }
        for (const auto& device : *devices) {
            DeviceFacts f{.id = device.id,
                          .name = device.name,
                          .is_default = device.is_default,
                          .accepts_eac3 = device.supports_eac3_passthrough,
                          .accepts_ac3 = device.supports_ac3_passthrough,
                          .shared_channels = device.channels};
            if (const auto spatial = ac3::audio::probe_spatial_capability(device.id);
                spatial && spatial->available) {
                f.spatial = true;
                f.spatial_max_objects = spatial->max_dynamic_objects;
            }
            facts.push_back(std::move(f));
        }
        return facts;
    }
    std::unique_ptr<BurstSink> burst_sink() override { return std::make_unique<WasapiBurstSink>(); }
    std::unique_ptr<PcmSink> pcm_sink() override { return std::make_unique<WasapiPcmSink>(); }
    std::unique_ptr<ObjectSink> object_sink() override { return std::make_unique<WasapiObjectSink>(); }
    std::unique_ptr<TapSource> tap() override { return std::make_unique<WasapiTap>(); }
};

}  // namespace

std::shared_ptr<AudioDevices> platform_audio_devices() {
    return std::make_shared<WasapiDevices>();
}

}  // namespace ac3::crucible
