#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

// Reading a render endpoint's own advertised capabilities - CEA-861 Short
// Audio Descriptors, the part of EDID (over HDMI) or ELD (ALSA's own
// EDID-Like Data, which carries the same SADs) that says which codecs, how
// many channels and which sample rates the sink accepts - rather than
// PassthroughSink's enumerate_render_devices(), which finds out the AC-3/
// E-AC-3 answer by opening the device and trying.
//
// The two are deliberately kept apart. enumerate_render_devices() is a probe
// against every render endpoint at once and only ever answers "AC-3/E-AC-3/
// exclusive PCM, yes or no" - the CLI's 'outputs' table. This is a read of
// what ONE endpoint's own descriptor says, in more detail (sample rates,
// channel count), and it is not available everywhere: reading raw EDID/ELD
// content is a per-platform question with a real "cannot" answer on some
// platforms today (see each backend/<platform>/sink_capabilities.cpp), so a
// caller that wants the AC-3/E-AC-3/PCM question answered unconditionally
// should fall back to enumerate_render_devices() rather than treat kNoBackend
// as a hard failure - see 'ac3cli play' (roadmap UX9) for that fallback.

namespace ac3::audio {

enum class EdidError : std::uint8_t {
    kNoBackend,    // this platform/backend has no EDID/ELD read path at all
    kDeviceNotFound,
    kNoEdid,       // the endpoint exists but reports no descriptor (nothing
                   // plugged in downstream, or a non-HDMI/DP output) - a
                   // real, expected outcome, not a failure to alarm about
    kParseFailed,  // a descriptor was read but did not parse
};

[[nodiscard]] std::string_view describe(EdidError error);

// What one sink's own Short Audio Descriptors say it accepts. Booleans only
// for AC-3/E-AC-3 - a receiver either has that decoder or it does not, unlike
// LPCM's genuinely per-rate/per-channel-count support.
struct SinkAudioCapabilities {
    bool pcm = false;
    bool ac3 = false;
    bool eac3 = false;
    // 0 when no LPCM descriptor was present to say - not "no channels".
    std::uint16_t max_pcm_channels = 0;
    // Every LPCM sample rate at least one LPCM descriptor advertised, in Hz.
    // Empty when pcm is false, or when pcm is true but no descriptor said
    // which rates (should not happen for a spec-conformant ELD, but a
    // truncated one is handled rather than assumed away).
    std::vector<std::uint32_t> pcm_sample_rates_hz;
};

// Reads `device_id`'s own advertised capabilities (the same id
// RenderDeviceInfo::id and PassthroughSink::start() use). Real only where a
// backend below actually implements it; everywhere else this reports
// kNoBackend rather than guessing - see that backend's own
// sink_capabilities.cpp for which platforms that is true on today, and why.
[[nodiscard]] std::expected<SinkAudioCapabilities, EdidError> read_sink_capabilities(
    const std::string& device_id);

}  // namespace ac3::audio
