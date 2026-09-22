#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/audio/passthrough.hpp"
#include "output_decision.hpp"

// ac3::hearth::choose_output's case table (apps/hearth/engine/output_decision.cpp).
//
// A2's exit for this is "the output decision's case table runs with no sound
// card", and that is the point of the decision being pure: every row here is
// a room this machine does not have - a receiver that takes E-AC-3, one that
// takes only AC-3, one that is switched off, a platform that cannot read a
// descriptor at all - put in front of the same function the application
// calls. Nothing here opens a device or makes a sound.
//
// The reason string is checked for the substance a person needs, not word for
// word: these tests should not have to be rewritten because a sentence was
// reworded, but a reason that stops saying WHY is a defect the Output screen
// would inherit.

namespace {

using ac3::audio::BitstreamFormat;
using ac3::hearth::CapabilitySource;
using ac3::hearth::choose_output;
using ac3::hearth::EndpointFacts;
using ac3::hearth::OutputMode;
using ac3::hearth::OutputRequest;

EndpointFacts receiver(std::string name, bool eac3, bool ac3, bool is_default = false) {
    return EndpointFacts{.id = name + "-id",
                         .name = std::move(name),
                         .is_default = is_default,
                         .accepts_ac3 = ac3,
                         .accepts_eac3 = eac3,
                         .accepts_pcm = true,
                         .source = CapabilitySource::kDescriptor,
                         .channels = 8,
                         .speakers = 0};
}

EndpointFacts analogue(std::string name, std::uint16_t channels, bool is_default = false) {
    return EndpointFacts{.id = name + "-id",
                         .name = std::move(name),
                         .is_default = is_default,
                         .accepts_ac3 = false,
                         .accepts_eac3 = false,
                         .accepts_pcm = true,
                         .source = CapabilitySource::kProbe,
                         .channels = channels,
                         .speakers = 0};
}

bool mentions(const std::string& reason, std::string_view needle) {
    return reason.find(needle) != std::string::npos;
}

// A complete request, since a designated initialiser that names only some of
// OutputRequest's fields is an error under this project's warning set. Each
// case takes one of these and changes the one thing it is about.
OutputRequest asking(std::span<const EndpointFacts> endpoints,
                     std::optional<BitstreamFormat> stream) {
    return OutputRequest{.endpoints = endpoints,
                         .stream = stream,
                         .has_objects = false,
                         .pinned = std::nullopt,
                         .preferred_endpoint_id = {},
                         .group_name = {},
                         .group_ready = false,
                         .follow_sink = true,
                         .transcode_available = true};
}

}  // namespace

TEST_CASE("output decision: a sink that takes the stream gets it untouched",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{receiver("Onkyo", /*eac3=*/true, /*ac3=*/true),
                                               analogue("Speakers", 2, /*is_default=*/true)};
    auto ask = asking(endpoints, BitstreamFormat::kEac3);
    ask.has_objects = true;
    const auto choice = choose_output(ask);

    CHECK(choice.mode == OutputMode::kBitstream);
    CHECK(choice.endpoint_name == "Onkyo");
    // Bitstreaming an object stream sends the objects on rather than
    // rendering them, and the screen should say so.
    CHECK(mentions(choice.reason, "object layer travels with it"));
    CHECK_FALSE(choice.reason.empty());
}

TEST_CASE("output decision: AC-3 only means a transcode, and says what that costs",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{receiver("Old AVR", /*eac3=*/false, /*ac3=*/true)};
    auto ask = asking(endpoints, BitstreamFormat::kEac3);
    ask.has_objects = true;
    const auto choice = choose_output(ask);

    CHECK(choice.mode == OutputMode::kBitstreamAsAc3);
    CHECK(choice.endpoint_name == "Old AVR");
    CHECK(mentions(choice.reason, "takes AC-3 but not E-AC-3"));
    CHECK(mentions(choice.reason, "cannot survive"));

    // Without a transcode in the build, the same sink gets a decode instead,
    // and the reason is the sink's limit rather than an offer.
    auto without = asking(endpoints, BitstreamFormat::kEac3);
    without.transcode_available = false;
    const auto no_transcode = choose_output(without);
    CHECK(no_transcode.mode == OutputMode::kLocalPcm);
    CHECK(mentions(no_transcode.reason, "No output takes E-AC-3"));
}

TEST_CASE("output decision: nothing that bitstreams means a decode here",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{analogue("Headphones", 2, /*is_default=*/true),
                                               analogue("USB DAC", 8)};
    const auto choice = choose_output(asking(endpoints, BitstreamFormat::kAc3));

    CHECK(choice.mode == OutputMode::kLocalPcm);
    // The default endpoint, not the widest: what the machine is set to play
    // through is the answer a player should take without being asked.
    CHECK(choice.endpoint_name == "Headphones");
    CHECK(mentions(choice.reason, "No output takes AC-3"));
    CHECK(mentions(choice.reason, "PCM"));
}

TEST_CASE("output decision: follow=off refuses instead of decoding",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{analogue("Speakers", 2, /*is_default=*/true)};
    auto ask = asking(endpoints, BitstreamFormat::kEac3);
    ask.follow_sink = false;
    const auto choice = choose_output(ask);

    CHECK(choice.mode == OutputMode::kNone);
    CHECK(mentions(choice.reason, "follow=off"));
    CHECK(choice.endpoint_id.empty());
}

TEST_CASE("output decision: a receiver that is off is not the same as a platform that cannot look",
          "[hearth][output-decision]") {
    // Gap 6. Both endpoints answer no to everything; what differs is why,
    // and the difference has to reach the reason - "your receiver is off" and
    // "this machine cannot read EDID" are different problems.
    EndpointFacts silent = analogue("HDMI", 0, /*is_default=*/true);
    silent.accepts_pcm = false;
    silent.source = CapabilitySource::kNoDescriptor;

    EndpointFacts unread = silent;
    unread.source = CapabilitySource::kNoReader;

    const std::vector<EndpointFacts> off{silent};
    const auto a = choose_output(asking(off, BitstreamFormat::kEac3));
    CHECK(mentions(a.reason, "reports no descriptor"));
    CHECK(mentions(a.reason, "off or on another input"));

    const std::vector<EndpointFacts> blind{unread};
    const auto b = choose_output(asking(blind, BitstreamFormat::kEac3));
    CHECK(mentions(b.reason, "cannot read a sink's descriptor"));
    CHECK_FALSE(mentions(b.reason, "off or on another input"));
}

TEST_CASE("output decision: the endpoint the user named is the one considered",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{receiver("Onkyo", /*eac3=*/true, /*ac3=*/true),
                                               analogue("Speakers", 2, /*is_default=*/true)};

    // Named the analogue output with a bitstreamable stream: it is taken with
    // the best it can do rather than the receiver being chosen behind the
    // user's back.
    auto ask = asking(endpoints, BitstreamFormat::kEac3);
    ask.preferred_endpoint_id = "Speakers-id";
    const auto choice = choose_output(ask);
    CHECK(choice.mode == OutputMode::kLocalPcm);
    CHECK(choice.endpoint_name == "Speakers");

    // And named the receiver, it bitstreams there even though the default
    // endpoint is something else.
    auto by_name = asking(endpoints, BitstreamFormat::kEac3);
    by_name.preferred_endpoint_id = "Onkyo-id";
    const auto named = choose_output(by_name);
    CHECK(named.mode == OutputMode::kBitstream);
    CHECK(named.endpoint_name == "Onkyo");
}

TEST_CASE("output decision: a pinned mode is honoured, or the reason says what stopped it",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{receiver("Onkyo", /*eac3=*/true, /*ac3=*/true),
                                               analogue("Speakers", 2, /*is_default=*/true)};

    // Pinned to a decode with a receiver sitting right there.
    auto wants_pcm = asking(endpoints, BitstreamFormat::kEac3);
    wants_pcm.pinned = OutputMode::kLocalPcm;
    const auto pinned_pcm = choose_output(wants_pcm);
    CHECK(pinned_pcm.mode == OutputMode::kLocalPcm);
    CHECK(pinned_pcm.endpoint_name == "Speakers");

    // Pinned to a bitstream for an item that carries none.
    auto wants_bitstream = asking(endpoints, std::nullopt);
    wants_bitstream.pinned = OutputMode::kBitstream;
    const auto impossible = choose_output(wants_bitstream);
    CHECK(impossible.mode == OutputMode::kNone);
    CHECK(mentions(impossible.reason, "nothing IEC 61937 can wrap"));

    // Pinned to a bitstream on a machine with nothing that takes one: the
    // decode happens and the reason says why it is not what was asked for.
    const std::vector<EndpointFacts> analogue_only{analogue("Speakers", 6, /*is_default=*/true)};
    auto wants_bitstream_here = asking(analogue_only, BitstreamFormat::kAc3);
    wants_bitstream_here.pinned = OutputMode::kBitstream;
    const auto fell_back = choose_output(wants_bitstream_here);
    CHECK(fell_back.mode == OutputMode::kLocalPcm);
    CHECK(mentions(fell_back.reason, "No available output takes AC-3"));
}

TEST_CASE("output decision: a pinned AC-3 bitstream transcodes only what needs it",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{receiver("Old AVR", /*eac3=*/false, /*ac3=*/true),
                                               analogue("Speakers", 2, /*is_default=*/true)};

    // AC-3 is sent as it is, with or without a transcode to hand.
    auto ac3 = asking(endpoints, BitstreamFormat::kAc3);
    ac3.pinned = OutputMode::kBitstreamAsAc3;
    ac3.transcode_available = false;
    const auto untouched = choose_output(ac3);
    CHECK(untouched.mode == OutputMode::kBitstream);
    CHECK(untouched.endpoint_name == "Old AVR");

    // E-AC-3 needs the transcode.
    auto eac3 = asking(endpoints, BitstreamFormat::kEac3);
    eac3.pinned = OutputMode::kBitstreamAsAc3;
    CHECK(choose_output(eac3).mode == OutputMode::kBitstreamAsAc3);

    // Without one, the receiver still takes AC-3, and the reason says what
    // was missing was the transcode, not an output.
    eac3.transcode_available = false;
    const auto decoded = choose_output(eac3);
    CHECK(decoded.mode == OutputMode::kLocalPcm);
    CHECK(mentions(decoded.reason, "An output takes AC-3, but E-AC-3 cannot be transcoded"));
    CHECK_FALSE(mentions(decoded.reason, "No available output"));

    eac3.preferred_endpoint_id = "Old AVR-id";
    CHECK(mentions(choose_output(eac3).reason, "The chosen output takes AC-3"));

    // With no output taking AC-3 at all, that is what it says.
    const std::vector<EndpointFacts> analogue_only{analogue("Speakers", 6, /*is_default=*/true)};
    auto nowhere = asking(analogue_only, BitstreamFormat::kEac3);
    nowhere.pinned = OutputMode::kBitstreamAsAc3;
    nowhere.transcode_available = false;
    CHECK(mentions(choose_output(nowhere).reason, "No available output takes AC-3"));
}

TEST_CASE("output decision: a selected group is played to, and a group that is not ready says so",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{analogue("Speakers", 2, /*is_default=*/true)};

    auto to_group = asking(endpoints, BitstreamFormat::kEac3);
    to_group.group_name = "Kitchen";
    to_group.group_ready = true;
    const auto playing = choose_output(to_group);
    CHECK(playing.mode == OutputMode::kNetworkGroup);
    CHECK(mentions(playing.reason, "Kitchen"));
    // What travels to a sink is the stream, not PCM - that is the whole point
    // of the extension role, and the screen should say it.
    CHECK(mentions(playing.reason, "not PCM"));

    // Not ready: the local output takes over, with the group's state said
    // first rather than silently dropped.
    auto group_down = asking(endpoints, BitstreamFormat::kEac3);
    group_down.group_name = "Kitchen";
    const auto not_ready = choose_output(group_down);
    CHECK(not_ready.mode == OutputMode::kLocalPcm);
    CHECK(mentions(not_ready.reason, "not ready"));

    // Pinned to the group, a group that is not ready is a refusal: the user
    // asked for those speakers, not these.
    auto pinned_group = asking(endpoints, BitstreamFormat::kEac3);
    pinned_group.pinned = OutputMode::kNetworkGroup;
    pinned_group.group_name = "Kitchen";
    const auto pinned = choose_output(pinned_group);
    CHECK(pinned.mode == OutputMode::kNone);
    CHECK(mentions(pinned.reason, "not ready"));
}

TEST_CASE("output decision: no outputs at all, and an item that cannot be bitstreamed",
          "[hearth][output-decision]") {
    const auto nothing = choose_output(asking({}, BitstreamFormat::kAc3));
    CHECK(nothing.mode == OutputMode::kNone);
    CHECK(mentions(nothing.reason, "no output at all"));
    CHECK(mentions(nothing.reason, "network sinks"));

    // A WAV, or an AC-4 stream: no IEC 61937 format covers it, so the only
    // question is which local output decodes it.
    const std::vector<EndpointFacts> endpoints{receiver("Onkyo", /*eac3=*/true, /*ac3=*/true,
                                                        /*is_default=*/true)};
    const auto decoded = choose_output(asking(endpoints, std::nullopt));
    CHECK(decoded.mode == OutputMode::kLocalPcm);
    CHECK(decoded.endpoint_name == "Onkyo");
    // Nothing about IEC 61937 belongs in that reason: there was never a
    // bitstream to refuse.
    CHECK_FALSE(mentions(decoded.reason, "IEC 61937"));
}

TEST_CASE("output decision: holding the default endpoint exclusively is called out",
          "[hearth][output-decision]") {
    const std::vector<EndpointFacts> endpoints{
        receiver("Onkyo", /*eac3=*/true, /*ac3=*/true, /*is_default=*/true)};
    const auto choice = choose_output(asking(endpoints, BitstreamFormat::kEac3));

    CHECK(choice.mode == OutputMode::kBitstream);
    // Not a refusal - a player asked to bitstream to a receiver is usually
    // being asked about exactly this endpoint - but the caller has to be
    // ready for the open to fail, and whoever reads the screen should know
    // their music is about to stop.
    CHECK(mentions(choice.reason, "default output"));
    CHECK(mentions(choice.reason, "refused"));
}

TEST_CASE("output decision: every mode and capability source describes itself",
          "[hearth][output-decision]") {
    for (const auto mode : {OutputMode::kBitstream, OutputMode::kBitstreamAsAc3,
                            OutputMode::kLocalPcm, OutputMode::kNetworkGroup, OutputMode::kNone}) {
        const std::string_view text = ac3::hearth::describe(mode);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown output mode");
    }
    for (const auto source : {CapabilitySource::kDescriptor, CapabilitySource::kProbe,
                              CapabilitySource::kNoDescriptor, CapabilitySource::kNoReader}) {
        const std::string_view text = ac3::hearth::describe(source);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown capability source");
    }
}
