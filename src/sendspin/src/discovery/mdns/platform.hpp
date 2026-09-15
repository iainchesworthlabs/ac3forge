#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// What the mdns backend needs from the operating system besides opening its sockets, which
// mdns.h does: starting the socket library, the IPv4 interfaces, the host name, and the socket
// calls whose types differ between platforms. One file per platform, chosen by CMake.

namespace ac3::sendspin::discovery::mdns_platform {

using Ipv4 = std::array<std::uint8_t, 4>;

struct Interface {
    Ipv4 address{};
    // The length of the network prefix, which tells which interface a peer's address is on.
    std::uint8_t prefix_length = 32;
};

struct Endpoint {
    Ipv4 address{};
    std::uint16_t port = 0;
};

// Starts the socket library where the platform needs it. False when it cannot.
[[nodiscard]] bool start_network();

// The IPv4 interfaces that are up, can multicast, and are not loopbacks or point-to-point links.
[[nodiscard]] std::vector<Interface> ipv4_interfaces();

// The computer's host name, as the operating system reports it.
[[nodiscard]] std::string host_name();

// The positions in `sockets` of those with a datagram to read within `timeout_ms`.
[[nodiscard]] std::vector<std::size_t> wait_readable(const std::vector<int>& sockets, int timeout_ms);

// One datagram from a non-blocking socket, with its sender; nothing when none is waiting.
[[nodiscard]] std::optional<std::size_t> receive_from(int socket, std::span<std::uint8_t> buffer, Endpoint& from);

bool send_to(int socket, std::span<const std::uint8_t> bytes, const Endpoint& to);

void close_socket(int socket);

}  // namespace ac3::sendspin::discovery::mdns_platform
