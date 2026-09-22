// ../ipv4_peer.hpp for lwIP without IPv6.

#include "peer/ipv4_peer.hpp"

#include <optional>

#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace ac3forge {

std::optional<Ipv4Peer> ipv4_peer(int fd) {
    sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    if (getpeername(fd, static_cast<sockaddr*>(static_cast<void*>(&peer)), &length) != 0 ||
        peer.sin_family != AF_INET) {
        return std::nullopt;
    }
    return Ipv4Peer{.address = peer.sin_addr.s_addr, .port = ntohs(peer.sin_port)};
}

}  // namespace ac3forge
