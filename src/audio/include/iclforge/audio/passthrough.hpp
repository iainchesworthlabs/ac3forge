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

#include "ac3/audio/monitor.hpp"
#include "ac3/iec61937/iec61937.hpp"

// Exclusive-mode IEC 61937 passthrough: hand already-packed AC-3, E-AC-3 or
// AC-4 bursts to an S/PDIF or HDMI endpoint so the AV receiver on the other
// end decodes them itself and lights its Dolby Digital / Dolby Digital Plus
// indicator.
//
// AC-4 (IEC 61937-14) goes only where the platform has a way to send it,
// which is where its audio API takes IEC 61937 bursts as opaque two-channel
// data with the non-audio flag set rather than naming the codec: ALSA, and
// Android's ENCODING_IEC61937. WASAPI negotiates a codec-specific subformat
// and the Windows SDK (ksmedia.h, 10.0.26100) defines none for AC-4;
// PipeWire's IEC 958 format names a codec and its list (spa/param/audio/
// iec958.h, 1.6) has no AC-4; Core Audio retunes a stream to a codec's format
// ID and its list (CoreAudioBaseTypes.h) has no AC-4 either. Those three
// refuse every AC-4 format with kUnsupportedFormat. AC-4 HBR16 needs the
// eight-channel high-bit-rate link, which no backend here opens, so ALSA and
// Android refuse it the same way.
//
// Exclusive mode is mandatory. In shared mode the Windows audio engine would
// treat the bursts as ordinary PCM and mix, resample or volume-scale them;
// any of those corrupts the bit pattern and the receiver hears static or
// loses lock. Exclusive mode hands the endpoint our bytes untouched.
//
// The burst packing itself lives in ac3::iec61937 (byte-exact against
// FFmpeg's spdif muxer, and for E-AC-3 also cross-checked against Microsoft's
// own IEC 61937 documentation); this is only delivery.

namespace ac3::audio {

enum class PassthroughError : std::uint8_t {
    kNoBackend,  // built without a platform passthrough backend
    kComFailure,
    kDeviceNotFound,
    kFormatRejected,        // endpoint will not accept this format over IEC 61937
    kExclusiveUnavailable,  // device busy, or exclusive access disabled for it
    kAlreadyRunning,
    kNotRunning,
    // This platform's audio API has no way to send the format at all, whatever
    // the device: see the header comment on AC-4.
    kUnsupportedFormat,
};

[[nodiscard]] std::string_view describe(PassthroughError error);

// Which IEC 61937 encapsulation to bitstream. AC-3 and E-AC-3 need different
// WASAPI subformats and different carrier (link) sample rates - Dolby Digital
// Plus runs the carrier at 4x the content rate (Microsoft's "Representing
// Formats for IEC 61937 Transmissions") - and different burst sizes
// (ac3::iec61937::kBurstBytes vs kEac3BurstBytes).
//
// The AC-4 formats are its links rather than its data types: kAc4 carries
// IEC 61937-14's AC-4 and AC-4 LD data-bursts on a link at the content rate,
// kAc4Hbr4 its HBR4 ones at four times it, as E-AC-3's, and kAc4Hbr16 its
// HBR16 ones at sixteen times it. An AC-4 burst is as long as its own
// repetition period, which follows the stream's frame rate
// (ac3::iec61937::Ac4BurstPacker).
enum class BitstreamFormat : std::uint8_t { kAc3, kEac3, kAc4, kAc4Hbr4, kAc4Hbr16 };

[[nodiscard]] constexpr bool is_ac4(BitstreamFormat format) {
    return format == BitstreamFormat::kAc4 || format == BitstreamFormat::kAc4Hbr4 ||
           format == BitstreamFormat::kAc4Hbr16;
}

// "AC-3", "E-AC-3", "AC-4", "AC-4 HBR4" or "AC-4 HBR16".
[[nodiscard]] constexpr std::string_view format_name(BitstreamFormat format) {
    switch (format) {
        case BitstreamFormat::kAc3:
            return "AC-3";
        case BitstreamFormat::kEac3:
            return "E-AC-3";
        case BitstreamFormat::kAc4:
            return "AC-4";
        case BitstreamFormat::kAc4Hbr4:
            return "AC-4 HBR4";
        case BitstreamFormat::kAc4Hbr16:
            return "AC-4 HBR16";
    }
    return "unknown";
}

// Link frames per content frame: 4 for E-AC-3 and AC-4 HBR4, whose links run
// at four times the content rate, 16 for AC-4 HBR16, and 1 for AC-3 and AC-4.
[[nodiscard]] constexpr std::uint32_t carrier_ratio(BitstreamFormat format) {
    switch (format) {
        case BitstreamFormat::kEac3:
        case BitstreamFormat::kAc4Hbr4:
            return 4U;
        case BitstreamFormat::kAc4Hbr16:
            return 16U;
        case BitstreamFormat::kAc3:
        case BitstreamFormat::kAc4:
            break;
    }
    return 1U;
}

// The longest burst `format` has, in bytes: every AC-3 and E-AC-3 burst is
// this long, and an AC-4 one is as long as its own repetition period, the
// longest of which this is (ac3::iec61937::repetition_period()).
[[nodiscard]] inline std::size_t max_burst_bytes(BitstreamFormat format) {
    switch (format) {
        case BitstreamFormat::kAc3:
            return iec61937::kBurstBytes;
        case BitstreamFormat::kEac3:
            return iec61937::kEac3BurstBytes;
        case BitstreamFormat::kAc4:
            return iec61937::repetition_period(iec61937::BurstDataType::kAc4);
        case BitstreamFormat::kAc4Hbr4:
            return iec61937::repetition_period(iec61937::BurstDataType::kAc4Hbr4);
        case BitstreamFormat::kAc4Hbr16:
            return iec61937::repetition_period(iec61937::BurstDataType::kAc4Hbr16);
    }
    return iec61937::kBurstBytes;
}

// Whether a burst of `bytes` is one `format`'s submit() takes: exactly
// max_burst_bytes() for AC-3 and E-AC-3, and for AC-4 any whole number of
// link frames up to it.
[[nodiscard]] inline bool burst_size_fits(BitstreamFormat format, std::size_t bytes) {
    if (!is_ac4(format)) {
        return bytes == max_burst_bytes(format);
    }
    return bytes > 0 && bytes % 4 == 0 && bytes <= max_burst_bytes(format);
}

struct RenderDeviceInfo {
    std::string id;
    std::string name;
    bool is_default = false;
    // IsFormatSupported() said yes to AC-3 over IEC 61937 in exclusive mode.
    // A GUI should grey out everything else rather than let the user pick a
    // device that can only fail.
    bool supports_ac3_passthrough = false;
    // As above, for E-AC-3 (Dolby Digital Plus, and Atmos riding inside it -
    // there is no separate passthrough format for Atmos, since the object
    // container is ordinary Annex E aux data).
    bool supports_eac3_passthrough = false;
    // As above, for AC-4 on a link at the content rate (BitstreamFormat::
    // kAc4). Set only where the platform can send AC-4 at all (the header
    // comment says which), and there it says what the AC-3 answer says: the
    // endpoint takes an IEC 61937 link at that rate. Whether the receiver
    // decodes AC-4 is not something any of these platforms reports.
    bool supports_ac4_passthrough = false;
    // Whether plain 16-bit stereo PCM is accepted in exclusive mode. This
    // separates the two reasons passthrough can be unavailable: a device that
    // refuses even PCM has exclusive mode switched off (or is in use), while
    // one that takes PCM but not IEC 61937 simply cannot bitstream - an
    // analog output, say, rather than S/PDIF or HDMI.
    bool supports_exclusive_pcm = false;
    // How many channels the endpoint itself renders, when the backend can
    // say. 0 means it cannot - not "no channels" - and a caller must treat
    // the two differently: the only safe reading of "unknown" is to leave
    // the audio alone. It exists so a decoded programme wider than the
    // endpoint can be folded (§7.8, ac3::OutputStage) before it is played,
    // rather than handed to a shared-mode mixer to average down however it
    // sees fit. MonitorSink opens in SHARED mode, so a wider programme is
    // not refused - which is exactly why the narrowing has to be noticed
    // here instead of being discovered as an error later.
    std::uint16_t channels = 0;
    // Which speakers those channels are, as WAVEFORMATEXTENSIBLE's channel
    // mask (speakers.hpp), when the backend can say. 0 means it cannot - not
    // "no speakers" - and speakers.hpp's default_speakers() is then the most a
    // caller can assume, from the width alone. With a mask, a routing patch
    // from the renderer's slots to this device's channels can be built rather
    // than guessed: locations_of() gives the location of each channel, in the
    // order an interleaved stream carries them.
    std::uint32_t speakers = 0;
    // The sample rates the endpoint itself takes, ascending, when the backend
    // can say; empty when it cannot. A rate this list omits may still play,
    // since a shared-mode engine resamples - the list is what the device does
    // without help, which is what a caller needs to decide whether to resample
    // before it or leave that to the engine.
    std::vector<std::uint32_t> sample_rates;

    // Every member, so that two enumerations can be compared for "has
    // anything changed" - which is how RenderDeviceWatch decides whether a
    // re-probe found a hot-plug (render_devices.hpp). A device renegotiating
    // its rates or its speakers is a change as much as one arriving is.
    friend bool operator==(const RenderDeviceInfo&, const RenderDeviceInfo&) = default;
};

// Every active render endpoint, each probed for AC-3 passthrough support at
// the given carrier rate (the AC-3 stream's own sample rate).
[[nodiscard]] std::expected<std::vector<RenderDeviceInfo>, PassthroughError>
enumerate_render_devices(std::uint32_t sample_rate = 48000);

struct PassthroughStats {
    std::uint64_t bursts_submitted = 0;
    std::uint64_t bursts_rendered = 0;
    // Render periods that found the queue empty. Any non-zero value means the
    // receiver heard a gap, which usually drops its lock.
    std::uint64_t underruns = 0;
};

class PassthroughSink {
public:
    PassthroughSink();
    ~PassthroughSink();
    PassthroughSink(const PassthroughSink&) = delete;
    PassthroughSink& operator=(const PassthroughSink&) = delete;

    // Opens `device_id` (empty selects the default render endpoint) in
    // exclusive mode with an IEC 61937 format at `sample_rate` (the CONTENT
    // rate; for E-AC-3 the carrier itself runs at 4x that), and starts the
    // render thread. `format` picks AC-3 vs E-AC-3 and, with it, which burst
    // size submit() expects. Refused while running(); once running() is
    // false - after stop(), or after the device went away - it may be called
    // again, with nothing to tidy up first.
    [[nodiscard]] std::expected<void, PassthroughError> start(
        const std::string& device_id, std::uint32_t sample_rate = 48000,
        BitstreamFormat format = BitstreamFormat::kAc3);

    // Queues one complete burst (ac3::iec61937::kBurstBytes for AC-3,
    // kEac3BurstBytes for E-AC-3 - see ac3::iec61937::wrap_frame /
    // Eac3BurstPacker). Returns false if the queue is full - the caller is
    // running ahead of real time and should wait rather than spin - and
    // whenever the sink is not running(), which no wait will change: a caller
    // that retries on false has to look at running() too.
    bool submit(std::span<const std::byte> burst);

    // Room for at least one more burst without blocking. False while not
    // running().
    [[nodiscard]] bool can_submit() const;

    // Where the device has got to, as MonitorSink::position() reports it and
    // with the same meaning, but in frames of the CONTENT: 1536 to a burst in
    // either format, the E-AC-3 link's four frames counting as one
    // (carrier_ratio()). Nothing while not running(), including once the
    // device has gone. Underrun silence counts as played, as a device's own
    // clock counts it.
    [[nodiscard]] std::optional<MonitorPosition> position() const;

    // Drops the bursts not yet played, here and in the device, and carries on
    // from the next submit(); position() then counts from zero again. Returns
    // at once when nothing is running, and as soon as the device goes away
    // if that happens while it waits. As MonitorSink::flush(), a device that
    // has stopped answering is not waited for past a moment, and its flush
    // drops only what was submitted before this call.
    void flush();

    // Stops the device without closing it, and starts it again: the format,
    // the device and the queue survive, and submit() goes on taking bursts.
    // What a pause button needs - closing and reopening would let another
    // application take the exclusive hold in between. The link stops with the
    // device, and a receiver drops its lock when it does, so the first moments
    // after resume() can be silent while it finds the stream again. Repeating
    // either call is harmless; both refuse only when nothing is running.
    [[nodiscard]] std::expected<void, PassthroughError> pause();
    [[nodiscard]] std::expected<void, PassthroughError> resume();
    // True between a pause() and a resume(), and only while running().
    [[nodiscard]] bool paused() const;

    // Stops and closes the device. Harmless when nothing is running, and
    // what lets go of a stream that ended with its device.
    void stop();

    // True from a successful start() until stop() - or until the device goes
    // away under the stream: unplugged, disabled, or its stream taken by a
    // format change or an audio-service restart. The sink stops itself then,
    // and answers every call as it would after stop(): position() reports
    // nothing, submit() and can_submit() refuse, flush() returns at once, and
    // pause() and resume() refuse with kNotRunning. A caller that started the
    // sink and finds this false without having stopped it has lost the
    // device. When the loss shows depends on the platform: WASAPI within a
    // fraction of a second, paused or not; ALSA at the render thread's next
    // wait or write, which for a paused stream is its resume; Core Audio when
    // the device reports itself dead; Android when a write finds the track
    // dead. On PipeWire, a stream the session manager moves to another sink
    // carries on, and one it holds unlinked until its sink comes back keeps
    // running with a position that stands still.
    [[nodiscard]] bool running() const;
    [[nodiscard]] PassthroughStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::audio
