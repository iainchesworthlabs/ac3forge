#include "ac3/audio/spatial.hpp"

// The Android spatial backend: there isn't one. ISpatialAudioObjectRenderStream
// is a Windows API with no AAudio equivalent - unlike monitor.cpp/passthrough.cpp,
// every non-Windows backend directory carries this exact stub, not just
// posix's. Every entry point fails with kNoBackend rather than the API
// disappearing. See posix/spatial.cpp for the same convention in full.

namespace ac3::audio {

std::string_view describe(SpatialError error) {
    switch (error) {
        case SpatialError::kNoBackend: return "no spatial backend on this platform";
        case SpatialError::kComFailure: return "a platform audio call failed";
        case SpatialError::kDeviceNotFound: return "the requested render device was not found";
        case SpatialError::kNoSpatialFormat:
            return "no spatial sound format is enabled on this endpoint";
        case SpatialError::kFormatRejected:
            return "the endpoint rejected the negotiated audio format";
        case SpatialError::kAlreadyRunning: return "spatial rendering is already running";
        case SpatialError::kNotRunning: return "spatial rendering is not running";
    }
    return "unknown spatial error";
}

std::expected<SpatialDeviceCapability, SpatialError> probe_spatial_capability(const std::string&) {
    return std::unexpected(SpatialError::kNoBackend);
}

struct SpatialObjectSink::Impl {};

SpatialObjectSink::SpatialObjectSink() : impl_(nullptr) {}
SpatialObjectSink::~SpatialObjectSink() = default;

std::expected<void, SpatialError> SpatialObjectSink::start(const std::string&, std::uint32_t,
                                                            std::uint32_t, std::uint32_t) {
    return std::unexpected(SpatialError::kNoBackend);
}

bool SpatialObjectSink::submit(std::span<const DynamicObjectUpdate>,
                               std::span<const StaticObjectUpdate>) {
    return false;
}
bool SpatialObjectSink::can_submit() const { return false; }
void SpatialObjectSink::stop() {}
bool SpatialObjectSink::running() const { return false; }
SpatialObjectStats SpatialObjectSink::stats() const { return {}; }

}  // namespace ac3::audio
