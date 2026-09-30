#include <httplib.h>

#include "listen_socket.hpp"

// Linux and macOS: SO_REUSEADDR alone. A restarted listener binds while the connections its
// predecessor closed wait out TIME_WAIT, and a second listener on the same port still fails.

namespace iclforge::sendspin::transport::websocket {

void set_listening_socket_options(::socket_t socket) {
    httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1);
}

}  // namespace iclforge::sendspin::transport::websocket
