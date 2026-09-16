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

// Exclusive-mode IEC 61937 passthrough: hand already-packed AC-3 or E-AC-3
// bursts to an S/PDIF or HDMI endpoint so the AV receiver on the other end
// decodes them itself and lights its Dolby Digital / Dolby Digital Plus
// indicator.
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
    kNoBackend,             // built without a platform passthrough backend
    kComFailure,
    kDeviceNotFound,
    kFormatRejected,        // endpoint will not accept this format over IEC 61937
    kExclusiveUnavailable,  // device busy, or exclusive access disabled for it
    kAlreadyRunning,
    kNotRunning,
};

[[nodiscard]] std::string_view describe(PassthroughError error);

// Which IEC 61937 encapsulation to bitstream. The two need different WASAPI
// subformats and different carrier (link) sample rates - Dolby Digital Plus
// runs the carrier at 4x the content rate (Microsoft's "Representing Formats
// for IEC 61937 Transmissions") - and different burst sizes
// (ac3::iec61937::kBurstBytes vs kEac3BurstBytes).
enum class BitstreamFormat : std::uint8_t { kAc3, kEac3 };

// Link frames per content frame: 4 for E-AC-3, whose link runs at four times
// the content rate, and 1 for AC-3. A burst is 1536 content frames either way.
[[nodiscard]] constexpr std::uint32_t carrier_ratio(BitstreamFormat format) {
    return format == BitstreamFormat::kEac3 ? 4U : 1U;
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
    // size submit() expects.
    [[nodiscard]] std::expected<void, PassthroughError> start(
        const std::string& device_id, std::uint32_t sample_rate = 48000,
        BitstreamFormat format = BitstreamFormat::kAc3);

    // Queues one complete burst (ac3::iec61937::kBurstBytes for AC-3,
    // kEac3BurstBytes for E-AC-3 - see ac3::iec61937::wrap_frame /
    // Eac3BurstPacker). Returns false if the queue is full - the caller is
    // running ahead of real time and should wait rather than spin.
    bool submit(std::span<const std::byte> burst);

    // Room for at least one more burst without blocking.
    [[nodiscard]] bool can_submit() const;

    // Where the device has got to, as MonitorSink::position() reports it and
    // with the same meaning, but in frames of the CONTENT: 1536 to a burst in
    // either format, the E-AC-3 link's four frames counting as one
    // (carrier_ratio()). Nothing while stopped. Underrun silence counts as
    // played, as a device's own clock counts it.
    [[nodiscard]] std::optional<MonitorPosition> position() const;

    // Drops the bursts not yet played, here and in the device, and carries on
    // from the next submit(); position() then counts from zero again. Harmless
    // when nothing is running. As MonitorSink::flush(), a device that has
    // stopped answering is not waited for past a moment, and its flush drops
    // only what was submitted before this call.
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
    [[nodiscard]] bool paused() const;

    void stop();

    [[nodiscard]] bool running() const;
    [[nodiscard]] PassthroughStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::audio
