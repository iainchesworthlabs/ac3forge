#include "../udp_socket.hpp"

// Berkeley sockets - Linux, macOS and Android all implement this identically
// (Android's bionic libc included), which is exactly why this file exists on
// its own small axis rather than as a fifth file repeated across
// src/audio/src/backend/{alsa,pipewire,macos,android,posix}/ - see
// udp_socket.hpp's own header comment.

#include <arpa/inet.h>
#include <cerrno>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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
    int fd = -1;
    std::uint16_t port = 0;
};

UdpSocket::UdpSocket() : impl_(std::make_unique<Impl>()) {}

UdpSocket::~UdpSocket() { close(); }

std::expected<void, UdpSocketError> UdpSocket::bind(std::string_view bind_address,
                                                     std::uint16_t port) {
    close();

    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return std::unexpected(UdpSocketError::kSocketFailed);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    // inet_pton needs a NUL-terminated C string; bind_address (a
    // std::string_view) is not guaranteed to carry one.
    const std::string address{bind_address};
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return std::unexpected(UdpSocketError::kBadAddress);
    }

    // The only reinterpret_cast in this file, and unavoidable: sockaddr and
    // sockaddr_in are deliberately unrelated types the POSIX sockets API
    // still requires this exact reinterpretation of - there is no other way
    // to call bind() at all.
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return std::unexpected(UdpSocketError::kBindFailed);
    }

    // port == 0 asked the OS for an ephemeral one; read back what it
    // actually picked. A getsockname() failure right after a successful
    // bind() is not a real-world case worth a distinct error for - it just
    // leaves local_port() reporting the (possibly 0) port that was asked
    // for, same as before this call.
    std::uint16_t resolved = port;
    sockaddr_in actual{};
    socklen_t actual_len = sizeof(actual);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0) {
        resolved = ntohs(actual.sin_port);
    }

    impl_->fd = fd;
    impl_->port = resolved;
    return {};
}

std::optional<std::size_t> UdpSocket::recv(std::span<std::byte> buffer, std::uint32_t timeout_ms) {
    if (impl_->fd < 0) {
        return std::nullopt;
    }
    timeval tv{.tv_sec = static_cast<time_t>(timeout_ms / 1000),
              .tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000)};
    // Cheap enough to set on every call (this runs on a control-rate thread,
    // never the audio path) and keeps the API stateless with respect to the
    // timeout a caller passes.
    ::setsockopt(impl_->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const auto got = ::recv(impl_->fd, buffer.data(), buffer.size(), 0);
    if (got < 0) {
        return std::nullopt;  // timeout (EAGAIN/EWOULDBLOCK) or a transient error alike
    }
    return static_cast<std::size_t>(got);
}

bool UdpSocket::send_to(std::string_view address, std::uint16_t port, std::span<const std::byte> data) {
    if (impl_->fd < 0) {
        // No bind() yet: an unbound sender needs no local address of its
        // own - the OS assigns an ephemeral one on the first send.
        impl_->fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (impl_->fd < 0) {
            return false;
        }
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const std::string dest{address};
    if (::inet_pton(AF_INET, dest.c_str(), &addr.sin_addr) != 1) {
        return false;
    }
    const auto sent =
        ::sendto(impl_->fd, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&addr),
                sizeof(addr));
    return sent == static_cast<ssize_t>(data.size());
}

std::uint16_t UdpSocket::local_port() const { return impl_->port; }

void UdpSocket::close() {
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
    impl_->port = 0;
}

}  // namespace ac3::audio
