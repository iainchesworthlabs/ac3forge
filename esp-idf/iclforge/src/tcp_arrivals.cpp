// The board's side of ../include/ac3forge/tcp_arrivals.hpp: the IPv4 input
// hook that logs the watched port's streams, and their readers' calls.

#include "ac3forge/tcp_arrivals.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/pbuf.h"

#include "ac3forge_lwip_hooks.h"
#include "peer/ipv4_peer.hpp"

namespace ac3forge::tcp_arrivals {
namespace {

// Streams logged at once: SendspinHost's four connections, and one that
// began and was never claimed.
constexpr std::size_t kStreams = 5;

struct Stream {
    bool used = false;
    bool claimed = false;
    std::uint32_t address = 0;  // network byte order
    std::uint16_t port = 0;
    std::int64_t began_us = 0;
    ArrivalLog log;
};

std::array<Stream, kStreams> g_streams;
// Held by the network task and the readers for a few instructions at a time.
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<std::uint16_t> g_port{0};

[[nodiscard]] std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8U) | p[1]);
}

[[nodiscard]] std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24U) | (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) | p[3];
}

[[nodiscard]] Stream* find(std::uint32_t address, std::uint16_t port) {
    for (Stream& stream : g_streams) {
        if (stream.used && stream.address == address && stream.port == port) {
            return &stream;
        }
    }
    return nullptr;
}

// Where a stream that has just begun is logged: its own entry again for a
// SYN sent twice, else a free one, else the oldest nobody claimed. A claimed
// entry is its reader's until released.
[[nodiscard]] Stream* entry_for(std::uint32_t address, std::uint16_t port) {
    if (Stream* same = find(address, port)) {
        return same->claimed ? nullptr : same;
    }
    Stream* pick = nullptr;
    for (Stream& stream : g_streams) {
        if (!stream.used) {
            return &stream;
        }
        if (!stream.claimed && (pick == nullptr || stream.began_us < pick->began_us)) {
            pick = &stream;
        }
    }
    return pick;
}

[[nodiscard]] bool valid(int handle) { return handle >= 0 && static_cast<std::size_t>(handle) < kStreams; }

}  // namespace

void watch(std::uint16_t port) { g_port.store(port); }

int claim(int fd) {
    const std::optional<Ipv4Peer> peer = ipv4_peer(fd);
    if (!peer) {
        return -1;
    }
    int handle = -1;
    portENTER_CRITICAL(&g_lock);
    for (std::size_t i = 0; i < kStreams; ++i) {
        Stream& stream = g_streams[i];
        if (stream.used && !stream.claimed && stream.address == peer->address && stream.port == peer->port) {
            stream.claimed = true;
            handle = static_cast<int>(i);
            break;
        }
    }
    portEXIT_CRITICAL(&g_lock);
    return handle;
}

void release(int handle) {
    if (!valid(handle)) {
        return;
    }
    portENTER_CRITICAL(&g_lock);
    g_streams[static_cast<std::size_t>(handle)].used = false;
    g_streams[static_cast<std::size_t>(handle)].claimed = false;
    portEXIT_CRITICAL(&g_lock);
}

std::optional<std::int64_t> arrival(int handle, std::uint64_t end) {
    if (!valid(handle)) {
        return std::nullopt;
    }
    portENTER_CRITICAL(&g_lock);
    const std::optional<std::int64_t> at = g_streams[static_cast<std::size_t>(handle)].log.arrival(end);
    portEXIT_CRITICAL(&g_lock);
    return at;
}

}  // namespace ac3forge::tcp_arrivals

// lwIP calls this for every IPv4 packet, on its own task, before it has
// checked the packet: every length is checked here. 0 lets lwIP carry on.
int ac3forge_lwip_ip4_input(struct pbuf* p, struct netif* /*inp*/) {
    namespace ta = ac3forge::tcp_arrivals;
    const std::uint16_t port = ta::g_port.load(std::memory_order_relaxed);
    if (port == 0 || p == nullptr || p->len < 40) {
        return 0;
    }
    const auto* ip = static_cast<const std::uint8_t*>(p->payload);
    const std::size_t ip_bytes = static_cast<std::size_t>(ip[0] & 0x0FU) * 4U;
    constexpr std::uint8_t kTcp = 6;
    // Not TCP, a header that is not all in the first buffer, or a fragment.
    if (ip[9] != kTcp || ip_bytes < 20 || p->len < ip_bytes + 20 || (ta::be16(ip + 6) & 0x3FFFU) != 0) {
        return 0;
    }
    const std::uint8_t* tcp = ip + ip_bytes;
    if (ta::be16(tcp + 2) != port) {
        return 0;
    }
    const std::size_t total = ta::be16(ip + 2);
    const std::size_t tcp_bytes = static_cast<std::size_t>(tcp[12] >> 4U) * 4U;
    if (tcp_bytes < 20 || total < ip_bytes + tcp_bytes) {
        return 0;
    }
    std::uint32_t address = 0;
    std::memcpy(&address, ip + 12, sizeof(address));
    const std::uint16_t from = ta::be16(tcp);
    const std::uint32_t sequence = ta::be32(tcp + 4);
    const std::size_t length = total - ip_bytes - tcp_bytes;
    constexpr std::uint8_t kSyn = 0x02;
    constexpr std::uint8_t kAck = 0x10;
    const bool opens = (tcp[13] & (kSyn | kAck)) == kSyn;
    const std::int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&ta::g_lock);
    if (opens) {
        if (ta::Stream* stream = ta::entry_for(address, from)) {
            stream->used = true;
            stream->claimed = false;
            stream->address = address;
            stream->port = from;
            stream->began_us = now;
            stream->log.start(sequence + 1);
        }
    } else if (length > 0) {
        if (ta::Stream* stream = ta::find(address, from)) {
            stream->log.segment(sequence, length, now);
        }
    }
    portEXIT_CRITICAL(&ta::g_lock);
    return 0;
}
