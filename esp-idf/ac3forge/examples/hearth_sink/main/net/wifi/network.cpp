// WiFi station: the network a board has. See ../../network.hpp.

#include "network.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "settings.hpp"

namespace player {
namespace {

EventGroupHandle_t g_events = nullptr;
constexpr int kConnectedBit = BIT0;
constexpr int kFailedBit = BIT1;
int g_retries = 0;
// Up once, however many callers ask: app_main brings it up at boot and the
// HTTP source asks again when it opens.
bool g_up = false;
esp_netif_t* g_netif = nullptr;

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
    if (g_up) {
        return true;
    }
    // The network the BOARD was told to join, which is the Kconfig one until
    // something stores another (settings.hpp). NVS itself - which WiFi's own
    // calibration data also needs - is initialised there, before this runs.
    settings_load();
    const Settings& stored = settings();
    // Stored first, then the image's own: CI flashes its SSID into the build
    // and never provisions anything, and a board provisioned over Improv
    // should not go back to the build's network at the next boot.
    const char* ssid =
        stored.ssid[0] != '\0' ? stored.ssid.data() : CONFIG_AC3FORGE_EXAMPLE_WIFI_SSID;
    const char* password =
        stored.ssid[0] != '\0' ? stored.password.data() : CONFIG_AC3FORGE_EXAMPLE_WIFI_PASSWORD;
    if (ssid[0] == '\0') {
        std::printf("error: no network stored and none built in; provision the board first\n");
        return false;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    g_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    g_events = xEventGroupCreate();
    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr));

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), ssid, sizeof(config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(config.sta.password), password,
                 sizeof(config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem sleep off, which a sink on mains power can afford. With it on, the
    // access point holds what it sends the board until the board next wakes,
    // up to a beacon interval later, while what the board sends leaves at
    // once. A Sendspin clock exchange reads that as an offset that moves by
    // milliseconds from one exchange to the next, and a server's read-ahead
    // that arrives while the board sleeps can overflow the access point's
    // queue for it, which stalls the stream's start on retransmissions.
    if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
        std::printf("network: could not turn modem sleep off; clock and stream timing will suffer\n");
    }

    const auto bits = xEventGroupWaitBits(g_events, kConnectedBit | kFailedBit, pdFALSE, pdFALSE,
                                          portMAX_DELAY);
    if ((bits & kConnectedBit) == 0) {
        std::printf("error: could not associate with '%s'\n", ssid);
        return false;
    }
    g_up = true;
    std::printf("network: wifi station on '%s'%s, address %s\n", ssid,
                stored.ssid[0] != '\0' ? " (stored)" : " (from the build)",
                network_address().c_str());
    return true;
}

bool network_ready() { return g_up; }

std::string network_address() {
    if (!g_up || g_netif == nullptr) {
        return {};
    }
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(g_netif, &info) != ESP_OK) {
        return {};
    }
    std::array<char, 16> text{};
    (void)std::snprintf(text.data(), text.size(), IPSTR, IP2STR(&info.ip));
    return std::string(text.data());
}

}  // namespace player
