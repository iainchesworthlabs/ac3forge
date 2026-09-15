#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/sendspin/transport.hpp"
#include "ac3/sendspin/websocket.hpp"

// The WebSocket transport over loopback: the threading contract test_transport.cpp holds the
// in-memory pair to, and what the network adds to it - closes that have to reach a reader, the
// listener's limits, and the reasons connect() fails.
//
// Under ThreadSanitizer on glibc, every test here that dials crashes before it reports anything,
// and none does with CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO undefined for ac3sendspin: the
// vcpkg port defines it, and it has cpp-httplib resolve the host through getaddrinfo_a, whose
// worker threads TSan does not track. A TSan run of these tests needs the macro undefined.

using ac3::sendspin::transport::Connection;
using ac3::sendspin::transport::Frame;
using ac3::sendspin::transport::FrameKind;
namespace websocket = ac3::sendspin::transport::websocket;

namespace {

using namespace std::chrono_literals;

// Accepted connections, handed from the listener's workers to the test.
class Accepted {
   public:
    void push(std::unique_ptr<Connection> connection) {
        {
            const std::lock_guard lock(mutex_);
            connections_.push_back(std::move(connection));
        }
        changed_.notify_all();
    }

    // The next accepted connection, or nothing if none arrives within the timeout.
    std::unique_ptr<Connection> pop(std::chrono::milliseconds timeout = 5s) {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, timeout, [this] { return !connections_.empty(); })) {
            return nullptr;
        }
        std::unique_ptr<Connection> connection = std::move(connections_.front());
        connections_.pop_front();
        return connection;
    }

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::unique_ptr<Connection>> connections_;
};

websocket::ListenerOptions loopback_options(std::size_t max_connections = 8) {
    websocket::ListenerOptions options;
    options.address = "127.0.0.1";
    options.max_connections = max_connections;
    return options;
}

std::unique_ptr<websocket::Listener> start_listener(Accepted& accepted,
                                                    websocket::ListenerOptions options = loopback_options()) {
    return websocket::Listener::start(std::move(options), [&accepted](std::unique_ptr<Connection> connection) {
        accepted.push(std::move(connection));
    });
}

std::string url_of(const websocket::Listener& listener, std::string_view path = websocket::kPath) {
    return "ws://127.0.0.1:" + std::to_string(listener.port()) + std::string(path);
}

std::unique_ptr<Connection> dial(const std::string& url) {
    auto dialled = websocket::connect(url);
    return dialled ? std::move(*dialled) : nullptr;
}

std::optional<websocket::ConnectError> connect_error(const std::string& url) {
    const auto dialled = websocket::connect(url);
    if (dialled) {
        return std::nullopt;
    }
    return dialled.error();
}

}  // namespace

TEST_CASE("websocket: a dialled and an accepted connection carry both kinds both ways",
          "[sendspin][transport][websocket]") {
    Accepted accepted;
    const auto listener = start_listener(accepted);
    REQUIRE(listener != nullptr);
    CHECK(listener->port() != 0);
    const std::unique_ptr<Connection> client = dial(url_of(*listener));
    REQUIRE(client != nullptr);
    const std::unique_ptr<Connection> server = accepted.pop();
    REQUIRE(server != nullptr);
    CHECK(server->peer().starts_with("127.0.0.1:"));
    CHECK(client->peer() == url_of(*listener));

    REQUIRE(client->send_text(R"({"type":"client/init"})"));
    const std::vector<std::uint8_t> bytes{0, 1, 2, 255};
    REQUIRE(client->send_binary(bytes));
    REQUIRE(server->send_binary(bytes));
    REQUIRE(server->send_text("reply"));

    const std::optional<Frame> text = server->receive();
    REQUIRE(text.has_value());
    CHECK(text->kind == FrameKind::kText);
    CHECK(text->text() == R"({"type":"client/init"})");
    const std::optional<Frame> binary = server->receive();
    REQUIRE(binary.has_value());
    CHECK(binary->kind == FrameKind::kBinary);
    CHECK(binary->bytes == bytes);

    const std::optional<Frame> back = client->receive();
    REQUIRE(back.has_value());
    CHECK(back->kind == FrameKind::kBinary);
    CHECK(back->bytes == bytes);
    const std::optional<Frame> reply = client->receive();
    REQUIRE(reply.has_value());
    CHECK(reply->kind == FrameKind::kText);
    CHECK(reply->text() == "reply");
}

TEST_CASE("websocket: a close on either side ends both waiting readers and later sends",
          "[sendspin][transport][websocket]") {
    Accepted accepted;
    const auto listener = start_listener(accepted);
    REQUIRE(listener != nullptr);
    const std::unique_ptr<Connection> client = dial(url_of(*listener));
    REQUIRE(client != nullptr);
    const std::unique_ptr<Connection> server = accepted.pop();
    REQUIRE(server != nullptr);

    std::optional<Frame> server_got;
    std::optional<Frame> client_got;
    std::thread server_reader([&] { server_got = server->receive(); });
    std::thread client_reader([&] { client_got = client->receive(); });
    std::this_thread::sleep_for(20ms);

    SECTION("the dialling side closes") {
        client->close();
    }
    SECTION("the accepting side closes") {
        server->close();
    }

    server_reader.join();
    client_reader.join();
    CHECK_FALSE(server_got.has_value());
    CHECK_FALSE(client_got.has_value());
    CHECK_FALSE(server->send_text("late"));
    CHECK_FALSE(client->send_binary(std::vector<std::uint8_t>{1}));
}

TEST_CASE("websocket: messages sent before a close are still received",
          "[sendspin][transport][websocket]") {
    Accepted accepted;
    const auto listener = start_listener(accepted);
    REQUIRE(listener != nullptr);
    const std::unique_ptr<Connection> client = dial(url_of(*listener));
    REQUIRE(client != nullptr);
    const std::unique_ptr<Connection> server = accepted.pop();
    REQUIRE(server != nullptr);

    REQUIRE(client->send_text(R"({"type":"client/goodbye","payload":{"reason":"shutdown"}})"));
    const std::vector<std::uint8_t> last{4, 0, 0, 0};
    REQUIRE(client->send_binary(last));
    // With no reader of its own, the client waits in close() for the server's answering Close,
    // which the server's reader sends once it reaches the client's.
    std::thread closer([&] { client->close(); });

    const std::optional<Frame> goodbye = server->receive();
    REQUIRE(goodbye.has_value());
    CHECK(goodbye->text().starts_with(R"({"type":"client/goodbye")"));
    const std::optional<Frame> binary = server->receive();
    REQUIRE(binary.has_value());
    CHECK(binary->bytes == last);
    CHECK_FALSE(server->receive().has_value());
    closer.join();
}

TEST_CASE("websocket: many senders, one reader, nothing lost", "[sendspin][transport][websocket]") {
    Accepted accepted;
    const auto listener = start_listener(accepted);
    REQUIRE(listener != nullptr);
    const std::unique_ptr<Connection> client = dial(url_of(*listener));
    REQUIRE(client != nullptr);
    const std::unique_ptr<Connection> server = accepted.pop();
    REQUIRE(server != nullptr);

    constexpr int kThreads = 4;
    constexpr int kEach = 250;
    std::vector<std::thread> senders;
    for (int t = 0; t < kThreads; ++t) {
        senders.emplace_back([&, t] {
            for (int i = 0; i < kEach; ++i) {
                const std::vector<std::uint8_t> message{static_cast<std::uint8_t>(t),
                                                        static_cast<std::uint8_t>(i & 0xFF)};
                client->send_binary(message);
            }
        });
    }
    std::vector<int> next(kThreads, 0);
    for (int n = 0; n < kThreads * kEach; ++n) {
        const std::optional<Frame> frame = server->receive();
        REQUIRE(frame.has_value());
        REQUIRE(frame->bytes.size() == 2);
        const auto t = static_cast<std::size_t>(frame->bytes[0]);
        REQUIRE(t < next.size());
        // Each sender's own messages arrive in the order it sent them.
        CHECK(frame->bytes[1] == static_cast<std::uint8_t>(next[t] & 0xFF));
        ++next[t];
    }
    for (std::thread& sender : senders) {
        sender.join();
    }
}

TEST_CASE("websocket: a message longer than one Noise message ends the connection",
          "[sendspin][transport][websocket]") {
    Accepted accepted;
    const auto listener = start_listener(accepted);
    REQUIRE(listener != nullptr);
    const std::unique_ptr<Connection> client = dial(url_of(*listener));
    REQUIRE(client != nullptr);
    const std::unique_ptr<Connection> server = accepted.pop();
    REQUIRE(server != nullptr);

    const std::vector<std::uint8_t> largest(65535, 0x5A);
    REQUIRE(client->send_binary(largest));
    const std::optional<Frame> frame = server->receive();
    REQUIRE(frame.has_value());
    CHECK(frame->bytes == largest);

    // The server may drop the connection before this send has finished, so its result is not
    // the point; what the server's reader makes of it is.
    client->send_binary(std::vector<std::uint8_t>(65536, 0x5A));
    CHECK_FALSE(server->receive().has_value());
    CHECK_FALSE(server->send_text("after"));
}

TEST_CASE("websocket: a handler may serve its connection before returning",
          "[sendspin][transport][websocket]") {
    // An echo server that runs each session on the worker, the way a player serves the one
    // server it is connected to.
    websocket::ListenerOptions options = loopback_options();
    const auto listener = websocket::Listener::start(std::move(options), [](std::unique_ptr<Connection> connection) {
        while (const std::optional<Frame> frame = connection->receive()) {
            if (frame->kind == FrameKind::kText) {
                connection->send_text(frame->text());
            } else {
                connection->send_binary(frame->bytes);
            }
        }
    });
    REQUIRE(listener != nullptr);
    const std::unique_ptr<Connection> client = dial(url_of(*listener));
    REQUIRE(client != nullptr);
    REQUIRE(client->send_text("echo"));
    const std::optional<Frame> echoed = client->receive();
    REQUIRE(echoed.has_value());
    CHECK(echoed->text() == "echo");
}

TEST_CASE("websocket: stop ends open connections, which outlive the listener, and refuses new ones",
          "[sendspin][transport][websocket]") {
    Accepted accepted;
    auto listener = start_listener(accepted);
    REQUIRE(listener != nullptr);
    const std::string url = url_of(*listener);
    const std::unique_ptr<Connection> client = dial(url);
    REQUIRE(client != nullptr);
    const std::unique_ptr<Connection> server = accepted.pop();
    REQUIRE(server != nullptr);

    std::optional<Frame> client_got;
    std::thread client_reader([&] { client_got = client->receive(); });
    listener->stop();
    client_reader.join();
    CHECK_FALSE(client_got.has_value());
    CHECK_FALSE(server->receive().has_value());
    CHECK_FALSE(server->send_text("after stop"));

    listener.reset();
    CHECK_FALSE(server->receive().has_value());
    CHECK(connect_error(url) == websocket::ConnectError::kUnreachable);
}

TEST_CASE("websocket: an upgrade past max_connections is closed at once",
          "[sendspin][transport][websocket]") {
    Accepted accepted;
    const auto listener = start_listener(accepted, loopback_options(1));
    REQUIRE(listener != nullptr);
    const std::string url = url_of(*listener);
    std::unique_ptr<Connection> first = dial(url);
    REQUIRE(first != nullptr);
    std::unique_ptr<Connection> first_accepted = accepted.pop();
    REQUIRE(first_accepted != nullptr);

    // The upgrade itself succeeds; the close follows it, and the handler never sees it.
    const std::unique_ptr<Connection> second = dial(url);
    REQUIRE(second != nullptr);
    CHECK_FALSE(second->receive().has_value());
    CHECK(accepted.pop(200ms) == nullptr);

    // Ending the first frees its slot once its worker has let go of it, which happens on the
    // worker's own time, so the next connection is tried until one is admitted.
    first_accepted.reset();
    first.reset();
    std::unique_ptr<Connection> admitted;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (admitted == nullptr && std::chrono::steady_clock::now() < deadline) {
        const std::unique_ptr<Connection> next = dial(url);
        REQUIRE(next != nullptr);
        admitted = accepted.pop(200ms);
    }
    CHECK(admitted != nullptr);
}

TEST_CASE("websocket: connect says why it failed", "[sendspin][transport][websocket]") {
    Accepted accepted;
    const auto listener = start_listener(accepted);
    REQUIRE(listener != nullptr);
    const std::string authority = "127.0.0.1:" + std::to_string(listener->port());

    CHECK(connect_error("http://" + authority + "/sendspin") == websocket::ConnectError::kInvalidUrl);
    CHECK(connect_error("wss://" + authority + "/sendspin") == websocket::ConnectError::kInvalidUrl);
    CHECK(connect_error("ws://" + authority) == websocket::ConnectError::kInvalidUrl);
    CHECK(connect_error("ws://" + authority + "/other") == websocket::ConnectError::kRejected);
    CHECK(accepted.pop(100ms) == nullptr);
}

TEST_CASE("websocket: start refuses bad options and a port another listener holds",
          "[sendspin][transport][websocket]") {
    Accepted accepted;
    websocket::ListenerOptions options = loopback_options();
    options.path = "sendspin";
    CHECK(start_listener(accepted, options) == nullptr);
    options.path = "/send.spin";
    CHECK(start_listener(accepted, options) == nullptr);
    CHECK(start_listener(accepted, loopback_options(0)) == nullptr);

    // cpp-httplib's own socket options would let this second bind succeed.
    const auto holder = start_listener(accepted);
    REQUIRE(holder != nullptr);
    options = loopback_options();
    options.port = holder->port();
    CHECK(start_listener(accepted, options) == nullptr);
}
