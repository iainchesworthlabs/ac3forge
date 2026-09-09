// An HTTP body, over whichever network net/ supplies.
//
// RUN BY CI UNDER QEMU since 2026-09-10, over the emulated Ethernet MAC
// (net/openeth/) with the host serving the stream; on a board it runs over
// WiFi (net/wifi/). The HTTP client below is the same code either way, which
// is the point of the seam: the part that could be wrong is exercised on
// every push, and only the radio waits for hardware.
//
// THE ONE THING THAT IS DIFFERENT FROM EVERY OTHER SOURCE: it cannot rewind.
// A partition and a file can seek; a socket has delivered what it has delivered.
// source_rewind() returns false and the player stops at end of stream instead of
// looping, which is the honest behaviour - re-requesting the URL would be a new
// stream, not a rewind, and the decoder's overlap-add state would carry across
// the seam as a click.
//
// It also does no buffering of its own beyond one read. A production player
// wants a ring buffer and a fetch task so a slow network does not stall the
// decode - that is a design decision about latency and RAM which belongs to the
// integrator, and putting one here would make this example about buffering
// rather than about where bytes come from. planning/esp32-player.md is where
// that design lives.

#include "byte_source.hpp"

#include <cstdio>

#include "esp_http_client.h"

#include "network.hpp"

namespace player {
namespace {

esp_http_client_handle_t g_client = nullptr;
std::size_t g_length = 0;

}  // namespace

bool source_open() {
    if (!network_up()) {
        return false;
    }

    esp_http_client_config_t config = {};
    config.url = CONFIG_AC3FORGE_EXAMPLE_HTTP_URL;
    config.timeout_ms = 10000;
    // Chunked responses are fine: esp_http_client_read hides the framing, and
    // the accumulator never cared about read boundaries anyway.
    g_client = esp_http_client_init(&config);
    if (g_client == nullptr) {
        std::printf("error: could not create an HTTP client\n");
        return false;
    }
    if (esp_http_client_open(g_client, 0) != ESP_OK) {
        std::printf("error: could not open %s\n", CONFIG_AC3FORGE_EXAMPLE_HTTP_URL);
        return false;
    }
    const auto length = esp_http_client_fetch_headers(g_client);
    const auto status = esp_http_client_get_status_code(g_client);
    if (status != 200) {
        std::printf("error: %s returned %d\n", CONFIG_AC3FORGE_EXAMPLE_HTTP_URL, status);
        return false;
    }
    // Negative means chunked, i.e. no Content-Length. 0 is this seam's own
    // spelling of "unknown", and the player treats it the same way: read until
    // the source says there is no more.
    g_length = length > 0 ? static_cast<std::size_t>(length) : 0;
    std::printf("source: http %s, %lu bytes\n", CONFIG_AC3FORGE_EXAMPLE_HTTP_URL,
                static_cast<unsigned long>(g_length));
    return true;
}

std::size_t source_read(std::span<std::byte> dst) {
    if (g_client == nullptr) {
        return 0;
    }
    const auto got = esp_http_client_read(g_client, reinterpret_cast<char*>(dst.data()),
                                          static_cast<int>(dst.size()));
    return got > 0 ? static_cast<std::size_t>(got) : 0;
}

bool source_rewind() {
    // A socket has delivered what it has delivered. Re-requesting the URL would
    // be a new stream rather than a rewind, and the decoder's overlap-add state
    // would carry across the seam as a click.
    return false;
}

const char* source_name() { return "http"; }

std::size_t source_length() { return g_length; }

}  // namespace player
