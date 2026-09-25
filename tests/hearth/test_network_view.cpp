#include <catch2/catch_test_macros.hpp>

#include <string>

#include "network_view.hpp"

// ac3::hearth::to_row()/to_detail() (apps/hearth/engine/network_view.cpp): what
// the Network page's list row and "THIS SINK" panel show, built from
// hand-written SinkFacts. Nothing here opens a socket - see
// tests/hearth/test_network_sinks.cpp for NetworkSinks itself, the way
// test_output_decision.cpp and player.cpp/device_sink.cpp split the same way.
//
// Reason and label text is checked for the substance a person needs, not word
// for word (output_decision's own tests explain why), except where the design
// fixes the exact words (network-pairing.png's "N slots at N-bit, as it
// reports", "not synchronised until paired").

using ac3::hearth::GroupFacts;
using ac3::hearth::GroupMemberFacts;
using ac3::hearth::PairState;
using ac3::hearth::SinkFacts;
using ac3::hearth::SinkKind;
using ac3::hearth::to_detail;
using ac3::hearth::to_group_detail;
using ac3::hearth::to_group_row;
using ac3::hearth::to_row;

namespace {

[[nodiscard]] bool mentions(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

// hearth-s3-kitchen from network-pairing.png: a Hearth sink, paired, both
// roles listed, 8 slots at 32-bit.
[[nodiscard]] SinkFacts hearth_sink() {
    SinkFacts facts;
    facts.id = "hearth-s3-kitchen";
    facts.name = "hearth-s3-kitchen";
    facts.kind = SinkKind::kHearthSink;
    facts.pair_state = PairState::kPaired;
    facts.address = "192.168.1.52";
    facts.port = 8928;
    facts.path = "/sendspin";
    facts.hardware = "ESP32-S3";
    facts.roles = {"_ac3forge_player@v1", "player@v1"};
    facts.data_types = {"ac3", "eac3"};
    facts.codecs = {"pcm", "flac"};
    facts.output_slots = 8;
    facts.output_bit_depth = 32;
    facts.required_lead_time_ms = 38;
    facts.clock_converged = true;
    facts.paired_on = "2026-09-14";
    return facts;
}

}  // namespace

TEST_CASE("network view: a paired Hearth sink's row", "[hearth][network-view]") {
    const auto row = to_row(hearth_sink());
    CHECK(row.id == "hearth-s3-kitchen");
    CHECK(row.icon == "HS");
    CHECK(row.badge == "paired");
    CHECK(row.badge_text == "paired");
    CHECK(mentions(row.subtitle, "Hearth sink"));
    CHECK(mentions(row.subtitle, "ESP32-S3"));
}

TEST_CASE("network view: a paired Hearth sink's detail panel", "[hearth][network-view]") {
    const auto detail = to_detail(hearth_sink());
    CHECK(detail.badge == "paired");
    CHECK(detail.kind_text == "Hearth sink · ESP32-S3");
    CHECK(detail.address == "192.168.1.52:8928 · /sendspin");
    CHECK(detail.roles_text == "_ac3forge_player@v1 · player@v1");
    // The extension spec's own example (planning/hearth-sendspin-extension.md,
    // The role _ac3forge_player@v1): a Hearth sink lists both roles, so
    // "Takes" combines what each accepts.
    CHECK(detail.takes_text == "AC-3 and E-AC-3 · PCM and FLAC");
    CHECK(detail.outputs_text == "8 slots at 32-bit, as it reports");
    CHECK(detail.latency_text == "38 ms, as it reports");
    CHECK(detail.clock_text == "synchronised");
    CHECK(detail.paired_on_text == "2026-09-14");
}

TEST_CASE("network view: a standard player carries only codecs, no data types", "[hearth][network-view]") {
    SinkFacts facts;
    facts.id = "kitchen-speaker";
    facts.name = "Kitchen speaker";
    facts.kind = SinkKind::kStandardPlayer;
    facts.pair_state = PairState::kPaired;
    facts.codecs = {"flac", "pcm"};

    const auto row = to_row(facts);
    CHECK(row.icon == "SP");
    CHECK(mentions(row.subtitle, "Sendspin player"));

    const auto detail = to_detail(facts);
    CHECK(detail.takes_text == "FLAC and PCM");
    CHECK(detail.roles_text.empty());
    CHECK(detail.outputs_text.empty());
}

TEST_CASE("network view: a test sink is named by its device info, not a role", "[hearth][network-view]") {
    SinkFacts facts;
    facts.id = "ac3hearth-testsink-1";
    facts.name = "ac3hearth-testsink-1";
    facts.kind = SinkKind::kTestSink;
    facts.pair_state = PairState::kPaired;

    const auto row = to_row(facts);
    CHECK(row.icon == "TS");
    CHECK(mentions(row.subtitle, "test sink"));
}

TEST_CASE("network view: a sink not yet paired, before hello has arrived", "[hearth][network-view]") {
    SinkFacts facts;
    facts.id = "hearth-s3-study";
    facts.name = "hearth-s3-study";
    facts.address = "192.168.1.60";
    facts.port = 8928;
    facts.pair_state = PairState::kNotPaired;

    const auto row = to_row(facts);
    CHECK(row.badge == "notPaired");
    CHECK(row.badge_text == "not paired");

    const auto detail = to_detail(facts);
    // The design's own words (network-pairing.png, "THIS SINK"): nothing is
    // guessed for a sink that has not said anything yet.
    CHECK(detail.clock_text == "not synchronised until paired");
    CHECK(detail.latency_text.empty());
    CHECK(detail.roles_text.empty());
    CHECK(detail.outputs_text.empty());
}

TEST_CASE("network view: paired but its clock has not converged yet", "[hearth][network-view]") {
    SinkFacts facts = hearth_sink();
    facts.clock_converged = false;
    const auto detail = to_detail(facts);
    CHECK(detail.clock_text == "not synchronised yet");
}

TEST_CASE("network view: paired, but the sink has not reported a lead time yet", "[hearth][network-view]") {
    SinkFacts facts = hearth_sink();
    facts.required_lead_time_ms.reset();
    const auto detail = to_detail(facts);
    CHECK(detail.latency_text == "not reported yet");
}

TEST_CASE("network view: an address with no path", "[hearth][network-view]") {
    SinkFacts facts;
    facts.id = "x";
    facts.address = "192.168.1.60";
    facts.port = 8928;
    const auto detail = to_detail(facts);
    CHECK(detail.address == "192.168.1.60:8928");
}

TEST_CASE("network view: no address known yet", "[hearth][network-view]") {
    SinkFacts facts;
    facts.id = "x";
    const auto detail = to_detail(facts);
    CHECK(detail.address.empty());
}

TEST_CASE("network view: a single data type or codec is not joined with itself", "[hearth][network-view]") {
    SinkFacts facts;
    facts.id = "x";
    facts.kind = SinkKind::kHearthSink;
    facts.data_types = {"eac3"};
    facts.codecs = {"pcm"};
    const auto detail = to_detail(facts);
    CHECK(detail.takes_text == "E-AC-3 · PCM");
}

TEST_CASE("network view: three codecs read as an Oxford list", "[hearth][network-view]") {
    SinkFacts facts;
    facts.id = "x";
    facts.codecs = {"flac", "pcm", "opus"};
    const auto detail = to_detail(facts);
    CHECK(detail.takes_text == "FLAC, PCM and Opus");
}

TEST_CASE("network view: a sink's notice reaches both the row and the detail panel",
          "[hearth][network-view]") {
    SinkFacts facts = hearth_sink();
    facts.notice = "In use by another server.";

    const auto row = to_row(facts);
    CHECK(row.notice == "In use by another server.");

    const auto detail = to_detail(facts);
    CHECK(detail.notice == "In use by another server.");
}

TEST_CASE("network view: no notice is the ordinary, empty case", "[hearth][network-view]") {
    const auto row = to_row(hearth_sink());
    CHECK(row.notice.empty());
    const auto detail = to_detail(hearth_sink());
    CHECK(detail.notice.empty());
}

namespace {

// "Living room" with two Hearth sinks and one Sendspin player, all
// connected, from network-group.png.
GroupFacts living_room() {
    GroupFacts facts;
    facts.id = "group-1";
    facts.name = "Living room";
    GroupMemberFacts kitchen;
    kitchen.sink_id = "hearth-s3-kitchen";
    kitchen.name = "hearth-s3-kitchen";
    kitchen.kind = SinkKind::kHearthSink;
    kitchen.connected = true;
    kitchen.volume = 80;
    kitchen.volume_supported = true;
    kitchen.mute_supported = true;
    kitchen.required_lead_time_ms = 38;
    GroupMemberFacts lounge;
    lounge.sink_id = "hearth-s3-lounge";
    lounge.name = "hearth-s3-lounge";
    lounge.kind = SinkKind::kHearthSink;
    lounge.connected = true;
    lounge.volume = 100;
    lounge.volume_supported = true;
    lounge.mute_supported = true;
    lounge.required_lead_time_ms = 180;
    GroupMemberFacts speaker;
    speaker.sink_id = "kitchen-speaker";
    speaker.name = "Kitchen speaker";
    speaker.kind = SinkKind::kStandardPlayer;
    speaker.connected = true;
    speaker.volume = 64;
    speaker.volume_supported = true;
    speaker.mute_supported = true;
    speaker.required_lead_time_ms = 60;
    facts.members = {kitchen, lounge, speaker};
    return facts;
}

}  // namespace

TEST_CASE("network view: a group's row names its members by kind", "[hearth][network-view]") {
    const auto row = to_group_row(living_room());
    CHECK(row.id == "group-1");
    CHECK(row.name == "Living room");
    CHECK(row.icon == "LR");
    CHECK(row.badge == "group");
    CHECK(row.badge_text == "group");
    CHECK(row.subtitle == "2 Hearth sinks · 1 Sendspin player");
}

TEST_CASE("network view: an empty group's row says so plainly", "[hearth][network-view]") {
    GroupFacts facts;
    facts.id = "group-2";
    facts.name = "New group";
    const auto row = to_group_row(facts);
    CHECK(row.icon == "NG");
    CHECK(row.subtitle == "No members yet");
}

TEST_CASE("network view: a group's editor rows, and its volume as the mean of the members",
          "[hearth][network-view]") {
    const auto detail = to_group_detail(living_room());
    CHECK(detail.id == "group-1");
    CHECK(detail.name == "Living room");
    REQUIRE(detail.members.size() == 3);

    CHECK(detail.members[0].sink_id == "hearth-s3-kitchen");
    CHECK(detail.members[0].volume == 80);
    CHECK(mentions(detail.members[0].gets_text, "E-AC-3"));
    CHECK(detail.members[1].sink_id == "hearth-s3-lounge");
    CHECK(detail.members[2].sink_id == "kitchen-speaker");
    CHECK(mentions(detail.members[2].gets_text, "Stereo"));

    // (80 + 100 + 64) / 3, rounded - state_roles.hpp's own group_volume().
    CHECK(detail.group_volume == 81);
    CHECK_FALSE(detail.group_muted);
    CHECK(detail.members_connected_text == "3 of 3 connected");
    // The largest of 38, 180 and 60.
    CHECK(detail.lead_time_text == "180 ms · the largest a member asks for");
}

TEST_CASE("network view: a disconnected member is shown but does not count toward the group volume or lead time",
          "[hearth][network-view]") {
    GroupFacts facts;
    facts.id = "group-1";
    facts.name = "Downstairs";
    GroupMemberFacts gone;
    // Its own sink has dropped off sinks_ entirely (network_sinks.hpp's own
    // comment on why a member survives that) - name falls back to its id.
    gone.sink_id = "hearth-s3-study";
    gone.name = "hearth-s3-study";
    gone.connected = false;
    GroupMemberFacts here;
    here.sink_id = "hearth-s3-kitchen";
    here.name = "hearth-s3-kitchen";
    here.kind = SinkKind::kHearthSink;
    here.connected = true;
    here.volume = 50;
    here.volume_supported = true;
    here.required_lead_time_ms = 40;
    facts.members = {gone, here};

    const auto detail = to_group_detail(facts);
    REQUIRE(detail.members.size() == 2);
    CHECK_FALSE(detail.members[0].connected);
    CHECK(mentions(detail.members[0].gets_text, "not connected"));
    // Only the connected member's volume counts.
    CHECK(detail.group_volume == 50);
    CHECK(detail.members_connected_text == "1 of 2 connected");
    CHECK(detail.lead_time_text == "40 ms · the largest a member asks for");
}

TEST_CASE("network view: an empty group has nothing to report yet", "[hearth][network-view]") {
    GroupFacts facts;
    facts.id = "group-3";
    facts.name = "Empty";
    const auto detail = to_group_detail(facts);
    CHECK(detail.members.empty());
    CHECK(detail.group_volume == 100);
    CHECK_FALSE(detail.group_muted);
    CHECK(detail.members_connected_text == "0 of 0 connected");
    CHECK(detail.lead_time_text.empty());
}

TEST_CASE("network view: the connection in words", "[hearth][network-view]") {
    using ac3::hearth::link_text;
    using ac3::hearth::SinkLink;
    SinkFacts facts = hearth_sink();

    facts.link = SinkLink::kConnected;
    CHECK(link_text(facts) == "connected");
    CHECK(to_row(facts).connected);
    facts.pairing_active = true;
    CHECK(mentions(link_text(facts), "pairing"));
    facts.wants_code = true;
    CHECK(mentions(link_text(facts), "code"));
    facts.pairing_active = false;
    facts.wants_code = false;

    facts.link = SinkLink::kConnecting;
    CHECK(mentions(link_text(facts), "connecting"));
    CHECK_FALSE(to_row(facts).connected);
    facts.pairing_requested = true;
    CHECK(mentions(link_text(facts), "pair"));
    facts.pairing_requested = false;

    // A connection that ended is being made again; a dial nothing answered is not answering.
    facts.link = SinkLink::kRetrying;
    facts.failed_dials = 1;
    CHECK(mentions(link_text(facts), "reconnecting"));
    facts.dial_failed = true;
    CHECK(mentions(link_text(facts), "not answering"));
    facts.failed_dials = 4;
    CHECK(mentions(link_text(facts), "4 times"));

    // Idle: a paired sink says it is not connected, unless the notice already says why.
    facts.link = SinkLink::kIdle;
    CHECK(mentions(link_text(facts), "not connected"));
    facts.held_elsewhere = true;
    CHECK(link_text(facts).empty());
    facts.pair_state = PairState::kNotPaired;
    facts.held_elsewhere = false;
    CHECK(link_text(facts).empty());
}

TEST_CASE("network view: what the page may offer for a sink", "[hearth][network-view]") {
    using ac3::hearth::SinkLink;
    SinkFacts paired = hearth_sink();
    paired.link = SinkLink::kIdle;
    auto detail = to_detail(paired);
    CHECK(detail.can_connect);
    CHECK_FALSE(detail.can_pair);
    CHECK(detail.page_url == "http://192.168.1.52/");
    CHECK(detail.pairing == "none");
    paired.link = SinkLink::kConnected;
    CHECK_FALSE(to_detail(paired).can_connect);

    SinkFacts unpaired = hearth_sink();
    unpaired.pair_state = PairState::kNotPaired;
    unpaired.link = SinkLink::kIdle;
    detail = to_detail(unpaired);
    CHECK(detail.can_pair);
    CHECK_FALSE(detail.can_connect);

    unpaired.pairing_requested = true;
    detail = to_detail(unpaired);
    CHECK(detail.pairing == "requested");
    CHECK_FALSE(detail.can_pair);
    unpaired.pairing_requested = false;
    unpaired.pairing_active = true;
    CHECK(to_detail(unpaired).pairing == "active");
    unpaired.wants_code = true;
    CHECK(to_detail(unpaired).pairing == "code");

    // A sink that offers no code the page can take cannot be paired from it.
    SinkFacts fixed = hearth_sink();
    fixed.pair_state = PairState::kNotPaired;
    fixed.offers_code_pairing = false;
    CHECK_FALSE(to_detail(fixed).can_pair);
}

TEST_CASE("network view: a group row says how many members are connected", "[hearth][network-view]") {
    GroupFacts group;
    group.id = "group-1";
    group.name = "Downstairs";
    auto row = to_group_row(group);
    CHECK(row.members_text.empty());
    CHECK(mentions(row.subtitle, "No members"));
    CHECK_FALSE(row.ready);

    GroupMemberFacts connected;
    connected.sink_id = "a";
    connected.connected = true;
    GroupMemberFacts away;
    away.sink_id = "b";
    group.members = {connected, away};
    row = to_group_row(group);
    CHECK(row.members_text == "1 of 2 connected");
    CHECK(row.ready);
}
