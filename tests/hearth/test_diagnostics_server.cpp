#include <catch2/catch_test_macros.hpp>

#include <httplib.h>

#include <string>

#include "diagnostics_server.hpp"

// DiagnosticsHttpServer (apps/hearth/engine/diagnostics_server.hpp): a real
// httplib::Client against a real bound server, the same reason
// test_sink_firmware_board.cpp is compiled on its own with cpp-httplib
// configured as src/sendspin configures it (tests/CMakeLists.txt's own
// comment on ac3tests_sink_firmware_board says why).

using iclforge::hearth::DiagnosticsHttpServer;

TEST_CASE("diagnostics server: GET /diagnostics returns exactly what the report callback gives, fresh each time",
          "[hearth][diagnostics]") {
    int calls = 0;
    DiagnosticsHttpServer server([&calls] {
        ++calls;
        return "report #" + std::to_string(calls);
    });
    REQUIRE(server.start(0));
    REQUIRE(server.port() != 0);

    httplib::Client client("127.0.0.1", server.port());
    const auto first = client.Get("/diagnostics");
    REQUIRE(first);
    CHECK(first->status == 200);
    CHECK(first->body == "report #1");

    // Not cached: a second request calls the callback again.
    const auto second = client.Get("/diagnostics");
    REQUIRE(second);
    CHECK(second->body == "report #2");

    const std::uint16_t stopped_port = server.port();
    server.stop();
    CHECK(server.port() == 0);
    // Nothing answers on that port once stopped.
    httplib::Client after_stop("127.0.0.1", stopped_port);
    const auto third = after_stop.Get("/diagnostics");
    CHECK_FALSE(third);
}

TEST_CASE("diagnostics server: start() is refused while already started, and works again after stop()",
          "[hearth][diagnostics]") {
    DiagnosticsHttpServer server([] { return std::string{"x"}; });
    REQUIRE(server.start(0));
    CHECK_FALSE(server.start(0));  // already started - refused regardless of the port asked for
    server.stop();
    CHECK(server.port() == 0);
    REQUIRE(server.start(0));
    CHECK(server.port() != 0);
    server.stop();
}
