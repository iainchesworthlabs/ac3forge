#pragma once

#include <optional>
#include <string>

// The board's network, as a seam CMake resolves - the same rule as
// byte_source.hpp and audio_sink.hpp beside it. Three implementations, one
// chosen per build in main/CMakeLists.txt from main/Kconfig.projbuild:
//
//   net/wifi/     a station on the access point the board was told about
//                 (settings.hpp, or Kconfig until something stores one). What
//                 a board has.
//   net/openeth/  the OpenCores Ethernet MAC qemu-system-xtensa emulates,
//                 which `idf.py qemu` attaches to the host's network. What CI
//                 has - it is how the HTTP source runs end to end with no
//                 board and no access point, the host serving at 10.0.2.2.
//   net/none/     a build that plays from flash, FAT or an SD card and joins
//                 nothing.
//
// IT COMES UP AT BOOT, not at the first play. A sink is found before it is
// played to: mDNS has to be answering and the control surface reachable while
// the board sits idle, which is the whole of B2
// (planning/hearth-reference-player.md). The HTTP source still calls
// network_up() when it opens, and gets the network that is already up. A
// board with no network at boot gets one when Improv gives it one
// (provision.hpp), and app_main then starts what boot would have.

namespace player {

// Brings the network up and blocks until it holds an address, or returns
// straight away if it holds one now. False means there is no network yet - no
// build with one, nothing stored to join, or the station's quick retries
// failed to join it - and the implementation has already said which on the
// console.
//
// The WiFi station does not give up there. After a false return it goes on
// trying the same network with a growing wait, up to 15 s between tries, and
// it does the same for a network it has joined and then lost, such as an
// access point that restarts. network_ready() says when it is back.
//
// Safe to call again after it returns false, from any task, and while a lost
// network is being retried: the next call starts over with whatever is
// stored by then. That is how Improv moves a board that is not on a network
// onto the one it has just been given, without a restart. A board that holds
// an address keeps its network, and one stored meanwhile (PUT /network) is
// the one it joins at its next boot. Whatever it returns, a build with a
// network has its IP stack running once it has been called, so a server can
// listen before there is an address to reach it at.
[[nodiscard]] bool network_up();

// Whether the board holds an address on its network now. The WiFi station
// clears it when it loses its network or its address and sets it again when
// it rejoins; QEMU's Ethernet keeps it once set. Asked by anything that wants
// to know whether to bother - mDNS, the Improv reply's device URL and state,
// app_main watching for a network that comes up after boot - rather than to
// bring the network up itself.
[[nodiscard]] bool network_ready();

// The address the board holds, as text ("192.168.1.45"), or empty when it
// holds none. For the URL an Improv client sends the user to, and for the
// console line that says where the page is.
[[nodiscard]] std::string network_address();

// What the board's network is, for GET /status (ac3forge::ControlNetwork):
// "wifi" or "ethernet", and on WiFi, while the station is associated, the
// access point's SSID and the signal from it. `kind` is null in a build with
// no network. Safe from any task.
struct NetworkLink {
    const char* kind = nullptr;
    std::string ssid;
    std::optional<int> rssi_dbm;
};
[[nodiscard]] NetworkLink network_link();

// Where the network this board joins comes from, for GET /firmware
// (planning/esp32-ota.md): "stored" in NVS, "built-in" to this image alone
// (CONFIG_AC3FORGE_EXAMPLE_WIFI_SSID), "wired" for a network that needs
// nothing stored (QEMU's Ethernet), or "none". An image with no network
// built in - every image CI publishes - cannot rejoin a "built-in" one.
[[nodiscard]] const char* network_source();

// At boot, before the network comes up: a network built into this image and
// none stored is stored, so the board keeps it through an update to an image
// built without one. Does nothing when one is stored already, or with no
// network built in.
void network_adopt_built_in();

}  // namespace player
