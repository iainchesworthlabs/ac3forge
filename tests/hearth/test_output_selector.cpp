#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/sink_capabilities.hpp"
#include "output_selector.hpp"

// ac3::hearth::OutputSelector (apps/hearth/engine/output_selector.cpp): the
// rows the output decision is asked about, and when they are read.
//
// The decision itself is test_output_decision.cpp's. What is checked here is
// how an endpoint's two readings - the platform's probe and the sink's own
// descriptor - become one row, and that the rows are the machine as it was
// last read: once per rate, again after refresh(), never from an empty
// enumeration, and never from a probe of the endpoint the player holds.

namespace {

using ac3::audio::BitstreamFormat;
using ac3::audio::EdidError;
using ac3::audio::RenderDeviceInfo;
using ac3::audio::SinkAudioCapabilities;
using ac3::hearth::CapabilitySource;
using ac3::hearth::EndpointReading;
using ac3::hearth::HeldOutput;
using ac3::hearth::ItemFacts;
using ac3::hearth::OutputMode;
using ac3::hearth::OutputPreferences;
using ac3::hearth::OutputSelector;
using ac3::hearth::endpoint_facts;

RenderDeviceInfo hdmi(bool ac3, bool eac3) {
    RenderDeviceInfo device;
    device.id = "hdmi-id";
    device.name = "HDMI";
    device.is_default = false;
    device.supports_ac3_passthrough = ac3;
    device.supports_eac3_passthrough = eac3;
    device.supports_exclusive_pcm = true;
    device.channels = 8;
    device.speakers = 0x63F;
    return device;
}

SinkAudioCapabilities descriptor(bool ac3, bool eac3) {
    SinkAudioCapabilities sink;
    sink.pcm = true;
    sink.ac3 = ac3;
    sink.eac3 = eac3;
    return sink;
}

EndpointReading probed(bool ac3, bool eac3) {
    return EndpointReading{.device = hdmi(ac3, eac3),
                           .descriptor = std::unexpected(EdidError::kNoBackend)};
}

ItemFacts eac3_item(std::uint32_t rate = 48000) {
    ItemFacts facts;
    facts.stream = BitstreamFormat::kEac3;
    facts.sample_rate = rate;
    facts.channels = 6;
    return facts;
}

// The player holding an E-AC-3 link open on the HDMI endpoint.
HeldOutput eac3_link(std::uint32_t rate = 48000) {
    return HeldOutput{.mode = OutputMode::kBitstream,
                      .endpoint_id = "hdmi-id",
                      .sample_rate = rate,
                      .stream = BitstreamFormat::kEac3};
}

bool mentions(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

}  // namespace

TEST_CASE("output selector: a row carries a format only when the probe and the descriptor both do",
          "[hearth][output-decision]") {
    // The descriptor says what the receiver decodes, the probe what this
    // machine can open.
    const auto both = endpoint_facts(hdmi(true, true), descriptor(true, false));
    CHECK(both.source == CapabilitySource::kDescriptor);
    CHECK(both.accepts_ac3);
    CHECK_FALSE(both.accepts_eac3);
    CHECK(both.accepts_pcm);
    CHECK(both.id == "hdmi-id");
    CHECK(both.name == "HDMI");
    CHECK(both.channels == 8);
    CHECK(both.speakers == 0x63F);

    const auto unopened = endpoint_facts(hdmi(false, true), descriptor(true, true));
    CHECK_FALSE(unopened.accepts_ac3);
    CHECK(unopened.accepts_eac3);

    // No descriptor: nothing is known of the receiver, whatever opened.
    const auto silent = endpoint_facts(hdmi(true, true), std::unexpected(EdidError::kNoEdid));
    CHECK(silent.source == CapabilitySource::kNoDescriptor);
    CHECK_FALSE(silent.accepts_ac3);
    CHECK_FALSE(silent.accepts_eac3);
    CHECK(silent.accepts_pcm);

    // No reader: the probe is all there is.
    const auto unread = endpoint_facts(probed(true, false));
    CHECK(unread.source == CapabilitySource::kNoReader);
    CHECK(unread.accepts_ac3);
    CHECK_FALSE(unread.accepts_eac3);

    // A reader that could not answer for this endpoint leaves the probe's.
    for (const auto error : {EdidError::kDeviceNotFound, EdidError::kParseFailed}) {
        const auto answered = endpoint_facts(hdmi(false, true), std::unexpected(error));
        CHECK(answered.source == CapabilitySource::kProbe);
        CHECK_FALSE(answered.accepts_ac3);
        CHECK(answered.accepts_eac3);
    }
}

TEST_CASE("output selector: rows are read once a rate, and again after a refresh",
          "[hearth][output-decision]") {
    std::vector<std::uint32_t> asked;
    bool eac3 = true;
    OutputSelector selector{[&](std::uint32_t rate) {
        asked.push_back(rate);
        return std::vector<EndpointReading>{probed(true, eac3)};
    }};

    auto choice = selector.choose(eac3_item());
    CHECK(choice.mode == OutputMode::kBitstream);
    CHECK(choice.endpoint_id == "hdmi-id");
    CHECK(selector.choose(eac3_item()).mode == OutputMode::kBitstream);
    CHECK(asked == std::vector<std::uint32_t>{48000});

    // Another rate is probed on its own; an unprobed item is asked about at
    // 48 kHz, which is already read.
    CHECK(selector.choose(eac3_item(44100)).mode == OutputMode::kBitstream);
    CHECK(selector.choose(eac3_item(0)).mode == OutputMode::kBitstream);
    CHECK(asked == std::vector<std::uint32_t>{48000, 44100});

    // The receiver changes; until the rows are read again, nothing knows.
    eac3 = false;
    CHECK(selector.choose(eac3_item()).mode == OutputMode::kBitstream);
    selector.refresh();
    choice = selector.choose(eac3_item());
    CHECK(asked == std::vector<std::uint32_t>{48000, 44100, 48000});
    // An endpoint that takes AC-3 only is transcoded to.
    CHECK(choice.mode == OutputMode::kBitstreamAsAc3);
    CHECK(choice.endpoint_id == "hdmi-id");
    CHECK(mentions(choice.reason, "transcoded to AC-3"));
}

TEST_CASE("output selector: a transcode is offered at the rates AC-3 has, over a link",
          "[hearth][output-decision]") {
    const auto source = [](std::uint32_t) {
        return std::vector<EndpointReading>{probed(/*ac3=*/true, /*eac3=*/false)};
    };
    OutputSelector selector{source};
    CHECK(selector.choose(eac3_item(48000)).mode == OutputMode::kBitstreamAsAc3);
    CHECK(selector.choose(eac3_item(44100)).mode == OutputMode::kBitstreamAsAc3);
    CHECK(selector.choose(eac3_item(32000)).mode == OutputMode::kBitstreamAsAc3);
    // AC-3 has no 24 kHz, so an E-AC-3 item there is decoded.
    const auto half = selector.choose(eac3_item(24000));
    CHECK(half.mode == OutputMode::kLocalPcm);
    CHECK(mentions(half.reason, "No output takes E-AC-3"));
    // AC-3 itself needs no transcode.
    ItemFacts ac3 = eac3_item();
    ac3.stream = BitstreamFormat::kAc3;
    CHECK(selector.choose(ac3).mode == OutputMode::kBitstream);

    // With no link, nothing is transcoded either.
    OutputSelector linkless{source, /*bitstream_output=*/false};
    CHECK(linkless.choose(eac3_item()).mode == OutputMode::kLocalPcm);
}

TEST_CASE("output selector: the endpoint the player holds is not judged by a probe that cannot open it",
          "[hearth][output-decision]") {
    int reads = 0;
    bool busy = false;
    bool receiver_takes_eac3 = true;
    const auto source = [&](std::uint32_t) {
        ++reads;
        // A device held open reads as refusing everything; its descriptor
        // is read as ever.
        EndpointReading reading{.device = hdmi(!busy, !busy),
                                .descriptor = descriptor(true, receiver_takes_eac3)};
        return std::vector<EndpointReading>{reading};
    };

    SECTION("a refresh while bitstreaming keeps the item on its link") {
        OutputSelector selector{source};
        CHECK(selector.choose(eac3_item()).mode == OutputMode::kBitstream);
        busy = true;
        selector.refresh();
        const auto held = selector.choose(eac3_item(), eac3_link());
        CHECK(held.mode == OutputMode::kBitstream);
        CHECK(held.endpoint_id == "hdmi-id");
        CHECK(reads == 2);
        // Read while held, so read again once the link has gone - and by
        // then the device is free.
        CHECK(selector.choose(eac3_item(), eac3_link()).mode == OutputMode::kBitstream);
        CHECK(reads == 2);
        busy = false;
        CHECK(selector.choose(eac3_item()).mode == OutputMode::kBitstream);
        CHECK(reads == 3);
        CHECK(selector.choose(eac3_item()).mode == OutputMode::kBitstream);
        CHECK(reads == 3);
    }

    SECTION("the descriptor still says when the receiver changes") {
        OutputSelector selector{source};
        CHECK(selector.choose(eac3_item()).mode == OutputMode::kBitstream);
        busy = true;
        receiver_takes_eac3 = false;
        selector.refresh();
        // It still takes AC-3, so the item is transcoded to that.
        CHECK(selector.choose(eac3_item(), eac3_link()).mode == OutputMode::kBitstreamAsAc3);
    }

    SECTION("an endpoint never read free is taken to carry what its link carries") {
        busy = true;
        OutputSelector selector{source};
        CHECK(selector.choose(eac3_item(), eac3_link()).mode == OutputMode::kBitstream);
        // At another rate the link proves nothing.
        CHECK(selector.choose(eac3_item(44100), eac3_link()).mode == OutputMode::kLocalPcm);
        // What it once proved is kept, as a free reading would be.
        selector.refresh();
        CHECK(selector.choose(eac3_item(), eac3_link()).mode == OutputMode::kBitstream);
        // And a decoded output proves nothing about bitstreams, whatever
        // stream it says it has.
        OutputSelector unproven{source};
        const HeldOutput decoding{.mode = OutputMode::kLocalPcm,
                                  .endpoint_id = "hdmi-id",
                                  .sample_rate = 48000,
                                  .stream = BitstreamFormat::kEac3};
        CHECK(unproven.choose(eac3_item(), decoding).mode == OutputMode::kLocalPcm);
    }

    SECTION("another endpoint held leaves this one's probe alone") {
        OutputSelector selector{source};
        busy = true;
        HeldOutput elsewhere = eac3_link();
        elsewhere.endpoint_id = "speakers-id";
        CHECK(selector.choose(eac3_item(), elsewhere).mode == OutputMode::kLocalPcm);
    }
}

TEST_CASE("output selector: an enumeration that finds nothing keeps the last list",
          "[hearth][output-decision]") {
    int reads = 0;
    bool fails = false;
    OutputSelector selector{[&](std::uint32_t) {
        ++reads;
        return fails ? std::vector<EndpointReading>{} : std::vector<EndpointReading>{probed(true, true)};
    }};
    CHECK(selector.choose(eac3_item()).mode == OutputMode::kBitstream);

    fails = true;
    selector.refresh();
    CHECK(selector.choose(eac3_item()).mode == OutputMode::kBitstream);
    CHECK(reads == 2);
    // Not kept as a reading: the next decision looks again.
    fails = false;
    CHECK(selector.choose(eac3_item()).mode == OutputMode::kBitstream);
    CHECK(reads == 3);

    // With nothing ever found, there is nothing to play to.
    OutputSelector empty{[](std::uint32_t) { return std::vector<EndpointReading>{}; }};
    const auto nothing = empty.choose(eac3_item());
    CHECK(nothing.mode == OutputMode::kNone);
    CHECK(mentions(nothing.reason, "reports no output at all"));
}

TEST_CASE("output selector: a player with no passthrough output is decided for as one",
          "[hearth][output-decision]") {
    OutputSelector selector{[](std::uint32_t) { return std::vector<EndpointReading>{probed(true, true)}; },
                            /*bitstream_output=*/false};
    const auto choice = selector.choose(eac3_item());
    CHECK(choice.mode == OutputMode::kLocalPcm);
    CHECK(choice.endpoint_id == "hdmi-id");
}

TEST_CASE("output selector: the Output screen's choices reach the decision",
          "[hearth][output-decision]") {
    RenderDeviceInfo speakers;
    speakers.id = "speakers-id";
    speakers.name = "Speakers";
    speakers.is_default = true;
    speakers.channels = 2;
    const std::vector<EndpointReading> readings{
        probed(true, true),
        EndpointReading{.device = speakers, .descriptor = std::unexpected(EdidError::kNoBackend)}};
    OutputSelector selector{[&](std::uint32_t) { return readings; }};
    CHECK(selector.preferences() == OutputPreferences{});

    // Automatic: the stream as it is, to the endpoint that takes it.
    CHECK(selector.choose(eac3_item()).endpoint_id == "hdmi-id");

    // A decode, pinned.
    selector.set_preferences(OutputPreferences{
        .pinned = OutputMode::kLocalPcm, .endpoint_id = {}, .follow_sink = true});
    auto choice = selector.choose(eac3_item());
    CHECK(choice.mode == OutputMode::kLocalPcm);
    CHECK(choice.endpoint_id == "speakers-id");

    // An endpoint chosen by hand, which cannot take the stream, with
    // follow=off: refused rather than decoded.
    selector.set_preferences(OutputPreferences{
        .pinned = std::nullopt, .endpoint_id = "speakers-id", .follow_sink = false});
    choice = selector.choose(eac3_item());
    CHECK(choice.mode == OutputMode::kNone);
    CHECK(mentions(choice.reason, "follow=off"));

    // A WAV is decoded, whatever else was asked.
    selector.set_preferences(OutputPreferences{});
    ItemFacts wav;
    wav.sample_rate = 48000;
    wav.channels = 2;
    CHECK(selector.choose(wav).mode == OutputMode::kLocalPcm);
}
