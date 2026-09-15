#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "ac3/sendspin/discovery.hpp"

// What an owner reads from a service browsing found: its TXT values and the URL to dial.

using ac3::sendspin::discovery::Service;

TEST_CASE("discovery: a service's TXT values and URL", "[sendspin][discovery]") {
    Service service{.instance = "Kitchen",
                    .host = "kitchen.local",
                    .addresses = {"192.168.1.20", "10.0.0.4"},
                    .port = 8928,
                    .txt = {{.key = "path", .value = "/sendspin"}, {.key = "name", .value = "Kitchen"}}};
    CHECK(service.txt_value("name") == std::optional<std::string>("Kitchen"));
    CHECK_FALSE(service.txt_value("missing").has_value());
    CHECK(service.url() == std::optional<std::string>("ws://192.168.1.20:8928/sendspin"));

    // Without a path the specification's recommended one is used, and a path is rooted.
    service.txt = {{.key = "path", .value = "player"}};
    CHECK(service.url() == std::optional<std::string>("ws://192.168.1.20:8928/player"));
    service.txt.clear();
    CHECK(service.url() == std::optional<std::string>("ws://192.168.1.20:8928/sendspin"));

    // No address, or no port: nothing to dial.
    service.port = 0;
    CHECK_FALSE(service.url().has_value());
    service.port = 8928;
    service.addresses.clear();
    CHECK_FALSE(service.url().has_value());
}
