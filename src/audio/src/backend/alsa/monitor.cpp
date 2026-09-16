#include "ac3/audio/monitor.hpp"

// The ALSA monitor backend. CMake compiles this directory's monitor.cpp on a
// Linux host whose libasound development headers are present and another
// directory's everywhere else, so there is no #ifdef - the file's path is what
// says "ALSA".
//
// This is the easy one of the three, and for the same reason it is easy on
// Windows: a preview wants to share the output with everything else on the
// machine, so it can accept whatever conversion the system offers instead of
// demanding the hardware exactly. ALSA's `default` already IS that - a plugin
// chain ending in dmix or in whatever sound server is running - which is why
// an empty device id resolves to it and needs nothing else said.
//
// The one place this differs from passthrough.cpp is the fallback below. A
// caller who names a specific device gets it opened directly first, and only
// if the device will not take the caller's format, rate or channel count does
// the request go through `plug`. Passthrough must never do that (conversion is
// exactly what corrupts a bitstream); a monitor should, because a preview that
// resamples is a preview and a preview that refuses to start is not.

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fmt/format.h>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/audio/playback_counter.hpp"
#include "ac3/audio/ring_buffer.hpp"
#include "alsa_support.hpp"

namespace ac3::audio {

namespace {

using alsa::FormatChoice;
using alsa::HwParams;
using alsa::Pcm;
using alsa::SampleFormat;
using alsa::SwParams;

constexpr snd_pcm_uframes_t kPreferredPeriod = 1024;
constexpr unsigned kPeriodsPerBuffer = 4;
constexpr int kWaitMs = 100;

// Convert normalised float into the device's format, in place into `out`.
//
// The reverse of the capture backend's convert(), and the reason both exist:
// ALSA hands a raw device's own format straight through, so whichever end
// wants float has to do the work. Clamped, because a decoder that produces a
// sample slightly outside [-1, 1] should sound loud, not wrap around to the
// opposite polarity and click.
void convert(std::span<const float> samples, SampleFormat format, std::vector<std::byte>& out) {
    out.resize(samples.size() * (format == SampleFormat::kPcm16      ? 2
                                 : format == SampleFormat::kPcm24Packed ? 3
                                                                        : 4));
    switch (format) {
        case SampleFormat::kFloat32: {
            std::memcpy(out.data(), samples.data(), samples.size() * sizeof(float));
            break;
        }
        case SampleFormat::kPcm16: {
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const float clamped = std::clamp(samples[i], -1.0f, 1.0f);
                const auto value = static_cast<std::int16_t>(clamped * 32767.0f);
                std::memcpy(out.data() + i * 2, &value, sizeof(value));
            }
            break;
        }
        case SampleFormat::kPcm24Packed: {
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const float clamped = std::clamp(samples[i], -1.0f, 1.0f);
                const auto value = static_cast<std::int32_t>(clamped * 8388607.0f);
                const auto bits = static_cast<std::uint32_t>(value);
                out[i * 3 + 0] = static_cast<std::byte>(bits & 0xFFu);
                out[i * 3 + 1] = static_cast<std::byte>((bits >> 8) & 0xFFu);
                out[i * 3 + 2] = static_cast<std::byte>((bits >> 16) & 0xFFu);
            }
            break;
        }
        case SampleFormat::kPcm32: {
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const float clamped = std::clamp(samples[i], -1.0f, 1.0f);
                const auto value = static_cast<std::int32_t>(clamped * 2147483520.0f);
                std::memcpy(out.data() + i * 4, &value, sizeof(value));
            }
            break;
        }
    }
}

struct Opened {
    Pcm pcm;
    FormatChoice format;
    snd_pcm_uframes_t period = 0;
    // Whether the hardware can pause and resume without losing what it
    // holds; read from the parameter space that was accepted, since
    // nothing can be asked of the handle once the worker owns it.
    bool can_pause = false;
};

// Open `name` and configure it for `channels` of PCM at `sample_rate`, or
// return nothing if it will not take them.
std::optional<Opened> open_configured(const std::string& name, std::uint32_t sample_rate,
                                      std::uint16_t channels, bool quiet) {
    // The first of the two attempts start() makes is allowed to fail as a
    // matter of course, so it is silenced; the second is the one whose failure
    // the caller actually hears about, and alsa-lib's own line about it is
    // worth having.
    const std::optional<alsa::QuietErrors> hush =
        quiet ? std::optional<alsa::QuietErrors>{std::in_place} : std::nullopt;

    snd_pcm_t* handle = nullptr;
    if (snd_pcm_open(&handle, name.c_str(), SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        return std::nullopt;
    }
    Opened opened{.pcm = Pcm{handle}, .format = {}, .period = 0, .can_pause = false};

    HwParams params;
    if (!params || snd_pcm_hw_params_any(handle, params.get()) < 0 ||
        snd_pcm_hw_params_set_access(handle, params.get(), SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
        return std::nullopt;
    }
    const auto format = alsa::choose_format(handle, params.get());
    if (!format) {
        return std::nullopt;
    }
    opened.format = *format;

    // Exact, not _near, for both: the caller is playing back decoded audio it
    // has already committed to a rate and a channel count for, and this
    // backend has no resampler or downmix matrix of its own. A device that
    // says no here is what the `plug` retry in start() is for.
    if (snd_pcm_hw_params_set_channels(handle, params.get(), channels) < 0 ||
        snd_pcm_hw_params_set_rate(handle, params.get(), sample_rate, 0) < 0) {
        return std::nullopt;
    }

    snd_pcm_uframes_t period = kPreferredPeriod;
    int direction = 0;
    if (snd_pcm_hw_params_set_period_size_near(handle, params.get(), &period, &direction) < 0) {
        return std::nullopt;
    }
    snd_pcm_uframes_t buffer = period * kPeriodsPerBuffer;
    if (snd_pcm_hw_params_set_buffer_size_near(handle, params.get(), &buffer) < 0 ||
        snd_pcm_hw_params(handle, params.get()) < 0) {
        return std::nullopt;
    }
    opened.period = period;
    opened.can_pause = snd_pcm_hw_params_can_pause(params.get()) == 1;

    SwParams software;
    if (software && snd_pcm_sw_params_current(handle, software.get()) >= 0) {
        snd_pcm_sw_params_set_start_threshold(handle, software.get(), buffer);
        snd_pcm_sw_params_set_avail_min(handle, software.get(), period);
        snd_pcm_sw_params(handle, software.get());
    }
    return opened;
}

// The same device behind alsa-lib's `plug` plugin, which inserts whatever
// format, rate and channel conversion is needed.
//
// Spelled with an inline configuration block rather than as "plug:<name>",
// because a slave name containing its own colons and commas ("hw:CARD=PCH,
// DEV=0") would otherwise be split into the plug plugin's own arguments.
std::string through_plug(const std::string& name) {
    return fmt::format("plug:{{SLAVE=\"{}\"}}", name);
}

}  // namespace

std::string_view describe(MonitorError error) {
    switch (error) {
        case MonitorError::kNoBackend: return "no monitor backend on this platform";
        case MonitorError::kComFailure: return "an ALSA call failed";
        case MonitorError::kDeviceNotFound:
            return "the requested playback device was not found, or will not play this many "
                   "channels at this rate";
        case MonitorError::kAlreadyRunning: return "monitor playback is already running";
        case MonitorError::kNotRunning: return "monitor playback is not running";
    }
    return "unknown monitor error";
}

struct MonitorSink::Impl {
    std::unique_ptr<RingBuffer> queue;
    std::jthread worker;
    snd_pcm_t* pcm = nullptr;
    // Raised by start(). Lowered by stop(), or by the render thread itself
    // when the device goes away under it (see the end of its loop).
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> underruns{0};
    std::uint16_t channels = 0;
    // What the render thread last read from the device, for position():
    // snd_pcm_delay() is the frames it still has to play, against the frames
    // handed over. Only the render thread touches the handle - alsa-lib's PCM
    // object is not for two threads at once - so a position() on the caller's
    // thread reads the counter.
    PlaybackCounter counter;
    // Whether the hardware can pause without losing what it holds
    // (snd_pcm_hw_params_can_pause); see pause() for what happens when it
    // cannot.
    bool can_pause = false;
    std::atomic_bool paused{false};
    std::atomic_bool flushing{false};
    std::atomic<std::uint64_t> flushes{0};
    // How far the queue had been written when the flush was asked for: what
    // the render thread drops.
    std::atomic<std::size_t> flush_mark{0};
};

MonitorSink::MonitorSink() : impl_(std::make_unique<Impl>()) {}

MonitorSink::~MonitorSink() {
    stop();
}

bool MonitorSink::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

MonitorStats MonitorSink::stats() const {
    return {.frames_submitted = impl_->submitted.load(),
            .frames_rendered = impl_->rendered.load(),
            .underruns = impl_->underruns.load()};
}

std::optional<MonitorPosition> MonitorSink::position() const {
    if (!running() || !impl_->queue || impl_->channels == 0) {
        return std::nullopt;
    }
    const std::uint64_t queued_here = impl_->queue->available() / impl_->channels;
    // snd_pcm_delay() already counts the whole path to the speaker, and it is
    // reported as the device's queue; ALSA offers no separate figure beyond
    // it, so there is no latency left to add.
    return impl_->counter.position(queued_here, /*latency=*/0);
}

void MonitorSink::flush() {
    if (!running()) {
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
    // The render thread did not get to it - a device that has stopped
    // answering snd_pcm_wait. The flush is left for the thread to make when
    // it next runs. It drops only what was queued before the mark, so audio
    // submitted after this call returned is kept. A device whose recovery
    // failed has ended the thread instead, which lowered `running` and ended
    // the wait at once.
}

std::expected<void, MonitorError> MonitorSink::pause() {
    if (!running()) {
        return std::unexpected(MonitorError::kNotRunning);
    }
    // A device whose hardware cannot pause is stopped and prepared again
    // instead, which drops the frames it was holding - up to a buffer's worth,
    // the same frames a flush() would drop. The queue survives either way, so
    // playback resumes from where the caller's stream had got to rather than
    // from silence.
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
    // A pause is a property of a running stream, so one whose device has
    // gone is not paused either.
    return running() && impl_->paused.load(std::memory_order_acquire);
}

bool MonitorSink::can_submit() const {
    if (!running() || !impl_->queue || impl_->channels == 0) {
        return false;
    }
    // Room for at least ~20 ms at a typical rate, in samples (interleaved).
    // A hint for callers deciding whether to spin-wait, not a correctness
    // guarantee - submit() below is what actually gates the write.
    return impl_->queue->capacity() - impl_->queue->available() >
           static_cast<std::size_t>(impl_->channels) * 960;
}

bool MonitorSink::submit(std::span<const float> interleaved) {
    if (!running() || !impl_->queue || impl_->channels == 0 ||
        interleaved.size() % impl_->channels != 0) {
        return false;
    }
    // Checked against THIS call's actual size rather than can_submit()'s
    // generic threshold, so a chunk larger than that threshold cannot be
    // partially written while submit() reports failure and the caller retries
    // it - see the Windows backend for the full account of that failure.
    if (impl_->queue->capacity() - impl_->queue->available() <= interleaved.size()) {
        return false;
    }
    const auto wrote = impl_->queue->write(interleaved);
    if (wrote != interleaved.size()) {
        return false;
    }
    impl_->submitted.fetch_add(interleaved.size() / impl_->channels);
    return true;
}

void MonitorSink::stop() {
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    if (impl_->pcm != nullptr) {
        snd_pcm_close(impl_->pcm);
        impl_->pcm = nullptr;
    }
    // A pause is a property of a running stream, so it does not outlive one.
    impl_->paused.store(false, std::memory_order_relaxed);
    impl_->flushing.store(false, std::memory_order_relaxed);
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, MonitorError> MonitorSink::start(const std::string& device_id,
                                                     std::uint32_t sample_rate,
                                                     std::uint16_t channels,
                                                     std::uint32_t /*channel_mask*/, bool /*low_latency*/) {
    // channel_mask is a WASAPI speaker mask and has no ALSA counterpart: an
    // ALSA PCM stream carries a channel COUNT and the convention that the
    // channels are in the order the card documents. It is accepted and ignored
    // rather than removed from the interface, because the Windows backend does
    // use it and the header is shared.
    if (running()) {
        return std::unexpected(MonitorError::kAlreadyRunning);
    }
    // A render thread that ended because its device went away still has its
    // handle open; stop() joins the one and closes the other. With nothing
    // started it does nothing.
    stop();
    if (channels == 0) {
        return std::unexpected(MonitorError::kComFailure);
    }

    const std::string name = device_id.empty() ? "default" : device_id;
    auto opened = open_configured(name, sample_rate, channels, /*quiet=*/true);
    if (!opened) {
        // Second chance through `plug`. Reached when the device is a raw one
        // that cannot itself do the caller's rate or channel count - and never
        // for `default`, which is already a plugin chain and either worked or
        // is not there at all.
        opened = open_configured(through_plug(name), sample_rate, channels, /*quiet=*/false);
    }
    if (!opened) {
        return std::unexpected(MonitorError::kDeviceNotFound);
    }
    if (snd_pcm_prepare(opened->pcm.get()) < 0) {
        return std::unexpected(MonitorError::kComFailure);
    }

    // Room for roughly a second of samples, so a caller decoding slightly
    // ahead of real time never has to spin.
    impl_->queue =
        std::make_unique<RingBuffer>(static_cast<std::size_t>(channels) * sample_rate);
    impl_->channels = channels;
    impl_->submitted.store(0);
    impl_->rendered.store(0);
    impl_->underruns.store(0);
    impl_->counter.restart();
    impl_->paused.store(false);
    impl_->flushing.store(false);
    impl_->flushes.store(0);
    impl_->can_pause = opened->can_pause;
    impl_->running.store(true, std::memory_order_release);
    impl_->pcm = opened->pcm.release();

    const FormatChoice format = opened->format;
    const snd_pcm_uframes_t period = opened->period;

    impl_->worker = std::jthread([this, format, period, channels](const std::stop_token& stop) {
        snd_pcm_t* pcm = impl_->pcm;
        const std::size_t samples_per_period = static_cast<std::size_t>(period) * channels;

        std::vector<float> chunk(samples_per_period);
        std::vector<std::byte> raw;
        std::uint64_t handed_over = 0;
        bool device_paused = false;
        // Whether the pause in force was performed by dropping rather than by
        // snd_pcm_pause, which decides how it is undone.
        bool dropped_to_pause = false;
        // Set when the loop ends because the device did - a recovery that
        // failed, -ENODEV for a card unplugged - rather than because stop()
        // asked it to. A device that goes while paused is found on resume,
        // when the first wait or write fails.
        bool lost = false;

        while (!stop.stop_requested()) {
            // The device and the queue belong to this thread; pause() and
            // flush() only raise a flag. A hardware pause keeps what the
            // device holds; a device that cannot pause is dropped and
            // prepared again, which loses it (see pause()).
            const bool wanted_pause = impl_->paused.load(std::memory_order_acquire);
            if (wanted_pause != device_paused) {
                if (wanted_pause) {
                    // snd_pcm_pause only works from RUNNING, and only on
                    // hardware that claimed it: a stream still PREPARED (the
                    // start threshold is a whole buffer, so an immediate
                    // pause after start() is exactly that case) refuses it.
                    // A refusal is not a pause, so fall through to the drop,
                    // which always works and loses what the device held -
                    // the same trade pause() documents for hardware that
                    // cannot pause at all.
                    const bool held = impl_->can_pause && snd_pcm_pause(pcm, 1) == 0;
                    if (!held) {
                        snd_pcm_drop(pcm);
                        snd_pcm_prepare(pcm);
                        // The dropped frames will never be heard, so what
                        // the device was given is what it played, and it now
                        // holds nothing.
                        handed_over = impl_->counter.played();
                        impl_->counter.report(handed_over, 0);
                    }
                    dropped_to_pause = !held;
                } else if (!dropped_to_pause) {
                    snd_pcm_pause(pcm, 0);
                } else {
                    // Dropped rather than paused, so the stream is PREPARED
                    // and the writes below start it again; prepare() only
                    // repeats what the drop path already did.
                    dropped_to_pause = false;
                }
                device_paused = wanted_pause;
            }
            if (device_paused && !impl_->flushing.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (impl_->flushing.exchange(false, std::memory_order_acq_rel)) {
                // drop discards what the device holds; prepare puts the stream
                // back in a state that can be written to.
                snd_pcm_drop(pcm);
                snd_pcm_prepare(pcm);
                impl_->queue->discard_to(impl_->flush_mark.load(std::memory_order_acquire));
                handed_over = 0;
                impl_->counter.restart();
                impl_->rendered.store(0, std::memory_order_relaxed);
                impl_->submitted.store(0, std::memory_order_relaxed);
                impl_->flushes.fetch_add(1, std::memory_order_release);
                if (device_paused) {
                    continue;
                }
            }
            snd_pcm_sframes_t delay = 0;
            if (snd_pcm_delay(pcm, &delay) == 0 && delay >= 0) {
                impl_->counter.report(handed_over, static_cast<std::uint64_t>(delay));
            }
            const int ready = snd_pcm_wait(pcm, kWaitMs);
            if (ready < 0) {
                if (snd_pcm_recover(pcm, ready, /*silent=*/1) < 0) {
                    lost = true;
                    break;
                }
                continue;
            }
            if (ready == 0) {
                continue;
            }

            const auto got = impl_->queue->read(chunk);
            if (got < chunk.size()) {
                // Nothing queued: emit silence for the remainder, counted
                // rather than hidden, matching PassthroughSink's discipline.
                std::fill(chunk.begin() + static_cast<std::ptrdiff_t>(got), chunk.end(), 0.0f);
                impl_->underruns.fetch_add(1);
            }
            convert(chunk, format.kind, raw);

            const snd_pcm_sframes_t written = snd_pcm_writei(pcm, raw.data(), period);
            if (written < 0) {
                if (snd_pcm_recover(pcm, static_cast<int>(written), /*silent=*/1) < 0) {
                    lost = true;
                    break;
                }
                continue;
            }
            handed_over += static_cast<std::uint64_t>(written);
            impl_->rendered.fetch_add(got / channels);
        }

        if (lost) {
            // As in the Windows backend: the stream has ended with its
            // device, and running() says so as a stop() would have it, so
            // position(), submit(), flush(), pause() and resume() answer at
            // once. Only the flag is touched; stop() still joins this thread,
            // closes the handle and lowers the caller's `paused` and
            // `flushing`.
            impl_->running.store(false, std::memory_order_release);
        }
        snd_pcm_drop(pcm);
    });

    return {};
}

}  // namespace ac3::audio
