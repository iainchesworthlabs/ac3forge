#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ac3/sendspin/discovery.hpp"

// The discovery seam's backend on a computer: DNS-SD over IPv4 mDNS with mjansson's mdns.
//
// Each uses one socket on UDP port 5353 per interface, so it answers and announces on every
// network the computer is on with that network's own address, a VMware or VPN adapter beside
// the LAN included. Answers follow RFC 6762: multicast unless the question asked for unicast,
// and the legacy unicast form for a query from a port other than 5353. An advertiser announces
// twice on start and says goodbye when destroyed; a browser queries at 1 s, 2 s, 4 s and so on
// up to once a minute, asks for the SRV, TXT and A records a response left out, and expires
// what it learnt by the records' TTLs.
//
// Not implemented: probing for a unique name (RFC 6762, section 8), so instances on one network
// need distinct names, which a friendly name usually gives; and IPv6.
//
// Each advertiser and browser runs a thread of its own. A browser calls its listener on that
// thread.

namespace ac3::sendspin::discovery::mdns {

struct Options {
    // The IPv4 addresses of the interfaces to use; empty for every interface that is up, can
    // multicast, and is not a loopback.
    std::vector<std::string> interfaces;
    // The host name the SRV record names, without .local; empty for the computer's own.
    std::string host;
    // Whether opening this socket should also make sure Windows' firewall allows inbound
    // traffic to it (firewall::ensure_inbound_rule(), ac3/sendspin/firewall.hpp) - on an
    // unelevated interactive session, that means relaunching this same process once for a UAC
    // prompt. True by default: a real advertiser or browser needs replies from other machines
    // to actually arrive. False for a caller whose socket never needs one, such as a test that
    // drives its BrowseListener with synthetic on_found()/on_lost() calls instead of real mDNS
    // traffic - left true, a binary with no main() of its own to answer the relaunch's
    // `--ac3-sendspin-firewall-helper` handshake (Catch2's, for instance) would just re-prompt
    // for elevation on every run, for a rule it can never actually finish adding.
    bool request_firewall_exception = true;
};

// Starts advertising. Nothing when no interface's socket could be opened.
[[nodiscard]] std::unique_ptr<Advertiser> advertise(Advertisement advertisement, Options options = {});

// Starts browsing for `service`, such as kPlayerService. Nothing when no interface's socket
// could be opened.
[[nodiscard]] std::unique_ptr<Browser> browse(std::string service, BrowseListener& listener, Options options = {});

}  // namespace ac3::sendspin::discovery::mdns
