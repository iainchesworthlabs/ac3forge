#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "ac3/audio/capture.hpp"
#include "ac3/audio/monitor.hpp"
#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/pcm_output.hpp"
#include "ac3/audio/sink_capabilities.hpp"
#include "ac3/audio/spatial.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "ac3/render/layout.hpp"
#include "audio/alsa_null_device.hpp"

// The ALSA backend's capture, monitor and passthrough classes against software
// ALSA devices (audio/alsa_null_device.hpp), so what they do once a device
// OPENS is checked on a machine with no sound card: samples arriving intact,
// frames reaching the device, pause/flush/resume, under-run recovery, channel
// status names, and a device that fails for good stopping its sink by itself.
//
// test_audio_backend.cpp holds the contract every backend keeps without a
// device; test_monitor_live.cpp and test_passthrough_live.cpp are the hidden
// cases for real hardware. This file is only built in the ALSA configuration
// (tests/CMakeLists.txt), and every case starts the backend's own threads, so
// every case carries [concurrency].

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

namespace alsa_null = ac3test::alsa_null;

constexpr std::size_t kReplayFrames = 4096;

// Polls `done` until it holds or `limit` passes. The software devices render
// as fast as they are fed, so every wait here ends in milliseconds; the limit
// only bounds a failure.
template <typename Done>
bool eventually(Done done, std::chrono::milliseconds limit = 5000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// The float sample the replayed capture carries at interleaved index `i`: a
// ramp on the left, its negation on the right, all exactly representable.
float replay_float_sample(std::size_t i) {
    const auto frame = static_cast<float>(i / 2);
    const float left = (frame - 2048.0F) / 4096.0F;
    return (i % 2 == 0) ? left : -left;
}

// The S32 sample the integer replay carries at interleaved index `i`.
std::int32_t replay_s32_sample(std::size_t i) {
    const auto frame = static_cast<std::int32_t>(i / 2);
    const std::int32_t left = (frame - 2048) * 65536;
    return (i % 2 == 0) ? left : -left;
}

template <typename T>
void write_samples(const fs::path& path, std::size_t count, T (*sample)(std::size_t)) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    REQUIRE(out.is_open());
    for (std::size_t i = 0; i < count; ++i) {
        const T value = sample(i);
        out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    REQUIRE(out.good());
}

// The named software devices, with this process's own replay files behind the
// two `file` ones, installed for the lifetime of the object.
class NullDevices {
public:
    NullDevices()
        : dir_(alsa_null::scratch_dir("alsa_null_backend")),
          config_(write_all(dir_)),
          scope_(config_) {}

private:
    static fs::path write_all(const fs::path& dir) {
        const auto floats = dir / "replay_float.raw";
        const auto ints = dir / "replay_s32.raw";
        write_samples<float>(floats, 2 * kReplayFrames, &replay_float_sample);
        write_samples<std::int32_t>(ints, 2 * kReplayFrames, &replay_s32_sample);
        return alsa_null::write_config(dir / "asound.conf", alsa_null::named_devices(floats, ints));
    }

    fs::path dir_;
    fs::path config_;
    alsa_null::AlsaConfigScope scope_;
};

// Reads `count` interleaved samples from a running capture.
std::vector<float> read_samples(ac3::audio::Capture& capture, std::size_t count) {
    std::vector<float> samples(count);
    std::size_t filled = 0;
    const bool complete = eventually([&] {
        filled += capture.buffer()->read(std::span{samples}.subspan(filled));
        return filled == count;
    });
    REQUIRE(complete);
    return samples;
}

// A quarter-second of stereo silence, in chunks of 480 frames.
std::vector<float> stereo_chunk() {
    return std::vector<float>(480 * 2, 0.0F);
}

}  // namespace

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

TEST_CASE("alsa capture: a float device's samples arrive exactly as it delivered them",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::Capture capture;
    REQUIRE(capture.start("replay_float", ac3::audio::DeviceKind::kInput).has_value());
    CHECK(capture.running());
    CHECK(capture.sample_rate() == 48000);
    CHECK(capture.channels() == 2);

    const auto samples = read_samples(capture, 2 * kReplayFrames);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        mismatches += samples[i] == replay_float_sample(i) ? 0U : 1U;
    }
    CHECK(mismatches == 0);
    CHECK(capture.stats().frames_captured >= kReplayFrames);

    capture.stop();
    CHECK_FALSE(capture.running());
}

TEST_CASE("alsa capture: a device offering only integers is read as S32 and scaled into [-1, 1)",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::Capture capture;
    REQUIRE(capture.start("replay_s32", ac3::audio::DeviceKind::kInput).has_value());
    const auto samples = read_samples(capture, 2 * kReplayFrames);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const float expected = static_cast<float>(replay_s32_sample(i)) / 2147483648.0F;
        mismatches += samples[i] == expected ? 0U : 1U;
    }
    CHECK(mismatches == 0);
}

TEST_CASE("alsa capture: the default endpoint is listed first, at the rate and width it takes",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    const auto listed = ac3::audio::enumerate_devices();
    REQUIRE(listed.has_value());
    REQUIRE_FALSE(listed->empty());
    const auto& first = listed->front();
    CHECK(first.id == "default");
    CHECK(first.is_default);
    CHECK(first.kind == ac3::audio::DeviceKind::kInput);
    CHECK(first.sample_rate == 48000);
    CHECK(first.channels == 2);

    // An empty id is that same default, opened for real.
    ac3::audio::Capture capture;
    REQUIRE(capture.start("", ac3::audio::DeviceKind::kInput).has_value());
    CHECK(capture.channels() == 2);
    CHECK(eventually([&] { return capture.stats().frames_captured > 0; }));
}

TEST_CASE("alsa capture: a one-channel device is opened at the width it has",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::Capture capture;
    REQUIRE(capture.start("mono", ac3::audio::DeviceKind::kInput).has_value());
    CHECK(capture.channels() == 1);
    const auto samples = read_samples(capture, 1024);
    CHECK(samples.size() == 1024);
}

TEST_CASE("alsa capture: a consumer that falls behind is told how much was dropped",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::Capture capture;
    // A ring of a few periods, never read: the null device delivers as fast as
    // it is asked, so the capture thread overruns it at once.
    REQUIRE(capture.start("null", ac3::audio::DeviceKind::kInput, 4096).has_value());
    CHECK(eventually([&] { return capture.stats().frames_dropped > 0; }));
    // Read once the thread has stopped: the counters are separate atomics and
    // only agree with each other when nothing is moving them.
    capture.stop();
    const auto stats = capture.stats();
    REQUIRE(stats.frames_captured > stats.frames_dropped);
    // What was not dropped is what the ring still holds: at most its 4096
    // samples, two channels to a frame.
    CHECK(stats.frames_captured - stats.frames_dropped <= 2048);
    CHECK(stats.frames_silence_filled == 0);
}

TEST_CASE("alsa capture: an overrun drops whole frames, so every channel stays in its slot",
          "[audio][alsa-null][concurrency]") {
    // A ring of 4096 samples holds 4095, never read until it has overrun:
    // the replay's periods are an even number of samples, so the write that
    // meets the full ring could keep an odd number of them - half a frame -
    // and every sample after it would arrive one channel late for the rest of
    // the take. Only whole frames may go in, and what is dropped is a whole
    // number of frames too.
    const NullDevices devices;
    ac3::audio::Capture capture;
    REQUIRE(capture.start("replay_float", ac3::audio::DeviceKind::kInput, 4096).has_value());
    REQUIRE(capture.channels() == 2);
    REQUIRE(eventually([&] { return capture.stats().frames_dropped > 0; }));
    CHECK(capture.buffer()->available() % 2 == 0);

    // What the ring held, then what arrives once there is room again (the
    // rest of the replay, or the silence after it): every left sample is the
    // ramp or silence, and its right partner is its negation.
    const auto samples = read_samples(capture, 4096 + 2048);
    std::size_t misplaced = 0;
    for (std::size_t i = 0; i + 1 < samples.size(); i += 2) {
        misplaced += samples[i + 1] == -samples[i] ? 0U : 1U;
    }
    CHECK(misplaced == 0);
    // The first frame is the replay's first, untouched.
    CHECK(samples[0] == replay_float_sample(0));
    CHECK(samples[1] == replay_float_sample(1));

    capture.stop();
    CHECK(capture.buffer()->dropped() % 2 == 0);
}

TEST_CASE("alsa capture: a device that is missing, or offers nothing readable, is refused by name",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    using ac3::audio::CaptureError;
    using ac3::audio::DeviceKind;
    {
        ac3::audio::Capture capture;
        const auto started = capture.start("no_such_alsa_device", DeviceKind::kInput);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error() == CaptureError::kDeviceNotFound);
    }
    {
        // A `hw` device on a card that is not there fails in the kernel's
        // terms (ENOENT) rather than in the configuration's.
        ac3::audio::Capture capture;
        const auto started = capture.start("nocard", DeviceKind::kInput);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error() == CaptureError::kDeviceNotFound);
    }
    {
        // mu-law only: none of float/S32/S24/S16 is on offer.
        ac3::audio::Capture capture;
        const auto started = capture.start("mulawonly", DeviceKind::kInput);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error() == CaptureError::kFormatUnsupported);
        CHECK_FALSE(capture.running());
    }
    {
        // Loopback with no id means snd-aloop's card, and there are no cards.
        ac3::audio::Capture capture;
        const auto started = capture.start("", DeviceKind::kLoopback);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error() == CaptureError::kDeviceNotFound);
    }
}

TEST_CASE("alsa capture: a second start is refused while running, and allowed after a stop",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::Capture capture;
    REQUIRE(capture.start("null", ac3::audio::DeviceKind::kInput).has_value());
    const auto again = capture.start("null", ac3::audio::DeviceKind::kInput);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == ac3::audio::CaptureError::kAlreadyRunning);
    capture.stop();
    REQUIRE(capture.start("mono", ac3::audio::DeviceKind::kInput).has_value());
    CHECK(capture.channels() == 1);
}

TEST_CASE("alsa capture: a named loopback device is captured with the loopback timeline",
          "[audio][alsa-null][concurrency]") {
    // kLoopback with an explicit id opens it as-is and keeps the loopback
    // gap-filling timeline running beside it; a device that keeps up (null
    // always does) needs no filling.
    const NullDevices devices;
    ac3::audio::Capture capture;
    REQUIRE(capture.start("null", ac3::audio::DeviceKind::kLoopback).has_value());
    const auto samples = read_samples(capture, 4096);
    CHECK(samples.size() == 4096);
    CHECK_FALSE(ac3::audio::process_loopback_available());
    const auto tap = capture.start_process_loopback(1);
    REQUIRE_FALSE(tap.has_value());
    CHECK(tap.error() == ac3::audio::CaptureError::kProcessLoopbackUnavailable);
}

// ---------------------------------------------------------------------------
// Monitor
// ---------------------------------------------------------------------------

TEST_CASE("alsa monitor: frames submitted to a software device are rendered and positioned",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::MonitorSink sink;
    REQUIRE(sink.start("null", 48000, 2).has_value());
    CHECK(sink.running());
    CHECK(sink.can_submit());

    const auto chunk = stereo_chunk();
    // A chunk that is not a whole number of frames is refused outright.
    CHECK_FALSE(sink.submit(std::span{chunk}.first(3)));
    for (int i = 0; i < 10; ++i) {
        CHECK(eventually([&] { return sink.submit(chunk); }));
    }
    CHECK(eventually([&] {
        const auto stats = sink.stats();
        return stats.frames_rendered >= stats.frames_submitted;
    }));
    CHECK(sink.stats().frames_submitted == 4800);
    const auto position = sink.position();
    REQUIRE(position.has_value());
    CHECK(position->latency_frames == 0);

    const auto again = sink.start("null", 48000, 2);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == ac3::audio::MonitorError::kAlreadyRunning);

    sink.stop();
    CHECK_FALSE(sink.running());
    CHECK_FALSE(sink.position().has_value());
}

TEST_CASE("alsa monitor: a pause before the device starts, a flush and a resume all take effect",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::MonitorSink sink;
    REQUIRE(sink.start("null", 48000, 2).has_value());
    // Straight after start the stream is still PREPARED, which refuses a
    // hardware pause, so the render thread drops and re-prepares instead.
    REQUIRE(sink.pause().has_value());
    CHECK(sink.paused());
    REQUIRE(sink.submit(stereo_chunk()));
    // The flush is carried out by the render thread, after the pause: once it
    // has been, the counters it resets read zero.
    sink.flush();
    CHECK(sink.stats().frames_submitted == 0);
    REQUIRE(sink.resume().has_value());
    CHECK_FALSE(sink.paused());
    REQUIRE(sink.submit(stereo_chunk()));
    CHECK(eventually([&] { return sink.stats().frames_rendered >= 480; }));
}

TEST_CASE("alsa monitor: a pause once the device is running holds it, and resume carries on",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::MonitorSink sink;
    REQUIRE(sink.start("null", 48000, 2).has_value());
    // A buffer's worth and more, so the start threshold has been crossed and
    // the stream is RUNNING when the pause arrives.
    for (int i = 0; i < 20; ++i) {
        REQUIRE(eventually([&] { return sink.submit(stereo_chunk()); }));
    }
    REQUIRE(eventually([&] { return sink.stats().frames_rendered >= 9600; }));
    REQUIRE(sink.pause().has_value());
    sink.flush();
    CHECK(sink.stats().frames_rendered == 0);
    REQUIRE(sink.resume().has_value());
    REQUIRE(sink.submit(stereo_chunk()));
    CHECK(eventually([&] { return sink.stats().frames_rendered >= 480; }));
    // And a flush while playing is carried out the same way.
    sink.flush();
    CHECK(sink.stats().frames_submitted == 0);
}

TEST_CASE("alsa monitor: a device that under-runs is recovered, keeps playing and drains",
          "[audio][alsa-null][concurrency]") {
    // `mono` (multi over null) drops into XRUN as soon as its start threshold
    // starts it, so every buffer's worth ends in -EPIPE: the render thread's
    // snd_pcm_recover path, over and over, with the sink still running.
    //
    // Not plug: through plug this same device leaves writei spinning inside
    // alsa-lib for ever, holding the PCM's own lock - non-blocking mode does
    // not change that, and nor can a snd_pcm_drop from another thread, which
    // waits on the same lock - so stop() would never join.
    const NullDevices devices;
    ac3::audio::MonitorSink sink;
    REQUIRE(sink.start("mono", 48000, 1).has_value());
    const std::vector<float> chunk(480, 0.0F);
    for (int i = 0; i < 40; ++i) {
        REQUIRE(eventually([&] { return sink.submit(chunk); }));
    }
    // Four periods of 1024 frames play between under-runs, so three buffers'
    // worth rendered means the thread came back from at least two of them.
    CHECK(eventually([&] { return sink.stats().frames_rendered >= 3 * 4096; }));
    CHECK(sink.running());
    // And every frame submitted is rendered: the period whose write met the
    // under-run is written again once recovered, not dropped uncounted, so a
    // caller draining the queue (ac3cli monitor waits for exactly this)
    // finishes. Before, one period in five went missing from the count and
    // the drain waited for ever.
    CHECK(eventually([&] {
        const auto stats = sink.stats();
        return stats.frames_rendered >= stats.frames_submitted;
    }));
    CHECK(sink.stats().frames_submitted == 40 * 480);
}

TEST_CASE("alsa monitor: a missing device and a zero-channel stream are refused by name",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::MonitorSink sink;
    const auto missing = sink.start("no_such_alsa_device", 48000, 2);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == ac3::audio::MonitorError::kDeviceNotFound);
    const auto empty = sink.start("null", 48000, 0);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == ac3::audio::MonitorError::kComFailure);
    CHECK_FALSE(sink.running());
}

TEST_CASE("alsa monitor: a device whose writes fail for good stops the sink, which starts again",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::MonitorSink sink;
    // Its writes fail with EIO, which snd_pcm_recover cannot mend.
    REQUIRE(sink.start("full", 48000, 2).has_value());
    CHECK(eventually([&] {
        std::ignore = sink.submit(stereo_chunk());
        return !sink.running();
    }));
    CHECK_FALSE(sink.position().has_value());
    CHECK_FALSE(sink.submit(stereo_chunk()));
    CHECK_FALSE(sink.paused());
    const auto paused = sink.pause();
    REQUIRE_FALSE(paused.has_value());
    CHECK(paused.error() == ac3::audio::MonitorError::kNotRunning);
    sink.flush();  // nothing running: returns at once

    // start() tidies away the thread that ended with its device.
    REQUIRE(sink.start("null", 48000, 2).has_value());
    CHECK(sink.running());
}

TEST_CASE("alsa pcm output: a layout is opened on a named software device and played",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::PcmOutput output;
    const auto layout = ac3::render::OutputLayout::stereo();
    const auto opened = output.start("null", 48000, layout);
    REQUIRE(opened.has_value());
    CHECK(opened->outputs == 2);
    CHECK_FALSE(opened->from_device);  // no card to have said so
    CHECK(output.running());
    CHECK(output.can_submit());

    const std::vector<float> left(480, 0.25F);
    const std::vector<float> right(480, -0.25F);
    const std::vector<std::span<const float>> rendered{left, right};
    REQUIRE(output.submit(rendered, 480));
    CHECK_FALSE(output.submit(rendered, 0));
    CHECK(eventually([&] { return output.stats().frames_rendered >= 480; }));
    CHECK(output.position().has_value());
    REQUIRE(output.pause().has_value());
    CHECK(output.paused());
    output.flush();
    REQUIRE(output.resume().has_value());
    CHECK_FALSE(output.paused());

    // A patch for another width is not this stream's.
    const auto three = ac3::render::Routing::parse("0,1", 3);
    if (three) {
        CHECK_FALSE(output.set_routing(*three));
    }
    const auto swap = ac3::render::Routing::parse("1,0", 2);
    REQUIRE(swap.has_value());
    CHECK(output.set_routing(*swap));
    CHECK(output.routing().output_of(0) == 1);

    // Spans shorter than the frame count the caller claims are refused
    // rather than submitted short.
    const std::vector<float> short_left(100, 0.0F);
    const std::vector<float> short_right(100, 0.0F);
    const std::vector<std::span<const float>> short_rendered{short_left, short_right};
    CHECK_FALSE(output.submit(short_rendered, 480));

    const auto again = output.start("null", 48000, layout);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == ac3::audio::MonitorError::kAlreadyRunning);
    output.stop();
    CHECK_FALSE(output.running());
    CHECK_FALSE(output.submit(rendered, 480));
}

TEST_CASE("alsa pcm output: an empty layout or a missing device is refused",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::PcmOutput output;
    const auto empty = output.start("null", 48000, ac3::render::OutputLayout{});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == ac3::audio::MonitorError::kComFailure);
    const auto missing =
        output.start("no_such_alsa_device", 48000, ac3::render::OutputLayout::stereo());
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == ac3::audio::MonitorError::kDeviceNotFound);
    CHECK_FALSE(output.running());
}

// ---------------------------------------------------------------------------
// Passthrough
// ---------------------------------------------------------------------------

TEST_CASE("alsa passthrough: AC-3 bursts reach a software device, burst for burst",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::PassthroughSink sink;
    REQUIRE(sink.start("null", 48000, ac3::audio::BitstreamFormat::kAc3).has_value());
    CHECK(sink.running());
    CHECK(sink.can_submit());

    const std::vector<std::byte> burst(ac3::iec61937::kBurstBytes);
    const std::vector<std::byte> wrong(ac3::iec61937::kEac3BurstBytes);
    CHECK_FALSE(sink.submit(wrong));
    for (int i = 0; i < 8; ++i) {
        REQUIRE(eventually([&] { return sink.submit(burst); }));
    }
    CHECK(eventually([&] {
        const auto stats = sink.stats();
        return stats.bursts_rendered >= stats.bursts_submitted;
    }));
    CHECK(sink.stats().bursts_submitted == 8);
    const auto position = sink.position();
    REQUIRE(position.has_value());
    CHECK(position->latency_frames == 0);

    const auto again = sink.start("null", 48000, ac3::audio::BitstreamFormat::kAc3);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == ac3::audio::PassthroughError::kAlreadyRunning);
    sink.stop();
    CHECK_FALSE(sink.running());
    CHECK_FALSE(sink.position().has_value());
}

TEST_CASE("alsa passthrough: E-AC-3 runs its link at 4x and takes only E-AC-3-sized bursts",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::PassthroughSink sink;
    REQUIRE(sink.start("null", 48000, ac3::audio::BitstreamFormat::kEac3).has_value());
    const std::vector<std::byte> ac3_sized(ac3::iec61937::kBurstBytes);
    const std::vector<std::byte> burst(ac3::iec61937::kEac3BurstBytes);
    CHECK_FALSE(sink.submit(ac3_sized));
    REQUIRE(sink.submit(burst));
    CHECK(eventually([&] { return sink.stats().bursts_rendered >= 1; }));
}

TEST_CASE("alsa passthrough: pause, flush and resume take effect before and after the link starts",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    const std::vector<std::byte> burst(ac3::iec61937::kBurstBytes);
    SECTION("paused while still PREPARED: dropped and prepared again") {
        ac3::audio::PassthroughSink sink;
        REQUIRE(sink.start("null", 48000, ac3::audio::BitstreamFormat::kAc3).has_value());
        REQUIRE(sink.pause().has_value());
        CHECK(sink.paused());
        REQUIRE(sink.submit(burst));
        sink.flush();
        CHECK(sink.stats().bursts_submitted == 0);
        REQUIRE(sink.resume().has_value());
        REQUIRE(sink.submit(burst));
        CHECK(eventually([&] { return sink.stats().bursts_rendered >= 1; }));
    }
    SECTION("paused while RUNNING: held by the device") {
        ac3::audio::PassthroughSink sink;
        REQUIRE(sink.start("null", 48000, ac3::audio::BitstreamFormat::kAc3).has_value());
        for (int i = 0; i < 8; ++i) {
            REQUIRE(eventually([&] { return sink.submit(burst); }));
        }
        REQUIRE(eventually([&] { return sink.stats().bursts_rendered >= 8; }));
        REQUIRE(sink.pause().has_value());
        sink.flush();
        CHECK(sink.stats().bursts_rendered == 0);
        REQUIRE(sink.resume().has_value());
        CHECK_FALSE(sink.paused());
        REQUIRE(sink.submit(burst));
        CHECK(eventually([&] { return sink.stats().bursts_rendered >= 1; }));
        sink.flush();
        CHECK(sink.stats().bursts_submitted == 0);
    }
}

TEST_CASE("alsa passthrough: iec958 and hdmi names carry the non-audio channel status",
          "[audio][alsa-null][concurrency]") {
    // The configuration's own iec958/hdmi templates are replaced by ones that
    // accept the AES arguments and resolve to `null`, so this is alsa-lib's
    // real argument parsing reading the four bytes the backend appended.
    const NullDevices devices;
    using ac3::audio::BitstreamFormat;
    {
        ac3::audio::PassthroughSink sink;
        CHECK(sink.start("iec958:CARD=Test,DEV=0", 48000, BitstreamFormat::kAc3).has_value());
    }
    {
        ac3::audio::PassthroughSink sink;
        CHECK(sink.start("hdmi:CARD=Test,DEV=0", 48000, BitstreamFormat::kEac3).has_value());
    }
    {
        // Channel status written by hand is passed through untouched.
        ac3::audio::PassthroughSink sink;
        CHECK(sink.start("iec958:CARD=Test,AES0=0x06,AES1=0x82,AES2=0x00,AES3=0x02", 44100,
                         BitstreamFormat::kAc3)
                  .has_value());
    }
    {
        // E-AC-3 from 32 kHz content wants a 128 kHz link, which IEC 60958
        // has no frequency code for.
        ac3::audio::PassthroughSink sink;
        const auto started = sink.start("iec958:CARD=Test", 32000, BitstreamFormat::kEac3);
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error() == ac3::audio::PassthroughError::kFormatRejected);
    }
}

TEST_CASE("alsa passthrough: a device that is missing or will not take the carrier is refused",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    using ac3::audio::BitstreamFormat;
    using ac3::audio::PassthroughError;
    const auto refusal = [](const std::string& name) {
        ac3::audio::PassthroughSink sink;
        const auto started = sink.start(name, 48000, BitstreamFormat::kAc3);
        CHECK_FALSE(sink.running());
        return started.has_value() ? std::optional<PassthroughError>{} : started.error();
    };
    // No such name, a card that is not there (ENOENT), and no default: with
    // no cards there is no digital output to pick.
    CHECK(refusal("no_such_alsa_device") == PassthroughError::kDeviceNotFound);
    CHECK(refusal("nocard") == PassthroughError::kDeviceNotFound);
    CHECK(refusal("") == PassthroughError::kDeviceNotFound);
    // Float only, and one channel only: the 16-bit stereo carrier is refused
    // rather than converted - conversion is what corrupts a bitstream.
    CHECK(refusal("floatonly") == PassthroughError::kFormatRejected);
    CHECK(refusal("mono") == PassthroughError::kFormatRejected);
}

TEST_CASE("alsa passthrough: a link that under-runs is recovered and keeps carrying bursts",
          "[audio][alsa-null][concurrency]") {
    // As the monitor's under-run case: four bursts play, the fifth write
    // meets -EPIPE, snd_pcm_recover prepares the stream again.
    const NullDevices devices;
    ac3::audio::PassthroughSink sink;
    REQUIRE(sink.start("stereo_xrun", 48000, ac3::audio::BitstreamFormat::kAc3).has_value());
    const std::vector<std::byte> burst(ac3::iec61937::kBurstBytes);
    for (int i = 0; i < 12; ++i) {
        REQUIRE(eventually([&] { return sink.submit(burst); }));
    }
    CHECK(eventually([&] { return sink.stats().bursts_rendered >= 6; }));
    CHECK(sink.running());
    // Every burst submitted is rendered, as the monitor's under-run case
    // checks: the burst whose write met -EPIPE is written again once
    // recovered, so a drain of the queue ends.
    CHECK(eventually([&] {
        const auto stats = sink.stats();
        return stats.bursts_rendered >= stats.bursts_submitted;
    }));
    CHECK(sink.stats().bursts_submitted == 12);
}

TEST_CASE("alsa passthrough: a device whose writes fail for good stops the sink by itself",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    ac3::audio::PassthroughSink sink;
    REQUIRE(sink.start("full", 48000, ac3::audio::BitstreamFormat::kAc3).has_value());
    const std::vector<std::byte> burst(ac3::iec61937::kBurstBytes);
    CHECK(eventually([&] {
        std::ignore = sink.submit(burst);
        return !sink.running();
    }));
    CHECK_FALSE(sink.submit(burst));
    CHECK_FALSE(sink.can_submit());
    const auto resumed = sink.resume();
    REQUIRE_FALSE(resumed.has_value());
    CHECK(resumed.error() == ac3::audio::PassthroughError::kNotRunning);
    REQUIRE(sink.start("null", 48000, ac3::audio::BitstreamFormat::kAc3).has_value());
    CHECK(sink.running());
}

// ---------------------------------------------------------------------------
// What a card walk finds when there are no cards
// ---------------------------------------------------------------------------

TEST_CASE("alsa: with no sound card there are no render endpoints and no descriptor to read",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    const auto render = ac3::audio::enumerate_render_devices(48000);
    REQUIRE(render.has_value());
    CHECK(render->empty());
    const auto edid = ac3::audio::read_sink_capabilities("hdmi:CARD=Test,DEV=0");
    REQUIRE_FALSE(edid.has_value());
    CHECK(edid.error() == ac3::audio::EdidError::kDeviceNotFound);
    using ac3::audio::EdidError;
    for (const auto error : {EdidError::kNoBackend, EdidError::kDeviceNotFound,
                             EdidError::kNoEdid, EdidError::kParseFailed}) {
        const std::string_view text = ac3::audio::describe(error);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown EDID error");
    }
}

TEST_CASE("alsa: spatial object rendering is refused outright, even with a device to hand",
          "[audio][alsa-null][concurrency]") {
    const NullDevices devices;
    const auto probed = ac3::audio::probe_spatial_capability("null");
    REQUIRE_FALSE(probed.has_value());
    CHECK(probed.error() == ac3::audio::SpatialError::kNoBackend);
    ac3::audio::SpatialObjectSink sink;
    const auto started = sink.start("null", 48000, 0, 4);
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == ac3::audio::SpatialError::kNoBackend);
    sink.stop();
    CHECK_FALSE(sink.running());
}

TEST_CASE("alsa: a configuration whose default card is not a number still enumerates",
          "[audio][alsa-null][concurrency]") {
    // defaults.pcm.card is read only to pick which enumerated output to flag
    // as the default; one that is not an integer means "the configuration
    // does not say", never a failure.
    const auto dir = alsa_null::scratch_dir("alsa_null_backend");
    const auto config = alsa_null::write_config(
        dir / "card_name.conf",
        std::string{alsa_null::kNullDevices} + "defaults.pcm.!card \"NotANumber\"\n");
    const alsa_null::AlsaConfigScope scope{config};
    const auto render = ac3::audio::enumerate_render_devices(48000);
    REQUIRE(render.has_value());
    CHECK(render->empty());
    // The named devices still open under it.
    ac3::audio::MonitorSink sink;
    CHECK(sink.start("null", 48000, 2).has_value());
}
