#pragma once

// The network under the HTTP source, as a seam CMake resolves - the same rule
// as byte_source.hpp and audio_sink.hpp one level up. Two implementations, one
// chosen per build in main/CMakeLists.txt from main/Kconfig.projbuild:
//
//   net/wifi/     a station on the access point Kconfig names. What a board has.
//   net/openeth/  the OpenCores Ethernet MAC qemu-system-xtensa emulates, which
//                 `idf.py qemu` attaches to the host's network. What CI has -
//                 it is how the HTTP source runs end to end with no board and
//                 no access point, the host serving the stream at 10.0.2.2.
//
// byte_source.cpp calls network_up() and then speaks HTTP; it never learns
// which of the two answered, and the HTTP client is compiled once for both.

namespace player {

// Brings the network up and blocks until it holds an address. False means
// the caller should stop and say why; the implementation has already said
// what it could not do.
[[nodiscard]] bool network_up();

}  // namespace player
