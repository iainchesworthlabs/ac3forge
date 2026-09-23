#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/sink_capabilities.hpp"
#include "output_decision.hpp"
#include "queue.hpp"

// Each item's output, decided (planning/hearth-reference-player.md, A3).
//
// choose_output() is pure; this gathers what it decides from. An endpoint's
// row comes from two readings, kept apart as sink_capabilities.hpp keeps
// them:
//   * the platform's probe: enumerate_render_devices() opens each format and
//     says whether it could;
//   * where the platform has a reader, the sink's own descriptor:
//     read_sink_capabilities().
// A format counts as carried only when both allow it. The descriptor says what
// the receiver decodes, and the probe says what this machine can open - an
// HDMI output on ALSA opens any IEC 61937 format, whatever the receiver
// behind it takes. An endpoint that reports no descriptor carries no
// bitstream, since nothing is known of the sink behind it.
//
// The readings are taken once per sample rate, since the probe answers for
// one. They are kept until refresh(), which the engine calls when a render
// device comes, goes or changes (ac3::audio::RenderDeviceWatch). Each item
// after that is decided against the machine as it is then: the appliance
// plan's gaps 3 and 5.
//
// A probe cannot see past an output this player holds open: the device reads
// as busy, and a link reads as refusing the very format it carries. So the
// endpoint the player holds (HeldOutput) keeps the record of its last reading
// taken while it was free, with a fresh descriptor, and anything read while
// an output is held is read again once it is not. Without that, a refresh
// while bitstreaming would move the item off its own link, and closing the
// link would move it back. An enumeration that finds nothing is taken as a
// failure to look, not as a machine with no outputs: the last list stands.
//
// One thread's: the engine's.

namespace ac3::hearth {

// The Output screen's choices.
struct OutputPreferences {
    // A mode the person chose (OutputRequest::pinned), or automatic.
    std::optional<OutputMode> pinned = std::nullopt;
    // An endpoint the person chose, or empty for automatic.
    std::string endpoint_id{};
    // follow=off: refuse rather than fall back.
    bool follow_sink = true;
    // A group of network sinks the person has selected in the Network page,
    // and whether it is ready to take a stream (paired, and not held by
    // another server) - mirror OutputRequest::group_name/group_ready
    // (output_decision.hpp), which choose() threads straight through. Set
    // by whatever couples this to the Network page's own state; nothing in
    // this engine populates them itself.
    std::string group_name{};
    bool group_ready = false;

    friend bool operator==(const OutputPreferences&, const OutputPreferences&) = default;
};

// One endpoint as the platform reports it: its device record, probed at a
// sample rate, and what its own descriptor says.
struct EndpointReading {
    audio::RenderDeviceInfo device{};
    std::expected<audio::SinkAudioCapabilities, audio::EdidError> descriptor =
        std::unexpected(audio::EdidError::kNoBackend);
};

// One endpoint's row, from its device record and its descriptor read.
[[nodiscard]] EndpointFacts endpoint_facts(
    const audio::RenderDeviceInfo& device,
    const std::expected<audio::SinkAudioCapabilities, audio::EdidError>& descriptor);
[[nodiscard]] EndpointFacts endpoint_facts(const EndpointReading& reading);

// Where the readings come from: every render endpoint, probed at a sample
// rate.
using EndpointSource = std::function<std::vector<EndpointReading>(std::uint32_t sample_rate)>;

// This machine's endpoints, through enumerate_render_devices() and
// read_sink_capabilities(). An enumeration that fails gives none.
[[nodiscard]] EndpointSource device_endpoints();

class OutputSelector {
public:
    // `bitstream_output` false: the player this decides for has no
    // passthrough output, so every endpoint is taken to carry no bitstream.
    explicit OutputSelector(EndpointSource source, bool bitstream_output = true);

    void set_preferences(OutputPreferences preferences);
    [[nodiscard]] const OutputPreferences& preferences() const { return preferences_; }

    // Forgets the readings taken so far; the next choice reads them again.
    void refresh();

    // The output for an item with these facts, while the player holds
    // `held` open.
    [[nodiscard]] OutputChoice choose(const ItemFacts& item, const HeldOutput& held = {});

    // The rows for a sample rate, read now if they need to be. Valid until
    // the next call.
    [[nodiscard]] const std::vector<EndpointFacts>& endpoints(std::uint32_t sample_rate,
                                                               const HeldOutput& held = {});

private:
    struct Readings {
        std::vector<EndpointReading> endpoints{};
        // Read since the last refresh; `held_by` is the endpoint that was
        // held open then, empty when none was.
        bool fresh = false;
        std::string held_by{};
    };

    EndpointSource source_;
    bool bitstream_output_ = true;
    OutputPreferences preferences_;
    std::map<std::uint32_t, Readings> readings_;
    std::vector<EndpointFacts> rows_;
};

}  // namespace ac3::hearth
