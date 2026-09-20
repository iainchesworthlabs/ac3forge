#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ac3/audio/audio_backend.hpp"
#include "ac3/audio/render_devices.hpp"

// ac3::audio::RenderDeviceWatch against a fake enumeration
// (src/audio/src/render_devices.cpp): a list that keeps itself current whether
// or not the platform reports that something changed.
//
// No sound card and no platform watcher: `Sources` takes an enumeration of
// the test's own and turns the watcher off, which is exactly the shape ALSA
// runs in - the timer is all there is. That is the half worth testing
// headlessly, because it is the half with the bookkeeping in it. The
// notification half belongs to DeviceWatcher, which has its own hidden
// hardware case, and to the Pi and the receiver.

namespace {

using ac3::audio::PassthroughError;
using ac3::audio::RenderDeviceInfo;
using ac3::audio::RenderDeviceWatch;

RenderDeviceInfo device_of(std::string id, std::uint16_t channels) {
    RenderDeviceInfo device;
    device.id = std::move(id);
    device.name = device.id;
    device.channels = channels;
    return device;
}

// A list the test moves under the watch's feet, the way a hot-plug does.
class FakeDevices {
public:
    void set(std::vector<RenderDeviceInfo> devices) {
        const std::lock_guard<std::mutex> guard{mutex_};
        devices_ = std::move(devices);
    }

    void fail(bool failing) { failing_.store(failing); }

    [[nodiscard]] RenderDeviceWatch::Enumerate enumerate() {
        return [this](std::uint32_t rate)
                   -> std::expected<std::vector<RenderDeviceInfo>, PassthroughError> {
            rates_.fetch_add(rate == 48000 ? 1 : 0);
            if (failing_.load()) {
                return std::unexpected(PassthroughError::kComFailure);
            }
            const std::lock_guard<std::mutex> guard{mutex_};
            return devices_;
        };
    }

    [[nodiscard]] std::uint64_t probes_at_48k() const { return rates_.load(); }

private:
    mutable std::mutex mutex_;
    std::vector<RenderDeviceInfo> devices_;
    std::atomic_bool failing_{false};
    std::atomic<std::uint64_t> rates_{0};
};

// Waits for `predicate`, so a test never sleeps for a fixed guess at how long
// a worker thread takes. Generous, since a loaded build machine is slow.
template <typename Predicate>
bool waited_for(Predicate predicate) {
    for (int attempt = 0; attempt < 2000; ++attempt) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

constexpr RenderDeviceWatch::Options kEager{.sample_rate = 48000,
                                            .reprobe = std::chrono::milliseconds{5}};

}  // namespace

TEST_CASE("render devices: the first list is the start's own", "[audio-backend][render-devices]") {
    FakeDevices fake;
    fake.set({device_of("hdmi", 8), device_of("jack", 2)});

    RenderDeviceWatch watch;
    const auto started = watch.start(kEager, /*on_change=*/{},
                                     {.enumerate = fake.enumerate(), .platform_watcher = false});
    REQUIRE(started.has_value());
    CHECK(watch.running());

    const auto snapshot = watch.snapshot();
    CHECK(snapshot.devices.size() == 2);
    CHECK(snapshot.devices[0].id == "hdmi");
    CHECK(snapshot.generation == 1);
    // No platform notifications behind this list, which is what ALSA always
    // reports and what the caller needs to know before it trusts freshness.
    CHECK_FALSE(snapshot.watched);
    CHECK(watch.stats().probes == 1);
    CHECK(watch.stats().changes == 0);
    CHECK(watch.stats().events == 0);
    CHECK(fake.probes_at_48k() >= 1);

    watch.stop();
    CHECK_FALSE(watch.running());
}

TEST_CASE("render devices: a device arriving is noticed without a notification",
          "[audio-backend][render-devices]") {
    FakeDevices fake;
    fake.set({device_of("jack", 2)});

    std::atomic<std::uint64_t> changes{0};
    RenderDeviceWatch watch;
    REQUIRE(watch
                .start(kEager, [&changes] { changes.fetch_add(1); },
                       {.enumerate = fake.enumerate(), .platform_watcher = false})
                .has_value());
    CHECK(watch.snapshot().generation == 1);

    // The receiver is plugged in.
    fake.set({device_of("jack", 2), device_of("hdmi", 8)});
    REQUIRE(waited_for([&watch] { return watch.snapshot().generation == 2; }));
    CHECK(changes.load() == 1);
    CHECK(watch.snapshot().devices.size() == 2);

    // And unplugged again.
    fake.set({device_of("jack", 2)});
    REQUIRE(waited_for([&watch] { return watch.snapshot().generation == 3; }));
    CHECK(changes.load() == 2);
    CHECK(watch.snapshot().devices.size() == 1);

    // A list that has not changed is not a change, however often it is
    // probed - a picker must not be rebuilt every two seconds for nothing.
    const auto probes = watch.stats().probes;
    REQUIRE(waited_for([&watch, probes] { return watch.stats().probes > probes + 2; }));
    CHECK(watch.snapshot().generation == 3);
    CHECK(changes.load() == 2);
    CHECK(watch.stats().changes == 2);

    watch.stop();
}

TEST_CASE("render devices: a device's own capabilities changing is a change too",
          "[audio-backend][render-devices]") {
    FakeDevices fake;
    fake.set({device_of("hdmi", 2)});

    RenderDeviceWatch watch;
    REQUIRE(watch
                .start(kEager, /*on_change=*/{},
                       {.enumerate = fake.enumerate(), .platform_watcher = false})
                .has_value());

    // The same endpoint, renegotiated: an eight-channel receiver where a
    // two-channel one was. Nothing arrived or left, and a list compared by
    // id alone would have missed it.
    fake.set({device_of("hdmi", 8)});
    REQUIRE(waited_for([&watch] { return watch.snapshot().generation == 2; }));
    CHECK(watch.snapshot().devices[0].channels == 8);

    watch.stop();
}

TEST_CASE("render devices: a refresh does not wait for the timer",
          "[audio-backend][render-devices]") {
    FakeDevices fake;
    fake.set({device_of("jack", 2)});

    // A re-probe interval long enough that the timer cannot be what answers.
    RenderDeviceWatch watch;
    REQUIRE(watch
                .start({.sample_rate = 48000, .reprobe = std::chrono::minutes{10}},
                       /*on_change=*/{},
                       {.enumerate = fake.enumerate(), .platform_watcher = false})
                .has_value());

    fake.set({device_of("jack", 2), device_of("hdmi", 8)});
    watch.refresh();
    REQUIRE(waited_for([&watch] { return watch.snapshot().generation == 2; }));
    CHECK(watch.snapshot().devices.size() == 2);

    watch.stop();
}

TEST_CASE("render devices: a failed probe keeps the last good list",
          "[audio-backend][render-devices]") {
    FakeDevices fake;
    fake.set({device_of("hdmi", 8)});

    RenderDeviceWatch watch;
    REQUIRE(watch
                .start(kEager, /*on_change=*/{},
                       {.enumerate = fake.enumerate(), .platform_watcher = false})
                .has_value());

    // The audio service restarts, or something holds a device exclusively:
    // enumeration refuses. An empty picker would be the wrong answer, and a
    // generation bump would tell a caller to rebuild one.
    fake.fail(true);
    REQUIRE(waited_for([&watch] { return watch.stats().probe_failures > 0; }));
    CHECK(watch.snapshot().devices.size() == 1);
    CHECK(watch.snapshot().generation == 1);

    // And it comes back.
    fake.fail(false);
    fake.set({device_of("hdmi", 8), device_of("jack", 2)});
    REQUIRE(waited_for([&watch] { return watch.snapshot().generation == 2; }));
    CHECK(watch.snapshot().devices.size() == 2);

    watch.stop();
}

TEST_CASE("render devices: nothing to enumerate is nothing to watch",
          "[audio-backend][render-devices]") {
    FakeDevices fake;
    fake.fail(true);

    // A platform with no audio backend at all: the first enumeration is this
    // call's own, so the refusal is reported here rather than by a worker
    // that would fail forever.
    RenderDeviceWatch watch;
    const auto started = watch.start(kEager, /*on_change=*/{},
                                     {.enumerate = fake.enumerate(), .platform_watcher = false});
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == ac3::audio::DeviceWatchError::kNoBackend);
    CHECK_FALSE(watch.running());
    CHECK(watch.snapshot().devices.empty());
}

TEST_CASE("render devices: a real watch agrees with what the backend reports",
          "[audio-backend][render-devices][concurrency]") {
    // The one case here that uses the platform's own enumeration and
    // watcher. It needs no sound card - registering for notifications does
    // not, and an empty device list is a valid list - so unlike playback it
    // can run on a CI runner or in a container, the same reasoning
    // test_audio_backend.cpp's own device-watch case gives.
    const auto& backend = ac3::audio::audio_backend();
    RenderDeviceWatch watch;
    const auto started =
        watch.start({.sample_rate = 48000, .reprobe = std::chrono::seconds{1}}, /*on_change=*/{});
    if (!started) {
        // Two ways to have nothing to watch: no enumeration in this build at
        // all, or one that refused here (no session daemon, no audio in the
        // container). Both are reported as kNoBackend by start(), which
        // cannot tell them apart and does not pretend to.
        CHECK(started.error() == ac3::audio::DeviceWatchError::kNoBackend);
        CHECK_FALSE(watch.running());
        CHECK_FALSE((backend.passthrough.available && backend.device_watch.available &&
                     backend.monitor.available));
        return;
    }
    CHECK(watch.running());
    CHECK(watch.snapshot().generation == 1);
    CHECK(watch.stats().probes == 1);
    // Only the negative direction is guaranteed: a backend with no watcher
    // cannot have registered one. The converse is not an assertion - PipeWire's
    // watcher is built but needs a session daemon to register with, so an
    // available capability can still refuse here (Crucible cross-platform promotion).
    if (!backend.device_watch.available) {
        CHECK_FALSE(watch.snapshot().watched);
    }
    watch.stop();
    CHECK_FALSE(watch.running());
}

TEST_CASE("render devices: stopping twice, and starting again after a stop",
          "[audio-backend][render-devices][concurrency]") {
    FakeDevices fake;
    fake.set({device_of("jack", 2)});

    RenderDeviceWatch watch;
    const RenderDeviceWatch::Sources sources{.enumerate = fake.enumerate(),
                                             .platform_watcher = false};
    REQUIRE(watch.start(kEager, /*on_change=*/{}, sources).has_value());
    // A second start while running is refused rather than leaking a worker.
    const auto again = watch.start(kEager, /*on_change=*/{}, sources);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == ac3::audio::DeviceWatchError::kAlreadyRunning);

    watch.stop();
    watch.stop();
    CHECK_FALSE(watch.running());

    // Started again, the count begins from one: a caller holding a
    // generation from the previous run must see it change.
    REQUIRE(watch.start(kEager, /*on_change=*/{}, sources).has_value());
    CHECK(watch.snapshot().generation == 1);
    watch.stop();
}
