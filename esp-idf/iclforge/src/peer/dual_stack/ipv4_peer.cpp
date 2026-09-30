// ../ipv4_peer.hpp for lwIP with IPv6, where an IPv4 peer of esp_http_server's
// IPv6 socket is given as ::ffff:a.b.c.d.

#include "peer/ipv4_peer.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace ac3forge {

std::optional<Ipv4Peer> ipv4_peer(int fd) {
    sockaddr_storage peer{};
    socklen_t length = sizeof(peer);
    if (getpeername(fd, static_cast<sockaddr*>(static_cast<void*>(&peer)), &length) != 0) {
        return std::nullopt;
    }
    if (peer.ss_family == AF_INET) {
        const auto* v4 = static_cast<const sockaddr_in*>(static_cast<const void*>(&peer));
        return Ipv4Peer{.address = v4->sin_addr.s_addr, .port = ntohs(v4->sin_port)};
    }
    if (peer.ss_family != AF_INET6) {
        return std::nullopt;
    }
    const auto* v6 = static_cast<const sockaddr_in6*>(static_cast<const void*>(&peer));
    constexpr std::array<std::uint8_t, 12> kMapped{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::memcmp(v6->sin6_addr.s6_addr, kMapped.data(), kMapped.size()) != 0) {
        return std::nullopt;
    }
    Ipv4Peer result{.address = 0, .port = ntohs(v6->sin6_port)};
    std::memcpy(&result.address, &v6->sin6_addr.s6_addr[kMapped.size()], sizeof(result.address));
    return result;
}

}  // namespace ac3forge
