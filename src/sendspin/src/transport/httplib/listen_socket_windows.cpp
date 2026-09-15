#include <httplib.h>

#include "listen_socket.hpp"

// Windows: SO_EXCLUSIVEADDRUSE, under which no other socket binds the port while this one holds
// it, whatever options that socket sets.

namespace ac3::sendspin::transport::websocket {

void set_listening_socket_options(::socket_t socket) {
    httplib::set_socket_opt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
}

}  // namespace ac3::sendspin::transport::websocket
