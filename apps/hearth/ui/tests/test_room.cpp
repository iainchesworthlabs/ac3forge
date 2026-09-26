#include "test_room.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <numbers>
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
#include "ac4enc/encoder.hpp"
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
        recent_.assign(format.layout.slots(), std::vector<float>(kToneWindow, 0.0F));
        recent_at_ = 0;
        recent_filled_ = 0;
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
        // The last kToneWindow samples of each slot, for tone_level_db().
        for (std::size_t n = 0; n < frames; ++n) {
            for (std::size_t s = 0; s < recent_.size(); ++s) {
                recent_[s][recent_at_] =
                    s < slots.size() && n < slots[s].size() ? slots[s][n] : 0.0F;
            }
            recent_at_ = (recent_at_ + 1) % kToneWindow;
        }
        recent_filled_ = std::min(recent_filled_ + frames, kToneWindow);
        submitted_ += frames;
        return true;
    }

    [[nodiscard]] double tone_level_db(std::size_t slot, double hz) const {
        const std::scoped_lock lock(mutex_);
        if (slot >= recent_.size() || recent_filled_ < kToneWindow || sample_rate_ == 0) {
            return -std::numeric_limits<double>::infinity();
        }
        const double w = 2.0 * std::numbers::pi * hz / static_cast<double>(sample_rate_);
        std::complex<double> sum{};
        double weights = 0.0;
        for (std::size_t k = 0; k < kToneWindow; ++k) {
            const double hann =
                0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(k) /
                                      static_cast<double>(kToneWindow - 1)));
            const float sample = recent_[slot][(recent_at_ + k) % kToneWindow];
            sum +=
                hann * static_cast<double>(sample) * std::polar(1.0, -w * static_cast<double>(k));
            weights += hann;
        }
        return 20.0 * std::log10(std::abs(2.0 * sum / weights));
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
    // Each rendered slot's last kToneWindow samples as submitted, a ring
    // written at recent_at_.
    std::vector<std::vector<float>> recent_;
    std::size_t recent_at_ = 0;
    std::size_t recent_filled_ = 0;
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

double tone_level_db(const FakeRoom& room, std::size_t slot, double hz) {
    return room.tone_level_db(slot, hz);
}

// --- AC-4 tone streams -----------------------------------------------------

namespace {

constexpr int kToneRate = 48000;
constexpr std::size_t kToneSamples = 20 * 48000;
constexpr double kToneAmplitude = 0.1;

[[nodiscard]] std::vector<float> tone(double hz) {
    std::vector<float> out(kToneSamples);
    for (std::size_t n = 0; n < out.size(); ++n) {
        out[n] = static_cast<float>(kToneAmplitude * std::sin(2.0 * std::numbers::pi * hz *
                                                              static_cast<double>(n) / kToneRate));
    }
    return out;
}

[[nodiscard]] ac4::EncoderConfig tones_config() {
    ac4::EncoderConfig config;
    config.channels = 6;
    config.bitrate_kbps = 384;
    config.dialnorm_db = -24.0;
    config.downmix = ac4::DownmixConfig{.loro_centre_db = -1.5,
                                        .loro_surround_db = -4.5,
                                        .ltrt_centre_db = -3.0,
                                        .ltrt_surround_db = -6.0,
                                        .lfe_db = -4.5,
                                        .preferred = ac4::PreferredDownmix::kLtRt,
                                        .loro_correction_db2 = std::nullopt,
                                        .ltrt_correction_db2 = std::nullopt};
    config.dialogue = ac4::DialogueConfig{};
    config.dialogue->max_gain_db = 9;
    return config;
}

[[nodiscard]] ac4::EncoderConfig presentations_config() {
    ac4::EncoderConfig config;
    config.bitrate_kbps = 256;
    ac4::SubstreamConfig music;
    music.channels = 2;
    music.bitrate_kbps = 128;
    music.content = ac4::ContentClassifier::kMusicAndEffects;
    ac4::SubstreamConfig dialogue;
    dialogue.channels = 1;
    dialogue.bitrate_kbps = 64;
    dialogue.content = ac4::ContentClassifier::kDialogue;
    dialogue.language = "en";
    dialogue.dialogue_mix = ac4::DialogueMix{.max_gain_db = 6, .pan_degrees = {}};
    ac4::SubstreamConfig described;
    described.channels = 1;
    described.bitrate_kbps = 48;
    described.content = ac4::ContentClassifier::kVisuallyImpaired;
    described.language = "qad";
    config.substreams = {music, dialogue, described};
    ac4::PresentationConfig plain;
    plain.config = 0;
    plain.substreams = {0, 1};
    plain.presentation_id = 1;
    ac4::PresentationConfig with_description;
    with_description.config = 3;
    with_description.substreams = {0, 1, 2};
    with_description.presentation_id = 2;
    config.presentations = {plain, with_description};
    return config;
}

}  // namespace

bool write_ac4_stream(const std::string& path, const std::string& kind, std::string* error) {
    const auto fail = [error](std::string why) {
        if (error != nullptr) {
            *error = std::move(why);
        }
        return false;
    };
    ac4::EncoderConfig config;
    std::vector<std::vector<float>> input;
    if (kind == "tones") {
        config = tones_config();
        for (const double hz : {440.0, 620.0, 800.0, 90.0, 1030.0, 1270.0}) {
            input.push_back(tone(hz));
        }
    } else if (kind == "presentations") {
        config = presentations_config();
        for (const double hz : {331.0, 457.0, 1117.0, 1531.0}) {
            input.push_back(tone(hz));
        }
    } else {
        return fail("no such kind of stream: " + kind);
    }
    auto encoder = ac4::Encoder::create(config);
    if (!encoder) {
        return fail(std::string{ac4::Encoder::refusal_reason(config)});
    }
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    if (!out) {
        return fail("could not open " + path);
    }
    const auto write = [&out](const std::vector<ac4::EncodedFrame>& frames) {
        for (const ac4::EncodedFrame& frame : frames) {
            const std::vector<std::byte> wrapped = ac4::sync_frame(frame.raw_ac4_frame, false);
            out.write(reinterpret_cast<const char*>(wrapped.data()),
                      static_cast<std::streamsize>(wrapped.size()));
        }
    };
    const std::vector<std::span<const float>> views(input.begin(), input.end());
    const auto frames = encoder->encode(views);
    if (!frames) {
        return fail("the encoder refused the input");
    }
    write(*frames);
    const auto rest = encoder->flush();
    if (!rest) {
        return fail("the encoder could not flush");
    }
    write(*rest);
    return out.good() ? true : fail("could not write " + path);
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
                                              bool accept_settings, std::string* error) {
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
    options.accept_settings = accept_settings;
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
