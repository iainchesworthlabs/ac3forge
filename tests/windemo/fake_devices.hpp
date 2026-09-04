#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "audio_devices.hpp"

// In-memory AudioDevices for the windemo tests: a scripted endpoint list
// and sinks that record what reached them, taps that synthesise a tone per
// process. Everything is shared_ptr'd so a test keeps a handle on the
// sinks the stage creates and can look inside after the fact. Thread-safe
// where the engine's frame thread and the test thread both touch it.

namespace ac3::windemo::testing {

struct SinkRecord {
    std::mutex mutex;
    std::string device_id;
    std::uint32_t sample_rate = 0;
    bool eac3 = false;
    std::uint16_t channels = 0;
    std::uint32_t channel_mask = 0;
    bool low_latency = false;
    std::uint32_t static_channels = 0;
    std::uint32_t max_dynamic_objects = 0;
    bool started = false;
    bool stopped = false;
    bool refuse_start = false;
    // submit() returns false this many times before accepting, to exercise
    // the stage's patience and its underrun count.
    int refuse_submits = 0;
    std::size_t submits = 0;
    std::size_t bytes = 0;    // bursts
    std::size_t samples = 0;  // interleaved PCM
    std::size_t queued_frames = 0;  // what a PCM sink reports as its queue depth
    std::vector<float> last_pcm;
    std::size_t last_dynamic_objects = 0;
    std::size_t last_static_objects = 0;
    std::vector<float> last_object_x;  // per dynamic object, last block
};

class FakeBurstSink final : public BurstSink {
public:
    explicit FakeBurstSink(std::shared_ptr<SinkRecord> record) : record_(std::move(record)) {}
    std::expected<void, std::string> start(const std::string& device_id, std::uint32_t sample_rate,
                                           bool eac3) override {
        const std::lock_guard lock(record_->mutex);
        if (record_->refuse_start) {
            return std::unexpected("refused by the test");
        }
        record_->device_id = device_id;
        record_->sample_rate = sample_rate;
        record_->eac3 = eac3;
        record_->started = true;
        return {};
    }
    bool submit(std::span<const std::byte> burst) override {
        const std::lock_guard lock(record_->mutex);
        if (record_->refuse_submits > 0) {
            --record_->refuse_submits;
            return false;
        }
        ++record_->submits;
        record_->bytes += burst.size();
        return true;
    }
    void stop() override {
        const std::lock_guard lock(record_->mutex);
        record_->stopped = true;
    }

private:
    std::shared_ptr<SinkRecord> record_;
};

class FakePcmSink final : public PcmSink {
public:
    explicit FakePcmSink(std::shared_ptr<SinkRecord> record) : record_(std::move(record)) {}
    std::expected<void, std::string> start(const std::string& device_id, std::uint32_t sample_rate,
                                           std::uint16_t channels, std::uint32_t channel_mask,
                                           bool low_latency) override {
        const std::lock_guard lock(record_->mutex);
        if (record_->refuse_start) {
            return std::unexpected("refused by the test");
        }
        record_->device_id = device_id;
        record_->sample_rate = sample_rate;
        record_->channels = channels;
        record_->channel_mask = channel_mask;
        record_->low_latency = low_latency;
        record_->started = true;
        return {};
    }
    bool submit(std::span<const float> interleaved) override {
        const std::lock_guard lock(record_->mutex);
        if (record_->refuse_submits > 0) {
            --record_->refuse_submits;
            return false;
        }
        ++record_->submits;
        record_->samples += interleaved.size();
        record_->last_pcm.assign(interleaved.begin(), interleaved.end());
        return true;
    }
    std::size_t queued_frames() const override {
        const std::lock_guard lock(record_->mutex);
        return record_->queued_frames;
    }
    void stop() override {
        const std::lock_guard lock(record_->mutex);
        record_->stopped = true;
    }

private:
    std::shared_ptr<SinkRecord> record_;
};

class FakeObjectSink final : public ObjectSink {
public:
    explicit FakeObjectSink(std::shared_ptr<SinkRecord> record) : record_(std::move(record)) {}
    std::expected<void, std::string> start(const std::string& device_id, std::uint32_t sample_rate,
                                           std::uint32_t static_channels,
                                           std::uint32_t max_dynamic_objects) override {
        const std::lock_guard lock(record_->mutex);
        if (record_->refuse_start) {
            return std::unexpected("refused by the test");
        }
        record_->device_id = device_id;
        record_->sample_rate = sample_rate;
        record_->static_channels = static_channels;
        record_->max_dynamic_objects = max_dynamic_objects;
        record_->started = true;
        return {};
    }
    bool submit(std::span<const ac3::audio::DynamicObjectUpdate> dynamic,
                std::span<const ac3::audio::StaticObjectUpdate> static_objects) override {
        const std::lock_guard lock(record_->mutex);
        if (record_->refuse_submits > 0) {
            --record_->refuse_submits;
            return false;
        }
        ++record_->submits;
        record_->last_dynamic_objects = dynamic.size();
        record_->last_static_objects = static_objects.size();
        record_->last_object_x.clear();
        for (const auto& d : dynamic) {
            record_->last_object_x.push_back(d.x);
            record_->samples += d.pcm.size();
        }
        return true;
    }
    void stop() override {
        const std::lock_guard lock(record_->mutex);
        record_->stopped = true;
    }

private:
    std::shared_ptr<SinkRecord> record_;
};

// A tap that hands out a sine at a per-process frequency (100 Hz times the
// pid's last two digits, so two taps never share one), at full scale on
// every channel, and is never starved unless told to be.
struct TapRecord {
    std::mutex mutex;
    std::uint32_t process_id = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    bool started = false;
    bool stopped = false;
    bool refuse_start = false;
    bool starve = false;  // read() returns nothing
    std::size_t backlog = 0;  // what available() reports; read() consumes it
    std::size_t samples_read = 0;
    double phase = 0.0;
};

class FakeTap final : public TapSource {
public:
    explicit FakeTap(std::shared_ptr<TapRecord> record) : record_(std::move(record)) {}
    std::expected<void, std::string> start(std::uint32_t process_id, std::uint32_t sample_rate,
                                           std::uint16_t channels) override {
        const std::lock_guard lock(record_->mutex);
        if (record_->refuse_start) {
            return std::unexpected("refused by the test");
        }
        record_->process_id = process_id;
        record_->sample_rate = sample_rate;
        record_->channels = channels;
        record_->started = true;
        return {};
    }
    std::size_t read(std::span<float> out) override {
        const std::lock_guard lock(record_->mutex);
        if (record_->starve || record_->channels == 0) {
            return 0;
        }
        const double hz = 100.0 * static_cast<double>(1 + record_->process_id % 100);
        const double step = 2.0 * std::numbers::pi * hz / static_cast<double>(record_->sample_rate);
        for (std::size_t i = 0; i < out.size(); i += record_->channels) {
            const auto v = static_cast<float>(std::sin(record_->phase));
            for (std::size_t c = 0; c < record_->channels && i + c < out.size(); ++c) {
                out[i + c] = v;
            }
            record_->phase += step;
        }
        record_->samples_read += out.size();
        record_->backlog -= std::min(record_->backlog, out.size());
        return out.size();
    }
    // A fake tap is a generator: it reports whatever backlog the test set,
    // and flush() drains that through read().
    std::size_t available() const override {
        const std::lock_guard lock(record_->mutex);
        return record_->starve ? 0 : record_->backlog;
    }
    void stop() override {
        const std::lock_guard lock(record_->mutex);
        record_->stopped = true;
    }

private:
    std::shared_ptr<TapRecord> record_;
};

class FakeDevices final : public AudioDevices {
public:
    std::mutex mutex;
    std::vector<DeviceFacts> devices;  // what render_devices() answers
    std::size_t enumerations = 0;
    // Every sink and tap ever created, in creation order, so a test can
    // find the one a mode switch replaced as well as the live one.
    std::vector<std::shared_ptr<SinkRecord>> burst_sinks, pcm_sinks, object_sinks;
    std::vector<std::shared_ptr<TapRecord>> taps;
    // Applied to the next sink/tap created.
    bool refuse_next_start = false;
    int refuse_next_submits = 0;

    std::vector<DeviceFacts> render_devices(std::uint32_t) override {
        const std::lock_guard lock(mutex);
        ++enumerations;
        return devices;
    }
    std::unique_ptr<BurstSink> burst_sink() override {
        const std::lock_guard lock(mutex);
        auto record = fresh_sink();
        burst_sinks.push_back(record);
        return std::make_unique<FakeBurstSink>(record);
    }
    std::unique_ptr<PcmSink> pcm_sink() override {
        const std::lock_guard lock(mutex);
        auto record = fresh_sink();
        pcm_sinks.push_back(record);
        return std::make_unique<FakePcmSink>(record);
    }
    std::unique_ptr<ObjectSink> object_sink() override {
        const std::lock_guard lock(mutex);
        auto record = fresh_sink();
        object_sinks.push_back(record);
        return std::make_unique<FakeObjectSink>(record);
    }
    std::unique_ptr<TapSource> tap() override {
        const std::lock_guard lock(mutex);
        auto record = std::make_shared<TapRecord>();
        record->refuse_start = refuse_next_start;
        refuse_next_start = false;
        taps.push_back(record);
        return std::make_unique<FakeTap>(record);
    }

private:
    std::shared_ptr<SinkRecord> fresh_sink() {
        auto record = std::make_shared<SinkRecord>();
        record->refuse_start = refuse_next_start;
        record->refuse_submits = refuse_next_submits;
        refuse_next_start = false;
        refuse_next_submits = 0;
        return record;
    }
};

// Handy endpoint facts.
inline DeviceFacts hdmi_avr(const std::string& id = "avr") {
    return {.id = id, .name = "AVR (HDMI)", .is_default = false, .accepts_eac3 = true, .accepts_ac3 = true, .shared_channels = 8};
}
inline DeviceFacts realtek_default(const std::string& id = "realtek") {
    return {.id = id, .name = "Speakers (Realtek)", .is_default = true, .shared_channels = 2};
}
inline DeviceFacts null_sink(const std::string& id = "null") {
    return {.id = id, .name = "Speakers (Desktop Atmos)", .is_default = false, .shared_channels = 8};
}
inline DeviceFacts headphones_spatial(const std::string& id = "hp") {
    return {.id = id, .name = "Headphones (USB)", .is_default = false, .shared_channels = 2, .spatial = true, .spatial_max_objects = 17};
}

}  // namespace ac3::windemo::testing
