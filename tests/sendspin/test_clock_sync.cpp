#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <random>

#include "ac3/sendspin/clock_sync.hpp"
#include "ac3/sendspin/messages.hpp"

// The client's clock synchronisation against a simulated server whose clock runs at an
// offset and a drift from the client's, over a network whose delay varies from exchange to
// exchange.

namespace {

using ac3::sendspin::ClockSync;
namespace m = ac3::sendspin::messages;

// A server clock: offset plus drift, in parts per million, relative to the client's.
struct ServerClock {
    std::int64_t offset_us;
    double drift_ppm;

    [[nodiscard]] std::int64_t at(std::int64_t local) const {
        return local + offset_us + static_cast<std::int64_t>(static_cast<double>(local) * drift_ppm * 1e-6);
    }
};

// Runs exchanges until `until`, delivering each request and reply after a random delay of
// `min_delay` to `max_delay` microseconds. Returns the local time reached.
std::int64_t simulate(ClockSync& sync, const ServerClock& server, std::int64_t start, std::int64_t until,
                      std::int64_t min_delay, std::int64_t max_delay, std::mt19937& random,
                      std::size_t* requests = nullptr) {
    std::uniform_int_distribution<std::int64_t> delay(min_delay, max_delay);
    std::int64_t now = start;
    while (now < until) {
        const std::optional<m::ClientTime> request = sync.poll(now);
        if (!request) {
            now = std::max(now + 1'000, std::min(sync.next_due(), until));
            continue;
        }
        if (requests != nullptr) {
            ++*requests;
        }
        const std::int64_t arrive = now + delay(random);
        const std::int64_t leave = arrive + 50;
        const std::int64_t back = leave + delay(random);
        sync.receive({.client_transmitted = request->client_transmitted,
                      .server_received = server.at(arrive),
                      .server_transmitted = server.at(leave)},
                     back);
        now = back;
    }
    return now;
}

}  // namespace

TEST_CASE("clock sync: converges on a local network and maps both ways", "[sendspin][clock_sync]") {
    ClockSync sync;
    const ServerClock server{.offset_us = 7'654'321'000, .drift_ppm = 35.0};
    std::mt19937 random(20260915);
    CHECK_FALSE(sync.converged());

    std::size_t requests = 0;
    std::int64_t now = 0;
    while (!sync.converged() && now < 60'000'000) {
        now = simulate(sync, server, now, now + 100'000, 500, 3'000, random, &requests);
    }
    REQUIRE(sync.converged());
    // Bursts follow one another while converging: well under a second on this network.
    CHECK(now < 2'000'000);
    CHECK(sync.updates() >= ClockSync::kConvergedUpdates);
    CHECK(requests == sync.updates() * ClockSync::kBurstLength);
    CHECK(sync.error_us() < ClockSync::kConvergedError);

    const std::int64_t local = now + 5'000;
    CHECK(std::llabs(sync.to_local(server.at(local)) - local) < 1'000);
    CHECK(std::llabs(sync.to_server(local) - server.at(local)) < 1'000);
}

TEST_CASE("clock sync: once converged a burst runs every ten seconds, and tracks drift",
          "[sendspin][clock_sync]") {
    ClockSync sync;
    const ServerClock server{.offset_us = -123'456, .drift_ppm = -80.0};
    std::mt19937 random(7);
    std::int64_t now = 0;
    while (!sync.converged()) {
        now = simulate(sync, server, now, now + 100'000, 800, 2'500, random);
    }
    const std::size_t updates = sync.updates();
    CHECK(sync.next_due() >= now);
    CHECK(sync.next_due() - now <= ClockSync::kBurstInterval);

    // Ten minutes more: one update per ten seconds, and still within a millisecond.
    std::size_t requests = 0;
    now = simulate(sync, server, now, now + 600'000'000, 800, 2'500, random, &requests);
    CHECK(sync.updates() - updates >= 59);
    CHECK(sync.updates() - updates <= 61);
    CHECK(requests == (sync.updates() - updates) * ClockSync::kBurstLength);
    CHECK(std::llabs(sync.to_local(server.at(now)) - now) < 1'000);
}

TEST_CASE("clock sync: a reply that never comes ends the burst after the timeout", "[sendspin][clock_sync]") {
    ClockSync sync;
    const std::optional<m::ClientTime> first = sync.poll(0);
    REQUIRE(first.has_value());
    CHECK_FALSE(sync.poll(1'000).has_value());
    CHECK_FALSE(sync.poll(ClockSync::kReplyTimeout - 1).has_value());
    // The overdue exchange is abandoned and the next one goes out.
    const std::optional<m::ClientTime> second = sync.poll(ClockSync::kReplyTimeout);
    REQUIRE(second.has_value());
    CHECK(second->client_transmitted == ClockSync::kReplyTimeout);
    // A late reply to the abandoned one is ignored.
    sync.receive({.client_transmitted = first->client_transmitted, .server_received = 1, .server_transmitted = 2},
                 ClockSync::kReplyTimeout + 10);
    CHECK_FALSE(sync.poll(ClockSync::kReplyTimeout + 20).has_value());
    CHECK(sync.updates() == 0);
}

TEST_CASE("clock sync: a local clock that reads negative", "[sendspin][clock_sync]") {
    // Nothing about a monotonic clock says it reads above zero, and the time filter ignores
    // updates at or before zero; ClockSync hands it times counted from its first exchange.
    ClockSync sync;
    const ServerClock server{.offset_us = 46'000'000, .drift_ppm = 12.0};
    std::mt19937 random(11);
    std::int64_t now = -45'000'000;
    const std::optional<m::ClientTime> first = sync.poll(now);
    REQUIRE(first.has_value());
    CHECK(first->client_transmitted == now);
    sync.reset();
    while (!sync.converged() && now < -40'000'000) {
        now = simulate(sync, server, now, now + 100'000, 500, 2'000, random);
    }
    REQUIRE(sync.converged());
    CHECK(now < 0);
    CHECK(std::llabs(sync.to_local(server.at(now)) - now) < 1'000);
    CHECK(std::llabs(sync.to_server(now) - server.at(now)) < 1'000);
}

TEST_CASE("clock sync: reset forgets the server", "[sendspin][clock_sync]") {
    ClockSync sync;
    const ServerClock server{.offset_us = 1'000'000, .drift_ppm = 0.0};
    std::mt19937 random(3);
    std::int64_t now = 0;
    while (!sync.converged()) {
        now = simulate(sync, server, now, now + 100'000, 500, 1'500, random);
    }
    sync.reset();
    CHECK_FALSE(sync.converged());
    CHECK(sync.updates() == 0);
    CHECK(sync.poll(now).has_value());
}
