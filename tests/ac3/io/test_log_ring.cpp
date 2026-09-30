// The console's last bytes for GET /log, tested on the host - see
// ac3forge/log_ring.hpp's own header comment.

#include <array>
#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "ac3forge/log_ring.hpp"

using ac3forge::LogRing;

TEST_CASE("a ring not yet full gives back what was put, from any count", "[io][log_ring]") {
    std::array<char, 16> storage{};
    LogRing ring(storage);
    ring.put("boot\n");
    ring.put("net up\n");
    CHECK(ring.written() == 12);
    const auto all = ring.read(0, 100);
    CHECK(all.text == "boot\nnet up\n");
    CHECK(all.from == 0);
    CHECK(all.next == 12);
    const auto rest = ring.read(5, 100);
    CHECK(rest.text == "net up\n");
    CHECK(rest.from == 5);
    CHECK(rest.next == 12);
}

TEST_CASE("a full ring keeps the newest bytes, across its end", "[io][log_ring]") {
    std::array<char, 8> storage{};
    LogRing ring(storage);
    ring.put("abcdef");
    ring.put("ghij");  // wraps: the ring holds "cdefghij"
    CHECK(ring.written() == 10);
    const auto all = ring.read(0, 100);
    CHECK(all.text == "cdefghij");
    // Asked for from before the oldest byte kept: the read starts at it.
    CHECK(all.from == 2);
    CHECK(all.next == 10);
    CHECK(ring.read(7, 100).text == "hij");
}

TEST_CASE("a write longer than the ring keeps its last bytes", "[io][log_ring]") {
    std::array<char, 4> storage{};
    LogRing ring(storage);
    ring.put("xy");
    ring.put("0123456789");
    CHECK(ring.written() == 12);
    const auto all = ring.read(0, 100);
    CHECK(all.text == "6789");
    CHECK(all.from == 8);
    CHECK(all.next == 12);
}

TEST_CASE("a read from the count written, or past it, is empty and asks from the count", "[io][log_ring]") {
    std::array<char, 8> storage{};
    LogRing ring(storage);
    ring.put("abc");
    for (const std::uint64_t from : {3ULL, 50ULL}) {
        const auto none = ring.read(from, 100);
        CHECK(none.text.empty());
        CHECK(none.from == 3);
        CHECK(none.next == 3);
    }
}

TEST_CASE("a read takes at most its limit, and the next read goes on from there", "[io][log_ring]") {
    std::array<char, 8> storage{};
    LogRing ring(storage);
    ring.put("abcdefghij");  // holds "cdefghij"
    const auto first = ring.read(0, 3);
    CHECK(first.text == "cde");
    CHECK(first.from == 2);
    CHECK(first.next == 5);
    const auto second = ring.read(first.next, 100);
    CHECK(second.text == "fghij");
    CHECK(second.next == 10);
}

TEST_CASE("a ring with no storage counts what it was given and keeps none of it", "[io][log_ring]") {
    LogRing ring({});
    ring.put("anything");
    CHECK(ring.written() == 8);
    CHECK(ring.capacity() == 0);
    const auto none = ring.read(0, 100);
    CHECK(none.text.empty());
    CHECK(none.next == 8);
}
