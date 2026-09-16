#pragma once

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
// network_up() when it opens, and gets the network that is already up.

namespace player {

// Brings the network up and blocks until it holds an address, or returns
// straight away if it already does. False means there is no network - no
// build with one, nothing stored to join, or the association failed - and the
// implementation has already said which on the console.
[[nodiscard]] bool network_up();

// Whether network_up() has succeeded. Asked by anything that wants to know
// whether to bother - mDNS, the Improv reply's device URL - rather than to
// bring it up itself.
[[nodiscard]] bool network_ready();

// The address the board holds, as text ("192.168.1.45"), or empty when it
// holds none. For the URL an Improv client sends the user to, and for the
// console line that says where the page is.
[[nodiscard]] std::string network_address();

}  // namespace player
