#include "ac3/audio/monitor.hpp"

// The Android monitor backend. CMake compiles this directory's monitor.cpp
// on Android and another platform directory's everywhere else, so there is
// no #ifdef - the file's path is what says "Android".
//
// This is genuine AAudio (the NDK's native audio API, <aaudio/AAudio.h>) -
// shared-mode float PCM playback needs nothing from Java, unlike
// passthrough.cpp in this same directory. See audio_backend.cpp for why
// that split exists.

#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

#include "ac3/audio/playback_counter.hpp"
#include "ac3/audio/ring_buffer.hpp"

namespace ac3::audio {

namespace {

// Frames pulled from the queue per AAudioStream_write() call. An arbitrary
// chunk size (~10ms @ 48kHz), not a driver requirement - AAudio's blocking
// write accepts any frame count and simply blocks until it is consumed or
// the timeout below elapses.
constexpr std::int32_t kFramesPerWrite = 480;
constexpr std::int64_t kWriteTimeoutNanos = 20'000'000;  // 20ms

// How long to give AAudio to reach the state a pause or flush asked for.
// Every transition here is asynchronous - requestPause() returns while the
// stream is still PAUSING - and requestFlush() is only legal once the stream
// has actually reached PAUSED, so the worker has to wait for each one.
constexpr std::int64_t kStateChangeTimeoutNanos = 200'000'000;  // 200ms

// waitForStateChange() returns as soon as the stream is no longer in the
// transient state the request put it in, and reports where it ended up; a
// stream already at the destination returns immediately, so repeating a
// request is harmless. Each of these returns whether it got there.
bool pause_stream(AAudioStream* stream) {
    if (AAudioStream_requestPause(stream) != AAUDIO_OK) {
        return false;
    }
    aaudio_stream_state_t state = AAUDIO_STREAM_STATE_UNKNOWN;
    AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_PAUSING, &state,
                                    kStateChangeTimeoutNanos);
    return state == AAUDIO_STREAM_STATE_PAUSED;
}

bool flush_stream(AAudioStream* stream) {
    if (AAudioStream_requestFlush(stream) != AAUDIO_OK) {
        return false;
    }
    aaudio_stream_state_t state = AAUDIO_STREAM_STATE_UNKNOWN;
    AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_FLUSHING, &state,
                                    kStateChangeTimeoutNanos);
    return state == AAUDIO_STREAM_STATE_FLUSHED;
}

bool start_stream(AAudioStream* stream) {
    if (AAudioStream_requestStart(stream) != AAUDIO_OK) {
        return false;
    }
    aaudio_stream_state_t state = AAUDIO_STREAM_STATE_UNKNOWN;
    AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_STARTING, &state,
                                    kStateChangeTimeoutNanos);
    return state == AAUDIO_STREAM_STATE_STARTED;
}

}  // namespace

std::string_view describe(MonitorError error) {
    switch (error) {
        case MonitorError::kNoBackend: return "no monitor backend on this platform";
        case MonitorError::kComFailure: return "an AAudio call failed";
        case MonitorError::kDeviceNotFound: return "AAudio could not open an output stream";
        case MonitorError::kAlreadyRunning: return "monitor playback is already running";
        case MonitorError::kNotRunning: return "monitor playback is not running";
    }
    return "unknown monitor error";
}

struct MonitorSink::Impl {
    AAudioStream* stream = nullptr;
    std::unique_ptr<RingBuffer> queue;
    // std::thread + a manual stop flag, not std::jthread/std::stop_token:
    // NDK r26's bundled libc++ does not implement <stop_token> at all (see
    // docs/platforms/android.md) - the Windows/ALSA backends use jthread
    // because their libstdc++/MSVC STL both have it, but that is not
    // universal yet.
    std::thread worker;
    std::atomic_bool worker_stop_requested{false};
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> underruns{0};
    std::uint16_t channels = 0;
    // What the worker last saw of the stream's own counters, for position().
    // Only the worker calls AAudio - a stream is explicitly not thread-safe -
    // so a position() on the caller's thread reads the counter instead.
    PlaybackCounter counter;
    // Frame counters survive a flush ("not reset by a flush; they may be
    // advanced", AAudio.h), so position() counting from the last flush needs
    // a baseline of its own rather than the raw counter.
    std::atomic<std::int64_t> frame_baseline{0};
    // Set by pause()/resume() and flush(); acted on by the worker, which owns
    // the stream and the queue's read side. `flushes` counts the flushes it
    // has completed, which is what flush() waits for.
    std::atomic_bool paused{false};
    std::atomic_bool flushing{false};
    std::atomic<std::uint64_t> flushes{0};
};

MonitorSink::MonitorSink() : impl_(std::make_unique<Impl>()) {}

MonitorSink::~MonitorSink() {
    stop();
}

bool MonitorSink::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

MonitorStats MonitorSink::stats() const {
    return {.frames_submitted = impl_->submitted.load(std::memory_order_relaxed),
            .frames_rendered = impl_->rendered.load(std::memory_order_relaxed),
            .underruns = impl_->underruns.load(std::memory_order_relaxed)};
}

std::optional<MonitorPosition> MonitorSink::position() const {
    if (!running() || !impl_->queue || impl_->channels == 0) {
        return std::nullopt;
    }
    const std::uint64_t queued_here = impl_->queue->available() / impl_->channels;
    // AAudio publishes no output-latency figure of its own, and none is
    // missing from the figures the counter holds: the played count comes from
    // the stream's presentation timestamp, which already counts the path to
    // the speaker. 0 latency here says there is nothing further to add, the
    // same value the field takes wherever a platform cannot say.
    return impl_->counter.position(queued_here, /*latency=*/0);
}

void MonitorSink::flush() {
    if (!running()) {
        return;
    }
    // The worker owns the stream and the queue's read side, so it does the
    // work and this waits for it - long enough for a pause, a flush and the
    // state changes between them, and giving up after that rather than
    // blocking a caller on a stream that has stopped answering.
    const std::uint64_t done = impl_->flushes.load(std::memory_order_acquire);
    impl_->flushing.store(true, std::memory_order_release);
    for (int waited = 0; waited < 600; ++waited) {
        if (impl_->flushes.load(std::memory_order_acquire) != done || !running()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::expected<void, MonitorError> MonitorSink::pause() {
    if (!running()) {
        return std::unexpected(MonitorError::kNotRunning);
    }
    impl_->paused.store(true, std::memory_order_release);
    return {};
}

std::expected<void, MonitorError> MonitorSink::resume() {
    if (!running()) {
        return std::unexpected(MonitorError::kNotRunning);
    }
    impl_->paused.store(false, std::memory_order_release);
    return {};
}

bool MonitorSink::paused() const {
    return impl_->paused.load(std::memory_order_acquire);
}

bool MonitorSink::can_submit() const {
    if (!impl_->queue || impl_->channels == 0) {
        return false;
    }
    // Same ~20ms-of-room heuristic as the Windows backend; see that file's
    // comment on why submit() below re-checks against the actual chunk size
    // rather than trusting this fixed threshold alone.
    return impl_->queue->capacity() - impl_->queue->available() >
           static_cast<std::size_t>(impl_->channels) * 960;
}

bool MonitorSink::submit(std::span<const float> interleaved) {
    if (!running() || !impl_->queue || impl_->channels == 0 ||
        interleaved.size() % impl_->channels != 0) {
        return false;
    }
    if (impl_->queue->capacity() - impl_->queue->available() <= interleaved.size()) {
        return false;
    }
    const auto wrote = impl_->queue->write(interleaved);
    if (wrote != interleaved.size()) {
        return false;
    }
    impl_->submitted.fetch_add(interleaved.size() / impl_->channels, std::memory_order_relaxed);
    return true;
}

void MonitorSink::stop() {
    if (impl_->worker.joinable()) {
        impl_->worker_stop_requested.store(true, std::memory_order_release);
        impl_->worker.join();
    }
    if (impl_->stream != nullptr) {
        AAudioStream_requestStop(impl_->stream);
        AAudioStream_close(impl_->stream);
        impl_->stream = nullptr;
    }
    // A pause is a property of a running stream, so it does not outlive one.
    impl_->paused.store(false, std::memory_order_relaxed);
    impl_->flushing.store(false, std::memory_order_relaxed);
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, MonitorError> MonitorSink::start(const std::string& /*device_id*/,
                                                      std::uint32_t sample_rate,
                                                      std::uint16_t channels,
                                                      std::uint32_t /*channel_mask*/, bool /*low_latency*/) {
    // device_id and channel_mask are accepted (the interface is shared with
    // the WASAPI/ALSA backends) but unused: AAudio has no way to name a
    // render endpoint by string id the way WASAPI does, and no channel-mask
    // input below API 32 - shared-mode output is Android's current system
    // route, and channel POSITION beyond a bare count is not something this
    // preview path needs (it exists to sanity-check levels/timing, not to
    // reproduce exact speaker placement).
    if (running()) {
        return std::unexpected(MonitorError::kAlreadyRunning);
    }
    if (channels == 0) {
        return std::unexpected(MonitorError::kComFailure);
    }

    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || builder == nullptr) {
        return std::unexpected(MonitorError::kComFailure);
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, static_cast<std::int32_t>(sample_rate));
    AAudioStreamBuilder_setChannelCount(builder, static_cast<std::int32_t>(channels));
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);

    AAudioStream* stream = nullptr;
    const aaudio_result_t open_result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (open_result != AAUDIO_OK || stream == nullptr) {
        return std::unexpected(MonitorError::kDeviceNotFound);
    }

    // Room for roughly a second of samples, so a caller decoding slightly
    // ahead of real time never has to spin - same sizing rationale as the
    // Windows/ALSA backends.
    impl_->queue =
        std::make_unique<RingBuffer>(static_cast<std::size_t>(channels) * sample_rate);
    impl_->stream = stream;
    impl_->channels = channels;
    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered.store(0, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);
    impl_->counter.restart();
    impl_->frame_baseline.store(0, std::memory_order_relaxed);
    impl_->paused.store(false, std::memory_order_relaxed);
    impl_->flushing.store(false, std::memory_order_relaxed);
    impl_->flushes.store(0, std::memory_order_relaxed);

    if (AAudioStream_requestStart(stream) != AAUDIO_OK) {
        AAudioStream_close(stream);
        impl_->stream = nullptr;
        return std::unexpected(MonitorError::kComFailure);
    }
    impl_->running.store(true, std::memory_order_release);
    impl_->worker_stop_requested.store(false, std::memory_order_relaxed);

    impl_->worker = std::thread([this, channels] {
        std::vector<float> chunk(static_cast<std::size_t>(kFramesPerWrite) * channels);
        bool device_running = true;
        while (!impl_->worker_stop_requested.load(std::memory_order_acquire)) {
            // A pause stops the stream and leaves everything else standing:
            // the queue keeps what it holds and goes on taking frames. A
            // paused AAudio stream accepts no writes, so the loop sleeps
            // instead - and lets a flush through, which is only legal here.
            if (impl_->paused.load(std::memory_order_acquire)) {
                if (device_running) {
                    pause_stream(impl_->stream);
                    device_running = false;
                }
                if (!impl_->flushing.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
            }
            // A flush drops both buffers. AAudio will only flush a paused
            // stream, so pause first whatever the caller asked for, and
            // start again below unless they had also asked for a pause.
            if (impl_->flushing.exchange(false, std::memory_order_acq_rel)) {
                if (device_running) {
                    pause_stream(impl_->stream);
                    device_running = false;
                }
                flush_stream(impl_->stream);
                impl_->queue->reset();
                // The frame counters carry on across a flush, so the new
                // zero is where they stand once the discarded frames have
                // been accounted for.
                impl_->frame_baseline.store(AAudioStream_getFramesWritten(impl_->stream),
                                            std::memory_order_relaxed);
                impl_->counter.restart();
                impl_->rendered.store(0, std::memory_order_relaxed);
                impl_->submitted.store(0, std::memory_order_relaxed);
                impl_->flushes.fetch_add(1, std::memory_order_release);
                continue;
            }
            if (!device_running) {
                start_stream(impl_->stream);
                device_running = true;
            }
            const auto got = impl_->queue->read(chunk);
            if (got < chunk.size()) {
                // Nothing queued: emit silence for the remainder, counted
                // rather than hidden, matching the other backends' underrun
                // discipline.
                std::fill(chunk.begin() + static_cast<std::ptrdiff_t>(got), chunk.end(), 0.0f);
                impl_->underruns.fetch_add(1, std::memory_order_relaxed);
            }
            const auto written = AAudioStream_write(impl_->stream, chunk.data(), kFramesPerWrite,
                                                     kWriteTimeoutNanos);
            if (written > 0) {
                impl_->rendered.fetch_add(static_cast<std::uint64_t>(written),
                                          std::memory_order_relaxed);
            }

            // The stream's own clock: the presentation timestamp, which is
            // the frame reaching the speaker now, and the same counter
            // getFramesWritten() reports against - so the difference is what
            // has been handed over and not yet heard. A stream that has just
            // started (or has just been flushed) has no timestamp to give
            // yet, and the frames its mixer has consumed are the closest
            // thing available until it does.
            std::int64_t position = 0;
            std::int64_t nanos = 0;
            if (AAudioStream_getTimestamp(impl_->stream, CLOCK_MONOTONIC, &position, &nanos) !=
                AAUDIO_OK) {
                position = AAudioStream_getFramesRead(impl_->stream);
            }
            const std::int64_t handed_over = AAudioStream_getFramesWritten(impl_->stream);
            const std::int64_t baseline = impl_->frame_baseline.load(std::memory_order_relaxed);
            impl_->counter.report(
                static_cast<std::uint64_t>(std::max<std::int64_t>(0, handed_over - baseline)),
                static_cast<std::uint64_t>(std::max<std::int64_t>(0, handed_over - position)));
        }
    });

    return {};
}

}  // namespace ac3::audio
