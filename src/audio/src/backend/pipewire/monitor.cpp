#include "ac3/audio/monitor.hpp"

// The PipeWire monitor backend. CMake compiles this directory's monitor.cpp
// on a Linux host that selected pipewire/ over alsa/ (see
// src/audio/CMakeLists.txt) and another directory's everywhere else, so
// there is no #ifdef - the file's path is what says "PipeWire".
//
// This is the easy one of the three, and for the same reason it is easy on
// every other backend: a preview wants to share the output with everything
// else on the machine, so it can accept whatever conversion the graph
// offers instead of demanding the hardware exactly. An ordinary pw_stream
// output already IS that - PipeWire's own audioadapter resamples, remixes
// and mixes with everything else targeting the same sink - which is why
// this backend has no ALSA-style `plug` fallback: there is nothing a raw
// request could fail to negotiate the way a raw ALSA hw: device can.
// channels/sample_rate are requested exactly, and PipeWire gets them
// exactly, because the graph - not this backend - is doing the conversion.

#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>

#include "ac3/audio/playback_counter.hpp"
#include "ac3/audio/ring_buffer.hpp"
#include "pipewire_support.hpp"

namespace ac3::audio {

namespace {

using ac3::pipewire::Stream;
using ac3::pipewire::ThreadLoop;

// See capture.cpp's identical constant for why this bound exists at all.
constexpr int kConnectTimeoutSeconds = 5;

enum class ConnectState : int { kPending, kReady, kError };

bool wait_for_connect(pw_thread_loop* loop, std::atomic<ConnectState>& state) {
    for (int waited = 0; waited < kConnectTimeoutSeconds; ++waited) {
        if (state.load(std::memory_order_acquire) != ConnectState::kPending) {
            break;
        }
        pw_thread_loop_timed_wait(loop, 1);
    }
    return state.load(std::memory_order_acquire) == ConnectState::kReady;
}

}  // namespace

// Impl is private (see capture.cpp's identical note on why its callbacks are
// members of Impl rather than free functions - the same reasoning applies
// here).
struct MonitorSink::Impl {
    ThreadLoop loop;
    Stream stream;
    std::unique_ptr<RingBuffer> queue;
    // Raised by start(). Lowered by stop(), or by state_changed() when the
    // stream ends on its own (see there).
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> underruns{0};
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    // What the process callback last worked out, for position(): the frames
    // it has handed the stream since the last start or flush, against those
    // still between the stream and the speaker (ac3::pipewire::
    // unplayed_frames()). pw_time's ticks are not used: they are the graph
    // driver's clock, which runs on while this stream is paused, so a
    // position built on them jumps forward by the length of every pause.
    PlaybackCounter counter;
    std::uint64_t handed_over = 0;
    // Set by pause() before the stream is made inactive, and cleared by
    // resume() before it is made active again. paused() reports it, since
    // the stream's own state follows pw_stream_set_active() only once the
    // server has acted on it. process() does nothing while it is set, and
    // `in_process` counts the calls under way, so pause() can wait for the
    // last of them to finish. From then until resume() the queue's read
    // side, `handed_over` and the counter are the caller's.
    std::atomic_bool paused{false};
    std::atomic<int> in_process{0};
    // A flush the process callback performs, for the same reason every other
    // backend's consumer performs its own: the callback owns the queue's read
    // side, and this one runs on the graph's DATA thread rather than the
    // thread loop (PW_STREAM_FLAG_RT_PROCESS), so pw_thread_loop_lock does
    // NOT exclude it. `flushes` counts the ones it has done, which is what
    // flush() waits for, and `flush_mark` is how far the queue had been
    // written when the flush was asked for: what it drops.
    std::atomic_bool flushing{false};
    std::atomic<std::uint64_t> flushes{0};
    std::atomic<std::size_t> flush_mark{0};

    // See capture.cpp's Impl for the locking discipline this shares.
    std::atomic<ConnectState> connect_state{ConnectState::kPending};

    static void state_changed(void* data, pw_stream_state /*old_state*/, pw_stream_state state,
                               const char* /*error*/) {
        auto& impl = *static_cast<Impl*>(data);
        if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
            // The stream's own end: an error (its node failed, or the daemon
            // went), or its node removed by the server, which is what a
            // device going away leads to when nothing links the stream
            // elsewhere. No more process() calls will come, so running() says
            // so as stop() would have it: position() reports nothing, and
            // submit(), flush(), pause() and resume() answer at once. start()
            // raises the flag with the loop locked, after the connect, so
            // this cannot come between the two; stop() and a failed start()
            // reach UNCONNECTED themselves, with the flag already down or
            // about to be. A stream the session manager leaves unlinked,
            // waiting for its target to come back, is only PAUSED - not an
            // end, and position() stands still until it is linked again.
            impl.running.store(false, std::memory_order_release);
        }
        if (state == PW_STREAM_STATE_STREAMING || state == PW_STREAM_STATE_PAUSED) {
            auto expected = ConnectState::kPending;
            impl.connect_state.compare_exchange_strong(expected, ConnectState::kReady);
        } else if (state == PW_STREAM_STATE_ERROR) {
            impl.connect_state.store(ConnectState::kError, std::memory_order_release);
        } else {
            return;  // still on its way; nobody is waiting for this one
        }
        // Wake the thread in wait_for_connect(). Without this the answer is
        // set and nobody is told, so the waiter sleeps out its full
        // one-second step - see capture.cpp's note and the measurement there.
        if (impl.loop) {
            pw_thread_loop_signal(impl.loop.get(), false);
        }
    }

    static void process(void* data) {
        auto& impl = *static_cast<Impl*>(data);
        // Counted before `paused` is read, as pause() sets `paused` before it
        // reads the count: whichever of the two goes first, the other sees it.
        impl.in_process.fetch_add(1);
        if (!impl.paused.load()) {
            fill(impl);
        }
        impl.in_process.fetch_sub(1);
    }

    static void fill(Impl& impl) {
        if (!impl.queue || impl.channels == 0) {
            return;
        }
        pw_buffer* buffer = pw_stream_dequeue_buffer(impl.stream.get());
        if (buffer == nullptr) {
            return;
        }
        spa_buffer* spa_buf = buffer->buffer;
        if (spa_buf->n_datas == 0 || spa_buf->datas[0].data == nullptr) {
            pw_stream_queue_buffer(impl.stream.get(), buffer);
            return;
        }

        // A flush the caller asked for. Done here, before this buffer is
        // filled, so the buffer goes out silent from the emptied queue and
        // the counters restart together with it.
        if (impl.flushing.exchange(false, std::memory_order_acq_rel)) {
            impl.queue->discard_to(impl.flush_mark.load(std::memory_order_acquire));
            impl.handed_over = 0;
            impl.counter.restart();
            impl.rendered.store(0, std::memory_order_relaxed);
            impl.submitted.store(0, std::memory_order_relaxed);
            impl.flushes.fetch_add(1, std::memory_order_release);
        }

        const std::uint32_t stride = static_cast<std::uint32_t>(sizeof(float)) * impl.channels;
        // Never more than the buffer holds, whatever the graph asks for.
        const std::uint32_t room = spa_buf->datas[0].maxsize / stride;
        const std::uint32_t requested =
            buffer->requested > 0
                ? static_cast<std::uint32_t>(std::min<std::uint64_t>(buffer->requested, room))
                : room;
        const std::size_t sample_count = static_cast<std::size_t>(requested) * impl.channels;

        auto* out = static_cast<float*>(spa_buf->datas[0].data);
        const auto got = impl.queue->read(std::span{out, sample_count});
        if (got < sample_count) {
            std::fill(out + got, out + sample_count, 0.0f);
            impl.underruns.fetch_add(1, std::memory_order_relaxed);
        }
        impl.rendered.fetch_add(got / impl.channels, std::memory_order_relaxed);

        spa_buf->datas[0].chunk->offset = 0;
        spa_buf->datas[0].chunk->stride = static_cast<std::int32_t>(stride);
        spa_buf->datas[0].chunk->size = static_cast<std::uint32_t>(sample_count) * sizeof(float);
        // In frames, which is then what pw_time.queued counts.
        buffer->size = requested;

        pw_stream_queue_buffer(impl.stream.get(), buffer);
        impl.handed_over += requested;

        // What of it all has been heard: everything handed over, less what
        // the stream and the graph still hold.
        pw_time time{};
        if (pw_stream_get_time_n(impl.stream.get(), &time, sizeof(time)) == 0) {
            impl.counter.report(impl.handed_over,
                                ac3::pipewire::unplayed_frames(time, impl.sample_rate));
        }
    }

    static const pw_stream_events& stream_events() {
        static const pw_stream_events events = [] {
            pw_stream_events value{};
            value.version = PW_VERSION_STREAM_EVENTS;
            value.state_changed = &Impl::state_changed;
            value.process = &Impl::process;
            return value;
        }();
        return events;
    }
};

std::string_view describe(MonitorError error) {
    switch (error) {
        case MonitorError::kNoBackend: return "no monitor backend on this platform";
        case MonitorError::kComFailure: return "a PipeWire call failed";
        case MonitorError::kDeviceNotFound:
            return "the requested playback device was not found (no such PipeWire node, or the "
                   "session manager refused to link it)";
        case MonitorError::kAlreadyRunning: return "monitor playback is already running";
        case MonitorError::kNotRunning: return "monitor playback is not running";
    }
    return "unknown monitor error";
}

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
    // pw_time's delay already counts the whole path to the speaker and is
    // reported as the device's queue; PipeWire offers no separate figure
    // beyond it, so there is no latency left to add.
    return impl_->counter.position(queued_here, /*latency=*/0);
}

void MonitorSink::flush() {
    if (!running() || !impl_->loop || !impl_->stream) {
        return;
    }
    // pw_stream_flush() drops what the stream holds, and must be called from
    // the loop's own context. The queue is NOT reset here: the process
    // callback runs on the graph's data thread, which this lock does not
    // exclude, so the reset is handed to the callback and waited for - see
    // Impl's `flushing`.
    pw_thread_loop_lock(impl_->loop.get());
    pw_stream_flush(impl_->stream.get(), false);
    pw_thread_loop_unlock(impl_->loop.get());

    // Paused: the process callback has stopped touching the queue and the
    // counts (see pause()), so the work is this thread's, including any
    // flush still waiting for the callback.
    if (impl_->paused.load()) {
        impl_->flushing.store(false, std::memory_order_release);
        impl_->queue->discard_to(impl_->queue->write_mark());
        impl_->handed_over = 0;
        impl_->counter.restart();
        impl_->rendered.store(0, std::memory_order_relaxed);
        impl_->submitted.store(0, std::memory_order_relaxed);
        return;
    }

    const std::uint64_t done = impl_->flushes.load(std::memory_order_acquire);
    impl_->flush_mark.store(impl_->queue->write_mark(), std::memory_order_release);
    impl_->flushing.store(true, std::memory_order_release);
    for (int waited = 0; waited < 200; ++waited) {
        if (impl_->flushes.load(std::memory_order_acquire) != done || !running()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // The graph stopped calling back before it could do the work. The flush
    // is left for the next callback - a resume, a route change - and drops
    // only what was queued before the mark, so audio submitted after this
    // call returned is kept. A stream that has ended will make no callback,
    // and state_changed() lowering `running` ends the wait above at once.
}

std::expected<void, MonitorError> MonitorSink::pause() {
    if (!running() || !impl_->loop || !impl_->stream) {
        return std::unexpected(MonitorError::kNotRunning);
    }
    if (impl_->paused.load()) {
        return {};
    }
    impl_->paused.store(true);
    pw_thread_loop_lock(impl_->loop.get());
    const int result = pw_stream_set_active(impl_->stream.get(), false);
    pw_thread_loop_unlock(impl_->loop.get());
    if (result < 0) {
        impl_->paused.store(false);
        return std::unexpected(MonitorError::kComFailure);
    }
    // A callback already past its check of `paused` finishes first; after
    // that, none touches the queue or the counts until resume(). A graph
    // cycle is well under this.
    for (int waited = 0; waited < 200 && impl_->in_process.load() != 0; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}

std::expected<void, MonitorError> MonitorSink::resume() {
    if (!running() || !impl_->loop || !impl_->stream) {
        return std::unexpected(MonitorError::kNotRunning);
    }
    if (!impl_->paused.load()) {
        return {};
    }
    // Cleared before the stream is made active, so its first callback fills.
    impl_->paused.store(false);
    pw_thread_loop_lock(impl_->loop.get());
    const int result = pw_stream_set_active(impl_->stream.get(), true);
    pw_thread_loop_unlock(impl_->loop.get());
    if (result < 0) {
        impl_->paused.store(true);
        return std::unexpected(MonitorError::kComFailure);
    }
    return {};
}

bool MonitorSink::paused() const {
    // What was asked for, not the stream's state, which follows it only once
    // the server has acted.
    return running() && impl_->paused.load();
}

bool MonitorSink::can_submit() const {
    if (!running() || !impl_->queue || impl_->channels == 0) {
        return false;
    }
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
    if (impl_->loop) {
        pw_thread_loop_stop(impl_->loop.get());
        pw_thread_loop_lock(impl_->loop.get());
        impl_->stream.reset();
        pw_thread_loop_unlock(impl_->loop.get());
    }
    impl_->stream.reset();
    impl_->loop.reset();
    // A flush nobody performed, and a pause, do not outlive the stream they
    // were asked of.
    impl_->flushing.store(false, std::memory_order_relaxed);
    impl_->paused.store(false);
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, MonitorError> MonitorSink::start(const std::string& device_id,
                                                      std::uint32_t sample_rate,
                                                      std::uint16_t channels,
                                                      std::uint32_t /*channel_mask*/, bool /*low_latency*/) {
    // channel_mask is a WASAPI speaker mask with no PipeWire counterpart -
    // see the ALSA backend's identical note on this parameter. Accepted and
    // ignored rather than removed, since the header is shared.
    if (running()) {
        return std::unexpected(MonitorError::kAlreadyRunning);
    }
    // A stream that ended on its own still has its loop and its stream, and
    // the loop must go after the stream, not be replaced before it. stop()
    // tears both down in that order; with nothing started it does nothing.
    stop();
    if (channels == 0) {
        return std::unexpected(MonitorError::kComFailure);
    }

    ac3::pipewire::ensure_initialized();

    impl_->loop = ThreadLoop{pw_thread_loop_new("ac3audio-monitor", nullptr)};
    if (!impl_->loop) {
        return std::unexpected(MonitorError::kComFailure);
    }
    if (pw_thread_loop_start(impl_->loop.get()) < 0) {
        impl_->loop.reset();
        return std::unexpected(MonitorError::kComFailure);
    }

    pw_thread_loop_lock(impl_->loop.get());

    // See capture.cpp's start() for why this all happens before connect():
    // the earliest Impl::process()/Impl::state_changed() can run is once connect()
    // releases the lock, so Impl is always complete by then.
    impl_->queue =
        std::make_unique<RingBuffer>(static_cast<std::size_t>(channels) * sample_rate);
    impl_->channels = channels;
    impl_->sample_rate = sample_rate;
    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered.store(0, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);
    impl_->counter.restart();
    impl_->handed_over = 0;
    impl_->paused.store(false);
    impl_->flushing.store(false, std::memory_order_relaxed);
    impl_->flushes.store(0, std::memory_order_relaxed);
    impl_->connect_state.store(ConnectState::kPending, std::memory_order_relaxed);

    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                                              "Playback", PW_KEY_MEDIA_ROLE, "Production", nullptr);
    if (!device_id.empty()) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, device_id.c_str());
    }

    Stream stream{pw_stream_new_simple(pw_thread_loop_get_loop(impl_->loop.get()),
                                        "ac3forge monitor", props, &Impl::stream_events(),
                                        impl_.get())};
    if (!stream) {
        pw_thread_loop_unlock(impl_->loop.get());
        impl_->queue.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        return std::unexpected(MonitorError::kComFailure);
    }

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = channels;
    info.rate = sample_rate;

    std::array<std::uint8_t, 1024> pod_buffer{};
    spa_pod_builder builder{};
    // The function, not the SPA_POD_BUILDER_INIT macro - see capture.cpp's
    // identical call for why.
    spa_pod_builder_init(&builder, pod_buffer.data(), static_cast<std::uint32_t>(pod_buffer.size()));
    const spa_pod* param = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
    const spa_pod* params[1] = {param};

    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                      PW_STREAM_FLAG_MAP_BUFFERS |
                                                      PW_STREAM_FLAG_RT_PROCESS);
    if (pw_stream_connect(stream.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) {
        pw_thread_loop_unlock(impl_->loop.get());
        stream.reset();
        impl_->queue.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        return std::unexpected(MonitorError::kComFailure);
    }

    impl_->stream = std::move(stream);
    const bool ready = wait_for_connect(impl_->loop.get(), impl_->connect_state);

    if (!ready) {
        pw_thread_loop_unlock(impl_->loop.get());
        impl_->stream.reset();
        impl_->queue.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        return std::unexpected(MonitorError::kDeviceNotFound);
    }

    // Raised before the loop is unlocked, so that a state change ending the
    // stream cannot fall between the connect and the flag (state_changed()).
    impl_->running.store(true, std::memory_order_release);
    pw_thread_loop_unlock(impl_->loop.get());
    return {};
}

}  // namespace ac3::audio
