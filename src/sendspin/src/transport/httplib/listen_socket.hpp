#pragma once

#include <httplib.h>

namespace ac3::sendspin::transport::websocket {

// The options a Listener's socket is bound with, in place of cpp-httplib's defaults. Those set
// SO_REUSEPORT where the platform has it and SO_REUSEADDR elsewhere, and either lets a second
// listener bind a port the first still holds: on Linux and macOS the two share the incoming
// connections, and on Windows which of them receives one is undefined. A second copy of a
// player or of ac3hearth would then start without an error and receive connections meant for
// the first. CMake picks the file for the platform, and each sets the option under which a
// second bind fails.
void set_listening_socket_options(::socket_t socket);

}  // namespace ac3::sendspin::transport::websocket
