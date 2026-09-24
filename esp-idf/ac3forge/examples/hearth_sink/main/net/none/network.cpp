// No network: what a build that plays from flash, a FAT volume or an SD card
// compiles. See ../../network.hpp.
//
// A file rather than a branch, for the reason audio_sink.hpp gives about the
// sinks: the player asks the same question of every build, and the answer is
// a directory CMake picks. Nothing here starts a driver or a task, so a board
// playing from its own storage pays no WiFi memory and no event loop, exactly
// as it did when only the HTTP source had a network at all.

#include "network.hpp"

#include <cstdio>

namespace player {

bool network_up() {
    std::printf("network: this build has none; nothing to join\n");
    return false;
}

bool network_ready() { return false; }

std::string network_address() { return {}; }

NetworkLink network_link() { return {}; }

}  // namespace player
