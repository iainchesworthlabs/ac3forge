#include "diagnostics_server.hpp"

#include <thread>
#include <utility>

// Last: windows.h's own macros (small, among others) would break the
// headers above on that platform - sink_firmware.cpp's own comment on this
// carries the identical rule, and httplib.h pulls windows.h in there.
#include <httplib.h>

namespace ac3::hearth {

struct DiagnosticsHttpServer::Impl {
    httplib::Server server;
    std::thread accept_thread;
    std::uint16_t port = 0;
};

DiagnosticsHttpServer::DiagnosticsHttpServer(std::function<std::string()> report)
    : impl_(std::make_unique<Impl>()) {
    // Registered once, here, rather than in start(): the route is
    // independent of any one bind/thread cycle, which is what makes
    // starting again after a stop() safe.
    impl_->server.Get("/diagnostics",
                      [report = std::move(report)](const httplib::Request&, httplib::Response& response) {
                          response.set_content(report(), "text/plain; charset=utf-8");
                      });
}

DiagnosticsHttpServer::~DiagnosticsHttpServer() { stop(); }

bool DiagnosticsHttpServer::start(std::uint16_t port) {
    if (impl_->port != 0) {
        return false;
    }
    httplib::Server& server = impl_->server;
    int bound = -1;
    if (port == 0) {
        bound = server.bind_to_any_port("127.0.0.1");
    } else if (server.bind_to_port("127.0.0.1", port)) {
        bound = port;
    }
    if (bound <= 0) {
        return false;
    }
    impl_->port = static_cast<std::uint16_t>(bound);
    // The socket is already listening, so a request that arrives before
    // this thread reaches accept() waits in the backlog rather than being
    // refused - the same reasoning src/sendspin's own Listener::start()
    // carries for the identical shape.
    impl_->accept_thread = std::thread([&server] { server.listen_after_bind(); });
    server.wait_until_ready();
    return true;
}

void DiagnosticsHttpServer::stop() {
    if (impl_->port == 0) {
        return;
    }
    impl_->server.stop();
    if (impl_->accept_thread.joinable()) {
        impl_->accept_thread.join();
    }
    impl_->port = 0;
}

std::uint16_t DiagnosticsHttpServer::port() const { return impl_->port; }

}  // namespace ac3::hearth
