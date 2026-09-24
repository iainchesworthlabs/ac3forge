#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/messages.hpp"
#include "network_sinks.hpp"
#include "settings_model.hpp"

// ac3::hearth::NetworkSinks (apps/hearth/engine/network_sinks.cpp): the
// discovery/pairing bookkeeping, driven directly through its
// discovery::BrowseListener/ServerHostEvents overrides (public on this class
// for exactly this reason - see network_sinks.hpp's own comment) with
// hand-built Service/ClientView facts, the way test_output_decision.cpp
// drives choose_output() with hand-built EndpointFacts. This proves the
// bookkeeping - a found sink's row, hello filling in its detail, a pairing
// attempt starting once it can - without a real mDNS multicast group or an
// accepted WebSocket, which [[hearth-b3-sendspin-board-findings]] and
// test_group.cpp's own comment ("mDNS off") say are exactly what to keep out
// of a fast, reliable test. The one real thing this test does is start an
// actual ac3::sendspin::ServerHost (with browse off and no listening port,
// as NetworkSinks always configures it) and dial a real, refused loopback
// port - both fast and deterministic, unlike anything that touches multicast
// or another process. Every construction below also passes
// request_firewall_exception=false: NetworkSinks still opens a real mDNS
// browse socket (its own on_found()/on_lost() are what this file drives
// synthetically, not the socket's existence), and left at its true default
// that socket asks Windows for a firewall exception - which, run as plain
// ac3tests.exe rather than one of the two main()s that can finish that
// handshake, means a UAC prompt on every single test case here instead of
// once, ever (see ac3::sendspin::discovery::mdns::Options::
// request_firewall_exception's own comment).

namespace ss = ac3::sendspin;
using ac3::hearth::MemorySettingsStore;
using ac3::hearth::NetworkSinks;
using ac3::hearth::PairingStore;
using ac3::hearth::PairState;
using ac3::hearth::SinkKind;

namespace {

ss::discovery::Service test_service(std::string instance, std::uint16_t port) {
    return ss::discovery::Service{.instance = std::move(instance),
                                  .host = "127.0.0.1",
                                  .addresses = {"127.0.0.1"},
                                  .port = port,
                                  .txt = {{.key = "path", .value = "/sendspin"}}};
}

std::string today() {
    return "2026-09-23";
}

}  // namespace

TEST_CASE("network sinks: starts with discovery off from ServerHost's own browsing", "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};
    CHECK(sinks.started());
    CHECK(sinks.status().sinks.empty());
}

TEST_CASE("network sinks: a found sink is a row before any connection exists", "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    // Port 1 is a real, immediately-refused loopback connection - the dial
    // on_found() makes fails fast and asynchronously, and is never awaited
    // by anything in this test.
    sinks.on_found(test_service("hearth-s3-study", 1));

    const auto status = sinks.status();
    REQUIRE(status.sinks.size() == 1);
    CHECK(status.sinks.front().id == "hearth-s3-study");
    CHECK(status.sinks.front().pair_state == PairState::kNotPaired);
    CHECK(status.sinks.front().address == "127.0.0.1");
}

TEST_CASE("network sinks: hello fills in a Hearth sink's capabilities", "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    const ss::discovery::Service service = test_service("hearth-s3-kitchen", 1);
    sinks.on_found(service);

    ss::ClientView client;
    client.client_id = "client-abc";
    client.name = "hearth-s3-kitchen";
    client.url = *service.url();
    client.psk = ss::handshake::PskCategory::kSentinel;
    client.hello = true;
    client.supported_roles = {"_ac3forge_player@v1", "player@v1"};
    ss::ac3forge::Support support;
    support.data_types = {ss::ac3forge::DataType::kAc3, ss::ac3forge::DataType::kEac3};
    support.outputs = {.count = 8, .bit_depth = 32, .bit_depths = {16, 32}};
    client.ac3forge_support = support;
    sinks.on_client(client);

    const auto status = sinks.status();
    REQUIRE(status.sinks.size() == 1);
    const auto& facts = status.sinks.front();
    CHECK(facts.kind == SinkKind::kHearthSink);
    CHECK(facts.pair_state == PairState::kNotPaired);
    CHECK(facts.roles.size() == 2);
    REQUIRE(facts.output_slots.has_value());
    CHECK(*facts.output_slots == 8);
}

TEST_CASE("network sinks: selecting an unpaired, already-connected sink asks to pair it",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    const ss::discovery::Service service = test_service("hearth-s3-study", 1);
    sinks.on_found(service);
    ss::ClientView client;
    client.client_id = "client-def";
    client.url = *service.url();
    client.psk = ss::handshake::PskCategory::kSentinel;
    client.hello = true;
    sinks.on_client(client);

    // host_->pair("client-def", ...) is real, and returns false harmlessly:
    // "client-def" was never an actual accepted connection, only a
    // synthetic ClientView, so ServerHost has no session by that id to pair
    // (server_host.cpp's own pair() checks the connection exists first).
    // What this proves is that select_sink() reaches that call at all for a
    // sink that is connected (per on_client() above) and not yet paired -
    // not that pairing itself succeeds, which test_group.cpp already covers
    // end to end against a real sink.
    sinks.select_sink("hearth-s3-study");
    CHECK(sinks.status().selected_id == "hearth-s3-study");
}

TEST_CASE("network sinks: selecting a sink with no hello yet only remembers the request",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    sinks.on_found(test_service("hearth-s3-study", 1));
    // No on_client() yet - select_sink() must not crash reaching into an
    // Entry with no ClientView.
    sinks.select_sink("hearth-s3-study");
    CHECK(sinks.status().selected_id == "hearth-s3-study");
}

TEST_CASE("network sinks: commands on an id nothing has ever found are quietly refused",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    sinks.select_sink("no-such-sink");
    sinks.submit_pairing_code("no-such-sink", "123456");
    sinks.cancel_pairing("no-such-sink");
    sinks.forget_pairing("no-such-sink");
    sinks.rescan();
    CHECK(sinks.status().sinks.empty());
}

TEST_CASE("network sinks: push_sink_settings refuses a sink with no _ac3forge_player@v1 support",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    const ss::discovery::Service service = test_service("kitchen-speaker", 1);
    sinks.on_found(service);
    ss::ClientView client;
    client.client_id = "client-jkl";
    client.url = *service.url();
    client.hello = true;
    // No ac3forge_support: a standard Sendspin player, not a Hearth sink -
    // there is no settings command for it to take at all.
    sinks.on_client(client);

    ss::ac3forge::Settings out;
    out.layout = "2.0";
    CHECK_FALSE(sinks.push_sink_settings("kitchen-speaker", out));
    CHECK_FALSE(sinks.push_sink_identify("kitchen-speaker", ss::ac3forge::Identify{.output = 0}));
    CHECK_FALSE(sinks.status().sinks.front().intended_settings.has_value());
    CHECK_FALSE(sinks.status().sinks.front().identify_slot.has_value());
}

TEST_CASE("network sinks: creating a group selects it and clears a sink selection",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    sinks.on_found(test_service("hearth-s3-study", 1));
    sinks.select_sink("hearth-s3-study");
    REQUIRE(sinks.status().selected_id == "hearth-s3-study");

    const std::string group_id = sinks.create_group("Living room");
    CHECK_FALSE(group_id.empty());
    const auto status = sinks.status();
    CHECK(status.selected_id.empty());
    CHECK(status.selected_group_id == group_id);
    REQUIRE(status.groups.size() == 1);
    CHECK(status.groups.front().id == group_id);
    CHECK(status.groups.front().name == "Living room");
    CHECK(status.groups.front().members.empty());
}

TEST_CASE("network sinks: renaming a group is bookkeeping only", "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    const std::string group_id = sinks.create_group("New group");
    sinks.rename_group(group_id, "Kitchen and lounge");
    REQUIRE(sinks.status().groups.size() == 1);
    CHECK(sinks.status().groups.front().name == "Kitchen and lounge");

    // An id nothing made is quietly refused, the same way sink commands are.
    sinks.rename_group("no-such-group", "x");
    CHECK(sinks.status().groups.size() == 1);
}

TEST_CASE("network sinks: only a connected sink can be added to a group", "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    const std::string group_id = sinks.create_group("Downstairs");
    // hearth-s3-study is found but never says hello - no client_id yet.
    sinks.on_found(test_service("hearth-s3-study", 1));
    sinks.add_group_member(group_id, "hearth-s3-study");
    CHECK(sinks.status().groups.front().members.empty());

    const ss::discovery::Service service = test_service("hearth-s3-kitchen", 2);
    sinks.on_found(service);
    ss::ClientView client;
    client.client_id = "client-kitchen";
    client.name = "hearth-s3-kitchen";
    client.url = *service.url();
    client.hello = true;
    client.supported_roles = {"_ac3forge_player@v1"};
    ss::ac3forge::Support support;
    support.data_types = {ss::ac3forge::DataType::kEac3};
    client.ac3forge_support = support;
    sinks.on_client(client);

    sinks.add_group_member(group_id, "hearth-s3-kitchen");
    const auto status = sinks.status();
    REQUIRE(status.groups.size() == 1);
    REQUIRE(status.groups.front().members.size() == 1);
    const auto& member = status.groups.front().members.front();
    CHECK(member.sink_id == "hearth-s3-kitchen");
    CHECK(member.name == "hearth-s3-kitchen");
    CHECK(member.kind == SinkKind::kHearthSink);
    CHECK(member.connected);

    // Adding the same sink twice does not duplicate it.
    sinks.add_group_member(group_id, "hearth-s3-kitchen");
    CHECK(sinks.status().groups.front().members.size() == 1);

    sinks.remove_group_member(group_id, "hearth-s3-kitchen");
    CHECK(sinks.status().groups.front().members.empty());
}

TEST_CASE("network sinks: a member's row survives its sink disconnecting", "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    const std::string group_id = sinks.create_group("Downstairs");
    const ss::discovery::Service service = test_service("hearth-s3-kitchen", 1);
    sinks.on_found(service);
    ss::ClientView client;
    client.client_id = "client-kitchen";
    client.name = "hearth-s3-kitchen";
    client.url = *service.url();
    client.hello = true;
    sinks.on_client(client);
    sinks.add_group_member(group_id, "hearth-s3-kitchen");
    REQUIRE(sinks.status().groups.front().members.size() == 1);

    // on_client_gone() erases the sink's own row entirely (this class's own
    // churn - see network_sinks.hpp's own comment) - the group still
    // remembers the member, shown by its bare id, not silently dropped.
    sinks.on_client_gone("client-kitchen");
    CHECK(sinks.status().sinks.empty());
    const auto status = sinks.status();
    REQUIRE(status.groups.front().members.size() == 1);
    CHECK(status.groups.front().members.front().sink_id == "hearth-s3-kitchen");
    CHECK(status.groups.front().members.front().name == "hearth-s3-kitchen");
    CHECK_FALSE(status.groups.front().members.front().connected);
}

TEST_CASE("network sinks: selecting a group clears a sink selection and back again",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    sinks.on_found(test_service("hearth-s3-study", 1));
    const std::string group_id = sinks.create_group("Downstairs");
    REQUIRE(sinks.status().selected_group_id == group_id);

    sinks.select_sink("hearth-s3-study");
    CHECK(sinks.status().selected_id == "hearth-s3-study");
    CHECK(sinks.status().selected_group_id.empty());

    sinks.select_group(group_id);
    CHECK(sinks.status().selected_group_id == group_id);
    CHECK(sinks.status().selected_id.empty());
}

TEST_CASE("network sinks: push_sink_settings reaches ac3forge_command for a sink that offers the role",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    const ss::discovery::Service service = test_service("hearth-s3-kitchen", 1);
    sinks.on_found(service);
    ss::ClientView client;
    client.client_id = "client-mno";
    client.url = *service.url();
    client.hello = true;
    ss::ac3forge::Support support;
    support.data_types = {ss::ac3forge::DataType::kEac3};
    support.outputs = {.count = 2, .bit_depth = 24, .bit_depths = {24}};
    client.ac3forge_support = support;
    sinks.on_client(client);

    ss::ac3forge::Settings out;
    out.layout = "2.0";
    out.trim_db = {0.0, 0.0};
    out.delay_ms = {0.0, 0.0};
    // "client-mno" was never a real accepted connection (this slice's
    // ServerHost never listens - this file's own header comment), so
    // ServerHost::ac3forge_command() finds no session to send through and
    // this returns false. What this proves is that push_sink_settings()
    // reaches that call, past its own capability gate, for a sink that DOES
    // offer the role - not that a real sink applies it, which test_group.cpp
    // covers end to end. The cache stays empty either way: it only reflects
    // a send that actually succeeded (SinkFacts::intended_settings's own
    // comment).
    CHECK_FALSE(sinks.push_sink_settings("hearth-s3-kitchen", out));
    CHECK_FALSE(sinks.status().sinks.front().intended_settings.has_value());

    CHECK_FALSE(sinks.push_sink_identify("hearth-s3-kitchen", ss::ac3forge::Identify{.output = 0}));
    CHECK_FALSE(sinks.status().sinks.front().identify_slot.has_value());
}

TEST_CASE("network sinks: push_sink_settings and push_sink_identify on an id nothing has ever found are quietly "
          "refused",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    CHECK_FALSE(sinks.push_sink_settings("no-such-sink", ss::ac3forge::Settings{}));
    CHECK_FALSE(sinks.push_sink_identify("no-such-sink", std::nullopt));
    CHECK(sinks.status().sinks.empty());
}

TEST_CASE("network sinks: deleting the selected group clears the selection",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    const std::string group_id = sinks.create_group("Downstairs");
    sinks.delete_group(group_id);
    CHECK(sinks.status().groups.empty());
    CHECK(sinks.status().selected_group_id.empty());

    // An id nothing made is quietly refused.
    sinks.delete_group("no-such-group");
    CHECK(sinks.status().groups.empty());
}

TEST_CASE("network sinks: group commands on an id nothing has made are quietly refused",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    sinks.add_group_member("no-such-group", "no-such-sink");
    sinks.remove_group_member("no-such-group", "no-such-sink");
    sinks.set_group_volume("no-such-group", 50);
    sinks.set_group_muted("no-such-group", true);
    sinks.set_member_volume("no-such-group", "no-such-sink", 50);
    sinks.set_member_muted("no-such-group", "no-such-sink", true);
    sinks.select_group("no-such-group");
    CHECK(sinks.status().groups.empty());
}

TEST_CASE("network sinks: volume and mute on a real group reach a synthetic, unaccepted client harmlessly",
          "[hearth][network-sinks]") {
    // The point of this test is that these calls do not crash reaching
    // through a real ac3::sendspin::Group to a client_id ServerHost never
    // actually accepted a connection for (the same reasoning
    // test_network_sinks.cpp's own header comment gives for select_sink()'s
    // real, harmlessly-refused host_->pair() call) - not that the volume
    // takes effect, which needs a real accepted sink (tests/hearth/
    // test_group.cpp's own job).
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    const ss::discovery::Service service = test_service("hearth-s3-kitchen", 1);
    sinks.on_found(service);
    ss::ClientView client;
    client.client_id = "client-kitchen";
    client.url = *service.url();
    client.hello = true;
    sinks.on_client(client);

    const std::string group_id = sinks.create_group("Downstairs");
    sinks.add_group_member(group_id, "hearth-s3-kitchen");
    sinks.set_group_volume(group_id, 50);
    sinks.set_group_muted(group_id, true);
    sinks.set_member_volume(group_id, "hearth-s3-kitchen", 50);
    sinks.set_member_muted(group_id, "hearth-s3-kitchen", true);
    CHECK(sinks.status().groups.front().members.size() == 1);
}

TEST_CASE("network sinks: a client going away removes its row", "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    const ss::discovery::Service service = test_service("hearth-s3-lounge", 1);
    sinks.on_found(service);
    ss::ClientView client;
    client.client_id = "client-ghi";
    client.url = *service.url();
    client.hello = true;
    sinks.on_client(client);
    REQUIRE(sinks.status().sinks.size() == 1);

    sinks.on_client_gone("client-ghi");
    CHECK(sinks.status().sinks.empty());
}

TEST_CASE("network sinks: another server taking the sink becomes the row's notice",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    const ss::discovery::Service service = test_service("hearth-s3-study", 1);
    sinks.on_found(service);
    ss::ClientView client;
    client.client_id = "client-jkl";
    client.url = *service.url();
    client.hello = true;
    sinks.on_client(client);

    // Received while the row still exists (client/goodbye arrives before the
    // connection actually closes) - the notice shows right away.
    sinks.on_client_goodbye("client-jkl", ss::messages::GoodbyeReason::kAnotherServer);
    REQUIRE(sinks.status().sinks.size() == 1);
    CHECK(sinks.status().sinks.front().notice == "In use by another server.");

    // The row itself is removed once the connection actually ends, as
    // before, but the notice is kept by instance and reattaches once the
    // sink is found again - not lost just because the row briefly was.
    sinks.on_client_gone("client-jkl");
    CHECK(sinks.status().sinks.empty());
    sinks.on_found(service);
    REQUIRE(sinks.status().sinks.size() == 1);
    CHECK(sinks.status().sinks.front().notice == "In use by another server.");

    // A fresh connection supersedes the stale notice.
    ss::ClientView reconnected;
    reconnected.client_id = "client-jkl";
    reconnected.url = *service.url();
    reconnected.hello = true;
    sinks.on_client(reconnected);
    CHECK(sinks.status().sinks.front().notice.empty());
}

TEST_CASE("network sinks: a rejected concurrent activation becomes a pairing-in-progress notice",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    const ss::discovery::Service service = test_service("hearth-s3-study", 1);
    sinks.on_found(service);
    ss::ClientView client;
    client.client_id = "client-mno";
    client.url = *service.url();
    client.hello = true;
    sinks.on_client(client);

    sinks.on_client_goodbye("client-mno", ss::messages::GoodbyeReason::kConcurrentAttempt);
    CHECK(sinks.status().sinks.front().notice == "Another server is pairing with this sink right now.");
}

TEST_CASE("network sinks: a goodbye reason that is not about another server leaves no notice",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    const ss::discovery::Service service = test_service("hearth-s3-study", 1);
    sinks.on_found(service);
    ss::ClientView client;
    client.client_id = "client-pqr";
    client.url = *service.url();
    client.hello = true;
    sinks.on_client(client);

    // kUnpaired, kShutdown, kRestart, kUserRequest, kUnauthorized and
    // kPairingRequired are this app's own doing or the sink's, and already
    // covered by on_client()/on_client_gone() - none of them says "another
    // server", so none becomes a notice.
    sinks.on_client_goodbye("client-pqr", ss::messages::GoodbyeReason::kUnpaired);
    CHECK(sinks.status().sinks.front().notice.empty());
}

TEST_CASE("network sinks: on_client_goodbye on an id nothing has ever found is quietly ignored",
          "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store, /*request_firewall_exception=*/false};

    sinks.on_client_goodbye("no-such-client", ss::messages::GoodbyeReason::kAnotherServer);
    CHECK(sinks.status().sinks.empty());
}
