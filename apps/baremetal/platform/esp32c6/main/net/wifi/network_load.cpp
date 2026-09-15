// A WiFi station with a TCP stream arriving: the load a Sendspin player carries
// while it decodes, since in Sendspin the server connects to the player. See
// ../../network_load.hpp and main/Kconfig.projbuild.
//
// The C6 has one core. The decode runs flat out on the main task at priority
// 1; this file's receiving task sits above it at 5, and the WiFi and lwIP tasks
// above that, so whenever bytes arrive they preempt the decode, and the
// probe's microseconds per frame include what that cost. That is the figure
// this load exists to produce.
//
// Nothing here uses `new`: probe.cpp replaces the global operator new to count
// the decoder's allocations, and the network's own go through malloc inside
// ESP-IDF, so the probe's heap lines stay the decoder's alone. What the network
// takes is read from the allocator instead (report_internal_sram).

#include "network_load.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace ac3probe {

void report_internal_sram(const char* when);  // main.cpp

namespace {

EventGroupHandle_t g_events = nullptr;
constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kFailedBit = BIT1;
constexpr EventBits_t kStreamBit = BIT2;

// Bounded, so a wrong password fails and says so rather than retrying forever.
constexpr int kAssociationAttempts = 5;
int g_attempts = 0;
esp_ip4_addr_t g_address{};

// The probe starts once this much has arrived: a stream, rather than the
// first segment of one.
constexpr std::uint32_t kStreamStartBytes = 32768;

// Written by the receiving task, read by app_main. 32-bit counters, since a
// 64-bit std::atomic needs libatomic on a 32-bit part; a run of the probe is
// minutes, far inside either range.
std::atomic<std::uint32_t> g_bytes{0};
std::atomic<std::uint32_t> g_connections{0};
std::atomic<std::uint32_t> g_max_gap_us{0};

// One TCP segment's worth. Static rather than on the task's stack, which then
// stays small; either way it comes out of the same SRAM.
std::array<char, 1536> g_buffer{};

void on_event(void*, esp_event_base_t base, std::int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (g_attempts < kAssociationAttempts) {
            ++g_attempts;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(g_events, kFailedBit);
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_address = static_cast<ip_event_got_ip_t*>(data)->ip_info.ip;
        xEventGroupSetBits(g_events, kConnectedBit);
    }
}

// Accepts one connection at a time and reads it until the peer closes, then
// waits for the next. The bytes are counted and dropped: what a player would
// keep them in is sized in a later phase, from what this load leaves free.
void receive_stream(void*) {
    const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(CONFIG_AC3FORGE_PROBE_TCP_PORT));
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (listener < 0 ||
        bind(listener, static_cast<sockaddr*>(static_cast<void*>(&address)), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        std::printf("net.error=listen\n");
        vTaskDelete(nullptr);
        return;
    }
    for (;;) {
        const int connection = accept(listener, nullptr, nullptr);
        if (connection < 0) {
            continue;
        }
        g_connections.fetch_add(1);
        std::int64_t previous_us = 0;
        for (;;) {
            const int received = recv(connection, g_buffer.data(), g_buffer.size(), 0);
            if (received <= 0) {
                break;
            }
            const std::int64_t now_us = esp_timer_get_time();
            if (previous_us != 0) {
                const auto gap = static_cast<std::uint32_t>(now_us - previous_us);
                if (gap > g_max_gap_us.load()) {
                    g_max_gap_us.store(gap);
                }
            }
            previous_us = now_us;
            if (g_bytes.fetch_add(static_cast<std::uint32_t>(received)) +
                    static_cast<std::uint32_t>(received) >=
                kStreamStartBytes) {
                xEventGroupSetBits(g_events, kStreamBit);
            }
        }
        close(connection);
    }
}

// Where the stream stood when the probe started, for the rate over the decode.
std::uint32_t g_bytes_at_start = 0;
std::int64_t g_start_us = 0;
TaskHandle_t g_receiver = nullptr;

}  // namespace

bool network_start() {
    std::printf("network=wifi\n");
    if (nvs_flash_init() == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    g_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr));

    wifi_config_t config = {};
    std::strncpy(static_cast<char*>(static_cast<void*>(config.sta.ssid)),
                 CONFIG_AC3FORGE_PROBE_WIFI_SSID, sizeof(config.sta.ssid) - 1);
    std::strncpy(static_cast<char*>(static_cast<void*>(config.sta.password)),
                 CONFIG_AC3FORGE_PROBE_WIFI_PASSWORD, sizeof(config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    const EventBits_t joined = xEventGroupWaitBits(g_events, kConnectedBit | kFailedBit, pdFALSE,
                                                   pdFALSE, portMAX_DELAY);
    if ((joined & kConnectedBit) == 0) {
        std::printf("result=fail reason=wifi_association\n");
        return false;
    }

    // Modem sleep off, which is what a player that has to keep its clock
    // within a millisecond of the server's would choose. IDF's default
    // (WIFI_PS_MIN_MODEM) holds received frames until the next beacon it wakes
    // for, which would show here as long gaps between reads.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_ap_record_t access_point{};
    if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
        std::printf("net.rssi_dbm=%d net.channel=%u net.phy_11ax=%u\n",
                    static_cast<int>(access_point.rssi),
                    static_cast<unsigned>(access_point.primary),
                    static_cast<unsigned>(access_point.phy_11ax));
    }
    // The address, so a host can connect; never the network's name.
    std::printf("net.ip=" IPSTR " net.port=%d\n", IP2STR(&g_address),
                CONFIG_AC3FORGE_PROBE_TCP_PORT);
    report_internal_sram("network");

    // The task's stack comes out of the same heap the decoder draws on;
    // network_report() says how much of it the task used.
    if (xTaskCreate(&receive_stream, "ac3probe_rx", 4096, nullptr, 5, &g_receiver) != pdPASS) {
        std::printf("result=fail reason=receive_task\n");
        return false;
    }

    const EventBits_t arriving =
        xEventGroupWaitBits(g_events, kStreamBit, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(CONFIG_AC3FORGE_PROBE_STREAM_WAIT_S * 1000));
    if ((arriving & kStreamBit) == 0) {
        std::printf("result=fail reason=no_tcp_stream waited_s=%d\n",
                    CONFIG_AC3FORGE_PROBE_STREAM_WAIT_S);
        return false;
    }
    g_bytes_at_start = g_bytes.load();
    g_start_us = esp_timer_get_time();
    g_max_gap_us.store(0);
    std::printf("net.stream=arriving\n");
    report_internal_sram("stream");
    return true;
}

void network_report() {
    const std::uint32_t received = g_bytes.load() - g_bytes_at_start;
    const std::int64_t elapsed_us = esp_timer_get_time() - g_start_us;
    // bytes x 8 / 1000 per second = bytes x 8,000 per million microseconds.
    const std::uint64_t kbit_per_s =
        elapsed_us > 0 ? static_cast<std::uint64_t>(received) * 8000U /
                             static_cast<std::uint64_t>(elapsed_us)
                       : 0;
    std::printf("net.bytes_during_probe=%lu net.probe_ms=%lu net.kbit_per_s=%lu "
                "net.max_read_gap_us=%lu net.connections=%lu net.rx_stack_free_bytes=%lu\n",
                static_cast<unsigned long>(received),
                static_cast<unsigned long>(elapsed_us / 1000),
                static_cast<unsigned long>(kbit_per_s),
                static_cast<unsigned long>(g_max_gap_us.load()),
                static_cast<unsigned long>(g_connections.load()),
                static_cast<unsigned long>(uxTaskGetStackHighWaterMark(g_receiver)));
}

}  // namespace ac3probe
