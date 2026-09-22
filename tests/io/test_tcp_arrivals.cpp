// The arrival log a board keeps for each Sendspin connection, tested on the
// host: which segment dates the last byte a reader has read, and how the log
// behaves when segments come out of order, twice, or past its capacity. None
// of it may date a byte earlier than the segment that made it readable.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "ac3forge/tcp_arrivals.hpp"

using ac3forge::ArrivalLog;

TEST_CASE("arrival log: dates a message by the segment that completed it", "[io][tcp_arrivals]") {
    ArrivalLog log;
    log.start(1'000);
    log.segment(1'000, 100, 10);
    log.segment(1'100, 100, 20);
    CHECK(log.contiguous() == 200);
    CHECK(log.arrival(50) == std::optional<std::int64_t>(10));
    CHECK(log.arrival(100) == std::optional<std::int64_t>(10));
    // A message whose last byte is in the second segment.
    CHECK(log.arrival(101) == std::optional<std::int64_t>(20));
    CHECK(log.arrival(200) == std::optional<std::int64_t>(20));
}

TEST_CASE("arrival log: says nothing before its stream starts or before anything is read",
          "[io][tcp_arrivals]") {
    ArrivalLog log;
    log.segment(0, 100, 10);
    CHECK_FALSE(log.started());
    CHECK_FALSE(log.arrival(50).has_value());
    log.start(0);
    log.segment(0, 100, 10);
    CHECK_FALSE(log.arrival(0).has_value());
    CHECK(log.arrival(1) == std::optional<std::int64_t>(10));
}

TEST_CASE("arrival log: a retransmission leaves the first arrival's date", "[io][tcp_arrivals]") {
    ArrivalLog log;
    log.start(0);
    log.segment(0, 100, 10);
    log.segment(0, 100, 50);
    CHECK(log.contiguous() == 100);
    CHECK(log.arrival(100) == std::optional<std::int64_t>(10));
}

TEST_CASE("arrival log: a segment overlapping the contiguous end dates the bytes past it",
          "[io][tcp_arrivals]") {
    ArrivalLog log;
    log.start(0);
    log.segment(0, 100, 10);
    log.segment(50, 100, 20);
    CHECK(log.contiguous() == 150);
    CHECK(log.arrival(100) == std::optional<std::int64_t>(10));
    CHECK(log.arrival(120) == std::optional<std::int64_t>(20));
}

TEST_CASE("arrival log: bytes past a gap are dated by the segment that fills it", "[io][tcp_arrivals]") {
    ArrivalLog log;
    log.start(0);
    log.segment(0, 100, 10);
    // Out of order: nobody can read these until bytes 100 to 199 come.
    log.segment(200, 100, 20);
    CHECK(log.contiguous() == 100);
    log.segment(100, 100, 30);
    CHECK(log.contiguous() == 200);
    CHECK(log.arrival(150) == std::optional<std::int64_t>(30));
    // The reader then has bytes 200 to 299 at once, which the log never took: they are not
    // dated, and the log goes on from where the reader is.
    CHECK_FALSE(log.arrival(250).has_value());
    CHECK(log.contiguous() == 250);
    log.segment(300, 100, 40);
    CHECK_FALSE(log.arrival(300).has_value());
    CHECK(log.contiguous() == 300);
    log.segment(300, 100, 50);
    CHECK(log.arrival(400) == std::optional<std::int64_t>(50));
}

TEST_CASE("arrival log: bytes from before the stream began are left out", "[io][tcp_arrivals]") {
    ArrivalLog log;
    log.start(1'000);
    log.segment(900, 50, 10);
    log.segment(990, 20, 20);
    CHECK(log.contiguous() == 0);
    log.segment(1'000, 20, 30);
    CHECK(log.arrival(20) == std::optional<std::int64_t>(30));
}

TEST_CASE("arrival log: counts on across sequence numbers that wrap and past 4 GB", "[io][tcp_arrivals]") {
    ArrivalLog log;
    log.start(0xFFFF'FF00U);
    log.segment(0xFFFF'FF00U, 0x100, 10);
    log.segment(0x0000'0000U, 100, 20);
    CHECK(log.contiguous() == 0x100 + 100);
    CHECK(log.arrival(0x100) == std::optional<std::int64_t>(10));
    CHECK(log.arrival(0x100 + 1) == std::optional<std::int64_t>(20));

    ArrivalLog long_stream;
    long_stream.start(5);
    long_stream.segment(5, 0xFFFF'FF00U, 30);
    long_stream.segment(5 + 0xFFFF'FF00U, 0x200, 40);
    CHECK(long_stream.contiguous() == 0x1'0000'0100ULL);
    CHECK(long_stream.arrival(0x1'0000'0000ULL) == std::optional<std::int64_t>(40));
    // A retransmission from before the wrap is still recognised as old.
    long_stream.segment(5 + 0xFFFF'FE00U, 0x100, 50);
    CHECK(long_stream.contiguous() == 0x1'0000'0100ULL);
}

TEST_CASE("arrival log: a full log drops its oldest entries and dates their bytes later, not earlier",
          "[io][tcp_arrivals]") {
    ArrivalLog log;
    log.start(0);
    const std::size_t segments = ArrivalLog::kCapacity + 8;
    for (std::size_t i = 0; i < segments; ++i) {
        log.segment(static_cast<std::uint32_t>(i * 10), 10, static_cast<std::int64_t>(100 + i));
    }
    // The first eight segments' entries are gone: their bytes take the oldest date kept.
    CHECK(log.arrival(10) == std::optional<std::int64_t>(108));
    CHECK(log.arrival(85) == std::optional<std::int64_t>(108));
    CHECK(log.arrival(95) == std::optional<std::int64_t>(109));
    const auto last_end = static_cast<std::uint64_t>(segments * 10);
    CHECK(log.arrival(last_end) == std::optional<std::int64_t>(static_cast<std::int64_t>(100 + segments - 1)));
}

TEST_CASE("arrival log: a new stream forgets the old one", "[io][tcp_arrivals]") {
    ArrivalLog log;
    log.start(0);
    log.segment(0, 100, 10);
    log.start(5'000);
    CHECK(log.contiguous() == 0);
    CHECK_FALSE(log.arrival(50).has_value());
    log.segment(5'000, 100, 20);
    CHECK(log.arrival(50) == std::optional<std::int64_t>(20));
}
