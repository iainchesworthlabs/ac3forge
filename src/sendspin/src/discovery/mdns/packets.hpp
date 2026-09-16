#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/sendspin/discovery.hpp"

// mDNS packets for the mdns backend (ac3/sendspin/mdns.hpp), apart from its sockets: reading a
// received packet, what one advertisement answers with, and what a browser learns from what it
// receives. mdns.cpp carries the packets over the network; the tests run everything here.

namespace ac3::sendspin::discovery::mdns_packets {

inline constexpr std::uint16_t kTypeA = 1;
inline constexpr std::uint16_t kTypePtr = 12;
inline constexpr std::uint16_t kTypeTxt = 16;
inline constexpr std::uint16_t kTypeSrv = 33;
inline constexpr std::uint16_t kTypeAny = 255;
inline constexpr std::uint16_t kPort = 5353;
// The largest packet this backend writes, which fits an Ethernet frame (RFC 6762, section 17).
inline constexpr std::size_t kMaxWrite = 1472;
// The largest it reads.
inline constexpr std::size_t kMaxRead = 9000;
// The DNS-SD service type enumeration (RFC 6763, section 9).
inline constexpr std::string_view kServiceTypes = "_services._dns-sd._udp.local.";

using Ipv4 = std::array<std::uint8_t, 4>;

struct Question {
    // With a trailing dot, as in "_sendspin._tcp.local.".
    std::string name;
    std::uint16_t type = 0;
    // The QU bit: the asker wants a unicast response.
    bool unicast = false;
};

struct Record {
    std::string name;
    std::uint16_t type = 0;
    std::uint32_t ttl = 0;
    // PTR: the name pointed to. SRV: the host.
    std::string target;
    // SRV.
    std::uint16_t port = 0;
    // A.
    std::optional<Ipv4> address;
    // TXT.
    std::vector<TxtEntry> txt;
};

struct Packet {
    std::uint16_t id = 0;
    bool response = false;
    std::vector<Question> questions;
    // Answers, authority and additional records alike.
    std::vector<Record> records;
};

// Reads a received packet: nothing when its header or a question is malformed. Reading stops at
// the first malformed record, keeping those before it.
[[nodiscard]] std::optional<Packet> parse(std::span<const std::uint8_t> bytes);

// Whether two names are the same, ignoring ASCII case and a trailing dot.
[[nodiscard]] bool same_name(std::string_view a, std::string_view b);

// A query holding one question.
[[nodiscard]] std::vector<std::uint8_t> query(const Question& question);

// "Kitchen" with dots replaced and cut to a DNS label's 63 bytes at a UTF-8 boundary.
[[nodiscard]] std::string instance_label(std::string_view friendly_name);
// A host name reduced to letters, digits and hyphens, cut to 63 bytes; "sendspin" when nothing
// is left.
[[nodiscard]] std::string host_label(std::string_view host_name);

// What one advertisement answers with, on the interface with `address`.
class Responder {
   public:
    Responder(const Advertisement& advertisement, std::string_view host, Ipv4 address);

    // The response to `question` from a packet with `id`, or nothing when the question is about
    // something else. `legacy` for a query from a port other than 5353, which is answered to
    // that port with the question and id repeated, short TTLs and no cache-flush bits (RFC 6762,
    // section 6.7).
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> respond(const Question& question, std::uint16_t id,
                                                                    bool legacy) const;
    // An unsolicited response announcing every record, or with `goodbye` withdrawing them.
    [[nodiscard]] std::vector<std::uint8_t> announcement(bool goodbye) const;

    // "_sendspin._tcp.local.", "Kitchen._sendspin._tcp.local." and "desk.local.".
    [[nodiscard]] const std::string& service_name() const { return service_; }
    [[nodiscard]] const std::string& instance_name() const { return instance_; }
    [[nodiscard]] const std::string& host_name() const { return host_; }

   private:
    std::string service_;
    std::string instance_;
    std::string host_;
    std::uint16_t port_;
    Ipv4 address_;
    std::vector<TxtEntry> txt_;
};

// What a browser knows about one service type, from the responses it has received.
class BrowseState {
   public:
    // At most this many instances and hosts are remembered, whatever the network sends.
    static constexpr std::size_t kMaxEntries = 64;

    explicit BrowseState(std::string_view service);

    struct Changes {
        std::vector<Service> found;
        std::vector<std::string> lost;
    };

    // Takes in a packet received at `now`, in microseconds.
    [[nodiscard]] Changes receive(const Packet& packet, std::int64_t now);
    // Forgets what has expired by `now`.
    [[nodiscard]] Changes expire(std::int64_t now);
    // Questions for the records a known instance still lacks: its SRV and TXT records, and its
    // host's address.
    [[nodiscard]] std::vector<Question> missing(std::int64_t now) const;

    [[nodiscard]] const std::string& service_name() const { return service_; }

   private:
    struct Instance {
        std::string name;
        std::int64_t expires = 0;
        std::string host;
        std::uint16_t port = 0;
        std::int64_t location_expires = 0;
        std::optional<std::vector<TxtEntry>> txt;
        std::optional<Service> reported;
    };
    struct Address {
        Ipv4 address{};
        std::int64_t expires = 0;
    };

    [[nodiscard]] Changes report(std::int64_t now);

    std::string service_;
    // Keyed by name in lower case.
    std::map<std::string, Instance> instances_;
    std::map<std::string, std::vector<Address>> hosts_;
};

}  // namespace ac3::sendspin::discovery::mdns_packets
