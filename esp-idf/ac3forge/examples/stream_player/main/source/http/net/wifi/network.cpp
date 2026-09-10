// WiFi station: the network a board has. See ../../network.hpp.

#include "network.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

namespace player {
namespace {

EventGroupHandle_t g_events = nullptr;
constexpr int kConnectedBit = BIT0;
constexpr int kFailedBit = BIT1;
int g_retries = 0;

void on_event(void*, esp_event_base_t base, std::int32_t id, void*) {
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
            xEventGroupSetBits(g_events, kFailedBit);
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_retries = 0;
        xEventGroupSetBits(g_events, kConnectedBit);
    }
}

}  // namespace

bool network_up() {
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

    g_events = xEventGroupCreate();
    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr));

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), CONFIG_AC3FORGE_EXAMPLE_WIFI_SSID,
                 sizeof(config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(config.sta.password),
                 CONFIG_AC3FORGE_EXAMPLE_WIFI_PASSWORD, sizeof(config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    const auto bits = xEventGroupWaitBits(g_events, kConnectedBit | kFailedBit, pdFALSE, pdFALSE,
                                          portMAX_DELAY);
    if ((bits & kConnectedBit) == 0) {
        std::printf("error: could not associate with '%s'\n", CONFIG_AC3FORGE_EXAMPLE_WIFI_SSID);
        return false;
    }
    std::printf("network: wifi station on '%s'\n", CONFIG_AC3FORGE_EXAMPLE_WIFI_SSID);
    return true;
}

}  // namespace player
