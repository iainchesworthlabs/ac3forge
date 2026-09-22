#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/playback_counter.hpp"

// The PipeWire passthrough backend. CMake compiles this directory's
// passthrough.cpp on a Linux host that selected pipewire/ over alsa/ (see
// src/audio/CMakeLists.txt) and another directory's everywhere else, so
// there is no #ifdef - the file's path is what says "PipeWire".
//
// ---------------------------------------------------------------------------
// PipeWire's native compressed-format path is real, and this is it
// ---------------------------------------------------------------------------
// PipeWire's own SPA headers (spa/param/audio/iec958.h, current as of
// libpipewire-0.3 1.6.x) define exactly what this backend needs:
// `enum spa_audio_iec958_codec` with SPA_AUDIO_IEC958_CODEC_AC3 and _EAC3
// members, `struct spa_audio_info_iec958 { codec; flags; rate; }`, and
// spa_format_audio_iec958_build()/_parse() to turn one into a
// SPA_MEDIA_SUBTYPE_iec958 format POD. Connecting with PW_STREAM_FLAG_
// EXCLUSIVE and that POD is a real, shipped mechanism - not aspirational
// API surface - confirmed against Kodi's own PipeWire passthrough
// implementation (xbmc PR #22560), which negotiates exactly this way and
// still hands PipeWire pre-packed IEC 61937 burst bytes as opaque S16
// carrier data, the same shape ac3::iec61937 already produces for the ALSA
// backend.
//
// ---------------------------------------------------------------------------
// The real gap, and why enumeration has to probe rather than ask
// ---------------------------------------------------------------------------
// Whether the format POD above is ACCEPTED is a per-node, per-deployment
// question this library cannot answer by reading a static property: an
// ALSA-backed PipeWire sink only offers a compressed codec once its
// `iec958Codecs` control has been explicitly populated, which is session-
// manager configuration (a WirePlumber ALSA-monitor rule, or a one-off
// `pw-cli s <id> Props '{ iec958Codecs: [ AC3 EAC3 ] }'`) that this library
// has no portable way to set on the user's behalf - unlike ALSA's own
// `iec958:CARD=...` device names, which just work unconditionally the
// moment the hardware exists. So on a stock desktop where nobody has ever
// touched that WirePlumber setting, every PipeWire sink genuinely, honestly
// offers no compressed codec at all, and this backend has no way to turn
// that on. That is the real, current shape of the gap, not an excuse for an
// incomplete implementation: enumerate_render_devices() below does not
// pretend to know the answer by inspecting a property (this library never
// found a documented, stable one to read) - it asks, the same way ALSA's own
// enumerate_render_devices() has no IsFormatSupported() and has to open each
// candidate to find out. The cost is the same kind ALSA already accepts:
// probing is intrusive (it briefly takes each candidate exclusively) and
// slower than a property read would be.
//
// Underneath, a PipeWire sink that DOES accept the format still reaches the
// hardware through the very same ALSA `iec958:`/`hdmi:` device the alsa/
// backend would open directly - see docs/building.md's "Why ALSA and not
// PipeWire" section for why ALSA stays the preferred backend when both are
// available, and CMakeLists.txt for the precedence itself.

#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ac3/audio/ring_buffer.hpp"
#include "ac3/audio/speakers.hpp"
#include "ac3/iec61937/iec61937.hpp"
#include "pipewire_support.hpp"

namespace ac3::audio {

namespace {

using ac3::pipewire::Stream;
using ac3::pipewire::ThreadLoop;

// The carrier is a 2-channel 16-bit stream whatever rides inside it - see
// ac3::pipewire::kCarrierFrameBytes and the ALSA backend's identical
// constant and comment.
constexpr std::size_t kCarrierFrameBytes = ac3::pipewire::kCarrierFrameBytes;

// A real connection attempt (start()) is allowed longer to settle than a
// probe made purely to answer an enumerate_render_devices() question -
// enumeration may run several of these per candidate output.
constexpr int kConnectTimeoutSeconds = 5;
constexpr int kProbeTimeoutSeconds = 2;

std::size_t burst_bytes_for(BitstreamFormat format) {
    return format == BitstreamFormat::kEac3 ? iec61937::kEac3BurstBytes : iec61937::kBurstBytes;
}

enum class ConnectState : int { kPending, kReady, kError };

bool wait_for_connect(pw_thread_loop* loop, std::atomic<ConnectState>& state, int timeout_seconds) {
    for (int waited = 0; waited < timeout_seconds; ++waited) {
        if (state.load(std::memory_order_acquire) != ConnectState::kPending) {
            break;
        }
        pw_thread_loop_timed_wait(loop, 1);
    }
    return state.load(std::memory_order_acquire) == ConnectState::kReady;
}

// The probe-only state callback: enumeration and the empty-device_id
// candidate walk in start() both only need "did it settle", not the full
// process()-wiring PassthroughSink::Impl carries. What it does need beside
// the answer is the loop to wake, or wait_for_connect() below sleeps out its
// whole one-second step for an answer that is already there - see
// capture.cpp's note and the measurement there.
struct ProbeState {
    std::atomic<ConnectState> connect_state{ConnectState::kPending};
    pw_thread_loop* loop = nullptr;
};

void on_probe_state_changed(void* data, pw_stream_state /*old_state*/, pw_stream_state state,
                             const char* /*error*/) {
    auto& probe = *static_cast<ProbeState*>(data);
    if (state == PW_STREAM_STATE_STREAMING || state == PW_STREAM_STATE_PAUSED) {
        auto expected = ConnectState::kPending;
        probe.connect_state.compare_exchange_strong(expected, ConnectState::kReady);
    } else if (state == PW_STREAM_STATE_ERROR) {
        probe.connect_state.store(ConnectState::kError, std::memory_order_release);
    } else {
        return;  // still on its way; nobody is waiting for this one
    }
    if (probe.loop != nullptr) {
        pw_thread_loop_signal(probe.loop, false);
    }
}

const pw_stream_events& probe_events() {
    static const pw_stream_events events = [] {
        pw_stream_events value{};
        value.version = PW_VERSION_STREAM_EVENTS;
        value.state_changed = on_probe_state_changed;
        return value;
    }();
    return events;
}

// Attempts a real, exclusive connection to `node_id` with `params` and tears
// it down immediately whatever the outcome - see this file's header comment
// for why a real connection attempt, not a property read, is the only
// honest way to answer "will this output take this format". Shared by
// enumerate_render_devices()'s three probes per candidate and start()'s
// empty-device_id candidate walk, which is exactly the same question asked
// with a different format POD each time.
bool probe_connect(const std::string& node_id, const spa_pod** params, std::uint32_t n_params,
                    int timeout_seconds) {
    ThreadLoop loop{pw_thread_loop_new("ac3audio-probe", nullptr)};
    if (!loop || pw_thread_loop_start(loop.get()) < 0) {
        return false;
    }

    pw_thread_loop_lock(loop.get());
    ProbeState probe{.connect_state = ConnectState::kPending, .loop = loop.get()};

    pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                           PW_KEY_MEDIA_ROLE, "Production", PW_KEY_TARGET_OBJECT, node_id.c_str(),
                           nullptr);

    Stream stream{pw_stream_new_simple(pw_thread_loop_get_loop(loop.get()), "ac3forge probe",
                                        props, &probe_events(), &probe)};
    if (!stream) {
        pw_thread_loop_unlock(loop.get());
        pw_thread_loop_stop(loop.get());
        return false;
    }

    const auto flags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_EXCLUSIVE | PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
    if (pw_stream_connect(stream.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, n_params) <
        0) {
        pw_thread_loop_unlock(loop.get());
        pw_thread_loop_stop(loop.get());
        pw_thread_loop_lock(loop.get());
        stream.reset();
        pw_thread_loop_unlock(loop.get());
        return false;
    }

    const bool ready = wait_for_connect(loop.get(), probe.connect_state, timeout_seconds);
    pw_thread_loop_unlock(loop.get());
    // Stop before destroying, not after. Every successful probe reached this
    // line, and on the first machine with a real PipeWire session it
    // deadlocked here: pw_stream_destroy() ran from the wrong context, and
    // the pw_thread_loop_stop() that followed never returned.
    pw_thread_loop_stop(loop.get());
    pw_thread_loop_lock(loop.get());
    stream.reset();
    pw_thread_loop_unlock(loop.get());
    return ready;
}

const spa_pod* build_iec958_pod(spa_pod_builder& builder, BitstreamFormat format,
                                 std::uint32_t carrier) {
    spa_audio_info_iec958 info{};
    info.codec = ac3::pipewire::iec958_codec_for(format);
    info.rate = carrier;
    return spa_format_audio_iec958_build(&builder, SPA_PARAM_EnumFormat, &info);
}

bool probe_iec958(const std::string& node_id, BitstreamFormat format, std::uint32_t sample_rate) {
    std::array<std::uint8_t, 1024> pod_buffer{};
    spa_pod_builder builder{};
    // The function, not the SPA_POD_BUILDER_INIT macro - see capture.cpp's
    // identical call for why.
    spa_pod_builder_init(&builder, pod_buffer.data(), static_cast<std::uint32_t>(pod_buffer.size()));
    const spa_pod* param =
        build_iec958_pod(builder, format, ac3::pipewire::carrier_rate(format, sample_rate));
    const spa_pod* params[1] = {param};
    return probe_connect(node_id, params, 1, kProbeTimeoutSeconds);
}

// The PipeWire analogue of ALSA's supports_exclusive_pcm control probe:
// PipeWire exposes no separate "raw hardware, no plugin chain" name the way
// ALSA's hw: does, so this asks the SAME node whether it will take plain
// stereo PCM exclusively, which still separates the two reasons passthrough
// can fail: a node that refuses even this has exclusive mode unavailable
// (in use, or disabled), while one that takes this but not either codec
// above simply has no compressed codec enabled.
bool probe_exclusive_pcm(const std::string& node_id, std::uint32_t sample_rate) {
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_S16_LE;
    info.channels = 2;
    info.rate = sample_rate;

    std::array<std::uint8_t, 1024> pod_buffer{};
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, pod_buffer.data(), static_cast<std::uint32_t>(pod_buffer.size()));
    const spa_pod* param = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
    const spa_pod* params[1] = {param};
    return probe_connect(node_id, params, 1, kProbeTimeoutSeconds);
}

// A sink, with the compressed codecs the session manager has enabled on it.
// `iec958.codecs` is what WirePlumber writes onto a node after reading the
// display's ELD - `["PCM","AC3","EAC3",...]` - and it is the only honest
// answer to "can this sink carry a bitstream". Absent or empty, it cannot.
struct CandidateSink {
    std::string id;
    std::string name;
    bool codec_ac3 = false;
    bool codec_eac3 = false;
    std::uint16_t channels = 0;
    std::uint32_t speakers = 0;
    std::uint32_t rate = 0;
};

// The SPEAKER_* bit one SPA channel name stands for (ac3::audio::speakers.hpp).
// These are the names audio.position carries, and the same ones ALSA's channel
// maps use; 0 for a name with no WAVEFORMATEXTENSIBLE position, including SPA's
// "NA" for a channel to leave alone, "MONO" and anything unrecognised.
std::uint32_t speaker_of_name(std::string_view name) {
    struct Named {
        std::string_view name;
        std::uint32_t speaker;
    };
    static constexpr std::array<Named, 18> kNames{{
        {"FL", kSpeakerFrontLeft},
        {"FR", kSpeakerFrontRight},
        {"FC", kSpeakerFrontCentre},
        {"LFE", kSpeakerLowFrequency},
        {"RL", kSpeakerBackLeft},
        {"RR", kSpeakerBackRight},
        {"FLC", kSpeakerFrontLeftOfCentre},
        {"FRC", kSpeakerFrontRightOfCentre},
        {"RC", kSpeakerBackCentre},
        {"SL", kSpeakerSideLeft},
        {"SR", kSpeakerSideRight},
        {"TC", kSpeakerTopCentre},
        {"TFL", kSpeakerTopFrontLeft},
        {"TFC", kSpeakerTopFrontCentre},
        {"TFR", kSpeakerTopFrontRight},
        {"TRL", kSpeakerTopBackLeft},
        {"TRC", kSpeakerTopBackCentre},
        {"TRR", kSpeakerTopBackRight},
    }};
    const auto found = std::find_if(kNames.begin(), kNames.end(),
                                    [&](const Named& entry) { return entry.name == name; });
    return found == kNames.end() ? 0 : found->speaker;
}

// The mask an audio.position list names. The list is SPA's own spelling, which
// is either a JSON array ("[ FL FR FC LFE ]") or a bare comma-separated list
// ("FL,FR"), so the separators are everything that is not a name character.
// 0 when the names do not account for every channel: a partly understood map
// would put audio at the wrong speaker, which is worse than saying nothing.
std::uint32_t speakers_of_position(std::string_view position, std::uint16_t channels) {
    std::uint32_t mask = 0;
    std::uint16_t named = 0;
    std::size_t at = 0;
    while (at < position.size()) {
        const std::size_t start = position.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", at);
        if (start == std::string_view::npos) {
            break;
        }
        const std::size_t stop =
            position.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", start);
        const std::size_t end = stop == std::string_view::npos ? position.size() : stop;
        const std::uint32_t speaker = speaker_of_name(position.substr(start, end - start));
        if (speaker == 0) {
            return 0;
        }
        mask |= speaker;
        ++named;
        at = end;
    }
    return named == channels && speaker_count(mask) == channels ? mask : 0;
}

std::vector<CandidateSink> candidate_sinks() {
    std::vector<CandidateSink> candidates;
    for (auto& sink : ac3::pipewire::audio_sinks_with_info()) {
        if (sink.name.empty()) {
            continue;
        }
        CandidateSink candidate{.id = std::move(sink.name), .name = std::move(sink.description)};
        candidate.codec_ac3 = ac3::pipewire::codec_listed(sink.codecs, "AC3");
        candidate.codec_eac3 = ac3::pipewire::codec_listed(sink.codecs, "EAC3");
        candidate.channels = sink.channels;
        candidate.speakers = speakers_of_position(sink.position, sink.channels);
        candidate.rate = sink.rate;
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

}  // namespace

std::string_view describe(PassthroughError error) {
    switch (error) {
        case PassthroughError::kNoBackend: return "no passthrough backend on this platform";
        case PassthroughError::kComFailure: return "a PipeWire call failed";
        case PassthroughError::kDeviceNotFound:
            return "no such output: either the named PipeWire node does not exist, or none was "
                   "named and this machine has no Audio/Sink node at all";
        case PassthroughError::kFormatRejected:
            return "no output accepted this bitstream over IEC 61937 - most PipeWire sinks have "
                   "no compressed codec enabled by default; see this file's own header comment "
                   "for the `iec958Codecs` session-manager setting that turns one on";
        case PassthroughError::kExclusiveUnavailable:
            return "the node could not be reached exclusively (another client holds it, or this "
                   "user lacks permission on the underlying device)";
        case PassthroughError::kAlreadyRunning: return "passthrough is already running";
        case PassthroughError::kNotRunning: return "passthrough is not running";
    }
    return "unknown passthrough error";
}

std::expected<std::vector<RenderDeviceInfo>, PassthroughError> enumerate_render_devices(
    std::uint32_t sample_rate) {
    std::vector<RenderDeviceInfo> devices;

    for (const auto& sink : candidate_sinks()) {
        // The connect probe alone is not enough, and the first machine with a
        // real session showed why: PipeWire's adapter accepted an IEC 958
        // AC-3 stream on the analogue headphone jack, which cannot carry a
        // bitstream, and reported it as a passthrough-capable output. The
        // adapter accepts the format and would render the bursts as PCM
        // noise. So the codec has to be enabled on the node first - that is
        // the session manager's judgement, from the sink's own ELD - and
        // only then is the connect worth asking. A sink with no
        // iec958.codecs at all is a plain PCM output and is never probed.
        RenderDeviceInfo info{
            .id = sink.id,
            .name = sink.name,
            .is_default = false,
            .supports_ac3_passthrough =
                sink.codec_ac3 && probe_iec958(sink.id, BitstreamFormat::kAc3, sample_rate),
            .supports_eac3_passthrough =
                sink.codec_eac3 && probe_iec958(sink.id, BitstreamFormat::kEac3, sample_rate),
            .supports_exclusive_pcm = probe_exclusive_pcm(sink.id, sample_rate),
            // What the node is configured as, which for a device sink is what
            // the device renders. The rate is the one rate it is running at,
            // not a list: PipeWire keeps the rates it may switch between in
            // the session manager's settings, not on the node, so the honest
            // answer here is the rate this sink has now (see
            // RenderDeviceInfo::sample_rates).
            .channels = sink.channels,
            .speakers = sink.speakers,
            .sample_rates = sink.rate != 0 ? std::vector<std::uint32_t>{sink.rate}
                                           : std::vector<std::uint32_t>{},
        };
        devices.push_back(std::move(info));
    }

    // No per-node "this is the default" metadata is read here - the same
    // fallback ac3::audio's PipeWire enumeration and ALSA's own passthrough
    // enumeration both use when they cannot resolve one either.
    if (!devices.empty()) {
        devices.front().is_default = true;
    }

    return devices;
}

// Impl is private (see capture.cpp's identical note on why its callbacks are
// members of Impl rather than free functions - the same reasoning applies
// here).
struct PassthroughSink::Impl {
    ThreadLoop loop;
    Stream stream;
    std::unique_ptr<ByteRingBuffer> queue;
    std::size_t burst_bytes = iec61937::kBurstBytes;
    // Raised by start(). Lowered by stop(), or by state_changed() when the
    // stream ends on its own.
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::size_t rendered_bytes = 0;  // process() only: the partial burst carried over
    std::atomic<std::uint64_t> underruns{0};
    // Link frames to a content frame (carrier_ratio()), and link frames a
    // second, for position().
    std::uint32_t ratio = 1;
    std::uint32_t carrier_rate = 0;
    // As MonitorSink's PipeWire backend keeps them, in link frames: the
    // frames the process callback has handed the stream since the last start
    // or flush, against those it has not had heard, for position().
    PlaybackCounter counter;
    std::uint64_t handed_over = 0;
    // As there too: set by pause() before the stream is made inactive and
    // cleared by resume() before it is made active again, with the calls
    // under way counted, so that while paused the queue's read side and the
    // counts are the caller's.
    std::atomic_bool paused{false};
    std::atomic<int> in_process{0};
    // A flush the process callback performs, since it runs on the graph's
    // data thread, which pw_thread_loop_lock does not exclude (see
    // MonitorSink's PipeWire backend). `flushes` counts the ones it has done,
    // and `flush_mark` is how far the queue had been written when the flush
    // was asked for: what it drops.
    std::atomic_bool flushing{false};
    std::atomic<std::uint64_t> flushes{0};
    std::atomic<std::size_t> flush_mark{0};

    // See capture.cpp's Impl for the locking discipline this shares.
    std::atomic<ConnectState> connect_state{ConnectState::kPending};

    static void state_changed(void* data, pw_stream_state /*old_state*/, pw_stream_state state,
                               const char* /*error*/) {
        auto& impl = *static_cast<Impl*>(data);
        if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
            // The stream's own end - its node failed or was removed with the
            // device behind it, or the daemon went - as MonitorSink's
            // PipeWire backend reads it, with the same ordering against
            // start() and stop(). A bitstream the session manager holds
            // unlinked until its sink comes back is only PAUSED, and not an
            // end.
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
        // As MonitorSink's PipeWire backend: counted before `paused` is read,
        // as pause() sets `paused` before it reads the count.
        impl.in_process.fetch_add(1);
        if (!impl.paused.load()) {
            fill(impl);
        }
        impl.in_process.fetch_sub(1);
    }

    static void fill(Impl& impl) {
        if (!impl.queue) {
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

        // A flush the caller asked for, done before this buffer is filled, so
        // the buffer goes out from the emptied queue and the counts restart
        // with it.
        if (impl.flushing.exchange(false, std::memory_order_acq_rel)) {
            impl.queue->discard_to(impl.flush_mark.load(std::memory_order_acquire));
            impl.handed_over = 0;
            impl.counter.restart();
            impl.rendered_bytes = 0;
            impl.rendered.store(0, std::memory_order_relaxed);
            impl.submitted.store(0, std::memory_order_relaxed);
            impl.flushes.fetch_add(1, std::memory_order_release);
        }

        const std::uint32_t requested =
            buffer->requested > 0 ? static_cast<std::uint32_t>(buffer->requested) *
                                         static_cast<std::uint32_t>(kCarrierFrameBytes)
                                   : spa_buf->datas[0].maxsize;
        const std::size_t byte_count = std::min<std::size_t>(requested, spa_buf->datas[0].maxsize);

        auto* out = static_cast<std::byte*>(spa_buf->datas[0].data);
        const auto got = impl.queue->read(std::span{out, byte_count});
        if (got < byte_count) {
            // A gap on the wire, exactly as underrun handling is described
            // in ac3::audio::PassthroughStats's own documentation - counted,
            // not hidden, matching every other backend's discipline.
            std::fill(out + got, out + byte_count, std::byte{0});
            impl.underruns.fetch_add(1, std::memory_order_relaxed);
        }
        // A callback usually asks for less than one burst, so counting
        // got / burst_bytes per callback rounds to zero every time and the
        // stat read 0 through a run that plainly played. Bytes accumulate
        // and a burst is counted each time a whole one has gone out.
        impl.rendered_bytes += got;
        while (impl.rendered_bytes >= impl.burst_bytes) {
            impl.rendered_bytes -= impl.burst_bytes;
            impl.rendered.fetch_add(1, std::memory_order_relaxed);
        }

        spa_buf->datas[0].chunk->offset = 0;
        spa_buf->datas[0].chunk->stride = static_cast<std::int32_t>(kCarrierFrameBytes);
        spa_buf->datas[0].chunk->size = static_cast<std::uint32_t>(byte_count);
        // In link frames, which is then what pw_time.queued counts.
        const std::uint64_t frames = byte_count / kCarrierFrameBytes;
        buffer->size = frames;

        pw_stream_queue_buffer(impl.stream.get(), buffer);
        impl.handed_over += frames;

        // What of it all has been heard: everything handed over, less what
        // the stream and the graph still hold.
        pw_time time{};
        if (pw_stream_get_time_n(impl.stream.get(), &time, sizeof(time)) == 0) {
            impl.counter.report(impl.handed_over,
                                ac3::pipewire::unplayed_frames(time, impl.carrier_rate));
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

PassthroughSink::PassthroughSink() : impl_(std::make_unique<Impl>()) {}

PassthroughSink::~PassthroughSink() {
    stop();
}

bool PassthroughSink::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

PassthroughStats PassthroughSink::stats() const {
    return {.bursts_submitted = impl_->submitted.load(std::memory_order_relaxed),
            .bursts_rendered = impl_->rendered.load(std::memory_order_relaxed),
            .underruns = impl_->underruns.load(std::memory_order_relaxed)};
}

std::optional<MonitorPosition> PassthroughSink::position() const {
    if (!running() || !impl_->queue) {
        return std::nullopt;
    }
    const std::uint64_t queued_here = impl_->queue->available() / kCarrierFrameBytes;
    // pw_time's delay already counts the whole path to the device, as its
    // queue; there is no latency left to add.
    return per_content_frame(impl_->counter.position(queued_here, /*latency=*/0), impl_->ratio);
}

void PassthroughSink::flush() {
    if (!running() || !impl_->loop || !impl_->stream) {
        return;
    }
    // As MonitorSink's PipeWire backend: pw_stream_flush() from the loop's
    // context, and the queue handed to the process callback to drop up to the
    // mark - or dropped here while paused, when the callback has stopped
    // touching it.
    pw_thread_loop_lock(impl_->loop.get());
    pw_stream_flush(impl_->stream.get(), false);
    pw_thread_loop_unlock(impl_->loop.get());

    if (impl_->paused.load()) {
        impl_->flushing.store(false, std::memory_order_release);
        impl_->queue->discard_to(impl_->queue->write_mark());
        impl_->handed_over = 0;
        impl_->rendered_bytes = 0;
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
    // Not reached in time: left for the next callback, and dropping only what
    // came before the mark. A stream that has ended makes no callback, and
    // its lowered `running` ends the wait above at once.
}

std::expected<void, PassthroughError> PassthroughSink::pause() {
    if (!running() || !impl_->loop || !impl_->stream) {
        return std::unexpected(PassthroughError::kNotRunning);
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
        return std::unexpected(PassthroughError::kComFailure);
    }
    // A callback already past its check of `paused` finishes first.
    for (int waited = 0; waited < 200 && impl_->in_process.load() != 0; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return {};
}

std::expected<void, PassthroughError> PassthroughSink::resume() {
    if (!running() || !impl_->loop || !impl_->stream) {
        return std::unexpected(PassthroughError::kNotRunning);
    }
    if (!impl_->paused.load()) {
        return {};
    }
    impl_->paused.store(false);
    pw_thread_loop_lock(impl_->loop.get());
    const int result = pw_stream_set_active(impl_->stream.get(), true);
    pw_thread_loop_unlock(impl_->loop.get());
    if (result < 0) {
        impl_->paused.store(true);
        return std::unexpected(PassthroughError::kComFailure);
    }
    return {};
}

bool PassthroughSink::paused() const {
    // What was asked for, as MonitorSink's PipeWire backend reports it.
    return running() && impl_->paused.load();
}

bool PassthroughSink::can_submit() const {
    if (!running() || !impl_->queue) {
        return false;
    }
    return impl_->queue->capacity() - impl_->queue->available() > impl_->burst_bytes;
}

bool PassthroughSink::submit(std::span<const std::byte> burst) {
    if (!running() || !impl_->queue || burst.size() != impl_->burst_bytes) {
        return false;
    }
    if (!can_submit()) {
        return false;
    }
    const auto wrote = impl_->queue->write(burst);
    if (wrote != burst.size()) {
        return false;
    }
    impl_->submitted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PassthroughSink::stop() {
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

std::expected<void, PassthroughError> PassthroughSink::start(const std::string& device_id,
                                                              std::uint32_t sample_rate,
                                                              BitstreamFormat format_kind) {
    if (running()) {
        return std::unexpected(PassthroughError::kAlreadyRunning);
    }
    // A stream that ended on its own still has its loop and its stream, and
    // still holds its sink exclusively; stop() tears both down in the right
    // order. With nothing started it does nothing.
    stop();

    ac3::pipewire::ensure_initialized();

    // Pick the target before touching anything, exactly as the ALSA backend
    // does: an empty id means "the default output", which for a bitstream
    // can only mean a sink that has already said yes - see this file's
    // header comment for why that has to be a real connection attempt.
    std::string target = device_id;
    if (target.empty()) {
        const auto candidates = candidate_sinks();
        if (candidates.empty()) {
            return std::unexpected(PassthroughError::kDeviceNotFound);
        }
        // The same gate enumerate_render_devices() applies, for the same
        // reason: the connect alone said yes on the headphone jack. A sink
        // whose session manager has not enabled this codec is not asked.
        for (const auto& sink : candidates) {
            const bool enabled = format_kind == BitstreamFormat::kEac3 ? sink.codec_eac3
                                                                        : sink.codec_ac3;
            if (enabled && probe_iec958(sink.id, format_kind, sample_rate)) {
                target = sink.id;
                break;
            }
        }
        if (target.empty()) {
            return std::unexpected(PassthroughError::kFormatRejected);
        }
    }

    const std::uint32_t carrier = ac3::pipewire::carrier_rate(format_kind, sample_rate);
    const std::size_t burst_bytes = burst_bytes_for(format_kind);

    impl_->loop = ThreadLoop{pw_thread_loop_new("ac3audio-passthrough", nullptr)};
    if (!impl_->loop) {
        return std::unexpected(PassthroughError::kComFailure);
    }
    if (pw_thread_loop_start(impl_->loop.get()) < 0) {
        impl_->loop.reset();
        return std::unexpected(PassthroughError::kComFailure);
    }

    pw_thread_loop_lock(impl_->loop.get());

    // Everything Impl::process()/Impl::state_changed() might touch is set up
    // before pw_stream_connect() below - see capture.cpp's start() for why
    // that ordering is what keeps this race-free.
    impl_->queue = std::make_unique<ByteRingBuffer>(burst_bytes * 40);
    impl_->burst_bytes = burst_bytes;
    impl_->ratio = carrier_ratio(format_kind);
    impl_->carrier_rate = carrier;
    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered.store(0, std::memory_order_relaxed);
    impl_->rendered_bytes = 0;
    impl_->underruns.store(0, std::memory_order_relaxed);
    impl_->counter.restart();
    impl_->handed_over = 0;
    impl_->paused.store(false);
    impl_->flushing.store(false, std::memory_order_relaxed);
    impl_->flushes.store(0, std::memory_order_relaxed);
    impl_->connect_state.store(ConnectState::kPending, std::memory_order_relaxed);

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE,
        "Production", PW_KEY_TARGET_OBJECT, target.c_str(), nullptr);

    Stream stream{pw_stream_new_simple(pw_thread_loop_get_loop(impl_->loop.get()),
                                        "ac3forge passthrough", props, &Impl::stream_events(),
                                        impl_.get())};
    if (!stream) {
        pw_thread_loop_unlock(impl_->loop.get());
        impl_->queue.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        return std::unexpected(PassthroughError::kComFailure);
    }

    std::array<std::uint8_t, 1024> pod_buffer{};
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, pod_buffer.data(), static_cast<std::uint32_t>(pod_buffer.size()));
    const spa_pod* param = build_iec958_pod(builder, format_kind, carrier);
    const spa_pod* params[1] = {param};

    const auto flags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_EXCLUSIVE | PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS);
    if (pw_stream_connect(stream.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) {
        pw_thread_loop_unlock(impl_->loop.get());
        stream.reset();
        impl_->queue.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        return std::unexpected(PassthroughError::kComFailure);
    }

    impl_->stream = std::move(stream);
    const bool ready =
        wait_for_connect(impl_->loop.get(), impl_->connect_state, kConnectTimeoutSeconds);

    if (!ready) {
        pw_thread_loop_unlock(impl_->loop.get());
        impl_->stream.reset();
        impl_->queue.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        // PipeWire reports a failed negotiation as a state plus a human-
        // readable string, not an errno the way ALSA does (see this file's
        // header comment), so - unlike ac3::alsa::open_failure() - this
        // cannot reliably tell "codec not enabled" apart from "node busy"
        // from that string alone. kFormatRejected is the more common real
        // cause (see the header comment) and the more actionable one to
        // report by default.
        return std::unexpected(PassthroughError::kFormatRejected);
    }

    // Raised before the loop is unlocked, as MonitorSink's PipeWire backend
    // raises it, so an end of the stream cannot fall between the two.
    impl_->running.store(true, std::memory_order_release);
    pw_thread_loop_unlock(impl_->loop.get());
    return {};
}

}  // namespace ac3::audio
