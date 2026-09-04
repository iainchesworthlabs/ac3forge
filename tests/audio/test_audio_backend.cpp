#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "ac3/audio/capture.hpp"
#include "ac3/audio/audio_backend.hpp"
#include "ac3/audio/device_watcher.hpp"
#include "ac3/audio/monitor.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/spatial.hpp"

// The backend tree, tested from outside it.
//
// One directory of src/audio/src/backend/ is compiled into the library and the
// others are not, so this file cannot name a backend - and does not want to.
// What it checks is the contract every backend has to keep, which is that
// ac3::audio::audio_backend() and the implementations beside it agree:
//
//   * a capability reported unavailable must actually refuse, with kNoBackend
//     and nothing else, because the CLI prints that report instead of calling
//     and would otherwise be lying to the user;
//   * a capability reported available must not answer kNoBackend, because the
//     report said the code exists;
//   * every error code must describe itself, because those strings are
//     printed verbatim.
//
// None of it touches audio hardware. Enumeration on a real backend walks the
// machine's sound cards, and a machine with none - a container, a CI runner,
// WSL - enumerates an empty list, which is a success. Nothing here opens a
// device, starts a thread or makes a sound, so the suite stays runnable
// headless on every platform it is built for.

TEST_CASE("audio_backend reports a reason exactly when a capability is missing",
          "[audio-backend][concurrency]") {
    const auto& backend = ac3::audio::audio_backend();

    for (const auto& capability :
         {backend.capture, backend.passthrough, backend.monitor, backend.spatial,
          backend.process_loopback, backend.device_watch}) {
        if (capability.available) {
            // Nothing to excuse, so nothing to say.
            CHECK(capability.reason.empty());
        } else {
            // The CLI prints this instead of running the command; an empty
            // string would print as "UNAVAILABLE HERE — ." and tell nobody
            // anything.
            CHECK_FALSE(capability.reason.empty());
        }
    }
}

TEST_CASE("capture enumeration agrees with the reported capability",
          "[audio-backend][concurrency]") {
    const auto& capture = ac3::audio::audio_backend().capture;
    const auto devices = ac3::audio::enumerate_devices();

    if (capture.available) {
        // A machine with no capture hardware is entitled to an empty list;
        // what it is not entitled to is claiming there is no backend when the
        // report next door says there is one.
        if (!devices.has_value()) {
            CHECK(devices.error() != ac3::audio::CaptureError::kNoBackend);
        }
    } else {
        REQUIRE_FALSE(devices.has_value());
        CHECK(devices.error() == ac3::audio::CaptureError::kNoBackend);
    }
}

TEST_CASE("passthrough enumeration agrees with the reported capability",
          "[audio-backend][concurrency]") {
    const auto& passthrough = ac3::audio::audio_backend().passthrough;
    const auto devices = ac3::audio::enumerate_render_devices();

    if (passthrough.available) {
        if (!devices.has_value()) {
            CHECK(devices.error() != ac3::audio::PassthroughError::kNoBackend);
        }
    } else {
        REQUIRE_FALSE(devices.has_value());
        CHECK(devices.error() == ac3::audio::PassthroughError::kNoBackend);
    }
}

TEST_CASE("a device list is well formed whatever the machine has in it",
          "[audio-backend][concurrency]") {
    const auto devices = ac3::audio::enumerate_devices();
    if (!devices) {
        SUCCEED("no capture backend in this build");
        return;
    }
    for (const auto& device : *devices) {
        // An id is what start() is handed back; an entry without one names a
        // device nobody can open.
        CHECK_FALSE(device.id.empty());
        CHECK_FALSE(device.name.empty());
    }
}

TEST_CASE("a render device list is well formed whatever the machine has in it",
          "[audio-backend][concurrency]") {
    const auto devices = ac3::audio::enumerate_render_devices();
    if (!devices) {
        SUCCEED("no passthrough backend in this build");
        return;
    }
    for (const auto& device : *devices) {
        CHECK_FALSE(device.id.empty());
        CHECK_FALSE(device.name.empty());
    }
}

TEST_CASE("spatial capability probing agrees with the reported capability",
          "[audio-backend][concurrency]") {
    // probe_spatial_capability("") probes the DEFAULT render endpoint, which
    // exists on every real machine but not in a container/CI runner with no
    // sound card at all - so, unlike the capture/passthrough enumeration
    // tests above, a missing endpoint (kDeviceNotFound) is not itself proof
    // of anything about `spatial.available`. What has to agree is only the
    // one signal that DOES mean "this build has no backend at all":
    // kNoBackend.
    const auto& spatial = ac3::audio::audio_backend().spatial;
    const auto probed = ac3::audio::probe_spatial_capability("");
    if (spatial.available) {
        if (!probed.has_value()) {
            CHECK(probed.error() != ac3::audio::SpatialError::kNoBackend);
        }
    } else {
        REQUIRE_FALSE(probed.has_value());
        CHECK(probed.error() == ac3::audio::SpatialError::kNoBackend);
    }
}

TEST_CASE("every spatial error describes itself", "[audio-backend][concurrency]") {
    using ac3::audio::SpatialError;
    for (const auto error :
         {SpatialError::kNoBackend, SpatialError::kComFailure, SpatialError::kDeviceNotFound,
          SpatialError::kNoSpatialFormat, SpatialError::kFormatRejected,
          SpatialError::kAlreadyRunning, SpatialError::kNotRunning}) {
        const std::string_view text = ac3::audio::describe(error);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown spatial error");
    }
}

TEST_CASE("a spatial sink that was never started refuses work", "[audio-backend][concurrency]") {
    // Same contract as PassthroughSink's own never-started test: true of
    // every backend including the stub, and the reason submit() checks
    // running() first.
    ac3::audio::SpatialObjectSink sink;
    CHECK_FALSE(sink.running());
    CHECK_FALSE(sink.can_submit());
    CHECK_FALSE(sink.submit({}, {}));

    const auto stats = sink.stats();
    CHECK(stats.updates_submitted == 0);
    CHECK(stats.updates_rendered == 0);
    CHECK(stats.underruns == 0);
    CHECK(stats.active_dynamic_objects == 0);
}

TEST_CASE("every capture error describes itself", "[audio-backend][concurrency]") {
    using ac3::audio::CaptureError;
    for (const auto error : {CaptureError::kNoBackend, CaptureError::kComFailure,
                             CaptureError::kDeviceNotFound, CaptureError::kFormatUnsupported,
                             CaptureError::kAlreadyRunning,
                             CaptureError::kProcessLoopbackUnavailable,
                             CaptureError::kProcessNotFound}) {
        const std::string_view text = ac3::audio::describe(error);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown capture error");
    }
}

TEST_CASE("process loopback refusals agree with the reported capability",
          "[audio-backend][concurrency]") {
    // Roadmap UX11. Two reports of the same fact have to agree, and the one
    // refusal that reaches no device - process id 0, which no process ever
    // has - has to come back with the right code on every platform: "there
    // is no such tap here" where there is none, "no such process" where
    // there is. A real process id is never tried: that would open a tap on
    // a developer's machine and start a capture thread.
    using ac3::audio::CaptureError;
    const auto& capability = ac3::audio::audio_backend().process_loopback;
    CHECK(capability.available == ac3::audio::process_loopback_available());

    ac3::audio::Capture capture;
    const auto result = capture.start_process_loopback(0);
    REQUIRE_FALSE(result.has_value());
    if (capability.available) {
        CHECK(result.error() == CaptureError::kProcessNotFound);
    } else {
        // posix/android have no capture backend at all; alsa/pipewire/macos
        // have one without a per-process tap. Both are honest answers.
        CHECK((result.error() == CaptureError::kNoBackend ||
               result.error() == CaptureError::kProcessLoopbackUnavailable));
    }
    CHECK_FALSE(capture.running());
    CHECK(capture.stats().frames_captured == 0);
}

TEST_CASE("every device watch error describes itself", "[audio-backend][concurrency]") {
    using ac3::audio::DeviceWatchError;
    for (const auto error : {DeviceWatchError::kNoBackend, DeviceWatchError::kComFailure,
                             DeviceWatchError::kAlreadyRunning}) {
        const std::string_view text = ac3::audio::describe(error);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown device watch error");
    }
}

TEST_CASE("a device watcher that was never started reports so", "[audio-backend][concurrency]") {
    ac3::audio::DeviceWatcher watcher;
    CHECK_FALSE(watcher.running());
    CHECK(watcher.stats().events_delivered == 0);
    watcher.stop();  // harmless when not running
    CHECK_FALSE(watcher.running());
}

TEST_CASE("device watching agrees with the reported capability",
          "[audio-backend][concurrency]") {
    // Registering for endpoint notifications needs no endpoint - a machine
    // with no sound card at all (a CI runner, a container) can still
    // register and simply never hear anything - so unlike the spatial probe
    // above this one CAN be exercised for real wherever the backend exists:
    // start, confirm it is running, refuse a second start, stop, and be
    // stopped. Nothing here opens a device or makes a sound.
    using ac3::audio::DeviceWatchError;
    const auto& capability = ac3::audio::audio_backend().device_watch;

    ac3::audio::DeviceWatcher watcher;
    const auto started = watcher.start([](const ac3::audio::DeviceChangeEvent&) {});
    if (capability.available) {
        REQUIRE(started.has_value());
        CHECK(watcher.running());
        const auto again = watcher.start([](const ac3::audio::DeviceChangeEvent&) {});
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error() == DeviceWatchError::kAlreadyRunning);
        watcher.stop();
        CHECK_FALSE(watcher.running());
        // And it can go round again: stop() left nothing behind.
        REQUIRE(watcher.start([](const ac3::audio::DeviceChangeEvent&) {}).has_value());
        watcher.stop();
        CHECK_FALSE(watcher.running());
    } else {
        // Two ways to be unavailable, and the error says which. A backend
        // that was never built refuses with kNoBackend. PipeWire's is built
        // but needs a session daemon to register with, so on a container or
        // a CI runner with none it reports the platform refusal instead -
        // which is the truthful answer, and why this accepts either
        // (roadmap UX12).
        REQUIRE_FALSE(started.has_value());
        CHECK((started.error() == DeviceWatchError::kNoBackend ||
               started.error() == DeviceWatchError::kComFailure));
        CHECK_FALSE(watcher.running());
    }
}

TEST_CASE("every passthrough error describes itself", "[audio-backend][concurrency]") {
    using ac3::audio::PassthroughError;
    for (const auto error :
         {PassthroughError::kNoBackend, PassthroughError::kComFailure,
          PassthroughError::kDeviceNotFound, PassthroughError::kFormatRejected,
          PassthroughError::kExclusiveUnavailable, PassthroughError::kAlreadyRunning,
          PassthroughError::kNotRunning}) {
        const std::string_view text = ac3::audio::describe(error);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown passthrough error");
    }
}

TEST_CASE("every monitor error describes itself", "[audio-backend][concurrency]") {
    using ac3::audio::MonitorError;
    for (const auto error : {MonitorError::kNoBackend, MonitorError::kComFailure,
                             MonitorError::kDeviceNotFound, MonitorError::kAlreadyRunning,
                             MonitorError::kNotRunning}) {
        const std::string_view text = ac3::audio::describe(error);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown monitor error");
    }
}

TEST_CASE("a monitor sink refuses a channel count it cannot interpret",
          "[audio-backend][concurrency]") {
    // Zero channels is nonsense on every backend and is rejected before any
    // device is touched, so this reaches no hardware even where a backend
    // exists. A real start() is not attempted anywhere in this file - that
    // would open an output and make a noise on a developer's machine.
    ac3::audio::MonitorSink sink;
    CHECK_FALSE(sink.start("", 48000, 0).has_value());
    CHECK_FALSE(sink.running());
    CHECK_FALSE(sink.can_submit());
    CHECK_FALSE(sink.submit({}));
}

TEST_CASE("a sink that was never started refuses work", "[audio-backend][concurrency]") {
    // True of every backend including the stub, and the reason submit() checks
    // running() first: a caller that ignores start()'s result must not be able
    // to write into a queue that does not exist.
    ac3::audio::PassthroughSink sink;
    CHECK_FALSE(sink.running());
    CHECK_FALSE(sink.can_submit());
    CHECK_FALSE(sink.submit({}));

    const auto stats = sink.stats();
    CHECK(stats.bursts_submitted == 0);
    CHECK(stats.bursts_rendered == 0);
    CHECK(stats.underruns == 0);
}

TEST_CASE("a capture that was never started reports nothing", "[audio-backend][concurrency]") {
    ac3::audio::Capture capture;
    CHECK_FALSE(capture.running());
    CHECK(capture.sample_rate() == 0);
    CHECK(capture.channels() == 0);
    CHECK(capture.stats().frames_captured == 0);
}
