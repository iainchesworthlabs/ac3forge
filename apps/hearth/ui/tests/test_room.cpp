#include "test_room.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

#include "ac3/audio/monitor.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/pcm_output.hpp"
#include "ac3/audio/speakers.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/routing.hpp"
#include "ac3/sendspin/discovery.hpp"
#include "network_sinks.hpp"
#include "output_selector.hpp"
#include "pcm_sink.hpp"
#include "sink.hpp"
#include "test_outputs.hpp"
#include "transport.hpp"

namespace ac3::hearth::uitest {

// The room: its endpoints, and the one device the engine has open among them
// - a PcmSink is one open stream at a time, whichever endpoint it names, the
// way DeviceSink wraps one ac3::audio::PcmOutput. Everything is under one
// lock: the engine thread submits and reads the position, the clock thread
// plays, and the GUI thread reads the totals a suite asserts on. The shape is
// tests/hearth/test_engine.cpp's ClockedDevice, plus what the Speakers page
// and the output picker read off a real device (routing, name, id, mask).
class FakeRoom {
public:
    explicit FakeRoom(std::vector<FakeEndpoint> endpoints)
        : endpoints_(std::move(endpoints)),
          clock_([this](const std::stop_token& stop) {
              while (!stop.stop_requested()) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
                  tick();
              }
          }) {}

    ~FakeRoom() {
        clock_.request_stop();
        clock_.join();
    }

    FakeRoom(const FakeRoom&) = delete;
    FakeRoom& operator=(const FakeRoom&) = delete;
    FakeRoom(FakeRoom&&) = delete;
    FakeRoom& operator=(FakeRoom&&) = delete;

    [[nodiscard]] const std::vector<FakeEndpoint>& endpoints() const { return endpoints_; }

    std::expected<OpenOutputFormat, std::string> open(const PcmSink::Format& format) {
        const std::scoped_lock lock(mutex_);
        const FakeEndpoint* endpoint = find_locked(format.endpoint_id);
        if (endpoint == nullptr) {
            return std::unexpected("The output could not be opened: no endpoint " + format.endpoint_id + ".");
        }
        open_ = true;
        paused_ = false;
        endpoint_ = *endpoint;
        sample_rate_ = format.sample_rate;
        width_ = endpoint->channels > 0 ? endpoint->channels : static_cast<std::uint16_t>(format.layout.slots());
        mask_ = endpoint->speakers != 0 ? endpoint->speakers : audio::default_speakers(width_);
        routing_ = audio::speaker_routing(format.layout, mask_, width_);
        submitted_ = 0;
        heard_ = 0;
        clock_frames_ = 0;
        carry_ = 0.0;
        ++opens_;
        return OpenOutputFormat{.sample_rate = format.sample_rate, .channels = width_, .mode = OutputMode::kLocalPcm};
    }

    void close() {
        const std::scoped_lock lock(mutex_);
        open_ = false;
    }

    [[nodiscard]] bool is_open() const {
        const std::scoped_lock lock(mutex_);
        return open_;
    }

    bool submit(std::span<const std::span<const float>> slots, std::size_t frames) {
        const std::scoped_lock lock(mutex_);
        if (!open_ || submitted_ - heard_ + frames > kCapacity) {
            return false;
        }
        for (const std::span<const float> slot : slots) {
            for (std::size_t n = 0; n < frames && n < slot.size(); ++n) {
                peak_ = std::max(peak_, static_cast<double>(std::fabs(slot[n])));
            }
        }
        submitted_ += frames;
        return true;
    }

    [[nodiscard]] std::optional<audio::MonitorPosition> position() const {
        const std::scoped_lock lock(mutex_);
        if (!open_) {
            return std::nullopt;
        }
        return audio::MonitorPosition{
            .frames_played = clock_frames_, .frames_queued = submitted_ - heard_, .latency_frames = 0};
    }

    void flush() {
        const std::scoped_lock lock(mutex_);
        submitted_ = 0;
        heard_ = 0;
        clock_frames_ = 0;
    }

    bool pause() {
        const std::scoped_lock lock(mutex_);
        paused_ = true;
        return open_;
    }

    bool resume() {
        const std::scoped_lock lock(mutex_);
        paused_ = false;
        return open_;
    }

    bool set_routing(const render::Routing& routing) {
        const std::scoped_lock lock(mutex_);
        if (!open_ || routing.outputs() != width_) {
            return false;
        }
        routing_ = routing;
        return true;
    }

    [[nodiscard]] render::Routing routing() const {
        const std::scoped_lock lock(mutex_);
        return open_ ? routing_ : render::Routing{};
    }

    [[nodiscard]] std::string device_name() const {
        const std::scoped_lock lock(mutex_);
        return open_ ? endpoint_.name : std::string();
    }

    [[nodiscard]] std::string device_id() const {
        const std::scoped_lock lock(mutex_);
        return open_ ? endpoint_.id : std::string();
    }

    [[nodiscard]] std::uint32_t speaker_mask() const {
        const std::scoped_lock lock(mutex_);
        return open_ ? mask_ : 0;
    }

    [[nodiscard]] RoomReading reading() const {
        const std::scoped_lock lock(mutex_);
        return RoomReading{.open = open_,
                           .paused = paused_,
                           .endpoint = open_ ? endpoint_.id : std::string(),
                           .sample_rate = open_ ? sample_rate_ : 0,
                           .channels = open_ ? width_ : std::uint16_t{0},
                           .opens = opens_,
                           .frames_heard = heard_total_,
                           .peak = peak_};
    }

    void reset_peak() {
        const std::scoped_lock lock(mutex_);
        peak_ = 0.0;
    }

    void set_speed(double speed) {
        const std::scoped_lock lock(mutex_);
        speed_ = std::max(0.0, speed);
    }

private:
    // Frames the device holds before submit() refuses - a third of a second
    // at 48 kHz, enough for the engine's own pump budget to keep it fed.
    static constexpr std::uint64_t kCapacity = 16384;

    // One millisecond of the device's own clock: what it holds is heard
    // first, and the clock runs on whether or not there is anything left to
    // hear, as a real device does through an underrun.
    void tick() {
        const std::scoped_lock lock(mutex_);
        if (!open_ || paused_ || sample_rate_ == 0) {
            return;
        }
        carry_ += static_cast<double>(sample_rate_) * speed_ / 1000.0;
        const auto frames = static_cast<std::uint64_t>(carry_);
        carry_ -= static_cast<double>(frames);
        const std::uint64_t now = std::min(submitted_ - heard_, frames);
        heard_ += now;
        heard_total_ += now;
        clock_frames_ += frames;
    }

    // An empty id is the engine's "the sink's own", which for a real device
    // is the enumeration's default - the same reading here.
    const FakeEndpoint* find_locked(const std::string& id) const {
        for (const FakeEndpoint& endpoint : endpoints_) {
            if (id.empty() ? endpoint.is_default : endpoint.id == id) {
                return &endpoint;
            }
        }
        return id.empty() && !endpoints_.empty() ? endpoints_.data() : nullptr;
    }

    const std::vector<FakeEndpoint> endpoints_;
    mutable std::mutex mutex_;
    bool open_ = false;
    bool paused_ = false;
    FakeEndpoint endpoint_{};
    std::uint32_t sample_rate_ = 0;
    std::uint16_t width_ = 0;
    std::uint32_t mask_ = 0;
    render::Routing routing_{};
    std::uint64_t submitted_ = 0;
    std::uint64_t heard_ = 0;
    std::uint64_t heard_total_ = 0;
    std::uint64_t clock_frames_ = 0;
    std::uint32_t opens_ = 0;
    double carry_ = 0.0;
    double speed_ = 1.0;
    double peak_ = 0.0;
    // Last, so it starts after, and is joined before, everything it reads.
    std::jthread clock_;
};

namespace {

// The engine owns its PcmSink outright, the room outlives it (TestServices
// holds it for the process): this forwards every call to the room.
class RoomSink final : public PcmSink {
public:
    explicit RoomSink(std::shared_ptr<FakeRoom> room) : room_(std::move(room)) {}

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override { return room_->open(format); }
    void close() override { room_->close(); }
    [[nodiscard]] bool is_open() const override { return room_->is_open(); }
    bool submit(std::span<const std::span<const float>> slots, std::size_t frames) override {
        return room_->submit(slots, frames);
    }
    [[nodiscard]] std::optional<audio::MonitorPosition> position() const override { return room_->position(); }
    void flush() override { room_->flush(); }
    bool pause() override { return room_->pause(); }
    bool resume() override { return room_->resume(); }
    bool set_routing(const render::Routing& routing) override { return room_->set_routing(routing); }
    [[nodiscard]] render::Routing routing() const override { return room_->routing(); }
    [[nodiscard]] std::string device_name() const override { return room_->device_name(); }
    [[nodiscard]] std::string device_id() const override { return room_->device_id(); }
    [[nodiscard]] std::uint32_t speaker_mask() const override { return room_->speaker_mask(); }

private:
    std::shared_ptr<FakeRoom> room_;
};

audio::RenderDeviceInfo device_info(const FakeEndpoint& endpoint) {
    audio::RenderDeviceInfo info;
    info.id = endpoint.id;
    info.name = endpoint.name;
    info.is_default = endpoint.is_default;
    info.supports_ac3_passthrough = endpoint.passthrough;
    info.supports_eac3_passthrough = endpoint.passthrough;
    info.supports_exclusive_pcm = true;
    info.channels = endpoint.channels;
    info.speakers = endpoint.speakers != 0 ? endpoint.speakers : audio::default_speakers(endpoint.channels);
    info.sample_rates = {44100, 48000};
    return info;
}

}  // namespace

std::shared_ptr<FakeRoom> make_room(std::vector<FakeEndpoint> endpoints) {
    return std::make_shared<FakeRoom>(std::move(endpoints));
}

std::shared_ptr<ac3::hearth::ui::TestOutputs> outputs_for(const std::shared_ptr<FakeRoom>& room) {
    auto outputs = std::make_shared<ac3::hearth::ui::TestOutputs>();
    outputs->make_pcm = [room] { return std::make_unique<RoomSink>(room); };
    outputs->endpoints = [room](std::uint32_t /*sample_rate*/) {
        std::vector<EndpointReading> readings;
        for (const FakeEndpoint& endpoint : room->endpoints()) {
            readings.push_back(EndpointReading{.device = device_info(endpoint)});
        }
        return readings;
    };
    outputs->enumerate = [room]() -> decltype(audio::enumerate_render_devices()) {
        std::vector<audio::RenderDeviceInfo> devices;
        for (const FakeEndpoint& endpoint : room->endpoints()) {
            devices.push_back(device_info(endpoint));
        }
        return devices;
    };
    return outputs;
}

RoomReading read(const FakeRoom& room) {
    return room.reading();
}

void reset_peak(FakeRoom& room) {
    room.reset_peak();
}

void set_speed(FakeRoom& room, double speed) {
    room.set_speed(speed);
}

// --- the test sink ---------------------------------------------------------

class TestSinkHost {
public:
    class Log final : public testsink::SinkLog {
    public:
        void line(std::string_view text) override {
            const std::scoped_lock lock(mutex_);
            lines_.emplace_back(text);
        }
        [[nodiscard]] std::vector<std::string> lines() const {
            const std::scoped_lock lock(mutex_);
            return lines_;
        }

    private:
        mutable std::mutex mutex_;
        std::vector<std::string> lines_;
    };

    std::string name;
    Log log;
    std::unique_ptr<testsink::Sink> sink;
};

std::shared_ptr<TestSinkHost> start_test_sink(const std::string& name, const std::string& directory,
                                              std::string* error) {
    auto host = std::make_shared<TestSinkHost>();
    host->name = name;
    testsink::SinkOptions options;
    options.name = name;
    options.address = "127.0.0.1";
    options.port = 0;
    options.state_directory = std::filesystem::path(directory) / "state";
    options.output_directory = std::filesystem::path(directory) / "out";
    options.advertise = false;
    options.code_method = testsink::CodeMethod::kDynamic;
    // A 5.1 sink, so its own Speakers tab has more than a stereo pair to
    // route and trim - the default 7.1.4 would do too, but twelve rows is
    // more page than a fast test needs to draw.
    options.layout = "5.1";
    auto started = testsink::Sink::start(options, host->log);
    if (!started.has_value()) {
        if (error != nullptr) {
            *error = started.error();
        }
        return nullptr;
    }
    host->sink = std::move(*started);
    return host;
}

void announce(ac3::hearth::NetworkSinks& sinks, const TestSinkHost& host) {
    sinks.on_found(sendspin::discovery::Service{.instance = host.name,
                                                .host = "127.0.0.1",
                                                .addresses = {"127.0.0.1"},
                                                .port = host.sink->port(),
                                                .txt = {{.key = "path", .value = "/sendspin"}}});
}

std::string pairing_code(const TestSinkHost& host) {
    const std::vector<std::string> lines = host.log.lines();
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const std::size_t at = it->find("PAIRING CODE ");
        if (at != std::string::npos) {
            std::string digits;
            for (const char c : it->substr(at + 13)) {
                if (c >= '0' && c <= '9') {
                    digits.push_back(c);
                }
            }
            return digits;
        }
    }
    return {};
}

std::vector<std::string> log_lines(const TestSinkHost& host) {
    return host.log.lines();
}

std::uint32_t connections(const TestSinkHost& host) {
    return host.sink->totals().connections;
}

}  // namespace ac3::hearth::uitest
