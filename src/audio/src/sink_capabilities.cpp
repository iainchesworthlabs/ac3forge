#include "ac3/audio/sink_capabilities.hpp"

// The one platform-independent piece of ac3::audio::sink_capabilities - see
// the header for the shape this is describing errors for. Compiled on every
// platform, the same way ac3::iec61937's byte framing is platform-independent
// while PassthroughSink's delivery of it is not; read_sink_capabilities()
// itself is implemented once per backend/<platform>/sink_capabilities.cpp.

namespace ac3::audio {

std::string_view describe(EdidError error) {
    switch (error) {
        case EdidError::kNoBackend:
            return "no EDID/ELD read path on this platform";
        case EdidError::kDeviceNotFound:
            return "the requested render device was not found";
        case EdidError::kNoEdid:
            return "the endpoint reports no descriptor (nothing plugged in downstream, or not "
                   "an HDMI/DisplayPort output)";
        case EdidError::kParseFailed:
            return "a descriptor was read but did not parse";
    }
    return "unknown EDID error";
}

}  // namespace ac3::audio
