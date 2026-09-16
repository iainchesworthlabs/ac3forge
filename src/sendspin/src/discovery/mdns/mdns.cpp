#include "ac3/sendspin/mdns.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <mdns.h>

#include "ac3/sendspin/discovery.hpp"
#include "packets.hpp"
#include "platform.hpp"

namespace ac3::sendspin::discovery::mdns {

namespace {

namespace packets = mdns_packets;
namespace platform = mdns_platform;
using Steady = std::chrono::steady_clock;

constexpr int kWaitMs = 200;
constexpr std::int64_t kSecond = 1'000'000;
constexpr std::int64_t kMaxQueryInterval = 60 * kSecond;
const platform::Endpoint kMulticast{.address = {224, 0, 0, 251}, .port = packets::kPort};

[[nodiscard]] std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(Steady::now().time_since_epoch()).count();
}

[[nodiscard]] std::optional<platform::Ipv4> parse_ipv4(std::string_view text) {
    platform::Ipv4 address{};
    std::size_t part = 0;
    unsigned value = 0;
    bool digits = false;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            value = (value * 10) + static_cast<unsigned>(c - '0');
            if (value > 255) {
                return std::nullopt;
            }
            digits = true;
        } else if (c == '.' && digits && part < 3) {
            address[part++] = static_cast<std::uint8_t>(value);
            value = 0;
            digits = false;
        } else {
            return std::nullopt;
        }
    }
    if (!digits || part != 3) {
        return std::nullopt;
    }
    address[3] = static_cast<std::uint8_t>(value);
    return address;
}

[[nodiscard]] std::uint32_t as_number(const platform::Ipv4& address) {
    return (std::uint32_t{address[0]} << 24U) | (std::uint32_t{address[1]} << 16U) | (std::uint32_t{address[2]} << 8U) |
           address[3];
}

struct Socket {
    int handle = -1;
    platform::Interface link;
};

// One mDNS socket on port 5353 per link.
class Sockets {
   public:
    explicit Sockets(const Options& options) {
        if (!platform::start_network()) {
            return;
        }
        std::vector<platform::Interface> links;
        if (options.interfaces.empty()) {
            links = platform::ipv4_interfaces();
        } else {
            const std::vector<platform::Interface> known = platform::ipv4_interfaces();
            for (const std::string& text : options.interfaces) {
                const std::optional<platform::Ipv4> address = parse_ipv4(text);
                if (!address) {
                    continue;
                }
                const auto match = std::find_if(known.begin(), known.end(), [&](const platform::Interface& link) {
                    return link.address == *address;
                });
                links.push_back(match != known.end() ? *match : platform::Interface{.address = *address});
            }
        }
        for (const platform::Interface& link : links) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            std::memcpy(&address.sin_addr, link.address.data(), link.address.size());
            // Port 5353 in network byte order.
            const std::array<std::uint8_t, 2> port{0x14, 0xE9};
            std::memcpy(&address.sin_port, port.data(), port.size());
            const int handle = mdns_socket_open_ipv4(&address);
            if (handle >= 0) {
                sockets_.push_back({.handle = handle, .link = link});
                handles_.push_back(handle);
            }
        }
    }

    ~Sockets() {
        for (const Socket& socket : sockets_) {
            platform::close_socket(socket.handle);
        }
    }
    Sockets(const Sockets&) = delete;
    Sockets& operator=(const Sockets&) = delete;
    Sockets(Sockets&&) = delete;
    Sockets& operator=(Sockets&&) = delete;

    [[nodiscard]] bool empty() const { return sockets_.empty(); }
    [[nodiscard]] std::size_t size() const { return sockets_.size(); }
    [[nodiscard]] const Socket& operator[](std::size_t index) const { return sockets_[index]; }
    [[nodiscard]] const std::vector<int>& handles() const { return handles_; }

    // The socket on the link whose network holds `peer`, or `fallback`.
    [[nodiscard]] std::size_t on_link_with(const platform::Ipv4& peer, std::size_t fallback) const {
        for (std::size_t i = 0; i < sockets_.size(); ++i) {
            const unsigned prefix = (std::min<unsigned>)(sockets_[i].link.prefix_length, 32U);
            const std::uint32_t mask = prefix == 0 ? 0U : ~std::uint32_t{0} << (32U - prefix);
            if ((as_number(sockets_[i].link.address) & mask) == (as_number(peer) & mask)) {
                return i;
            }
        }
        return fallback;
    }

    // Reads every datagram waiting on any socket for up to kWaitMs, handing each to `take` with
    // the position of the socket it arrived on.
    void receive(const std::function<void(std::span<const std::uint8_t>, const platform::Endpoint&, std::size_t)>& take) {
        for (const std::size_t index : platform::wait_readable(handles_, kWaitMs)) {
            platform::Endpoint from;
            while (const std::optional<std::size_t> size = platform::receive_from(handles_[index], buffer_, from)) {
                take(std::span<const std::uint8_t>(buffer_).first(*size), from, index);
            }
        }
    }

   private:
    std::vector<Socket> sockets_;
    std::vector<int> handles_;
    std::vector<std::uint8_t> buffer_ = std::vector<std::uint8_t>(packets::kMaxRead);
};

// Where the same multicast datagram reaches several sockets, as Linux delivers it to every socket
// that joined the group on any link, it is handled once.
class Recent {
   public:
    [[nodiscard]] bool seen(std::span<const std::uint8_t> bytes, const platform::Endpoint& from, std::int64_t now) {
        std::erase_if(entries_, [now](const Entry& entry) { return now - entry.at > kWindow; });
        const std::size_t hash = std::hash<std::string_view>{}(
            std::string_view(static_cast<const char*>(static_cast<const void*>(bytes.data())), bytes.size()));
        const bool found = std::any_of(entries_.begin(), entries_.end(), [&](const Entry& entry) {
            return entry.hash == hash && entry.from.address == from.address && entry.from.port == from.port;
        });
        if (!found) {
            entries_.push_back({.hash = hash, .from = from, .at = now});
        }
        return found;
    }

   private:
    static constexpr std::int64_t kWindow = 100'000;
    struct Entry {
        std::size_t hash = 0;
        platform::Endpoint from;
        std::int64_t at = 0;
    };
    std::deque<Entry> entries_;
};

class MdnsAdvertiser final : public Advertiser {
   public:
    MdnsAdvertiser(std::unique_ptr<Sockets> sockets, const Advertisement& advertisement, const std::string& host)
        : sockets_(std::move(sockets)) {
        for (std::size_t i = 0; i < sockets_->size(); ++i) {
            responders_.emplace_back(advertisement, host, (*sockets_)[i].link.address);
        }
        thread_ = std::thread([this] { run(); });
    }

    ~MdnsAdvertiser() override {
        stop_ = true;
        thread_.join();
        for (std::size_t i = 0; i < sockets_->size(); ++i) {
            (void)platform::send_to((*sockets_)[i].handle, responders_[i].announcement(true), kMulticast);
        }
    }
    MdnsAdvertiser(const MdnsAdvertiser&) = delete;
    MdnsAdvertiser& operator=(const MdnsAdvertiser&) = delete;
    MdnsAdvertiser(MdnsAdvertiser&&) = delete;
    MdnsAdvertiser& operator=(MdnsAdvertiser&&) = delete;

   private:
    void announce() {
        for (std::size_t i = 0; i < sockets_->size(); ++i) {
            (void)platform::send_to((*sockets_)[i].handle, responders_[i].announcement(false), kMulticast);
        }
    }

    void run() {
        // Two announcements a second apart (RFC 6762, section 8.3).
        const std::int64_t started = now_us();
        int announced = 0;
        while (!stop_) {
            if (announced < 2 && now_us() >= started + (announced * kSecond)) {
                announce();
                ++announced;
            }
            sockets_->receive([&](std::span<const std::uint8_t> bytes, const platform::Endpoint& from, std::size_t index) {
                if (recent_.seen(bytes, from, now_us())) {
                    return;
                }
                const std::optional<packets::Packet> packet = packets::parse(bytes);
                if (!packet || packet->response) {
                    return;
                }
                // Answer with the records of the link on the asker's network.
                const std::size_t on_link = sockets_->on_link_with(from.address, index);
                const bool legacy = from.port != packets::kPort;
                for (const packets::Question& question : packet->questions) {
                    const std::optional<std::vector<std::uint8_t>> answer =
                        responders_[on_link].respond(question, packet->id, legacy);
                    if (answer) {
                        (void)platform::send_to((*sockets_)[on_link].handle, *answer,
                                                legacy || question.unicast ? from : kMulticast);
                    }
                }
            });
        }
    }

    std::unique_ptr<Sockets> sockets_;
    std::vector<packets::Responder> responders_;
    Recent recent_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

class MdnsBrowser final : public Browser {
   public:
    MdnsBrowser(std::unique_ptr<Sockets> sockets, std::string_view service, BrowseListener& listener)
        : sockets_(std::move(sockets)), state_(service), listener_(&listener) {
        thread_ = std::thread([this] { run(); });
    }

    ~MdnsBrowser() override {
        stop_ = true;
        thread_.join();
    }
    MdnsBrowser(const MdnsBrowser&) = delete;
    MdnsBrowser& operator=(const MdnsBrowser&) = delete;
    MdnsBrowser(MdnsBrowser&&) = delete;
    MdnsBrowser& operator=(MdnsBrowser&&) = delete;

    void refresh() override { refresh_ = true; }

   private:
    void send(const packets::Question& question) {
        const std::vector<std::uint8_t> packet = packets::query(question);
        for (std::size_t i = 0; i < sockets_->size(); ++i) {
            (void)platform::send_to((*sockets_)[i].handle, packet, kMulticast);
        }
    }

    void deliver(const packets::BrowseState::Changes& changes) {
        for (const Service& service : changes.found) {
            listener_->on_found(service);
        }
        for (const std::string& instance : changes.lost) {
            listener_->on_lost(instance);
        }
    }

    void run() {
        // Queries at 1 s, 2 s, 4 s and so on, up to once a minute (RFC 6762, section 5.2).
        std::int64_t next_query = now_us();
        std::int64_t interval = kSecond;
        std::int64_t next_missing = next_query + kSecond;
        while (!stop_) {
            const std::int64_t now = now_us();
            if (refresh_.exchange(false)) {
                next_query = now;
                interval = kSecond;
            }
            if (now >= next_query) {
                send({.name = state_.service_name(), .type = packets::kTypePtr, .unicast = false});
                next_query = now + interval;
                interval = (std::min)(interval * 2, kMaxQueryInterval);
            }
            if (now >= next_missing) {
                for (const packets::Question& question : state_.missing(now)) {
                    send(question);
                }
                next_missing = now + kSecond;
            }
            sockets_->receive([&](std::span<const std::uint8_t> bytes, const platform::Endpoint& from, std::size_t) {
                const std::int64_t at = now_us();
                if (recent_.seen(bytes, from, at)) {
                    return;
                }
                if (const std::optional<packets::Packet> packet = packets::parse(bytes)) {
                    deliver(state_.receive(*packet, at));
                }
            });
            deliver(state_.expire(now_us()));
        }
    }

    std::unique_ptr<Sockets> sockets_;
    packets::BrowseState state_;
    BrowseListener* listener_;
    Recent recent_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> refresh_{false};
    std::thread thread_;
};

}  // namespace

std::unique_ptr<Advertiser> advertise(Advertisement advertisement, Options options) {
    auto sockets = std::make_unique<Sockets>(options);
    if (sockets->empty()) {
        return nullptr;
    }
    const std::string host = options.host.empty() ? platform::host_name() : options.host;
    return std::make_unique<MdnsAdvertiser>(std::move(sockets), advertisement, host);
}

std::unique_ptr<Browser> browse(std::string service, BrowseListener& listener, Options options) {
    auto sockets = std::make_unique<Sockets>(options);
    if (sockets->empty()) {
        return nullptr;
    }
    return std::make_unique<MdnsBrowser>(std::move(sockets), service, listener);
}

}  // namespace ac3::sendspin::discovery::mdns
