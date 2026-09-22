#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

// A UDP socket: bind to a local IPv4 address, block-with-timeout for one
// datagram, send one, close - LivePositionSource itself only ever needs the
// receiving half; send_to() exists for the test suite (see its own comment
// below). Pimpl'd
// like every other OS-facing type in this library (Capture, MonitorSink,
// PassthroughSink - see capture.hpp's own struct Impl/unique_ptr shape) -
// not a virtual interface, since this codebase's platform seam has never
// been one.
//
// Its own small platform tree, src/audio/src/net/{posix,windows}/ - a
// SECOND axis from src/audio/src/backend/<backend>/, deliberately, not a
// fifth file added to every one of that tree's six directories. See
// src/audio/CMakeLists.txt's own comment on the net/ block for why: the
// backend axis is the AUDIO SUBSYSTEM (WASAPI vs ALSA vs PipeWire vs
// CoreAudio genuinely differ), sockets are the OPERATING SYSTEM (Berkeley
// sockets are one implementation across Linux/macOS/Android; only Windows
// differs), and folding one into the other would mean five near-identical
// copies of the same POSIX file.
//
// Internal - not installed under include/ac3/audio/. Nothing outside
// live_positions.cpp needs a socket type at all.

namespace ac3::audio {

enum class UdpSocketError : std::uint8_t {
    kBadAddress,    // bind_address did not parse as an IPv4 dotted-quad
    kSocketFailed,  // socket()/WSAStartup-equivalent itself failed
    kBindFailed,    // the OS refused the bind (port in use, permission, ...)
};

[[nodiscard]] std::string_view describe(UdpSocketError error);

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&&) = delete;
    UdpSocket& operator=(UdpSocket&&) = delete;

    // Binds an IPv4 UDP socket to bind_address:port - bind_address is a
    // dotted-quad literal ("127.0.0.1", "0.0.0.0"), never a hostname (no
    // resolver dependency, no blocking DNS lookup at session start - see
    // LivePositionSource's own header). port == 0 asks the OS for an
    // ephemeral port; local_port() reads back what it actually got, which
    // is how a hermetic test binds without racing a fixed port number.
    [[nodiscard]] std::expected<void, UdpSocketError> bind(std::string_view bind_address,
                                                            std::uint16_t port);

    // Blocks for up to `timeout_ms`, writing at most `buffer.size()` bytes of
    // ONE datagram into it and returning how many. nullopt on timeout or a
    // transient receive error - never throws, never blocks past the
    // timeout. A datagram cannot be silently truncated in practice as long
    // as `buffer` is at least 65,535 bytes: UDP's own length field is 16
    // bits, so no legal datagram is ever larger (LivePositionSource's own
    // receive buffer is sized exactly for this).
    [[nodiscard]] std::optional<std::size_t> recv(std::span<std::byte> buffer,
                                                  std::uint32_t timeout_ms);

    [[nodiscard]] std::uint16_t local_port() const;

    // Fire-and-forget: sends one UDP datagram to address:port, true if the
    // OS accepted it for sending (which is not a delivery guarantee - UDP
    // has none). Creates its own underlying socket on first use if bind()
    // was never called first - an unbound sender needs no local address of
    // its own, the OS assigns an ephemeral one on the first send exactly as
    // it would for port 0 above.
    //
    // LivePositionSource itself never calls this - it only ever receives.
    // It exists so tests/audio/test_live_positions.cpp can send itself real
    // loopback datagrams through this exact class rather than a second,
    // parallel socket implementation that would only exist for the test.
    [[nodiscard]] bool send_to(std::string_view address, std::uint16_t port,
                               std::span<const std::byte> data);

    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ac3::audio
