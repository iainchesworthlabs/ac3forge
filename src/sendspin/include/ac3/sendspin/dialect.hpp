#pragma once

#include <cstdint>

// Which Sendspin a peer speaks: the specification at the commit Hearth implements, or
// aiosendspin 9.1.1, which Music Assistant pins and which differs from it in framing, the
// player@v1 chunk header, several messages and pairing
// (planning/hearth-sendspin-extension.md, Music Assistant and aiosendspin 9.1.1).
//
// A connection has one dialect, settled before anything that depends on it is sent: a player
// learns it from Noise message 1, a server from client/hello. Every function whose bytes
// differ between the two takes it.

namespace ac3::sendspin {

enum class Dialect : std::uint8_t {
    kSpecification,
    kAiosendspin911,
};

}  // namespace ac3::sendspin
