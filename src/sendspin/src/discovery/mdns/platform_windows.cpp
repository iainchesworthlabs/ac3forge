// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
// clang-format on

#include <array>
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

[[nodiscard]] SOCKET handle_of(int socket) {
    return static_cast<SOCKET>(static_cast<unsigned int>(socket));
}

[[nodiscard]] sockaddr_in address_of(const Endpoint& endpoint) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    std::memcpy(&address.sin_addr, endpoint.address.data(), endpoint.address.size());
    address.sin_port = htons(endpoint.port);
    return address;
}

}  // namespace

bool start_network() {
    static const bool started = [] {
        WSADATA data{};
        return WSAStartup(static_cast<WORD>(0x0202), &data) == 0;
    }();
    return started;
}

std::vector<Interface> ipv4_interfaces() {
    // GetAdaptersAddresses fills a caller's buffer, which must be aligned for its structures.
    ULONG size = 32 * 1024;
    std::vector<std::uint64_t> storage;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        storage.assign((size / sizeof(std::uint64_t)) + 1, 0);
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, static_cast<IP_ADAPTER_ADDRESSES*>(static_cast<void*>(storage.data())),
                                      &size);
    }
    std::vector<Interface> interfaces;
    if (result != NO_ERROR) {
        return interfaces;
    }
    for (const IP_ADAPTER_ADDRESSES* adapter = static_cast<IP_ADAPTER_ADDRESSES*>(static_cast<void*>(storage.data()));
         adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
            adapter->IfType == IF_TYPE_TUNNEL || (adapter->Flags & IP_ADAPTER_NO_MULTICAST) != 0) {
            continue;
        }
        for (const IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr || unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            Interface found;
            const auto* in = static_cast<const sockaddr_in*>(static_cast<const void*>(unicast->Address.lpSockaddr));
            std::memcpy(found.address.data(), &in->sin_addr, found.address.size());
            found.prefix_length = unicast->OnLinkPrefixLength;
            if (found.address[0] != 127) {
                interfaces.push_back(found);
            }
        }
    }
    return interfaces;
}

std::string host_name() {
    std::array<char, 256> name{};
    DWORD length = static_cast<DWORD>(name.size());
    if (GetComputerNameExA(ComputerNameDnsHostname, name.data(), &length) == 0) {
        return {};
    }
    return {name.data(), length};
}

std::vector<std::size_t> wait_readable(const std::vector<int>& sockets, int timeout_ms) {
    std::vector<WSAPOLLFD> polled;
    polled.reserve(sockets.size());
    for (const int socket : sockets) {
        polled.push_back({.fd = handle_of(socket), .events = POLLRDNORM, .revents = 0});
    }
    std::vector<std::size_t> readable;
    if (polled.empty() || WSAPoll(polled.data(), static_cast<ULONG>(polled.size()), timeout_ms) <= 0) {
        return readable;
    }
    for (std::size_t i = 0; i < polled.size(); ++i) {
        if ((polled[i].revents & (POLLRDNORM | POLLERR | POLLHUP)) != 0) {
            readable.push_back(i);
        }
    }
    return readable;
}

std::optional<std::size_t> receive_from(int socket, std::span<std::uint8_t> buffer, Endpoint& from) {
    sockaddr_in address{};
    int length = sizeof(address);
    const int received = recvfrom(handle_of(socket), static_cast<char*>(static_cast<void*>(buffer.data())),
                                  static_cast<int>(buffer.size()), 0,
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
    return sendto(handle_of(socket), static_cast<const char*>(static_cast<const void*>(bytes.data())),
                  static_cast<int>(bytes.size()), 0, static_cast<const sockaddr*>(static_cast<const void*>(&address)),
                  sizeof(address)) == static_cast<int>(bytes.size());
}

void close_socket(int socket) {
    closesocket(handle_of(socket));
}

}  // namespace ac3::sendspin::discovery::mdns_platform
