#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "ac3/sendspin/transport.hpp"

// The transport seam's backend on a computer: Sendspin's plain ws:// WebSocket over
// cpp-httplib (connection.md, Establishing a Connection). The spec lets either side dial,
// so both halves need both directions: a player that advertises _sendspin._tcp listens and
// the server dials it, and a server that advertises _sendspin-server._tcp listens for the
// players that dial it. Listener accepts; connect() dials.
//
// Every connection keeps transport.hpp's threading contract, and on top of it:
//   - a receive() in progress returns within ConnectionOptions::poll_interval of a close()
//     from another thread, whether or not the peer answers the Close frame;
//   - an accepted connection may outlive its Listener: stop() ends it, and afterwards its
//     receive() returns nothing and its sends fail.
//
// An accepted connection holds one of the Listener's worker threads until it ends, since
// cpp-httplib keeps each WebSocket on the worker that upgraded it. A dialled one holds no
// thread of its own. Each also has cpp-httplib's ping thread while ConnectionOptions::
// ping_interval is non-zero.

namespace ac3::sendspin::transport::websocket {

// The recommended ports and path (connection.md): a client listens on 8928, a server on 8927,
// and both advertise the path in their mDNS TXT record.
inline constexpr std::uint16_t kClientPort = 8928;
inline constexpr std::uint16_t kServerPort = 8927;
inline constexpr std::string_view kPath = "/sendspin";

struct ConnectionOptions {
    // How long one read waits before receive() looks again for a close from another thread.
    // Once part of a frame has come, receive() waits for the rest past it, until the rest comes
    // or the connection is closed: over Wi-Fi the rest can come later.
    std::chrono::milliseconds poll_interval{100};
    // WebSocket Ping is Sendspin's liveness mechanism (messaging.md). A peer that leaves
    // max_missed_pongs pings in a row unanswered is closed, unless max_missed_pongs is 0; a
    // zero interval sends no pings at all. aiosendspin pings every 30 s.
    std::chrono::seconds ping_interval{30};
    int max_missed_pongs = 2;
    // How long one send may wait on a peer that has stopped reading. A send that runs out of
    // time ends the connection, since part of a message may already be on the wire.
    std::chrono::seconds write_timeout{5};
};

struct ListenerOptions {
    std::string address = "0.0.0.0";
    // 0 takes any free port, which Listener::port() then reports.
    std::uint16_t port = 0;
    // Upgrades are accepted on this path only. It starts with '/' and holds only letters,
    // digits and -._~/.
    std::string path{kPath};
    // An upgrade past this many open connections is closed at once with status 1013, try
    // again later.
    std::size_t max_connections = 8;
    ConnectionOptions connection;
};

class Listener {
   public:
    // Called on a worker thread for each accepted connection. The handler may keep the
    // connection, hand it elsewhere, or serve it before returning; the worker stays with it
    // until it ends either way. It must not throw.
    using Handler = std::function<void(std::unique_ptr<Connection>)>;

    // Binds and starts accepting. Nothing when the options are invalid or the address cannot
    // be bound.
    [[nodiscard]] static std::unique_ptr<Listener> start(ListenerOptions options, Handler handler);

    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener(Listener&&) = delete;
    Listener& operator=(Listener&&) = delete;

    [[nodiscard]] std::uint16_t port() const;

    // Stops accepting, ends every open connection with status 1001, going away, and waits
    // for the workers, which needs every handler still running to return. The destructor
    // calls it; call it from one thread at a time.
    void stop();

   private:
    struct State;
    explicit Listener(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

struct ConnectOptions {
    // The bound on the TCP connection and on the WebSocket upgrade after it.
    std::chrono::milliseconds connect_timeout{5000};
    ConnectionOptions connection;
};

enum class ConnectError : std::uint8_t {
    kInvalidUrl,   // not ws://host[:port]/path
    kUnreachable,  // no HTTP response within the timeout
    kRejected,     // an HTTP response other than the upgrade, such as 404 for a wrong path
};

[[nodiscard]] std::expected<std::unique_ptr<Connection>, ConnectError> connect(
    const std::string& url, const ConnectOptions& options = {});

}  // namespace ac3::sendspin::transport::websocket
