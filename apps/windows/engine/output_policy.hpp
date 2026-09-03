#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Which output mode, on which endpoint (docs/platforms/windows-demo.md,
// "Output modes and hot switching").
//
// The decision is pure: it takes what the probe found and what the user
// pinned, and names a mode, an endpoint and a one-line reason. The platform
// layer gathers the facts (enumerate_render_devices, probe_spatial_capability,
// the current default) and acts on the answer; nothing here touches a device,
// which is what lets the whole table of cases be tested on every CI leg.
//
// One rule from spike S1 is built in rather than left to the caller: an
// exclusive-mode open on the endpoint applications are rendering to is
// refused by Windows AND invalidates their streams, so the bitstream modes
// are only ever chosen on an endpoint that is not the current default. Until
// the default has been moved to the null sink, the best a receiver on the
// default endpoint can get is the reason saying so.

namespace ac3::windemo {

enum class OutputMode : std::uint8_t {
    kAtmos,        // E-AC-3 JOC, exclusive bitstream, objects intact
    kDdPlus51,     // E-AC-3 5.1, exclusive bitstream, no object metadata (no key)
    kDd51,         // AC-3 5.1, exclusive bitstream
    kPcmSurround,  // decoded 5.1/7.1 PCM, shared mode
    kHeadphones,   // decoded objects through Windows Spatial Sound
    kStereo,       // decoded Lo/Ro, shared mode
    kNone,         // nothing usable
};

[[nodiscard]] std::string_view describe(OutputMode mode);

// What the platform layer found out about one render endpoint.
struct EndpointFacts {
    std::string id;
    std::string name;
    bool is_default = false;      // applications render here
    bool is_null_sink = false;    // the demo's own (or a stand-in) silent endpoint
    bool accepts_eac3 = false;    // IsFormatSupported, exclusive, E-AC-3 IEC 61937
    bool accepts_ac3 = false;     // likewise AC-3
    std::uint16_t shared_channels = 0;  // the shared-mode mix format's channel count
    bool spatial = false;         // a spatial sound format is enabled on it
    std::uint32_t spatial_max_objects = 0;
};

struct OutputPolicyInput {
    std::span<const EndpointFacts> endpoints;
    bool signing_key_loaded = false;
    std::optional<OutputMode> pinned;  // the user's choice of mode, honoured when feasible
    // The user's choice of endpoint (its id; empty for automatic): taken
    // with the best mode it can carry, the pinned mode first when it can,
    // and never a bitstream mode while it is the default. Absent or unable
    // to carry anything, the automatic choice stands and the reason says so.
    std::string preferred_endpoint_id;
};

struct OutputChoice {
    OutputMode mode = OutputMode::kNone;
    std::string endpoint_id;
    std::string endpoint_name;
    // One line for the UI's Output screen: why this, or why not something
    // better. Never empty.
    std::string reason;
};

[[nodiscard]] OutputChoice choose_output(const OutputPolicyInput& input);

}  // namespace ac3::windemo
