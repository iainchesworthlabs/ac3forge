#pragma once

#include <string>

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
// without a network, or once it has started. app_main calls it at boot, and
// again when a network joined after boot comes up.
void discovery_start();

// Flash mode (planning/esp32-ota.md): the Sendspin service is withdrawn, so
// servers stop dialling a board that is not listening, and the host name
// stays, so a tool still finds the board by name.
void discovery_withdraw();

// The host name the board answers to, without ".local": what its stored
// name becomes under mDNS's rules ("Sitting Room" is sitting-room).
[[nodiscard]] std::string discovery_host_name();

}  // namespace player
