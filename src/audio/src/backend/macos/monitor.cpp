#include "ac3/audio/monitor.hpp"

// The macOS monitor backend. CMake compiles this directory's monitor.cpp
// under APPLE and another platform directory's everywhere else, so there is
// no #ifdef here - the file's path is what says "macOS".
//
// This is meant to be the easy one, and for the same reason it is easy on
// Windows/ALSA: a preview wants to share the output with everything else on
// the machine rather than demand the hardware exactly. Where this backend
// differs is channel count and sample rate, and for a reason neither of the
// other two has: a raw HAL device has no audio-engine layer of its own to
// adapt a foreign format the way WASAPI shared mode does, and no `plug`-
// style conversion plugin to fall through to the way ALSA does - the
// physical device itself runs at exactly one nominal sample rate at a time.
// So this backend requires an exact channel-count match (no downmix/upmix
// matrix of its own, the same limit platform/alsa/monitor.cpp documents for
// itself) and, if the device's CURRENT nominal rate does not match the
// caller's, asks the device to retune via kAudioDevicePropertyNominalSampleRate
// - the same kind of asynchronous-settle wait passthrough.cpp needs for its
// own, very different reason (see that file's header and
// coreaudio_support.hpp's wait_for_nominal_rate). Device-wide and thus not
// perfectly "shared" in WASAPI's sense, but no worse in kind than what
// passthrough.cpp already does, and unavoidable at this API layer - a
// device already running at 44.1 or 48 kHz (by far the common case) never
// touches this path at all.
//
// ---------------------------------------------------------------------------
// No worker thread
// ---------------------------------------------------------------------------
// See coreaudio_support.hpp's own header comment: the IOProc callback
// start() registers below runs on a realtime thread the OS owns, not one
// this library spawned - there is no jthread field in Impl the way the
// ALSA/Windows backends each have one. It is a captureless lambda defined
// inside start() itself, not a free function - see platform/macos/capture.cpp's
// own start() for why (the private Impl type is only reachable from there).

#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include "ac3/audio/playback_counter.hpp"
#include "ac3/audio/ring_buffer.hpp"
#include "coreaudio_names.hpp"
#include "coreaudio_support.hpp"

namespace ac3::audio {

namespace {

constexpr auto kRateChangeTimeout = std::chrono::milliseconds{2000};

}  // namespace

std::string_view describe(MonitorError error) {
    switch (error) {
        case MonitorError::kNoBackend: return "no monitor backend on this platform";
        case MonitorError::kComFailure: return "a Core Audio HAL call failed";
        case MonitorError::kDeviceNotFound:
            return "the requested output device was not found, or will not play this many "
                   "channels at this rate";
        case MonitorError::kAlreadyRunning: return "monitor playback is already running";
        case MonitorError::kNotRunning: return "monitor playback is not running";
    }
    return "unknown monitor error";
}

struct MonitorSink::Impl {
    AudioObjectID device = kAudioObjectUnknown;
    AudioDeviceIOProcID io_proc_id = nullptr;
    std::unique_ptr<RingBuffer> queue;
    coreaudio::SampleFormat format = coreaudio::SampleFormat::kFloat32;
    bool interleaved = true;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> underruns{0};
    // What the IOProc last saw of the device's own clock, for position(): the
    // frames handed over against the lead between the device's time now and
    // the time the frames it is writing will be heard.
    PlaybackCounter counter;
    // kAudioDevicePropertyLatency plus the safety offset, read at start: the
    // delay past the buffer the IOProc fills.
    std::atomic<std::uint32_t> latency{0};
    // Set by pause()/resume(); the device is stopped and started while the
    // IOProc stays registered, so the queue and the format survive.
    std::atomic_bool paused{false};
    // A flush the IOProc performs, since the queue's read side is its own;
    // `flushes` counts the ones it has done, which is what flush() waits for.
    std::atomic_bool flushing{false};
    std::atomic<std::uint64_t> flushes{0};
    // Reused by the IOProc so it never allocates on Apple's realtime I/O
    // thread once warmed up - see platform/macos/capture.cpp's own comment
    // on why this lives here rather than as a lambda capture.
    std::vector<float> scratch;
    std::vector<float> channel_scratch;
    // Frames handed to the device since the last start() or flush(); only the
    // IOProc touches it.
    std::uint64_t handed_over = 0;
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
    return impl_->counter.position(queued_here, impl_->latency.load(std::memory_order_relaxed));
}

void MonitorSink::flush() {
    if (!running() || !impl_->queue) {
        return;
    }
    // What a flush cannot do here: recall the frames the hardware already
    // holds. The HAL has no counterpart to WASAPI's Reset or ALSA's drop -
    // the IOProc writes into the buffer the device is about to play, so the
    // output latency's worth already handed over will still be heard. Both
    // buffers this sink owns are dropped, which is the part it can promise.
    //
    // A stopped device has no IOProc running, so while paused there is nobody
    // to hand the work to and nobody to race with either: AudioDeviceStop
    // does not return while the IOProc is still in use.
    if (impl_->paused.load(std::memory_order_acquire)) {
        impl_->queue->reset();
        impl_->handed_over = 0;
        impl_->counter.restart();
        impl_->rendered.store(0, std::memory_order_relaxed);
        impl_->submitted.store(0, std::memory_order_relaxed);
        return;
    }
    // Running: the IOProc owns the queue's read side, so it does the work and
    // this waits for it. Longer than any device period, and giving up after
    // that is better than blocking a caller on a device that has stopped
    // calling back.
    const std::uint64_t done = impl_->flushes.load(std::memory_order_acquire);
    impl_->flushing.store(true, std::memory_order_release);
    for (int waited = 0; waited < 200; ++waited) {
        if (impl_->flushes.load(std::memory_order_acquire) != done || !running()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::expected<void, MonitorError> MonitorSink::pause() {
    if (!running() || impl_->io_proc_id == nullptr) {
        return std::unexpected(MonitorError::kNotRunning);
    }
    if (impl_->paused.exchange(true, std::memory_order_acq_rel)) {
        return {};
    }
    // The IOProc stays registered: stopping the device only stops it being
    // called, so the queue, the format and the device are all still here for
    // resume().
    if (AudioDeviceStop(impl_->device, impl_->io_proc_id) != noErr) {
        impl_->paused.store(false, std::memory_order_release);
        return std::unexpected(MonitorError::kComFailure);
    }
    return {};
}

std::expected<void, MonitorError> MonitorSink::resume() {
    if (!running() || impl_->io_proc_id == nullptr) {
        return std::unexpected(MonitorError::kNotRunning);
    }
    if (!impl_->paused.exchange(false, std::memory_order_acq_rel)) {
        return {};
    }
    if (AudioDeviceStart(impl_->device, impl_->io_proc_id) != noErr) {
        impl_->paused.store(true, std::memory_order_release);
        return std::unexpected(MonitorError::kComFailure);
    }
    return {};
}

bool MonitorSink::paused() const {
    return impl_->paused.load(std::memory_order_acquire);
}

bool MonitorSink::can_submit() const {
    if (!impl_->queue || impl_->channels == 0) {
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
    // generic threshold - see the Windows backend for the full account of
    // why a fixed threshold smaller than the chunk being pushed is unsafe.
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
    if (impl_->io_proc_id != nullptr) {
        AudioDeviceStop(impl_->device, impl_->io_proc_id);
        AudioDeviceDestroyIOProcID(impl_->device, impl_->io_proc_id);
        impl_->io_proc_id = nullptr;
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
    // channel_mask is a WASAPI speaker mask with no HAL counterpart - a HAL
    // stream carries a channel COUNT and the driver's own documented
    // channel order, the same reason platform/alsa/monitor.cpp also accepts
    // and ignores it. Kept in the signature rather than removed because the
    // header is shared with the Windows backend, which does use it.
    if (running()) {
        return std::unexpected(MonitorError::kAlreadyRunning);
    }
    if (channels == 0) {
        return std::unexpected(MonitorError::kComFailure);
    }

    const AudioObjectID device = device_id.empty() ? coreaudio::default_device(/*input=*/false)
                                                    : coreaudio::device_for_uid(device_id);
    if (device == kAudioObjectUnknown) {
        return std::unexpected(MonitorError::kDeviceNotFound);
    }
    if (coreaudio::channel_count(device, kAudioDevicePropertyScopeOutput) !=
        static_cast<std::uint32_t>(channels)) {
        return std::unexpected(MonitorError::kDeviceNotFound);
    }

    const auto current_rate = coreaudio::nominal_sample_rate(device);
    if (std::abs(current_rate - static_cast<Float64>(sample_rate)) >= 1.0) {
        if (!coreaudio::set_property(device,
                                     coreaudio::address(kAudioDevicePropertyNominalSampleRate),
                                     static_cast<Float64>(sample_rate)) ||
            !coreaudio::wait_for_nominal_rate(device, static_cast<Float64>(sample_rate),
                                              kRateChangeTimeout)) {
            return std::unexpected(MonitorError::kDeviceNotFound);
        }
    }

    const auto asbd = coreaudio::get_property<AudioStreamBasicDescription>(
        device,
        coreaudio::address(kAudioDevicePropertyStreamFormat, kAudioDevicePropertyScopeOutput));
    if (!asbd) {
        return std::unexpected(MonitorError::kComFailure);
    }
    const auto format = coreaudio::classify_pcm(*asbd);
    if (format == coreaudio::SampleFormat::kUnsupported) {
        return std::unexpected(MonitorError::kComFailure);
    }

    // Room for roughly a second of samples, so a caller decoding slightly
    // ahead of real time never has to spin.
    impl_->queue =
        std::make_unique<RingBuffer>(static_cast<std::size_t>(channels) * sample_rate);
    impl_->device = device;
    impl_->channels = channels;
    impl_->sample_rate = sample_rate;
    impl_->format = format;
    impl_->interleaved = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0;
    impl_->scratch.reserve(static_cast<std::size_t>(channels) * 4096);
    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered.store(0, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);
    impl_->counter.restart();
    impl_->paused.store(false, std::memory_order_relaxed);
    impl_->flushing.store(false, std::memory_order_relaxed);
    impl_->flushes.store(0, std::memory_order_relaxed);
    impl_->handed_over = 0;
    // The delay past the buffer the IOProc fills, in frames, which is what
    // MonitorPosition::latency_frames reports: the device's own presentation
    // latency and the safety offset the HAL adds in front of it. Both are
    // already in frames at the device's nominal rate. Either may be missing
    // on a device that does not publish it, and zero then means "cannot
    // say", as the header's comment on the field has it.
    const auto device_latency = coreaudio::get_property<UInt32>(
        device, coreaudio::address(kAudioDevicePropertyLatency, kAudioDevicePropertyScopeOutput));
    const auto safety_offset = coreaudio::get_property<UInt32>(
        device,
        coreaudio::address(kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeOutput));
    impl_->latency.store(device_latency.value_or(0) + safety_offset.value_or(0),
                         std::memory_order_relaxed);

    // A captureless lambda, not a free function - see
    // platform/macos/capture.cpp's own start() for why: AudioDeviceIOProc
    // needs a plain C function pointer, but `Impl` is private to
    // MonitorSink, so only a member function (or something lexically
    // nested inside one, which is exactly what this is) has access to it.
    const auto io_proc = [](AudioObjectID /*device*/, const AudioTimeStamp* now,
                            const AudioBufferList* /*input_data*/,
                            const AudioTimeStamp* /*input_time*/, AudioBufferList* output,
                            const AudioTimeStamp* output_time, void* client_data) -> OSStatus {
        auto* impl = static_cast<Impl*>(client_data);
        if (output == nullptr || output->mNumberBuffers == 0 || impl->channels == 0) {
            return noErr;
        }
        const auto bytes = coreaudio::bytes_per_sample(impl->format);
        if (bytes == 0) {
            return noErr;
        }
        // A flush the caller asked for: drop what this sink holds and restart
        // the counters with it, which is what position() promises. This
        // buffer then goes out silent from the empty queue, and counts as an
        // underrun - the same bookkeeping the other backends show after a
        // flush, and the honest description of what the device played.
        if (impl->flushing.exchange(false, std::memory_order_acq_rel)) {
            impl->queue->reset();
            impl->handed_over = 0;
            impl->counter.restart();
            impl->rendered.store(0, std::memory_order_relaxed);
            impl->submitted.store(0, std::memory_order_relaxed);
            impl->flushes.fetch_add(1, std::memory_order_release);
        }

        const std::size_t frames = impl->interleaved
                                       ? output->mBuffers[0].mDataByteSize /
                                             (bytes * static_cast<std::size_t>(impl->channels))
                                       : output->mBuffers[0].mDataByteSize / bytes;
        if (frames == 0) {
            return noErr;
        }

        impl->scratch.resize(frames * impl->channels);
        const auto got = impl->queue->read(impl->scratch);
        if (got < impl->scratch.size()) {
            // Nothing queued: emit silence for the remainder, counted
            // rather than hidden, matching PassthroughSink's underrun
            // discipline.
            std::fill(impl->scratch.begin() + static_cast<std::ptrdiff_t>(got),
                      impl->scratch.end(), 0.0f);
            impl->underruns.fetch_add(1, std::memory_order_relaxed);
        }

        if (impl->interleaved) {
            coreaudio::float_to_samples(impl->scratch, impl->format,
                                        static_cast<std::byte*>(output->mBuffers[0].mData));
        } else {
            impl->channel_scratch.resize(frames);
            const auto channel_limit = static_cast<UInt32>(impl->channels);
            for (UInt32 ch = 0; ch < output->mNumberBuffers && ch < channel_limit; ++ch) {
                for (std::size_t i = 0; i < frames; ++i) {
                    impl->channel_scratch[i] = impl->scratch[i * impl->channels + ch];
                }
                coreaudio::float_to_samples(impl->channel_scratch, impl->format,
                                            static_cast<std::byte*>(output->mBuffers[ch].mData));
            }
        }
        impl->rendered.fetch_add(got / impl->channels, std::memory_order_relaxed);

        // The device's own clock, which the HAL hands over on every call: the
        // first frame of this buffer is heard at output_time, so at now every
        // frame handed over before it has been heard bar the lead between the
        // two. What the device still holds is this buffer plus that lead.
        // Both timestamps carry flags saying which of their fields are
        // filled in, and a device is free to leave the sample time out. A
        // lead of more than a second is a driver saying something this code
        // has no use for, and is dropped rather than reported as a queue
        // depth nothing put there.
        std::uint64_t lead = 0;
        if (now != nullptr && output_time != nullptr &&
            (now->mFlags & kAudioTimeStampSampleTimeValid) != 0 &&
            (output_time->mFlags & kAudioTimeStampSampleTimeValid) != 0 &&
            output_time->mSampleTime > now->mSampleTime) {
            const auto reported =
                static_cast<std::uint64_t>(output_time->mSampleTime - now->mSampleTime);
            lead = reported <= impl->sample_rate ? reported : 0;
        }
        impl->handed_over += frames;
        impl->counter.report(impl->handed_over, frames + lead);
        return noErr;
    };

    AudioDeviceIOProcID proc_id = nullptr;
    if (AudioDeviceCreateIOProcID(device, io_proc, impl_.get(), &proc_id) != noErr ||
        proc_id == nullptr) {
        return std::unexpected(MonitorError::kComFailure);
    }
    if (AudioDeviceStart(device, proc_id) != noErr) {
        AudioDeviceDestroyIOProcID(device, proc_id);
        return std::unexpected(MonitorError::kComFailure);
    }

    impl_->io_proc_id = proc_id;
    impl_->running.store(true, std::memory_order_release);
    return {};
}

}  // namespace ac3::audio
