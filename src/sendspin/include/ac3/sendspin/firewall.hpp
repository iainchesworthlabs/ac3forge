#pragma once

#include <cstdint>
#include <string>

// A best-effort Windows Firewall exception for a socket mdns.cpp/websocket.cpp is about to bind
// to a real network interface, so the operating system's own "these features have been blocked"
// prompt does not fire on the first run. One file per platform, chosen by CMake (CONTRIBUTING.md,
// "One subdirectory per platform audio backend"); the POSIX side does nothing at all, since only
// Windows gates an unlisted listener like this.
//
// Never a hard failure for the caller either way: a socket binds and serves exactly as it does
// today whether or not a rule could be added, this only changes whether the operating system has
// to ask about it first.

namespace ac3::sendspin::firewall {

enum class Protocol { kTcp, kUdp };

// Enough to name and scope one inbound rule. `name` becomes part of the Windows Firewall rule's
// display name (ensure_inbound_rule() appends this executable's own name to it, so two binaries
// asking for "the same" rule do not collide - see that function's own comment) and must not
// contain a double quote, since it is also passed as one argv token to the relaunch
// maybe_run_as_firewall_helper_and_exit() answers to.
struct RuleSpec {
    std::string name;
    Protocol protocol = Protocol::kTcp;
    std::uint16_t port = 0;
};

// Makes sure an inbound rule scoped to this executable and `spec` exists, so the bind that
// follows does not trigger Windows' own prompt. Cheapest path first: once the rule exists (every
// run after the first), this is one lookup and nothing else. Missing and this process already
// runs elevated: added directly. Missing, not elevated, and a desktop session is attached: this
// relaunches its own executable once, elevated, through maybe_run_as_firewall_helper_and_exit()
// below, waits for it, and re-checks. Anything else - no desktop session (a service, a CI
// runner), the user declines the elevation prompt, the relaunch itself fails - is left for
// Windows' own prompt, same as if this were never called.
//
// Windows only; the POSIX implementation does nothing and returns true. A caller with a loopback
// bind address does not need this at all - Windows never gates loopback traffic - and skips
// calling it rather than relying on it to notice.
bool ensure_inbound_rule(const RuleSpec& spec);

// The first thing every main() that can reach ensure_inbound_rule() calls, before its own
// argument parsing or any window/session/test-run setup: when argv is the relaunch
// ensure_inbound_rule() makes of its own executable to get elevation for one rule, this adds
// that rule and calls std::exit() - it never returns in that case. An ordinary launch (argv is
// whatever the user or a shortcut actually passed) returns immediately having done nothing; the
// POSIX implementation always does this, since ensure_inbound_rule() never relaunches there.
void maybe_run_as_firewall_helper_and_exit(int argc, char** argv);

}  // namespace ac3::sendspin::firewall
