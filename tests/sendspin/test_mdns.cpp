#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "ac3/sendspin/discovery.hpp"
#include "ac3/sendspin/mdns.hpp"
#include "discovery/mdns/packets.hpp"

// The mdns backend's packets, with no network: a responder's answers read back by a browser's
// state, the questions a browser asks about an instance and its host, legacy unicast queries,
// goodbyes and expiry, and packets that end early or point at themselves.

namespace {

namespace packets = ac3::sendspin::discovery::mdns_packets;
using ac3::sendspin::discovery::Advertisement;
using ac3::sendspin::discovery::Service;

const Advertisement kKitchen{.service = "_sendspin._tcp",
                             .instance = "Kitchen",
                             .port = 8928,
                             .txt = {{.key = "path", .value = "/sendspin"}, {.key = "name", .value = "Kitchen"}}};
const packets::Ipv4 kAddress{192, 168, 1, 20};
constexpr std::int64_t kSecond = 1'000'000;

packets::Packet parsed(const std::vector<std::uint8_t>& bytes) {
    std::optional<packets::Packet> packet = packets::parse(bytes);
    REQUIRE(packet.has_value());
    return *packet;
}

packets::Question ptr_question() {
    return {.name = "_sendspin._tcp.local.", .type = packets::kTypePtr, .unicast = false};
}

}  // namespace

TEST_CASE("mdns: a PTR question is answered with everything a browser needs", "[sendspin][mdns]") {
    const packets::Responder responder(kKitchen, "Desk PC", kAddress);
    CHECK(responder.service_name() == "_sendspin._tcp.local.");
    CHECK(responder.instance_name() == "Kitchen._sendspin._tcp.local.");
    CHECK(responder.host_name() == "Desk-PC.local.");

    const packets::Packet question = parsed(packets::query(ptr_question()));
    CHECK_FALSE(question.response);
    REQUIRE(question.questions.size() == 1);
    CHECK(question.questions[0].name == "_sendspin._tcp.local.");
    CHECK(question.questions[0].type == packets::kTypePtr);
    CHECK_FALSE(question.questions[0].unicast);

    const std::optional<std::vector<std::uint8_t>> answer = responder.respond(question.questions[0], question.id, false);
    REQUIRE(answer.has_value());
    const packets::Packet response = parsed(*answer);
    CHECK(response.response);
    CHECK(response.id == 0);
    CHECK(response.questions.empty());
    // PTR, SRV, A, and one TXT record holding both entries.
    REQUIRE(response.records.size() == 4);
    CHECK(response.records[0].ttl == 4500);

    packets::BrowseState state("_sendspin._tcp");
    const packets::BrowseState::Changes changes = state.receive(response, 0);
    REQUIRE(changes.found.size() == 1);
    const Service& found = changes.found[0];
    CHECK(found.instance == "Kitchen");
    CHECK(found.host == "Desk-PC.local");
    CHECK(found.addresses == std::vector<std::string>{"192.168.1.20"});
    CHECK(found.port == 8928);
    CHECK(found.txt_value("name") == std::optional<std::string>("Kitchen"));
    CHECK(found.url() == std::optional<std::string>("ws://192.168.1.20:8928/sendspin"));
    CHECK(state.missing(0).empty());
    // The same response again changes nothing.
    CHECK(state.receive(response, kSecond).found.empty());
}

TEST_CASE("mdns: questions about the instance, its host and the service types", "[sendspin][mdns]") {
    const packets::Responder responder(kKitchen, "desk", kAddress);
    const auto ask = [&](std::string name, std::uint16_t type) {
        return responder.respond({.name = std::move(name), .type = type, .unicast = true}, 0, false);
    };

    // Names compare without regard to case.
    const std::optional<std::vector<std::uint8_t>> srv = ask("KITCHEN._sendspin._tcp.local.", packets::kTypeSrv);
    REQUIRE(srv.has_value());
    const packets::Packet srv_packet = parsed(*srv);
    REQUIRE_FALSE(srv_packet.records.empty());
    CHECK(srv_packet.records[0].type == packets::kTypeSrv);
    CHECK(srv_packet.records[0].port == 8928);
    CHECK(srv_packet.records[0].target == "desk.local.");
    CHECK(srv_packet.records[0].ttl == 120);

    const std::optional<std::vector<std::uint8_t>> txt = ask("Kitchen._sendspin._tcp.local.", packets::kTypeTxt);
    REQUIRE(txt.has_value());
    const packets::Packet txt_packet = parsed(*txt);
    REQUIRE(txt_packet.records.size() == 1);
    CHECK(txt_packet.records[0].txt == kKitchen.txt);

    const std::optional<std::vector<std::uint8_t>> a = ask("desk.local", packets::kTypeA);
    REQUIRE(a.has_value());
    const packets::Packet a_packet = parsed(*a);
    REQUIRE(a_packet.records.size() == 1);
    CHECK(a_packet.records[0].address == std::optional<packets::Ipv4>(kAddress));

    const std::optional<std::vector<std::uint8_t>> types = ask(std::string(packets::kServiceTypes), packets::kTypePtr);
    REQUIRE(types.has_value());
    CHECK(parsed(*types).records.at(0).target == "_sendspin._tcp.local.");

    // Not this responder's: another service, another instance, a record type it has none of.
    CHECK_FALSE(ask("_sendspin-server._tcp.local.", packets::kTypePtr).has_value());
    CHECK_FALSE(ask("Lounge._sendspin._tcp.local.", packets::kTypeSrv).has_value());
    CHECK_FALSE(ask("desk.local.", 28).has_value());
}

TEST_CASE("mdns: a legacy unicast query gets its question and id back, with short TTLs", "[sendspin][mdns]") {
    const packets::Responder responder(kKitchen, "desk", kAddress);
    const std::optional<std::vector<std::uint8_t>> answer = responder.respond(ptr_question(), 0x1234, true);
    REQUIRE(answer.has_value());
    const packets::Packet response = parsed(*answer);
    CHECK(response.id == 0x1234);
    REQUIRE(response.questions.size() == 1);
    CHECK(response.questions[0].name == "_sendspin._tcp.local.");
    REQUIRE_FALSE(response.records.empty());
    for (const packets::Record& record : response.records) {
        CHECK(record.ttl <= 10);
    }
}

TEST_CASE("mdns: a browser asks for what a response left out", "[sendspin][mdns]") {
    const packets::Responder responder(kKitchen, "desk", kAddress);
    packets::BrowseState state("_sendspin._tcp");

    // Only the PTR record: the instance is known, and not yet usable.
    packets::Packet partial = parsed(*responder.respond(ptr_question(), 0, false));
    partial.records.resize(1);
    CHECK(state.receive(partial, 0).found.empty());
    const std::vector<packets::Question> missing = state.missing(0);
    REQUIRE(missing.size() == 2);
    CHECK(missing[0].type == packets::kTypeSrv);
    CHECK(missing[1].type == packets::kTypeTxt);
    CHECK(missing[0].unicast);

    // The SRV answer without its A record: now the host's address is asked for.
    packets::Packet srv = parsed(*responder.respond(missing[0], 0, false));
    srv.records.resize(1);
    CHECK(state.receive(srv, 0).found.empty());
    std::vector<packets::Question> still = state.missing(0);
    REQUIRE(still.size() == 2);
    CHECK(still[0].type == packets::kTypeTxt);
    CHECK(still[1].name == "desk.local.");
    CHECK(still[1].type == packets::kTypeA);

    std::vector<Service> found;
    for (const packets::Question& question : still) {
        const packets::BrowseState::Changes changes = state.receive(parsed(*responder.respond(question, 0, false)), 0);
        found.insert(found.end(), changes.found.begin(), changes.found.end());
    }
    REQUIRE(found.size() == 1);
    CHECK(found[0].url() == std::optional<std::string>("ws://192.168.1.20:8928/sendspin"));
    CHECK(state.missing(0).empty());
}

TEST_CASE("mdns: goodbyes and expiry lose an instance", "[sendspin][mdns]") {
    const packets::Responder responder(kKitchen, "desk", kAddress);

    SECTION("a goodbye takes effect a second later") {
        packets::BrowseState state("_sendspin._tcp");
        REQUIRE(state.receive(parsed(responder.announcement(false)), 0).found.size() == 1);
        const packets::Packet goodbye = parsed(responder.announcement(true));
        for (const packets::Record& record : goodbye.records) {
            CHECK(record.ttl == 0);
        }
        CHECK(state.receive(goodbye, 10 * kSecond).lost.empty());
        CHECK(state.expire(10 * kSecond + (kSecond / 2)).lost.empty());
        CHECK(state.expire(11 * kSecond).lost == std::vector<std::string>{"Kitchen"});
        CHECK(state.expire(12 * kSecond).lost.empty());
    }
    SECTION("records not refreshed expire by their TTL") {
        packets::BrowseState state("_sendspin._tcp");
        REQUIRE(state.receive(parsed(responder.announcement(false)), 0).found.size() == 1);
        CHECK(state.expire(4499 * kSecond).lost.empty());
        CHECK(state.expire(4500 * kSecond).lost == std::vector<std::string>{"Kitchen"});
    }
    SECTION("another service type's records are not this browser's") {
        packets::BrowseState state("_sendspin-server._tcp");
        CHECK(state.receive(parsed(responder.announcement(false)), 0).found.empty());
        CHECK(state.missing(0).empty());
    }
}

TEST_CASE("mdns: packets that end early or point at themselves, and names that need changing",
          "[sendspin][mdns]") {
    CHECK_FALSE(packets::parse(std::vector<std::uint8_t>(11, 0)).has_value());
    // One question whose name is a compression pointer to itself.
    const std::vector<std::uint8_t> loop{0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 12, 0, 1};
    CHECK_FALSE(packets::parse(loop).has_value());

    // A packet cut into its last record keeps the records before it.
    const packets::Responder responder(kKitchen, "desk", kAddress);
    std::vector<std::uint8_t> bytes = *responder.respond(ptr_question(), 0, false);
    bytes.resize(bytes.size() - 5);
    const std::optional<packets::Packet> truncated = packets::parse(bytes);
    REQUIRE(truncated.has_value());
    CHECK(truncated->records.size() == 3);

    // A flood of instances is remembered only up to the bound.
    packets::BrowseState state("_sendspin._tcp");
    for (int i = 0; i < 100; ++i) {
        Advertisement other = kKitchen;
        other.instance = "Speaker " + std::to_string(i);
        (void)state.receive(parsed(packets::Responder(other, "desk", kAddress).announcement(false)), 0);
    }
    CHECK(state.missing(0).empty());
    CHECK(state.expire(5000 * kSecond).lost.size() == packets::BrowseState::kMaxEntries);

    CHECK(packets::instance_label("Living.Room") == "Living-Room");
    CHECK(packets::instance_label(std::string(70, 'x')).size() == 63);
    CHECK(packets::host_label("my_host.example.com") == "my-host");
    CHECK(packets::host_label("") == "sendspin");
}

namespace {

class Found final : public ac3::sendspin::discovery::BrowseListener {
   public:
    void on_found(const Service& service) override {
        {
            const std::lock_guard lock(mutex_);
            services_.push_back(service);
        }
        changed_.notify_all();
    }
    void on_lost(const std::string& instance) override {
        {
            const std::lock_guard lock(mutex_);
            lost_.push_back(instance);
        }
        changed_.notify_all();
    }

    std::optional<Service> wait_found(const std::string& instance, std::chrono::seconds timeout) {
        std::unique_lock lock(mutex_);
        std::optional<Service> match;
        changed_.wait_for(lock, timeout, [&] {
            for (const Service& service : services_) {
                if (service.instance == instance) {
                    match = service;
                }
            }
            return match.has_value();
        });
        return match;
    }

    bool wait_lost(const std::string& instance, std::chrono::seconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] {
            return std::find(lost_.begin(), lost_.end(), instance) != lost_.end();
        });
    }

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Service> services_;
    std::vector<std::string> lost_;
};

}  // namespace

TEST_CASE("mdns: an advertiser and a browser find each other on the loopback interface", "[sendspin][mdns][network]") {
    namespace mdns = ac3::sendspin::discovery::mdns;
    // A name of this run's own, so that another run on the same computer is not found instead.
    std::random_device random;
    const std::string instance = "Hearth test " + std::to_string(random() % 1'000'000);
    Advertisement advertisement = kKitchen;
    advertisement.instance = instance;
    const mdns::Options loopback{.interfaces = {"127.0.0.1"}, .host = "hearth-test"};

    Found found;
    std::unique_ptr<ac3::sendspin::discovery::Advertiser> advertiser = mdns::advertise(advertisement, loopback);
    REQUIRE(advertiser != nullptr);
    const std::unique_ptr<ac3::sendspin::discovery::Browser> browser = mdns::browse("_sendspin._tcp", found, loopback);
    REQUIRE(browser != nullptr);

    const std::optional<Service> service = found.wait_found(instance, std::chrono::seconds(10));
    REQUIRE(service.has_value());
    CHECK(service->host == "hearth-test.local");
    CHECK(service->url() == std::optional<std::string>("ws://127.0.0.1:8928/sendspin"));

    // Destroying the advertiser says goodbye.
    advertiser.reset();
    CHECK(found.wait_lost(instance, std::chrono::seconds(10)));
}
