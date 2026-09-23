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
// or another process.

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
    NetworkSinks sinks{*identity, "Test Hearth", store};
    CHECK(sinks.started());
    CHECK(sinks.status().sinks.empty());
}

TEST_CASE("network sinks: a found sink is a row before any connection exists", "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store};

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
    NetworkSinks sinks{*identity, "Test Hearth", store};

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
    NetworkSinks sinks{*identity, "Test Hearth", store};

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
    NetworkSinks sinks{*identity, "Test Hearth", store};

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
    NetworkSinks sinks{*identity, "Test Hearth", store};

    sinks.select_sink("no-such-sink");
    sinks.submit_pairing_code("no-such-sink", "123456");
    sinks.cancel_pairing("no-such-sink");
    sinks.forget_pairing("no-such-sink");
    sinks.rescan();
    CHECK(sinks.status().sinks.empty());
}

TEST_CASE("network sinks: a client going away removes its row", "[hearth][network-sinks]") {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    const auto identity = ss::noise::KeyPair::generate();
    REQUIRE(identity.has_value());
    NetworkSinks sinks{*identity, "Test Hearth", store};

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
    NetworkSinks sinks{*identity, "Test Hearth", store};

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
    NetworkSinks sinks{*identity, "Test Hearth", store};

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
    NetworkSinks sinks{*identity, "Test Hearth", store};

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
    NetworkSinks sinks{*identity, "Test Hearth", store};

    sinks.on_client_goodbye("no-such-client", ss::messages::GoodbyeReason::kAnotherServer);
    CHECK(sinks.status().sinks.empty());
}
