#pragma once

#include <cstdint>
#include <optional>

// The peer of a connected socket as IPv4: its address in network byte order
// and its port. Two files of this name, one picked by the component's
// CMakeLists.txt from CONFIG_LWIP_IPV6: ipv4_only/ for lwIP without IPv6, and
// dual_stack/, where esp_http_server listens on an IPv6 socket and an IPv4
// peer comes mapped into IPv6. Nothing for a peer on IPv6.

namespace ac3forge {

struct Ipv4Peer {
    std::uint32_t address = 0;
    std::uint16_t port = 0;
};

[[nodiscard]] std::optional<Ipv4Peer> ipv4_peer(int fd);

}  // namespace ac3forge
