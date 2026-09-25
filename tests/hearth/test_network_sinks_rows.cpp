#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "ac3/sendspin/ac3forge_player.hpp"
#include "ac3/sendspin/messages.hpp"
#include "network_sinks.hpp"
#include "settings_model.hpp"

// More of NetworkSinks' bookkeeping (apps/hearth/engine/network_sinks.cpp), driven the way
// test_network_sinks.cpp drives it - hand-built discovery::Service and ClientView facts through
// the listener overrides, a real ServerHost with browse off behind them, and a refused loopback
// port for every dial: how a row reads a standard player, a test sink and a paired sink, what
// happens to a row when its mDNS record expires, a pairing asked for before hello, the pairing
// outcomes the page shows as an error, and the per-sink commands reaching a host that has no such
// connection.

namespace ss = ac3::sendspin;
using ac3::hearth::MemorySettingsStore;
using ac3::hearth::NetworkSinks;
using ac3::hearth::PairingStore;
using ac3::hearth::PairState;
using ac3::hearth::SinkKind;

namespace {

// Each on its own refused low port, so that no two rows share a URL.
ss::discovery::Service service_named(std::string instance, std::uint16_t port = 1) {
    return ss::discovery::Service{.instance = std::move(instance),
                                  .host = "127.0.0.1",
                                  .addresses = {"127.0.0.1"},
                                  .port = port,
                                  .txt = {{.key = "path", .value = "/sendspin"}}};
}

std::string today() {
    return "2026-09-24";
}

// As a Hearth sink says hello: pairable by its pairing PSK or a dynamic code.
ss::ClientView connected(const ss::discovery::Service& service, std::string client_id) {
    ss::ClientView client;
    client.client_id = std::move(client_id);
    client.url = *service.url();
    client.psk = ss::handshake::PskCategory::kSentinel;
    client.hello = true;
    client.pair_methods = {ss::messages::PairMethod::kPairingPsk, ss::messages::PairMethod::kDynamicCode};
    return client;
}

struct Rig {
    MemorySettingsStore settings;
    PairingStore store{settings, today};
    std::optional<NetworkSinks> sinks;

    Rig() {
        const auto identity = ss::noise::KeyPair::generate();
        REQUIRE(identity.has_value());
        // No mDNS browsing: see test_network_sinks.cpp's own header comment.
        sinks.emplace(*identity, "Test Hearth", store, ac3::hearth::NetworkSinksOptions{.browse = false});
        REQUIRE(sinks->started());
    }
};

}  // namespace

TEST_CASE("network sinks rows: a standard player lists its codecs and its player lead time",
          "[hearth][network-sinks]") {
    Rig rig;
    const auto service = service_named("living-room");
    rig.sinks->on_found(service);
    auto client = connected(service, "client-std");
    client.name = "Living room";
    ss::messages::PlayerSupport support;
    support.supported_formats = {{.codec = ss::messages::Codec::kFlac, .channels = 2, .sample_rate = 48000, .bit_depth = 16},
                                 {.codec = ss::messages::Codec::kOpus, .channels = 2, .sample_rate = 48000, .bit_depth = 16}};
    client.player_support = support;
    ss::messages::PlayerState state;
    state.required_lead_time_ms = 750;
    client.player_state = state;
    client.available = true;
    rig.sinks->on_client(client);

    const auto status = rig.sinks->status();
    REQUIRE(status.sinks.size() == 1);
    const auto& row = status.sinks.front();
    CHECK(row.name == "Living room");
    CHECK(row.kind == SinkKind::kStandardPlayer);
    CHECK(row.codecs == std::vector<std::string>{"flac", "opus"});
    CHECK(row.required_lead_time_ms == 750U);
    CHECK(row.clock_converged);
    CHECK(row.path == "/sendspin");
}

TEST_CASE("network sinks rows: a Hearth sink's own lead time wins over its player's, and a test sink is named as one",
          "[hearth][network-sinks]") {
    Rig rig;
    const auto service = service_named("bench");
    rig.sinks->on_found(service);
    auto client = connected(service, "client-bench");
    client.name = "Hearth TestSink bench";
    ss::ac3forge::Support support;
    support.data_types = {ss::ac3forge::DataType::kEac3};
    support.outputs = {.count = 12, .bit_depth = 24, .bit_depths = {24}};
    client.ac3forge_support = support;
    ss::ac3forge::State ac3forge_state;
    ac3forge_state.required_lead_time_ms = 300;
    client.ac3forge_state = ac3forge_state;
    ss::messages::PlayerState player_state;
    player_state.required_lead_time_ms = 900;
    client.player_state = player_state;
    client.player_support = ss::messages::PlayerSupport{};
    rig.sinks->on_client(client);

    const auto row = rig.sinks->status().sinks.front();
    CHECK(row.kind == SinkKind::kTestSink);  // named "testsink", whatever else it offers
    CHECK(row.data_types == std::vector<std::string>{"eac3"});
    CHECK(row.output_slots == 12U);
    CHECK(row.output_bit_depth == 24U);
    CHECK(row.required_lead_time_ms == 300U);
}

TEST_CASE("network sinks rows: a paired sink shows the day it paired on", "[hearth][network-sinks]") {
    Rig rig;
    ss::crypto::Key32 key{};
    key[0] = 0x42;
    ss::crypto::Key32 psk{};
    psk[5] = 0x17;
    REQUIRE(rig.store.store_record(key, psk));
    ss::crypto::Key32 other{};
    other[0] = 0x43;
    REQUIRE(rig.store.store_record(other, psk));

    const auto service = service_named("den");
    rig.sinks->on_found(service);
    auto client = connected(service, "client-den");
    client.client_key = other;
    client.psk = ss::handshake::PskCategory::kLongTerm;
    rig.sinks->on_client(client);

    const auto row = rig.sinks->status().sinks.front();
    CHECK(row.pair_state == PairState::kPaired);
    CHECK(row.paired_on == "2026-09-24");
}

TEST_CASE("network sinks rows: an expired mDNS record drops a row only while nothing is connected",
          "[hearth][network-sinks]") {
    Rig rig;
    const auto quiet = service_named("quiet");
    const auto busy = service_named("busy", 2);
    rig.sinks->on_found(quiet);
    rig.sinks->on_found(busy);
    rig.sinks->on_client(connected(busy, "client-busy"));
    const auto before = rig.sinks->status().generation;

    rig.sinks->on_lost("quiet");
    rig.sinks->on_lost("busy");
    rig.sinks->on_lost("never-found");
    const auto status = rig.sinks->status();
    REQUIRE(status.sinks.size() == 1);
    CHECK(status.sinks.front().id == "busy");
    CHECK(status.generation > before);

    // The connection going is what removes the busy row; an unknown client id changes nothing.
    rig.sinks->on_client_gone("client-nobody");
    CHECK(rig.sinks->status().sinks.size() == 1);
    rig.sinks->on_client_gone("client-busy");
    CHECK(rig.sinks->status().sinks.empty());
}

TEST_CASE("network sinks rows: a client the host never dialled is not given a row", "[hearth][network-sinks]") {
    Rig rig;
    ss::ClientView client;
    client.client_id = "client-inbound";
    client.url = "ws://10.0.0.9:8928/sendspin";
    client.hello = true;
    rig.sinks->on_client(client);
    CHECK(rig.sinks->status().sinks.empty());
}

TEST_CASE("network sinks rows: a pairing asked for before hello waits for the attempt to run",
          "[hearth][network-sinks]") {
    Rig rig;
    const auto service = service_named("attic");
    rig.sinks->on_found(service);
    rig.sinks->select_sink("attic");
    rig.sinks->pair_sink("attic");
    CHECK(rig.sinks->status().sinks.front().pairing_requested);
    // Hello from a connection with no attempt running yet keeps the request;
    // one whose attempt runs clears it, and the page moves on to the code.
    rig.sinks->on_client(connected(service, "client-attic"));
    CHECK(rig.sinks->status().sinks.front().pairing_requested);
    auto pairing = connected(service, "client-attic");
    pairing.pairing = true;
    pairing.pairing_attempt = true;
    pairing.wants_code = true;
    rig.sinks->on_client(pairing);
    auto row = rig.sinks->status().sinks.front();
    CHECK_FALSE(row.pairing_requested);
    CHECK(row.pairing_active);
    CHECK(row.wants_code);
    CHECK(ac3::hearth::to_detail(row).pairing == "code");
    CHECK(rig.sinks->status().selected_id == "attic");

    // An attempt that ended without pairing leaves the activity declared until the host decides
    // again: nothing runs, and the page offers to pair once more.
    auto ended = pairing;
    ended.pairing_attempt = false;
    ended.wants_code = false;
    rig.sinks->on_client(ended);
    row = rig.sinks->status().sinks.front();
    CHECK_FALSE(row.pairing_active);
    CHECK(ac3::hearth::to_detail(row).pairing == "none");
    CHECK(ac3::hearth::to_detail(row).can_pair);

    // Cancelling clears the request, and the per-sink commands reach the host for a known id.
    rig.sinks->on_client(connected(service, "client-attic"));
    rig.sinks->pair_sink("attic");
    CHECK(rig.sinks->status().sinks.front().pairing_requested);
    rig.sinks->cancel_pairing("attic");
    CHECK_FALSE(rig.sinks->status().sinks.front().pairing_requested);
    rig.sinks->submit_pairing_code("attic", "123456");
    rig.sinks->forget_pairing("attic");
    CHECK(rig.sinks->status().sinks.size() == 1);
}

TEST_CASE("network sinks rows: forgetting a paired sink that is not connected forgets its record",
          "[hearth][network-sinks]") {
    Rig rig;
    ss::crypto::Key32 key{};
    key[0] = 0x51;
    ss::crypto::Key32 psk{};
    psk[3] = 0x09;
    REQUIRE(rig.store.store_record(key, psk));
    const auto service = service_named("porch");
    rig.sinks->on_found(service);
    auto client = connected(service, "client-porch");
    client.client_key = key;
    client.psk = ss::handshake::PskCategory::kLongTerm;
    rig.sinks->on_client(client);
    rig.sinks->on_client_gone("client-porch");
    REQUIRE(rig.sinks->status().sinks.front().pair_state == PairState::kPaired);

    rig.sinks->forget_pairing("porch");
    CHECK_FALSE(rig.store.paired(key));
    CHECK(rig.sinks->status().sinks.front().pair_state == PairState::kNotPaired);
}

TEST_CASE("network sinks rows: a sink that lost its pairing says so", "[hearth][network-sinks]") {
    Rig rig;
    const auto service = service_named("hall");
    rig.sinks->on_found(service);
    auto client = connected(service, "client-hall");
    client.credential_mismatch = true;
    rig.sinks->on_client(client);
    const auto row = rig.sinks->status().sinks.front();
    CHECK(row.lost_pairing);
    CHECK(row.pair_state == PairState::kNotPaired);
    CHECK_FALSE(row.notice.empty());
    CHECK(ac3::hearth::to_detail(row).can_pair);
}

TEST_CASE("network sinks rows: mDNS's own name stands until the sink says its own", "[hearth][network-sinks]") {
    Rig rig;
    auto service = service_named("hearth-abc123");
    service.txt.push_back({.key = "name", .value = "Kitchen"});
    rig.sinks->on_found(service);
    CHECK(rig.sinks->status().sinks.front().name == "Kitchen");
    auto client = connected(service, "client-kitchen");
    client.name = "Kitchen sink";
    rig.sinks->on_client(client);
    CHECK(rig.sinks->status().sinks.front().name == "Kitchen sink");
}

TEST_CASE("network sinks rows: pairing outcomes for the selected sink become the page's error",
          "[hearth][network-sinks]") {
    using ss::pairing_messages::AbortReason;
    Rig rig;
    const auto service = service_named("office");
    const auto elsewhere = service_named("hall", 2);
    rig.sinks->on_found(service);
    rig.sinks->on_found(elsewhere);
    rig.sinks->on_client(connected(service, "client-office"));
    rig.sinks->on_client(connected(elsewhere, "client-hall"));
    rig.sinks->select_sink("office");

    const auto error_after = [&](std::optional<AbortReason> reason) {
        rig.sinks->on_pairing_ended("client-office", reason);
        return rig.sinks->status().pairing_error;
    };
    CHECK(error_after(AbortReason::kCodeMismatch) == "That code was not right. Pair again for a new one.");
    CHECK(error_after(AbortReason::kAttemptTimeout) == "Took too long. Pair again for a new code.");
    CHECK(error_after(AbortReason::kConcurrentAttempt) == "Another pairing attempt is already in progress.");
    CHECK(error_after(AbortReason::kMethodNotSupported) == "The sink could not complete pairing.");
    CHECK(error_after(AbortReason::kPinLengthUnacceptable) == "The sink could not complete pairing.");
    CHECK(error_after(AbortReason::kUserCancelled).empty());

    // Ignored: no reason, a sink that is not selected, a client nothing knows.
    rig.sinks->on_pairing_ended("client-office", AbortReason::kCodeMismatch);
    rig.sinks->on_pairing_ended("client-office", std::nullopt);
    rig.sinks->on_pairing_ended("client-hall", AbortReason::kAttemptTimeout);
    rig.sinks->on_pairing_ended("client-nobody", AbortReason::kAttemptTimeout);
    CHECK(rig.sinks->status().pairing_error == "That code was not right. Pair again for a new one.");

    // Pairing succeeding clears it; the code-wanted and log hooks change nothing.
    rig.sinks->on_pairing_code_wanted("client-office");
    rig.sinks->on_log("a line");
    rig.sinks->on_paired("client-office");
    CHECK(rig.sinks->status().pairing_error.empty());
}

TEST_CASE("network sinks rows: settings and identify for a Hearth sink with no live connection are not recorded",
          "[hearth][network-sinks]") {
    Rig rig;
    const auto service = service_named("cinema");
    rig.sinks->on_found(service);
    auto client = connected(service, "client-cinema");
    client.ac3forge_support = ss::ac3forge::Support{};
    rig.sinks->on_client(client);

    ss::ac3forge::Settings settings;
    settings.layout = "5.1";
    // The host has no connection by that id, so neither command is sent, and the row keeps no
    // optimistic record of either.
    CHECK_FALSE(rig.sinks->push_sink_settings("cinema", settings));
    CHECK_FALSE(rig.sinks->push_sink_identify("cinema", ss::ac3forge::Identify{.output = 2}));
    CHECK_FALSE(rig.sinks->push_sink_identify("cinema", std::nullopt));
    const auto row = rig.sinks->status().sinks.front();
    CHECK_FALSE(row.intended_settings.has_value());
    CHECK_FALSE(row.identify_slot.has_value());
}

TEST_CASE("network sinks rows: a group is looked up by its id, and an unknown id finds none",
          "[hearth][network-sinks]") {
    Rig rig;
    const std::string id = rig.sinks->create_group("Downstairs");
    REQUIRE_FALSE(id.empty());
    const auto group = rig.sinks->group(id);
    REQUIRE(group != nullptr);
    CHECK(group->id() == id);
    CHECK(rig.sinks->group("no-such-group") == nullptr);
}
