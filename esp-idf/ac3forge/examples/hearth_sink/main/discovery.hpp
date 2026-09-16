#pragma once

// How a server finds this sink: mDNS, advertising the Sendspin player service
// (planning/hearth-reference-player.md B2, planning/hearth-sendspin-extension.md
// row T4).
//
//   _sendspin._tcp on port 8928, TXT path=/sendspin and name=<the board's>
//
// `path` is required of a player by the specification - it is where the server
// opens the WebSocket - and `name` should match what the player will say in
// client/hello, which is the board's own name (settings.hpp).
//
// The Sendspin player (sendspin.hpp) is what answers on that port, so a build
// without one advertises its host name and no service: a sink a server found
// and could not open would be worse than one it never listed.

namespace player {

// Starts mDNS and advertises the board under its stored name. Does nothing
// without a network. Safe to call once, after network_up().
void discovery_start();

}  // namespace player
