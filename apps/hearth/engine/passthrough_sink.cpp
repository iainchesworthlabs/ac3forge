#include <fmt/format.h>

#include <utility>

#include "ac3/audio/passthrough.hpp"
#include "bitstream_sink.hpp"

// The BitstreamSink over a real local endpoint: ac3::audio::PassthroughSink.
// Everything here is a translation between the two interfaces, as
// device_sink.cpp is for the PCM side.

namespace ac3::hearth {

namespace {

class PassthroughDeviceSink final : public BitstreamSink {
public:
    explicit PassthroughDeviceSink(std::string device_id) : device_id_(std::move(device_id)) {}

    ~PassthroughDeviceSink() override { sink_.stop(); }

    PassthroughDeviceSink(const PassthroughDeviceSink&) = delete;
    PassthroughDeviceSink& operator=(const PassthroughDeviceSink&) = delete;

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        sink_.stop();
        const std::string& endpoint = format.endpoint_id.empty() ? device_id_ : format.endpoint_id;
        const auto started = sink_.start(endpoint, format.sample_rate, format.format);
        if (!started) {
            return std::unexpected(fmt::format(
                "The output would not take {} at {} Hz over IEC 61937: {}.",
                format.format == audio::BitstreamFormat::kAc3 ? "AC-3" : "E-AC-3",
                format.sample_rate, audio::describe(started.error())));
        }
        return OpenOutputFormat{.sample_rate = format.sample_rate,
                                .channels = 2,
                                .mode = OutputMode::kBitstream,
                                .stream = format.format};
    }

    void close() override { sink_.stop(); }

    [[nodiscard]] bool is_open() const override { return sink_.running(); }

    bool submit(std::span<const std::byte> burst) override { return sink_.submit(burst); }

    [[nodiscard]] std::optional<audio::MonitorPosition> position() const override {
        return sink_.position();
    }

    void flush() override { sink_.flush(); }

    bool pause() override { return sink_.pause().has_value(); }

    bool resume() override { return sink_.resume().has_value(); }

private:
    std::string device_id_;
    audio::PassthroughSink sink_;
};

}  // namespace

std::unique_ptr<BitstreamSink> make_passthrough_sink(std::string device_id) {
    return std::make_unique<PassthroughDeviceSink>(std::move(device_id));
}

}  // namespace ac3::hearth
