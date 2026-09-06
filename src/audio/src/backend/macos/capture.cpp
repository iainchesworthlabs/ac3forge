#include "ac3/audio/capture.hpp"

// The macOS capture backend. CMake compiles this directory's capture.cpp
// under APPLE and another platform directory's everywhere else, so there is
// no #ifdef here - the file's path is what says "macOS".
//
// This is the CoreAudio Audio HAL (AudioObjectID / AudioDeviceIOProc) - the
// same layer passthrough.cpp uses, not the higher-level AVAudioEngine or
// Audio Queue Services APIs. That choice mirrors why platform/alsa is ALSA
// rather than PulseAudio/PipeWire (see platform/alsa/passthrough.cpp's own
// header): passthrough's exclusive-mode physical-format switch has to
// happen at the HAL, so the HAL is where device enumeration, hog mode and
// format negotiation all live in one place (coreaudio_support.hpp), and
// capture sharing that layer means it shares that code rather than talking
// to a second, higher-level API with its own device model. AVAudioEngine
// also has no equivalent to a persistent per-endpoint UID the way
// kAudioDevicePropertyDeviceUID gives every HAL object, which every
// DeviceInfo.id in this project's other backends already assumes is stable
// across a session (see capture.hpp).
//
// ---------------------------------------------------------------------------
// Loopback
// ---------------------------------------------------------------------------
// Unlike WASAPI (any render endpoint reopened in loopback mode) or PipeWire
// (a sink's monitor, targeted via stream.capture.sink), there is no HAL-level
// "capture what a render device is playing" - the Audio HAL this file already
// uses for real input has no loopback concept at all. So enumerate_devices()
// below reports every real input endpoint and no loopback ones, and start()
// refuses DeviceKind::kLoopback outright rather than silently substituting a
// microphone. That is the same category of gap platform/alsa/capture.cpp
// documents for a machine with no snd-aloop module loaded: the honest answer
// rather than a failure.
//
// What macOS 14.2 does provide is the PER-PROCESS tap, and
// start_process_loopback() at the bottom of this file is built on it: a
// CATapDescription over one audio process object, carried by a private
// aggregate device whose IOProc reads exactly the way a real input device's
// does. process_tap.hpp and process_tap.mm hold the Objective-C++ half -
// CATapDescription has no C entry point - and describe the three objects
// involved. What belongs here is what a CALLER of Capture sees, which is three
// places where this backend's contract is narrower than the Windows one it is
// modelled on (see CONTRIBUTING.md's "Where behaviour is deliberately
// narrower than the standard, say so and say why"), plus a fourth where what
// it REPORTS differs rather than what it promises:
//
//   - ONE PROCESS, NOT A TREE. ProcessLoopbackMode::kIncludeProcessTree is
//     honoured as "this process", kExcludeProcessTree as "everything except
//     this process". Windows' activation walks the target's children because
//     a browser renders its audio from a utility process under the browser;
//     a CATapDescription takes a list of audio process objects with no
//     descendant relation for it to follow, so there is nothing here to walk.
//     A caller that wants a browser's audio has to name the process that is
//     playing it. PipeWire's tap has the same property for its own reason
//     (see docs/crucible/promotion.md's Phase 4 record), so Windows is the
//     only one of the three backends with a tap where that mode name is
//     literal.
//
//   - MONO OR STEREO, AND NOTHING ELSE. WASAPI's activation lets a caller
//     state any WAVEFORMATEXTENSIBLE and has the audio engine convert to it;
//     a CATapDescription's mixdown descriptions are mono and stereo, with no
//     converter behind them at all. ProcessLoopbackFormat::channels of 1 or 2
//     is accepted and anything else - including the eight channels
//     capture.hpp offers so a surround-rendering application's tap reaches a
//     bed intact - is refused with kFormatUnsupported rather than quietly
//     delivering two and letting the caller believe it asked for them.
//
//   - THE RATE IS THE MACHINE'S, AND IS CHECKED RATHER THAN CONVERTED. A
//     mixdown tap runs at the rate of the device it mixes down to, which
//     neither this code nor its caller chose. capture.hpp promises samples
//     land in the ring "at exactly `format`", so a tap whose own
//     kAudioTapPropertyFormat disagrees with what was asked for is refused
//     with kFormatUnsupported. The alternative - taking it anyway and
//     reporting the true rate through sample_rate() - would keep more
//     machines working at the price of the one promise that header makes,
//     and a caller that had already sized an encoder from the number it
//     passed in would be the one to discover the difference.
//
//   - NO SILENCE IS SYNTHESISED, and this is the bullet to distrust. The
//     Windows and PipeWire taps poll and fill wall-clock gaps because a quiet
//     process delivers no packets at all. The aggregate device here is clocked
//     by the output device it names as its main sub-device rather than by the
//     tapped application, so on the API's own account its IOProc is called
//     every device period whether that application is playing or not, the
//     timeline advances on its own, and CaptureStats::frames_silence_filled
//     stays zero for a tap as it already does for a real input device. That
//     reading of the API has not been observed to be right - no Mac has run
//     this - and it is the assumption most likely to be wrong, because a tap
//     that instead delivers nothing while its process is quiet would need the
//     wall-clock fill the other two backends have and this file has not
//     written.
//
// Two things this file cannot answer, and neither of them is a code question.
// Creating a tap raises a TCC consent prompt under its own permission
// category (SystemAudioCaptureRequests, driven by an
// NSAudioCaptureUsageDescription Info.plist key, separate from microphone
// access); that prompt is keyed to the requesting binary's code-signing
// identity and, per every report surveyed while writing this, never fires at
// all for an unsigned binary - and ac3cli/ac3gui/Crucible ship unsigned today
// (roadmap DR6, blocked on certificates). A denial and a prompt that never
// appeared both arrive here as one refusal from AudioHardwareCreateProcessTap
// and are reported as kComFailure, because the API offers nothing to tell
// them apart. And no Mac has ever run any of this backend (DR9): what is
// claimed for everything past this comment is that it compiles, which is the
// whole of the claim and is why nothing below says "verified" or "works".
// See docs/platforms/macos.md.
//
// ---------------------------------------------------------------------------
// No worker thread
// ---------------------------------------------------------------------------
// See coreaudio_support.hpp's own header comment: the IOProc callback both
// start paths below register runs on a realtime thread the OS owns (between
// AudioDeviceStart and AudioDeviceStop), not one this library spawned -
// there is no jthread field in Impl the way the ALSA/Windows backends each
// have one. It is a static member of Impl rather than a free function or a
// lambda - see Impl::io_proc's own comment for why. The Windows tap needs a
// polling thread of its own to synthesise silence for a quiet process; this
// one does not, because the aggregate device carrying the tap is clocked
// whether the tapped application is playing or not.

#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "coreaudio_names.hpp"
#include "coreaudio_support.hpp"
#include "process_tap.hpp"

namespace ac3::audio {

namespace {

// Weaves however many buffers `input` carries (one interleaved buffer, or
// one per channel) into `out`, normalised to float - mirrors
// platform/alsa/capture.cpp's own convert(), generalised over the layout
// question ALSA never has (alsa-lib always hands back one interleaved
// buffer).
void collect(const AudioBufferList* input, std::uint16_t channels, bool interleaved,
            coreaudio::SampleFormat format, std::vector<float>& out) {
    const auto bytes = coreaudio::bytes_per_sample(format);
    if (input == nullptr || input->mNumberBuffers == 0 || bytes == 0 || channels == 0) {
        out.clear();
        return;
    }
    if (interleaved) {
        const auto& buffer = input->mBuffers[0];
        const std::size_t sample_count = buffer.mDataByteSize / bytes;
        out.resize(sample_count);
        coreaudio::samples_to_float(static_cast<const std::byte*>(buffer.mData), sample_count,
                                    format, out);
        return;
    }

    const std::size_t frames = input->mBuffers[0].mDataByteSize / bytes;
    out.assign(frames * channels, 0.0f);
    std::vector<float> channel_scratch;
    const auto channel_limit = static_cast<UInt32>(channels);
    for (UInt32 ch = 0; ch < input->mNumberBuffers && ch < channel_limit; ++ch) {
        const auto& buffer = input->mBuffers[ch];
        const std::size_t ch_frames = buffer.mDataByteSize / bytes;
        channel_scratch.resize(ch_frames);
        coreaudio::samples_to_float(static_cast<const std::byte*>(buffer.mData), ch_frames, format,
                                    channel_scratch);
        for (std::size_t i = 0; i < ch_frames && i < frames; ++i) {
            out[i * channels + ch] = channel_scratch[i];
        }
    }
}

// Why a tap was not created, in the vocabulary capture.hpp already has. The
// mapping lives here rather than in process_tap.hpp so that the Objective-C++
// seam names nothing from the public capture contract and can stay a
// self-contained account of the HAL calls it makes.
//
// Three separate refusals collapse into kComFailure, deliberately: a tap the
// HAL declined, a consent the user denied, and a consent prompt that never
// appeared because the binary is unsigned are one OSStatus as far as
// AudioHardwareCreateProcessTap is concerned, so inventing three error codes
// would be claiming a distinction this backend cannot actually make.
[[nodiscard]] CaptureError refusal_for(coreaudio::TapStatus status) {
    switch (status) {
        case coreaudio::TapStatus::kOsTooOld: return CaptureError::kProcessLoopbackUnavailable;
        case coreaudio::TapStatus::kProcessNotFound: return CaptureError::kProcessNotFound;
        case coreaudio::TapStatus::kNoDefaultOutputDevice: return CaptureError::kDeviceNotFound;
        case coreaudio::TapStatus::kTapRefused: return CaptureError::kComFailure;
        case coreaudio::TapStatus::kAggregateRefused: return CaptureError::kComFailure;
        case coreaudio::TapStatus::kFormatUnreadable: return CaptureError::kFormatUnsupported;
        // Never asked: the one caller tests for kOk first. Listed so that a
        // status added later cannot slip through an inexhaustive switch.
        case coreaudio::TapStatus::kOk: return CaptureError::kComFailure;
    }
    return CaptureError::kComFailure;
}

}  // namespace

std::string_view describe(CaptureError error) {
    switch (error) {
        case CaptureError::kNoBackend: return "no capture backend on this platform";
        case CaptureError::kComFailure:
            return "a Core Audio HAL call failed; a process tap the system refused, or refused "
                   "consent for, arrives here too";
        case CaptureError::kDeviceNotFound:
            return "the requested capture device was not found, or a process tap has no default "
                   "output device to clock against";
        case CaptureError::kFormatUnsupported:
            return "the device offers no sample format this backend can read, or a process tap "
                   "cannot deliver the rate and channel count asked of it (a tap mixes down to "
                   "mono or stereo only, at the machine's own rate)";
        case CaptureError::kAlreadyRunning: return "capture is already running";
        case CaptureError::kProcessLoopbackUnavailable:
            // Whichever of the two gates turned the caller away, in that
            // gate's own words - see coreaudio_names.hpp, where both live
            // beside the argument for them. audio_backend()'s capability
            // reason calls the same function, so the two reports of this fact
            // cannot say different things.
            return coreaudio::system_audio_tap_refusal();
        case CaptureError::kProcessNotFound:
            return "no process has the requested id, or it has one and has never played audio: "
                   "Core Audio has an audio process object only for a process that has opened "
                   "an audio client, and a tap can only name a process object";
    }
    return "unknown capture error";
}

std::expected<std::vector<DeviceInfo>, CaptureError> enumerate_devices() {
    std::vector<DeviceInfo> devices;
    const auto default_id = coreaudio::device_uid(coreaudio::default_device(/*input=*/true));

    for (const auto device : coreaudio::device_list()) {
        const auto channels = coreaudio::channel_count(device, kAudioDevicePropertyScopeInput);
        if (channels == 0) {
            continue;  // output-only, or a dead/disconnected object
        }
        const std::string uid = coreaudio::device_uid(device);
        if (uid.empty()) {
            continue;  // nothing start() could ever open again by id
        }
        const std::string name = coreaudio::device_name(device);
        devices.push_back(DeviceInfo{
            .id = uid,
            .name = name.empty() ? coreaudio::fallback_name(uid) : name,
            .kind = DeviceKind::kInput,
            .sample_rate = static_cast<std::uint32_t>(coreaudio::nominal_sample_rate(device)),
            .channels = static_cast<std::uint16_t>(channels),
            .is_default = uid == default_id,
        });
    }
    return devices;
}

struct Capture::Impl {
    AudioObjectID device = kAudioObjectUnknown;
    AudioDeviceIOProcID io_proc_id = nullptr;
    // Empty for every capture except a process tap, where it holds the tap
    // and the private aggregate device `device` above is a copy of. Owned
    // from the moment start_process_loopback() succeeds; released by stop(),
    // which can call destroy_process_tap() unconditionally because it is a
    // no-op on this default-constructed value.
    coreaudio::ProcessTap tap;
    std::unique_ptr<RingBuffer> ring;
    coreaudio::SampleFormat format = coreaudio::SampleFormat::kFloat32;
    bool interleaved = true;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> frames_captured{0};
    std::atomic<std::uint64_t> frames_silence{0};
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    // Reused by the IOProc, which reaches it through `client_data` rather
    // than through a capture: AudioDeviceIOProc is a plain C function pointer
    // and a capturing closure cannot decay to one.
    std::vector<float> scratch;

    // The one callback both start paths register - a real input device's and
    // a process tap's aggregate device's, which deliver input buffers in
    // exactly the same shape and so need exactly the same reader.
    //
    // A static member of Impl rather than a free function in the anonymous
    // namespace above, for the reason the PipeWire backend's own Impl::on_*
    // members exist: `Impl` is private to Capture, so a free function has no
    // access to it at all, while a static member does - and a static member
    // function still converts to the plain function pointer
    // AudioDeviceCreateIOProcID wants, which is the other half of what this
    // has to be. It runs on the realtime thread the OS owns (see
    // coreaudio_support.hpp's "No worker thread"), which is what RingBuffer's
    // lock-free, allocation-free discipline is for.
    // The first parameter is spelled `object` rather than the HAL's own
    // `inDevice` because `device` is already a member of this struct, and one
    // name for two things in a function that reaches the struct through
    // `client_data` reads badly whichever way -Wshadow happens to feel about
    // it in a static member function.
    static OSStatus io_proc(AudioObjectID object, const AudioTimeStamp* now,
                            const AudioBufferList* input_data, const AudioTimeStamp* input_time,
                            AudioBufferList* output_data, const AudioTimeStamp* output_time,
                            void* client_data);
};

OSStatus Capture::Impl::io_proc(AudioObjectID /*object*/, const AudioTimeStamp* /*now*/,
                                const AudioBufferList* input_data,
                                const AudioTimeStamp* /*input_time*/,
                                AudioBufferList* /*output_data*/,
                                const AudioTimeStamp* /*output_time*/, void* client_data) {
    auto* impl = static_cast<Impl*>(client_data);
    collect(input_data, impl->channels, impl->interleaved, impl->format, impl->scratch);
    if (!impl->scratch.empty() && impl->channels > 0) {
        impl->ring->write(impl->scratch);
        impl->frames_captured.fetch_add(impl->scratch.size() / impl->channels,
                                        std::memory_order_relaxed);
    }
    return noErr;
}

Capture::Capture() : impl_(std::make_unique<Impl>()) {}

Capture::~Capture() {
    stop();
}

bool Capture::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

std::uint32_t Capture::sample_rate() const {
    return impl_->sample_rate;
}

std::uint16_t Capture::channels() const {
    return impl_->channels;
}

CaptureStats Capture::stats() const {
    return {.frames_captured = impl_->frames_captured.load(std::memory_order_relaxed),
            .frames_silence_filled = impl_->frames_silence.load(std::memory_order_relaxed),
            .frames_dropped = impl_->ring ? impl_->ring->dropped() /
                                                std::max<std::size_t>(impl_->channels, 1)
                                          : 0};
}

RingBuffer* Capture::buffer() {
    return impl_->ring.get();
}

void Capture::stop() {
    if (impl_->io_proc_id != nullptr) {
        AudioDeviceStop(impl_->device, impl_->io_proc_id);
        AudioDeviceDestroyIOProcID(impl_->device, impl_->io_proc_id);
        impl_->io_proc_id = nullptr;
    }
    // After the IOProc, never before: the aggregate device a tap capture runs
    // on is about to stop existing, and no callback may still be running
    // against it when it does. Unconditional and idempotent - a capture that
    // was never a tap holds a default-constructed ProcessTap, on which this
    // is a no-op, and one that was clears its ids here, so the destructor's
    // own stop() cannot destroy them a second time.
    coreaudio::destroy_process_tap(impl_->tap);
    // Cleared with them: for a tap capture this id WAS the aggregate device
    // destroy_process_tap() has just destroyed, and a destroyed object's id
    // is not something to leave sitting in a member that AudioDeviceStop is
    // called against. Costs the ordinary path nothing - start() writes it
    // before anything uses it.
    impl_->device = kAudioObjectUnknown;
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, CaptureError> Capture::start(const std::string& device_id, DeviceKind kind,
                                                 std::size_t ring_capacity_samples) {
    if (running()) {
        return std::unexpected(CaptureError::kAlreadyRunning);
    }

    AudioObjectID device = kAudioObjectUnknown;
    if (device_id.empty()) {
        if (kind == DeviceKind::kLoopback) {
            // No loopback endpoint is ever enumerated (see this file's own
            // header comment) and there is no "the default one" to fall
            // back to the way ALSA's `default` PCM or a WASAPI render
            // endpoint provide, so this is refused outright rather than
            // silently opening a microphone instead.
            return std::unexpected(CaptureError::kDeviceNotFound);
        }
        device = coreaudio::default_device(/*input=*/true);
    } else {
        // Handed to device_for_uid verbatim regardless of `kind`, the same
        // discipline platform/alsa/capture.cpp documents for its own
        // device_id: a caller naming a specific endpoint (a virtual
        // loopback driver's own input, say) is trusted to know what they
        // asked for.
        device = coreaudio::device_for_uid(device_id);
    }
    if (device == kAudioObjectUnknown) {
        return std::unexpected(CaptureError::kDeviceNotFound);
    }

    const auto asbd = coreaudio::get_property<AudioStreamBasicDescription>(
        device,
        coreaudio::address(kAudioDevicePropertyStreamFormat, kAudioDevicePropertyScopeInput));
    if (!asbd || asbd->mChannelsPerFrame == 0) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }
    const auto format = coreaudio::classify_pcm(*asbd);
    if (format == coreaudio::SampleFormat::kUnsupported) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }

    impl_->ring = std::make_unique<RingBuffer>(ring_capacity_samples);
    impl_->device = device;
    impl_->format = format;
    impl_->interleaved = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0;
    impl_->channels = static_cast<std::uint16_t>(asbd->mChannelsPerFrame);
    impl_->sample_rate = static_cast<std::uint32_t>(asbd->mSampleRate);
    impl_->frames_captured.store(0, std::memory_order_relaxed);
    impl_->frames_silence.store(0, std::memory_order_relaxed);
    impl_->scratch.reserve(static_cast<std::size_t>(impl_->channels) * 4096);

    // Impl::io_proc, shared with start_process_loopback() below - see its own
    // comment for why the reader is a static member of Impl.
    AudioDeviceIOProcID proc_id = nullptr;
    if (AudioDeviceCreateIOProcID(device, &Impl::io_proc, impl_.get(), &proc_id) != noErr ||
        proc_id == nullptr) {
        return std::unexpected(CaptureError::kComFailure);
    }
    if (AudioDeviceStart(device, proc_id) != noErr) {
        AudioDeviceDestroyIOProcID(device, proc_id);
        return std::unexpected(CaptureError::kComFailure);
    }

    impl_->io_proc_id = proc_id;
    impl_->running.store(true, std::memory_order_release);
    return {};
}

// Not a constant, and not a build-time answer: capture.hpp calls this the one
// report that depends on the machine the code is RUNNING on rather than the
// one it was compiled for, and on macOS that is the OS version. Which
// version, and why that one rather than the other figure in circulation, is
// argued once beside the gate itself - coreaudio_names.hpp's
// kSystemAudioTapMinimumOs.
bool process_loopback_available() {
    // Two gates, and the second one is not a version test: see
    // coreaudio_names.hpp's system_audio_tap_enabled() for the machine that
    // hung and why the default is off. Both are read here so that this
    // function stays the single answer capture.hpp promises it is.
    return coreaudio::system_audio_tap_api_available() && coreaudio::system_audio_tap_enabled();
}

std::expected<void, CaptureError> Capture::start_process_loopback(
    std::uint32_t process_id, ProcessLoopbackMode mode, ProcessLoopbackFormat format,
    std::size_t ring_capacity_samples) {
    if (running()) {
        return std::unexpected(CaptureError::kAlreadyRunning);
    }
    // Availability before the argument checks, the same order the Windows
    // backend takes and for the same reason: a machine that cannot do this at
    // all should say so whatever it was asked. It is also what
    // tests/audio/test_audio_backend.cpp's "process loopback refusals agree
    // with the reported capability" case depends on - the two have to agree
    // on which of kProcessNotFound and kProcessLoopbackUnavailable comes
    // back. Since 2026-09-06 the macOS CI legs take the unavailable branch,
    // because the second gate is off by default (coreaudio_names.hpp's
    // system_audio_tap_enabled(), and the hang that put it there).
    if (!process_loopback_available()) {
        return std::unexpected(CaptureError::kProcessLoopbackUnavailable);
    }
    if (process_id == 0) {
        // Refused here rather than by the HAL, matching the Windows backend's
        // own reasoning: no process has id 0, and a tap that is allowed to
        // name one activates and delivers zeros for ever instead of failing.
        return std::unexpected(CaptureError::kProcessNotFound);
    }
    // One or two channels, nothing else - see this file's header comment on
    // what a CATapDescription's mixdown descriptions actually offer. Checked
    // before the tap is created so that a caller asking for eight is turned
    // away without a consent prompt having been raised for a capture that
    // could not have been delivered.
    if (format.sample_rate == 0 || (format.channels != 1 && format.channels != 2)) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }

    coreaudio::ProcessTap tap;
    const auto status = coreaudio::create_process_tap(
        process_id,
        format.channels == 1 ? coreaudio::TapMixdown::kMono : coreaudio::TapMixdown::kStereo,
        mode == ProcessLoopbackMode::kIncludeProcessTree
            ? coreaudio::TapScope::kOnlyListedProcess
            : coreaudio::TapScope::kEverythingExceptListedProcess,
        tap);
    if (status != coreaudio::TapStatus::kOk) {
        // create_process_tap() destroys whatever it had already built on
        // every failing path, so there is nothing to release here.
        return std::unexpected(refusal_for(status));
    }

    // What the tap says it will deliver, against what the caller asked for.
    // The channel count follows from the mixdown chosen above and should
    // agree; the rate is the machine's and may not, and capture.hpp's "at
    // exactly `format`" is the promise being kept by refusing rather than
    // resampling.
    const auto sample_format = coreaudio::classify_pcm(tap.format);
    const auto tap_channels = static_cast<std::uint16_t>(tap.format.mChannelsPerFrame);
    const auto tap_rate = static_cast<std::uint32_t>(tap.format.mSampleRate);
    if (sample_format == coreaudio::SampleFormat::kUnsupported ||
        tap_channels != format.channels || tap_rate != format.sample_rate) {
        coreaudio::destroy_process_tap(tap);
        return std::unexpected(CaptureError::kFormatUnsupported);
    }

    impl_->ring = std::make_unique<RingBuffer>(ring_capacity_samples);
    impl_->format = sample_format;
    impl_->interleaved = (tap.format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0;
    impl_->channels = tap_channels;
    impl_->sample_rate = tap_rate;
    impl_->frames_captured.store(0, std::memory_order_relaxed);
    impl_->frames_silence.store(0, std::memory_order_relaxed);
    impl_->scratch.reserve(static_cast<std::size_t>(impl_->channels) * 4096);

    AudioDeviceIOProcID proc_id = nullptr;
    if (AudioDeviceCreateIOProcID(tap.aggregate_device, &Impl::io_proc, impl_.get(), &proc_id) !=
            noErr ||
        proc_id == nullptr) {
        coreaudio::destroy_process_tap(tap);
        return std::unexpected(CaptureError::kComFailure);
    }
    if (AudioDeviceStart(tap.aggregate_device, proc_id) != noErr) {
        AudioDeviceDestroyIOProcID(tap.aggregate_device, proc_id);
        coreaudio::destroy_process_tap(tap);
        return std::unexpected(CaptureError::kComFailure);
    }

    // Ownership of both objects moves into Impl here, and only here: every
    // path above this line either destroyed them or never created them, and
    // from this line on stop() is the only thing that releases them. The
    // local keeps its ids, which is why nothing below it may destroy the tap.
    // `device` is written here for the same reason rather than earlier - it
    // is what stop() calls AudioDeviceStop against, and a failed start must
    // not leave a destroyed aggregate device's id sitting in it.
    impl_->tap = tap;
    impl_->device = tap.aggregate_device;
    impl_->io_proc_id = proc_id;
    impl_->running.store(true, std::memory_order_release);
    return {};
}

}  // namespace ac3::audio
