// Winsock2 - deliberately never includes <windows.h>: nothing here needs
// anything outside the sockets API, and <winsock2.h> must precede any
// <windows.h> a translation unit might otherwise pull in (the classic
// winsock.h/winsock2.h redefinition clash windows.h's default include of
// the former would cause) - not including it at all sidesteps the ordering
// trap entirely rather than relying on getting the order right.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "../udp_socket.hpp"

#include <memory>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace ac3::audio {

std::string_view describe(UdpSocketError error) {
    switch (error) {
        case UdpSocketError::kBadAddress:
            return "not a valid IPv4 dotted-quad address";
        case UdpSocketError::kSocketFailed:
            return "socket() failed";
        case UdpSocketError::kBindFailed:
            return "bind() failed - the port may already be in use";
    }
    return "unknown error";
}

struct UdpSocket::Impl {
    // WSAStartup/WSACleanup are refcounted by Winsock itself across however
    // many times this process calls them, so pairing one of each with every
    // UdpSocket's lifetime (rather than a process-wide once-only init) is
    // correct even with several instances alive at once - each instance's
    // own pair simply adds to and later removes from the same shared count.
    WSADATA wsa_data{};
    bool wsa_started = false;
    SOCKET fd = INVALID_SOCKET;
    std::uint16_t port = 0;

    Impl() { wsa_started = WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0; }
    ~Impl() {
        if (fd != INVALID_SOCKET) {
            closesocket(fd);
        }
        if (wsa_started) {
            WSACleanup();
        }
    }
};

UdpSocket::UdpSocket() : impl_(std::make_unique<Impl>()) {}

UdpSocket::~UdpSocket() = default;  // ~Impl() closes the socket and calls WSACleanup

std::expected<void, UdpSocketError> UdpSocket::bind(std::string_view bind_address,
                                                     std::uint16_t port) {
    close();
    if (!impl_->wsa_started) {
        return std::unexpected(UdpSocketError::kSocketFailed);
    }

    const SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) {
        return std::unexpected(UdpSocketError::kSocketFailed);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    // InetPtonA explicitly, not the UNICODE-dispatched InetPton macro: this
    // project takes a std::string_view, never a TCHAR, so there is exactly
    // one encoding to support regardless of how a caller's build defines
    // UNICODE.
    const std::string address{bind_address};
    if (InetPtonA(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        closesocket(fd);
        return std::unexpected(UdpSocketError::kBadAddress);
    }

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(fd);
        return std::unexpected(UdpSocketError::kBindFailed);
    }

    // port == 0 asked the OS for an ephemeral one; read back what it
    // actually picked, same reasoning as the POSIX backend.
    std::uint16_t resolved = port;
    sockaddr_in actual{};
    int actual_len = sizeof(actual);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0) {
        resolved = ntohs(actual.sin_port);
    }

    impl_->fd = fd;
    impl_->port = resolved;
    return {};
}

std::optional<std::size_t> UdpSocket::recv(std::span<std::byte> buffer, std::uint32_t timeout_ms) {
    if (impl_->fd == INVALID_SOCKET) {
        return std::nullopt;
    }
    // SO_RCVTIMEO on Windows takes a DWORD of milliseconds directly, unlike
    // POSIX's timeval - simpler here, cheap enough to set on every call
    // (this runs on a control-rate thread, never the audio path).
    const DWORD timeout = timeout_ms;
    setsockopt(impl_->fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
              sizeof(timeout));

    // recv()'s length parameter is a plain int; this project's own receive
    // buffer is a fixed 64 KiB (LivePositionSource's own header), always
    // well inside int's range, so the cast below never truncates in
    // practice - it exists to satisfy -Wsign-conversion, not to guard
    // against a real overflow.
    const auto got = ::recv(impl_->fd, reinterpret_cast<char*>(buffer.data()),
                            static_cast<int>(buffer.size()), 0);
    if (got == SOCKET_ERROR || got < 0) {
        return std::nullopt;  // timeout (WSAETIMEDOUT) or a transient error alike
    }
    return static_cast<std::size_t>(got);
}

bool UdpSocket::send_to(std::string_view address, std::uint16_t port, std::span<const std::byte> data) {
    if (impl_->fd == INVALID_SOCKET) {
        // No bind() yet: an unbound sender needs no local address of its
        // own - the OS assigns an ephemeral one on the first send.
        impl_->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (impl_->fd == INVALID_SOCKET) {
            return false;
        }
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const std::string dest{address};
    if (InetPtonA(AF_INET, dest.c_str(), &addr.sin_addr) != 1) {
        return false;
    }
    const auto sent = sendto(impl_->fd, reinterpret_cast<const char*>(data.data()),
                             static_cast<int>(data.size()), 0,
                             reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<int>(data.size());
}

std::uint16_t UdpSocket::local_port() const { return impl_->port; }

void UdpSocket::close() {
    if (impl_->fd != INVALID_SOCKET) {
        closesocket(impl_->fd);
        impl_->fd = INVALID_SOCKET;
    }
    impl_->port = 0;
}

}  // namespace ac3::audio
