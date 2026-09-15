#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The transport seam: one Sendspin WebSocket, whichever side dialled
// (connection.md, Establishing a Connection). Sessions see only this interface, so
// the same session code runs over cpp-httplib on a computer, esp_http_server on a
// board, and the in-memory pair the tests and loopback groups use.
//
// The threading contract every backend keeps:
//   - receive() is called from one thread at a time, the connection's reader;
//   - send_text() and send_binary() may be called from any thread and are
//     serialised by the backend, one whole WebSocket message at a time;
//   - close() may be called from any thread. It ends the connection without
//     touching the reader's state: a receive() in progress returns nothing, and
//     every later send fails.
// Close frames, pings and pongs are the backend's business and never reach a
// session.

namespace ac3::sendspin::transport {

enum class FrameKind : std::uint8_t {
    kText,
    kBinary,
};

struct Frame {
    FrameKind kind = FrameKind::kBinary;
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] std::string_view text() const {
        return {static_cast<const char*>(static_cast<const void*>(bytes.data())), bytes.size()};
    }
};

class Connection {
   public:
    Connection() = default;
    virtual ~Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    // The next message, or nothing once the connection has ended and every message
    // the peer sent before that has been received.
    [[nodiscard]] virtual std::optional<Frame> receive() = 0;

    virtual bool send_text(std::string_view text) = 0;
    virtual bool send_binary(std::span<const std::uint8_t> bytes) = 0;

    virtual void close() = 0;

    // The remote address as the backend reports it, for logs and diagnostics.
    [[nodiscard]] virtual std::string peer() const = 0;
};

// Two connections joined in memory: what one sends, the other receives, in order.
// Closing either ends both. Each reports the other's name as its peer.
[[nodiscard]] std::pair<std::unique_ptr<Connection>, std::unique_ptr<Connection>> memory_pair(
    std::string first_name = "memory:a", std::string second_name = "memory:b");

}  // namespace ac3::sendspin::transport
