#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Windows Spatial Sound object rendering (roadmap UX8): hand decoded Atmos
// objects to the OS's own object renderer - ISpatialAudioObjectRenderStream -
// as DYNAMIC objects carrying their real OAMD positions, and the programme's
// bed as STATIC ones.
//
// This is the one path that lets Dolby's own renderer engage with this
// project's reconstructed objects at all: a licensed decoder refuses to
// object-decode a stream this project has no signing key for (see
// CONTRIBUTING.md's "Object reconstruction has none of the four [oracles]"),
// but ISpatialAudioObjectRenderStream is gated on nothing but a spatial-
// sound-capable endpoint. What this sink does with a position is genuinely
// computed by Windows Sonic/Dolby Atmos's HRTF renderer, not by anything in
// this repository - the whole reason it is worth building.
//
// This header is deliberately codec-blind, the same way ac3::iec61937's
// framing is the only ac3::forge-adjacent thing ac3::audio depends on: it
// knows nothing about OAMD, JOC, or which of a programme's objects are bed
// vs dynamic. That interpretation (see apps/cli/commands/live_audio.cpp's
// run_spatial) is the caller's job, for the same layering reason PassthroughSink
// doesn't know what IEC 61937 framing means either. A static object's
// channel identity is expressed as a WAVEFORMATEXTENSIBLE SPEAKER_* bit - the
// same vocabulary MonitorSink::start's channel_mask already uses - rather
// than a Windows-Spatial-API-specific enum, so the library has one channel-
// identity vocabulary throughout; the Windows backend privately maps
// SPEAKER_FRONT_LEFT etc. to AudioObjectType_FrontLeft etc.

namespace ac3::audio {

enum class SpatialError : std::uint8_t {
    kNoBackend,        // built without a platform spatial backend
    kComFailure,       // a Windows audio (WASAPI/COM) call failed
    kDeviceNotFound,
    // The endpoint exists and ISpatialAudioClient activates, but no spatial
    // sound format (Windows Sonic for Headphones, Dolby Atmos for Home
    // Theater/Headphones, DTS:X) is currently enabled on it - the clean
    // refusal the roadmap calls for, distinct from kNoBackend ("this build
    // cannot do this at all"). Fixed in Settings, not in code.
    kNoSpatialFormat,
    kFormatRejected,   // the endpoint rejected the negotiated audio format
    kAlreadyRunning,
    kNotRunning,
};

[[nodiscard]] std::string_view describe(SpatialError error);

// One render endpoint's spatial capability, for 'ac3cli outputs' to print
// alongside the passthrough columns it already has (RenderDeviceInfo in
// passthrough.hpp) - probed independently rather than folded into that
// struct, so the two capabilities' device *indices* never have to agree,
// only their device *ids*.
struct SpatialDeviceCapability {
    // False when even ISpatialAudioClient itself could not be activated
    // (kNoBackend/kComFailure/kDeviceNotFound territory) - see `reason`.
    bool available = false;
    // 0 when unavailable, or when the endpoint has no spatial format enabled
    // (GetMaxDynamicObjectCount()'s own "no spatial audio option is engaged"
    // answer) - the same "unknown vs none" ambiguity RenderDeviceInfo::channels
    // documents does not apply here: 0 always means "cannot render a dynamic
    // object right now", never "cannot say".
    std::uint32_t max_dynamic_objects = 0;
    // Empty when max_dynamic_objects > 0; otherwise what to tell a user, e.g.
    // "no spatial sound format is enabled on this endpoint - enable Windows
    // Sonic for Headphones or Dolby Atmos for Home Theater/Headphones in
    // Settings > System > Sound".
    std::string reason;
};

// Probes `device_id` (empty = default render endpoint) directly - this is a
// real device query, unlike enumerate_render_devices()'s own IsFormatSupported
// probing, because GetMaxDynamicObjectCount() is a per-endpoint runtime fact
// that changes the moment a user flips Settings > System > Sound, not
// something a capability flag can precompute.
[[nodiscard]] std::expected<SpatialDeviceCapability, SpatialError> probe_spatial_capability(
    const std::string& device_id);

struct SpatialObjectStats {
    std::uint64_t updates_submitted = 0;  // per-object blocks handed to submit()
    std::uint64_t updates_rendered = 0;
    // Render periods where an object's ring ran dry and silence went out
    // instead - counted per the whole stream, not hidden, matching
    // MonitorSink/PassthroughSink's underrun discipline.
    std::uint64_t underruns = 0;
    // How many of the dynamic object slots requested at start() the endpoint
    // actually granted (GetMaxDynamicObjectCount() may be lower than asked).
    std::uint32_t active_dynamic_objects = 0;
};

// One block's worth of a dynamic object: mono PCM plus the OAMD position and
// gain it was decoded with. `pcm` covers exactly one render period; the
// caller (run_spatial) hands over one JOC-reconstructed frame's worth per
// submit() call, and the sink's own ring absorbs the difference between
// decode cadence (1536 samples) and the endpoint's render period.
struct DynamicObjectUpdate {
    std::span<const float> pcm;
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float gain = 1.0F;  // linear
};

// One block's worth of a bed channel, anchored to a fixed speaker position
// rather than a moving one. `channel` is a WAVEFORMATEXTENSIBLE SPEAKER_*
// bit (exactly one bit set) - see this header's own comment on why that
// vocabulary and not a Windows-Spatial-API-specific enum.
struct StaticObjectUpdate {
    std::span<const float> pcm;
    std::uint32_t channel = 0;
};

// Real-time, no-allocation contract: start() pre-activates every object this
// stream will ever use (the static set fixed at the size `static_count`
// passed there, the dynamic pool fixed at `max_dynamic_objects`) and
// pre-sizes one ring per object. submit() and the render thread only push
// into / pop from those existing rings and write into already-activated
// ISpatialAudioObject buffers - nothing here allocates once start() returns.
class SpatialObjectSink {
public:
    SpatialObjectSink();
    ~SpatialObjectSink();
    SpatialObjectSink(const SpatialObjectSink&) = delete;
    SpatialObjectSink& operator=(const SpatialObjectSink&) = delete;

    // Opens `device_id` (empty selects the default render endpoint) and
    // activates a spatial audio object render stream at `sample_rate`.
    // `static_channels` fixes the bed's shape for the session (one bit per
    // static object this session will ever feed, OR'd together);
    // `max_dynamic_objects` is the dynamic pool size requested - the
    // endpoint's own ceiling (ISpatialAudioClient::GetMaxDynamicObjectCount)
    // wins if lower, and the granted count is in stats().active_dynamic_objects
    // after a successful start(). Returns kNoSpatialFormat, not a hard
    // failure, when the endpoint has no spatial format enabled at all.
    [[nodiscard]] std::expected<void, SpatialError> start(const std::string& device_id,
                                                           std::uint32_t sample_rate,
                                                           std::uint32_t static_channels,
                                                           std::uint32_t max_dynamic_objects);

    // One decode block's worth of updates. `dynamic.size()` must not exceed
    // the granted dynamic pool size; `static_objects` must name only bits
    // present in the `static_channels` mask start() was given. Returns false
    // if any object's ring is too full to accept this block - the caller is
    // running ahead of real time and should wait rather than spin, exactly
    // like MonitorSink::submit().
    bool submit(std::span<const DynamicObjectUpdate> dynamic,
                std::span<const StaticObjectUpdate> static_objects);

    // Room for at least one more block on every active object without
    // blocking.
    [[nodiscard]] bool can_submit() const;

    void stop();

    [[nodiscard]] bool running() const;
    [[nodiscard]] SpatialObjectStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::audio
