// mDNS: the board's name and its Sendspin service. See discovery.hpp.

#include "discovery.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

#include "mdns.h"

#include "network.hpp"
#include "settings.hpp"

namespace player {
namespace {

// planning/hearth-sendspin-extension.md, row T4: a player advertises
// _sendspin._tcp with the path the server opens its WebSocket on. The port is
// the one that page fixes for a player, and B3 is what answers on it - until
// then this board is found and then found wanting, which is the order the plan
// puts these phases in.
constexpr const char* kService = "_sendspin";
constexpr const char* kProtocol = "_tcp";
constexpr std::uint16_t kSendspinPort = 8927;
constexpr const char* kPath = "/sendspin";

// mDNS hostnames take letters, digits and hyphens. A name someone typed into
// the page may have anything in it, so it is filtered here rather than
// refused there: "Sitting Room" becomes "sitting-room" and still answers.
void hostname_from(const char* name, std::span<char> out) {
    std::size_t at = 0;
    for (const char* c = name; *c != '\0' && at + 1 < out.size(); ++c) {
        const char lower = (*c >= 'A' && *c <= 'Z') ? static_cast<char>(*c - 'A' + 'a') : *c;
        const bool keep = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9');
        if (keep) {
            out[at++] = lower;
        } else if (at > 0 && out[at - 1] != '-') {
            out[at++] = '-';
        }
    }
    while (at > 0 && out[at - 1] == '-') {
        --at;  // no trailing hyphen, which a label may not have
    }
    out[at] = '\0';
    if (at == 0) {
        (void)std::snprintf(out.data(), out.size(), "hearth");
    }
}

}  // namespace

void discovery_start() {
    static bool started = false;
    if (started) {
        return;
    }
    if (!network_ready()) {
        std::printf("mdns: no network, so nothing to advertise\n");
        return;
    }
    if (mdns_init() != ESP_OK) {
        std::printf("warning: mdns did not start; this sink will not be found by name\n");
        return;
    }
    started = true;

    const char* name = settings().name.data();
    std::array<char, kMaxNameBytes + 1> host{};
    hostname_from(name, host);
    (void)mdns_hostname_set(host.data());
    (void)mdns_instance_name_set(name);

    const std::array<mdns_txt_item_t, 2> txt{{
        {"path", kPath},
        {"name", name},
    }};
    if (mdns_service_add(name, kService, kProtocol, kSendspinPort,
                         const_cast<mdns_txt_item_t*>(txt.data()), txt.size()) != ESP_OK) {
        std::printf("warning: mdns could not advertise %s.%s\n", kService, kProtocol);
        return;
    }
    std::printf("mdns: %s.local, %s.%s on %u, path %s\n", host.data(), kService, kProtocol,
                static_cast<unsigned>(kSendspinPort), kPath);
}

}  // namespace player
