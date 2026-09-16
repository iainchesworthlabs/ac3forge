#include <fmt/format.h>

#include <utility>

#include "ac3/audio/pcm_output.hpp"
#include "pcm_sink.hpp"

// The PcmSink over a real local device: ac3::audio::PcmOutput (A2), which
// opens at the device's own width and places each rendered slot by the
// device's speakers. Everything here is a translation between the two
// interfaces; the decisions are PcmOutput's.

namespace ac3::hearth {

namespace {

class DeviceSink final : public PcmSink {
public:
    DeviceSink(std::string device_id, bool low_latency)
        : device_id_(std::move(device_id)), low_latency_(low_latency) {}

    ~DeviceSink() override { output_.stop(); }

    DeviceSink(const DeviceSink&) = delete;
    DeviceSink& operator=(const DeviceSink&) = delete;

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        output_.stop();
        const auto opened = output_.start(device_id_, format.sample_rate, format.layout,
                                          low_latency_);
        if (!opened) {
            return std::unexpected(fmt::format("The output could not be opened at {} Hz: {}.",
                                               format.sample_rate,
                                               audio::describe(opened.error())));
        }
        return OpenOutputFormat{.sample_rate = opened->sample_rate,
                                .channels = opened->outputs,
                                .mode = OutputMode::kLocalPcm};
    }

    void close() override { output_.stop(); }

    [[nodiscard]] bool is_open() const override { return output_.running(); }

    bool submit(std::span<const std::span<const float>> slots, std::size_t frames) override {
        return output_.submit(slots, frames);
    }

    [[nodiscard]] std::optional<audio::MonitorPosition> position() const override {
        return output_.position();
    }

    void flush() override { output_.flush(); }

    bool pause() override { return output_.pause().has_value(); }

    bool resume() override { return output_.resume().has_value(); }

private:
    std::string device_id_;
    bool low_latency_ = false;
    audio::PcmOutput output_;
};

}  // namespace

std::unique_ptr<PcmSink> make_device_sink(std::string device_id, bool low_latency) {
    return std::make_unique<DeviceSink>(std::move(device_id), low_latency);
}

}  // namespace ac3::hearth
