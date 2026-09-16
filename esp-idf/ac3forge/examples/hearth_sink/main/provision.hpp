#pragma once

// Improv Wi-Fi over the console serial port: how a board with nothing stored
// is told which network to join (planning/hearth-reference-player.md, B2).
//
// A browser page at improv-wifi.com, or ESPHome's tooling, opens the same port
// the console prints on, asks the board what it is, and hands it an SSID and a
// password. The board stores them (settings.hpp) and brings its network up.
// Nothing here is Hearth's own invention: the packet format is
// ac3forge/improv.hpp, tested on the host, and the transport is the console.
//
// SERIAL, NOT BLUETOOTH. The specification has a BLE transport too, and the
// plan leaves it out until the memory it costs has been measured: a board that
// cannot decode 7.1.4 because its Bluetooth stack is resident would be a poor
// trade for a provisioning convenience that USB already provides.

namespace player {

// Starts the task that listens for Improv packets on the console, if this
// board needs it: a board already on a network is provisioned, and the page's
// own PUT /network is what moves it to another one.
//
// That test is not tidiness, it is memory. The task costs 4 KB of internal
// RAM, and the shape that decodes twelve channels out of three substreams
// runs with about 35 KB of it free - a margin PR #707 had to go and find.
// A board with no network is not decoding anything, so the two never want
// the same kilobytes at the same time.
void provisioning_start();

}  // namespace player
