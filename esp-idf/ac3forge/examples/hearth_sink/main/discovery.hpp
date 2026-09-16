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
// ADVERTISED BEFORE IT CAN BE PLAYED TO. B3 is what answers on that port; this
// is what makes the board appear in ac3hearth's and Music Assistant's lists,
// which is B2's own exit. A sink that answered nothing would be worse than
// invisible, so the advertisement waits until B3's player exists - see
// discovery_start's own comment on the port it opens today.

namespace player {

// Starts mDNS and advertises the board under its stored name. Does nothing
// without a network. Safe to call once, after network_up().
void discovery_start();

}  // namespace player
