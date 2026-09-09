// An HTTP body, over WiFi.
//
// COMPILED BY CI, NOT RUN, for the same reason the SD source is not: QEMU has
// no network. Kept small on purpose.
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
// rather than about where bytes come from.

#include "byte_source.hpp"

#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

namespace player {
namespace {

esp_http_client_handle_t g_client = nullptr;
std::size_t g_length = 0;
EventGroupHandle_t g_wifi_events = nullptr;
constexpr int kConnectedBit = BIT0;
constexpr int kFailedBit = BIT1;
int g_retries = 0;

void on_wifi_event(void*, esp_event_base_t base, std::int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // Bounded, so a wrong password fails instead of retrying forever. A
        // player that silently never starts is harder to diagnose than one that
        // says it could not associate.
        if (g_retries < CONFIG_AC3FORGE_EXAMPLE_WIFI_RETRIES) {
            ++g_retries;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(g_wifi_events, kFailedBit);
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        (void)data;
        g_retries = 0;
        xEventGroupSetBits(g_wifi_events, kConnectedBit);
    }
}

bool wifi_up() {
    if (nvs_flash_init() == ESP_ERR_NVS_NO_FREE_PAGES) {
        // The calibration data WiFi keeps lives in NVS; a partition left over
        // from a different build can be the wrong version.
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    g_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event,
                                               nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event,
                                               nullptr));

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), CONFIG_AC3FORGE_EXAMPLE_WIFI_SSID,
                 sizeof(config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(config.sta.password),
                 CONFIG_AC3FORGE_EXAMPLE_WIFI_PASSWORD, sizeof(config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    const auto bits = xEventGroupWaitBits(g_wifi_events, kConnectedBit | kFailedBit, pdFALSE,
                                          pdFALSE, portMAX_DELAY);
    return (bits & kConnectedBit) != 0;
}

}  // namespace

bool source_open() {
    if (!wifi_up()) {
        std::printf("error: could not associate with '%s'\n", CONFIG_AC3FORGE_EXAMPLE_WIFI_SSID);
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
