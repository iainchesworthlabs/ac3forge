#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Shared-mode PCM playback: a sanity-check/preview path that decodes what is
// being encoded and plays it back through an ordinary output, so a live
// capture->encode session can be listened to before (or instead of) IEC 61937
// hardware passthrough (ac3::audio::PassthroughSink).
//
// Unlike PassthroughSink this deliberately does NOT use exclusive mode: the
// audio engine is free to resample, mix and volume-scale, which is exactly
// what a preview wants (it shares the device with everything else on the
// machine) and is the opposite of what a bitstreamed IEC 61937 burst can
// tolerate. There is no format negotiation dance to speak of - shared mode
// adapts to whatever the caller asks for.

namespace ac3::audio {

enum class MonitorError : std::uint8_t {
    kNoBackend,       // built without a platform monitor backend
    kComFailure,      // a Windows audio (WASAPI/COM) call failed
    kDeviceNotFound,
    kAlreadyRunning,
    kNotRunning,
};

[[nodiscard]] std::string_view describe(MonitorError error);

struct MonitorStats {
    std::uint64_t frames_submitted = 0;  // sample-frames (one per channel-set), not bytes
    std::uint64_t frames_rendered = 0;
    // Render periods that found the queue empty. Audible as a click or a gap,
    // not silent failure - counted rather than hidden, matching
    // PassthroughSink's underrun discipline.
    std::uint64_t underruns = 0;
};

// Where the device has got to in what it has been given, as it says rather
// than as the submitting side counts: what a meter released at play time needs
// (planning/hearth-reference-player.md, Monitor), and what tells a caller
// running slightly ahead of real time how far ahead it is.
//
// Every figure is in sample-frames and counts from the last start() or
// flush(); a frame submitted and dropped by flush() is in none of them.
struct MonitorPosition {
    // Frames the device itself has played, from its own clock: the frames
    // handed to it less the ones it still holds unplayed.
    std::uint64_t frames_played = 0;
    // Frames handed over and not yet played - the device's own buffer - plus
    // the ones still waiting in this sink's queue. frames_played +
    // frames_queued is everything submit() has taken since the last flush,
    // less what an underrun replaced with silence.
    std::uint64_t frames_queued = 0;
    // What the platform says its output path adds beyond the buffer above:
    // the further delay between a frame leaving the device buffer and being
    // heard. 0 where the platform does not say, which is not "no latency".
    std::uint32_t latency_frames = 0;
};

class MonitorSink {
public:
    MonitorSink();
    ~MonitorSink();
    MonitorSink(const MonitorSink&) = delete;
    MonitorSink& operator=(const MonitorSink&) = delete;

    // Opens `device_id` (empty selects the default render endpoint) in shared
    // mode for `channels` of float32 PCM at `sample_rate`, and starts the
    // render thread. `channel_mask` is the WAVEFORMATEXTENSIBLE speaker mask
    // (e.g. KSAUDIO_SPEAKER_5POINT1) describing what each channel position
    // means; 0 lets the platform pick a default for the channel count.
    // `low_latency` asks the platform for its smallest render period rather
    // than its default one (Windows: IAudioClient3's shared-mode engine
    // period, typically 2.7 ms against the default 10; falls back to the
    // default when the engine will not run this format at that size). A
    // caller submitting small chunks at a steady cadence gets a shorter
    // queue-to-speaker path; one submitting 32 ms frames gains nothing and
    // should leave it off. Ignored on platforms without such a knob.
    [[nodiscard]] std::expected<void, MonitorError> start(const std::string& device_id,
                                                           std::uint32_t sample_rate,
                                                           std::uint16_t channels,
                                                           std::uint32_t channel_mask = 0,
                                                           bool low_latency = false);

    // Queues interleaved float samples (a multiple of `channels` long).
    // Returns false if the queue is full - the caller is running ahead of
    // real time and should wait rather than spin.
    bool submit(std::span<const float> interleaved);

    // Room for at least one more period's worth of samples without blocking.
    [[nodiscard]] bool can_submit() const;

    void stop();

    // Where playback has got to, while it is running; nothing when it is not,
    // or on a platform whose backend cannot ask.
    [[nodiscard]] std::optional<MonitorPosition> position() const;

    // Drops what has been submitted and not yet played, here and in the
    // device, and carries on from the next submit(): a seek, or the end of a
    // track a caller has decided not to finish. Blocks until the render
    // thread has done it, so nothing submitted beforehand is heard
    // afterwards, and position() then counts from zero again. A frame already
    // past the device's own buffer - inside whatever mixer or DAC sits beyond
    // it, latency_frames' worth - cannot be recalled by anyone.
    void flush();

    // Stops the device without closing the stream: the format, the device and
    // the queue survive, submit() goes on taking frames, and nothing is
    // rendered until resume(). What a pause button needs - closing and
    // reopening would drop the queue and let another application take an
    // exclusive hold of the device in between. Repeating either call is
    // harmless; both refuse only when nothing is running.
    [[nodiscard]] std::expected<void, MonitorError> pause();
    [[nodiscard]] std::expected<void, MonitorError> resume();
    [[nodiscard]] bool paused() const;

    [[nodiscard]] bool running() const;
    [[nodiscard]] MonitorStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::audio
