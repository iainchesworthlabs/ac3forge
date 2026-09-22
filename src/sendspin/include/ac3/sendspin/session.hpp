#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ac3/sendspin/transport.hpp"

// What the two session classes share: how they hand frames to their driver.
//
// A session never touches a socket. It is given the frames that arrive and the time, and
// answers with the frames to send and whether to close the connection after them, so one
// session runs under a desktop thread over a transport::Connection and inside a board's
// WebSocket handler alike. drive() in session_driver.hpp is the desktop's loop.

namespace ac3::sendspin {

struct SessionOutput {
    // To send, in order.
    std::vector<transport::Frame> frames;
    // Close the connection once the frames are sent.
    bool close = false;

    void text(std::string_view message) {
        transport::Frame frame{.kind = transport::FrameKind::kText, .bytes = {}};
        frame.bytes.assign(message.begin(), message.end());
        frames.push_back(std::move(frame));
    }

    void binary(std::vector<std::uint8_t> bytes) {
        frames.push_back({.kind = transport::FrameKind::kBinary, .bytes = std::move(bytes)});
    }

    void append(SessionOutput&& other) {
        for (transport::Frame& frame : other.frames) {
            frames.push_back(std::move(frame));
        }
        close = close || other.close;
    }
};

// Local monotonic time in microseconds. The session asks for it at the points the
// specification ties to a clock: when a frame arrived, and just before a message is sealed.
class Clock {
   public:
    Clock() = default;
    virtual ~Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;

    [[nodiscard]] virtual std::int64_t now_us() const = 0;
};

// The handshake phase's per-message timeout (connection.md, Failure Handling).
inline constexpr std::int64_t kHandshakeTimeout = 30'000'000;
// How long a connection may wait for its first server/activate (connection.md, Multiple
// servers).
inline constexpr std::int64_t kProvisionalTimeout = 30'000'000;

}  // namespace ac3::sendspin
