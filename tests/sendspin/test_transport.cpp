#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ac3/sendspin/transport.hpp"

// The transport seam's threading contract, on the in-memory pair the session tests
// and loopback groups run over.

using ac3::sendspin::transport::Frame;
using ac3::sendspin::transport::FrameKind;

TEST_CASE("transport: a memory pair delivers both kinds in order", "[sendspin][transport]") {
    auto [a, b] = ac3::sendspin::transport::memory_pair("server", "client");
    CHECK(a->peer() == "client");
    CHECK(b->peer() == "server");

    REQUIRE(a->send_text(R"({"type":"client/init"})"));
    const std::vector<std::uint8_t> bytes{0, 1, 2, 255};
    REQUIRE(a->send_binary(bytes));
    REQUIRE(b->send_text("reply"));

    const std::optional<Frame> first = b->receive();
    REQUIRE(first.has_value());
    CHECK(first->kind == FrameKind::kText);
    CHECK(first->text() == R"({"type":"client/init"})");
    const std::optional<Frame> second = b->receive();
    REQUIRE(second.has_value());
    CHECK(second->kind == FrameKind::kBinary);
    CHECK(second->bytes == bytes);
    const std::optional<Frame> reply = a->receive();
    REQUIRE(reply.has_value());
    CHECK(reply->text() == "reply");
}

TEST_CASE("transport: close unblocks a waiting reader and fails later sends",
          "[sendspin][transport]") {
    auto [a, b] = ac3::sendspin::transport::memory_pair();
    std::atomic<bool> returned{false};
    std::optional<Frame> got;
    std::thread reader([&, &connection = *b] {
        got = connection.receive();
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(returned.load());
    a->close();
    reader.join();
    CHECK(returned.load());
    CHECK_FALSE(got.has_value());
    CHECK_FALSE(a->send_text("late"));
    CHECK_FALSE(b->send_binary(std::vector<std::uint8_t>{1}));
}

TEST_CASE("transport: messages sent before a close are still received",
          "[sendspin][transport]") {
    auto [a, b] = ac3::sendspin::transport::memory_pair();
    REQUIRE(a->send_text(R"({"type":"client/goodbye","payload":{"reason":"shutdown"}})"));
    a->close();
    const std::optional<Frame> goodbye = b->receive();
    REQUIRE(goodbye.has_value());
    CHECK(goodbye->text().starts_with(R"({"type":"client/goodbye")"));
    CHECK_FALSE(b->receive().has_value());
}

TEST_CASE("transport: many senders, one reader, nothing lost", "[sendspin][transport]") {
    auto [a, b] = ac3::sendspin::transport::memory_pair();
    constexpr int kThreads = 4;
    constexpr int kEach = 250;
    std::vector<std::thread> senders;
    for (int t = 0; t < kThreads; ++t) {
        senders.emplace_back([&, t, &connection = *a] {
            for (int i = 0; i < kEach; ++i) {
                const std::vector<std::uint8_t> message{static_cast<std::uint8_t>(t),
                                                        static_cast<std::uint8_t>(i & 0xFF)};
                connection.send_binary(message);
            }
        });
    }
    for (std::thread& sender : senders) {
        sender.join();
    }
    std::vector<int> next(kThreads, 0);
    for (int n = 0; n < kThreads * kEach; ++n) {
        const std::optional<Frame> frame = b->receive();
        REQUIRE(frame.has_value());
        REQUIRE(frame->bytes.size() == 2);
        const int t = frame->bytes[0];
        // Each sender's own messages arrive in the order it sent them.
        CHECK(frame->bytes[1] == static_cast<std::uint8_t>(next[static_cast<std::size_t>(t)] & 0xFF));
        ++next[static_cast<std::size_t>(t)];
    }
}
