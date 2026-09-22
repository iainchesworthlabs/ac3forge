#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "platform.hpp"

namespace ac3::sendspin::discovery::mdns_platform {

namespace {

[[nodiscard]] sockaddr_in address_of(const Endpoint& endpoint) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    std::memcpy(&address.sin_addr, endpoint.address.data(), endpoint.address.size());
    address.sin_port = htons(endpoint.port);
    return address;
}

[[nodiscard]] bool has(unsigned flags, unsigned flag) {
    return (flags & flag) != 0;
}

}  // namespace

bool start_network() {
    return true;
}

std::vector<Interface> ipv4_interfaces() {
    std::vector<Interface> interfaces;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        return interfaces;
    }
    for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
        const unsigned flags = entry->ifa_flags;
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET || !has(flags, IFF_UP) ||
            !has(flags, IFF_MULTICAST) || has(flags, IFF_LOOPBACK) || has(flags, IFF_POINTOPOINT)) {
            continue;
        }
        Interface found;
        const auto* in = static_cast<const sockaddr_in*>(static_cast<const void*>(entry->ifa_addr));
        std::memcpy(found.address.data(), &in->sin_addr, found.address.size());
        if (entry->ifa_netmask != nullptr) {
            const auto* mask = static_cast<const sockaddr_in*>(static_cast<const void*>(entry->ifa_netmask));
            found.prefix_length = static_cast<std::uint8_t>(std::popcount(static_cast<std::uint32_t>(mask->sin_addr.s_addr)));
        }
        interfaces.push_back(found);
    }
    freeifaddrs(list);
    return interfaces;
}

std::string host_name() {
    std::array<char, 256> name{};
    if (gethostname(name.data(), name.size() - 1) != 0) {
        return {};
    }
    return name.data();
}

std::vector<std::size_t> wait_readable(const std::vector<int>& sockets, int timeout_ms) {
    std::vector<pollfd> polled;
    polled.reserve(sockets.size());
    for (const int socket : sockets) {
        polled.push_back({.fd = socket, .events = POLLIN, .revents = 0});
    }
    std::vector<std::size_t> readable;
    // nfds_t is 32-bit on macOS and 64-bit on glibc, so the count is cast rather than left to
    // an implicit conversion that one of the two warns about. One entry per socket, and a
    // browse holds one per interface, so nothing can be lost.
    if (polled.empty() ||
        poll(polled.data(), static_cast<nfds_t>(polled.size()), timeout_ms) <= 0) {
        return readable;
    }
    for (std::size_t i = 0; i < polled.size(); ++i) {
        if ((polled[i].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
            readable.push_back(i);
        }
    }
    return readable;
}

std::optional<std::size_t> receive_from(int socket, std::span<std::uint8_t> buffer, Endpoint& from) {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    const ssize_t received = recvfrom(socket, buffer.data(), buffer.size(), 0,
                                      static_cast<sockaddr*>(static_cast<void*>(&address)), &length);
    if (received <= 0 || address.sin_family != AF_INET) {
        return std::nullopt;
    }
    std::memcpy(from.address.data(), &address.sin_addr, from.address.size());
    from.port = ntohs(address.sin_port);
    return static_cast<std::size_t>(received);
}

bool send_to(int socket, std::span<const std::uint8_t> bytes, const Endpoint& to) {
    const sockaddr_in address = address_of(to);
    return sendto(socket, bytes.data(), bytes.size(), 0,
                  static_cast<const sockaddr*>(static_cast<const void*>(&address)), sizeof(address)) ==
           static_cast<ssize_t>(bytes.size());
}

void close_socket(int socket) {
    close(socket);
}

}  // namespace ac3::sendspin::discovery::mdns_platform
