#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The discovery seam: DNS-SD over mDNS (connection.md, Establishing a Connection). A client
// that waits for servers to dial it advertises _sendspin._tcp, and a server that waits for
// clients advertises _sendspin-server._tcp; each browses for the other's. Sessions never see
// discovery: an owner advertises the listener it runs, and dials what browsing finds.
//
// On a computer the backend is mjansson's mdns (mdns.hpp); a board brings espressif/mdns.

namespace ac3::sendspin::discovery {

// The service types, without the .local domain.
inline constexpr std::string_view kPlayerService = "_sendspin._tcp";
inline constexpr std::string_view kServerService = "_sendspin-server._tcp";

struct TxtEntry {
    std::string key;
    std::string value;

    bool operator==(const TxtEntry&) const = default;
};

// What one listener advertises.
struct Advertisement {
    // kPlayerService or kServerService.
    std::string service;
    // The friendly name, which should match client/hello's or server/hello's. A DNS label holds
    // at most 63 bytes, and the backend replaces a dot, which its names cannot escape.
    std::string instance;
    std::uint16_t port = 0;
    // `path` is required and `name` recommended (connection.md).
    std::vector<TxtEntry> txt;
};

// A service instance browsing has found, with its host's addresses.
struct Service {
    std::string instance;
    std::string host;
    // IPv4, dotted decimal.
    std::vector<std::string> addresses;
    std::uint16_t port = 0;
    std::vector<TxtEntry> txt;

    [[nodiscard]] std::optional<std::string> txt_value(std::string_view key) const;
    // ws://<address>:<port><path> for the first address, with the TXT record's path, or
    // /sendspin where the record has none; nothing without an address or a port.
    [[nodiscard]] std::optional<std::string> url() const;

    bool operator==(const Service&) const = default;
};

// Advertises while it exists; destroying it withdraws the advertisement.
class Advertiser {
   public:
    Advertiser() = default;
    virtual ~Advertiser() = default;
    Advertiser(const Advertiser&) = delete;
    Advertiser& operator=(const Advertiser&) = delete;
    Advertiser(Advertiser&&) = delete;
    Advertiser& operator=(Advertiser&&) = delete;
};

class BrowseListener {
   public:
    BrowseListener() = default;
    virtual ~BrowseListener() = default;
    BrowseListener(const BrowseListener&) = delete;
    BrowseListener& operator=(const BrowseListener&) = delete;
    BrowseListener(BrowseListener&&) = delete;
    BrowseListener& operator=(BrowseListener&&) = delete;

    // An instance was found, or its host, addresses, port or TXT record changed.
    virtual void on_found(const Service& service) = 0;
    // An instance said goodbye, or its records expired.
    virtual void on_lost(const std::string& instance) = 0;
};

// Browses while it exists.
class Browser {
   public:
    Browser() = default;
    virtual ~Browser() = default;
    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;
    Browser(Browser&&) = delete;
    Browser& operator=(Browser&&) = delete;

    // Queries now instead of at the next scheduled query.
    virtual void refresh() = 0;
};

}  // namespace ac3::sendspin::discovery
