#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ac3/audio/passthrough.hpp"

// Which output Hearth plays through, on which endpoint, and why
// (planning/hearth-reference-player.md, A3).
//
// Pure: it takes what a probe found and what the user asked for, and names a
// mode, an endpoint and a one-line reason. Nothing here opens a device, which
// is what lets the whole table of cases run on a machine with no sound card -
// including the cases one desk with one receiver can never produce.
//
// Shaped after Crucible's own decision (apps/crucible/engine/output_policy.hpp)
// so the two applications' Output screens can be read against each other.
// What differs is what is being chosen between: Hearth plays a stream that
// already exists rather than encoding a live one, so its modes are "bitstream
// this file as it is", "transcode it to AC-3 and bitstream that", "decode it
// and play PCM here", "hand it to a group of network sinks", and "nothing
// usable".
//
// The other difference is that Hearth has to answer the sink-following gaps
// the appliance plan left open (planning/player-appliance.md, "What UX9
// needs"), and three of the six are settled by the shape of this function
// rather than by its contents:
//
//   * Gap 1, the default endpoint never probed. There is no "the default is
//     taken at its word" path here: an endpoint's capabilities arrive as
//     facts, and the default endpoint is one row like any other. A caller
//     that cannot probe an endpoint says so with CapabilitySource, and gets a
//     decision that accounts for not knowing.
//   * Gaps 3 and 5, a sink that changed and an EDID read once. A pure
//     function has nothing to remember, so re-deciding on a device change is
//     the whole mechanism: ac3::audio::RenderDeviceWatch reports the change,
//     the caller re-reads the facts and calls this again.
//   * Gap 6, "no descriptor" and "no reader" collapsed into one note. They
//     are separate CapabilitySource values, and the reason text says which,
//     because "your receiver is off" and "this machine cannot read EDID" are
//     different problems for whoever is holding the phone.

namespace ac3::hearth {

enum class OutputMode : std::uint8_t {
    // IEC 61937 to a sink that takes the stream as it is: no decode, no
    // re-encode, the bytes the file carries.
    kBitstream,
    // E-AC-3 transcoded to AC-3 first, for a sink that takes only AC-3.
    // Lossy, and chosen only when the alternative is not bitstreaming at all.
    kBitstreamAsAc3,
    // Decoded and rendered here, to a local device at the device's own width
    // (ac3::audio::PcmOutput).
    kLocalPcm,
    // Handed to Sendspin players as the group's source (src/sendspin).
    kNetworkGroup,
    // Nothing usable. The reason says what was in the way.
    kNone,
};

[[nodiscard]] std::string_view describe(OutputMode mode);

// Where an endpoint's codec answers came from. The distinction gap 6 asks for
// is between the last two: an endpoint that reports no descriptor is a real,
// expected outcome (nothing connected downstream, or not an HDMI/DP output),
// while a platform with no reader has not answered the question at all.
enum class CapabilitySource : std::uint8_t {
    // The sink's own CEA-861 Short Audio Descriptors, over EDID or ALSA's
    // ELD (ac3::audio::read_sink_capabilities).
    kDescriptor,
    // The platform was asked to open each format and said yes or no
    // (ac3::audio::enumerate_render_devices).
    kProbe,
    // The endpoint is there and reports no descriptor.
    kNoDescriptor,
    // This platform cannot read a descriptor at all.
    kNoReader,
};

[[nodiscard]] std::string_view describe(CapabilitySource source);

// What a caller found out about one local render endpoint. The booleans are
// what it will CARRY; `source` is how confidently that is known.
struct EndpointFacts {
    std::string id{};
    std::string name{};
    // Applications render here, and on Windows an exclusive-mode open of this
    // endpoint can be refused outright while they do (and invalidates their
    // streams when it succeeds). It is not a reason to refuse a bitstream -
    // a player asked to bitstream to a receiver is usually being asked about
    // exactly this endpoint - but it is a reason the caller should be ready
    // for the open to fail, and the reason text says so.
    bool is_default = false;
    bool accepts_ac3 = false;
    bool accepts_eac3 = false;
    bool accepts_pcm = false;
    CapabilitySource source = CapabilitySource::kProbe;
    // The device's own width and speaker mask (ac3::audio::RenderDeviceInfo);
    // 0 means the backend cannot say, not none.
    std::uint16_t channels = 0;
    std::uint32_t speakers = 0;
};

struct OutputRequest {
    std::span<const EndpointFacts> endpoints{};
    // What the item carries, and std::nullopt for anything that cannot be
    // bitstreamed at all - a WAV, or an AC-4 stream, which no IEC 61937
    // format covers.
    std::optional<audio::BitstreamFormat> stream = std::nullopt;
    // Object audio (E-AC-3 JOC). It rides inside the ordinary Annex E
    // bitstream, so it changes no capability question - it only changes what
    // is LOST by not bitstreaming, which the reason text says.
    bool has_objects = false;
    // The user's choice of mode, honoured when it can be; otherwise the
    // reason says what stopped it.
    std::optional<OutputMode> pinned = std::nullopt;
    // The user's choice of endpoint (its id; empty for automatic), taken with
    // the best mode it can carry.
    std::string preferred_endpoint_id{};
    // A group of network sinks the user has selected, and whether it is ready
    // to take a stream (paired, and not held by another server).
    std::string group_name{};
    bool group_ready = false;
    // The CLI's `follow=off`: refuse rather than fall back, so that a sink
    // which will not take the stream is reported instead of quietly played
    // some other way.
    bool follow_sink = true;
    // Whether this build can transcode E-AC-3 to AC-3 for a sink that takes
    // only AC-3. False turns the kBitstreamAsAc3 leg off, and the reason
    // then says the sink's limit rather than offering it.
    bool transcode_available = true;
};

struct OutputChoice {
    OutputMode mode = OutputMode::kNone;
    std::string endpoint_id{};
    std::string endpoint_name{};
    // One line for the Output screen and the log: why this, or why not
    // something better. Never empty.
    std::string reason{};
};

[[nodiscard]] OutputChoice choose_output(const OutputRequest& request);

}  // namespace ac3::hearth
