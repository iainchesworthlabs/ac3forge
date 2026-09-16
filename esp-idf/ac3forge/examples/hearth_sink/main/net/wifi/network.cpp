// WiFi station: the network a board has. See ../../network.hpp.

#include "network.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "settings.hpp"

namespace player {
namespace {

constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kFailedBit = BIT1;
constexpr EventBits_t kStoppedBit = BIT2;
constexpr EventBits_t kAssociatedBit = BIT3;
// How long a retry waits for the event loop to hand on the stop. The station
// has stopped by the time esp_wifi_stop() returns; this is only the loop
// catching up with the event it posted.
constexpr TickType_t kStopWait = pdMS_TO_TICKS(5000);
// How long an association may go without an address. The retries in on_event
// bound how long associating takes, but DHCP asks forever, so without this a
// network that accepts the board and never gives it an address would hold
// the caller for good: Improv's task in the middle of an answer, or app_main
// at boot before Improv has started, with that network stored.
constexpr unsigned kAddressWaitSeconds = 30;
constexpr TickType_t kAddressWait = pdMS_TO_TICKS(kAddressWaitSeconds * 1000U);

// network_up() is asked by app_main at boot, by Improv's task when a client
// hands the board a network, and by the HTTP source when it opens, so the
// callers take turns. Held for everything from here down to g_netif.
std::mutex g_mutex;
// Each of these exists once per boot. A second default event loop is an error
// (esp_event_loop_create_default() answers ESP_ERR_INVALID_STATE, which
// ESP_ERROR_CHECK made a reboot), so is a second default station interface,
// and the driver is initialised once. Set up by the first call that needs
// them and reused by every attempt after it.
bool g_stack = false;
bool g_driver = false;
// esp_wifi_start() has been called since the station last stopped. A started
// station has to stop before the driver takes another network.
bool g_started = false;
EventGroupHandle_t g_events = nullptr;
esp_netif_t* g_netif = nullptr;

// Read and written by the event loop's task alone, in the order the driver
// posted its events, so nothing an earlier attempt set off can touch them once
// a later attempt has started.
int g_retries = 0;
// An attempt is under way: from its STA_START until it gives up or the
// station stops. A station that joined stays under way, so a disconnect after
// the join is still retried.
bool g_attempting = false;
// Why the attempt gave up without the driver's last disconnect to say so: a
// connect the driver refused on the spot, after which no disconnect comes.
// Set by the event loop's task before kFailedBit and read after it.
std::atomic<esp_err_t> g_connect_error{ESP_OK};

// Up once, however many callers ask: app_main brings it up at boot and the
// HTTP source asks again when it opens. Read from any task.
std::atomic<bool> g_up{false};

void fail_attempt() {
    g_attempting = false;
    xEventGroupSetBits(g_events, kFailedBit);
}

void request_connect() {
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        g_connect_error = err;
        fail_attempt();
    }
}

void on_event(void*, esp_event_base_t base, std::int32_t id, void*) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Each attempt counts its own retries: one that follows a failed
        // attempt starts with all of them, not with none left.
        g_retries = 0;
        g_connect_error = ESP_OK;
        g_attempting = true;
        request_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        g_attempting = false;
        xEventGroupSetBits(g_events, kStoppedBit);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(g_events, kAssociatedBit);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // No attempt to retry: the disconnect a stop brings, whichever side of
        // STA_STOP it lands, or one after an attempt gave up. Counted, it
        // would fail the next attempt before that attempt had tried anything.
        if (!g_attempting) {
            return;
        }
        // Bounded, so a wrong password fails instead of retrying forever. A
        // player that silently never starts is harder to diagnose than one that
        // says it could not associate.
        if (g_retries < CONFIG_AC3FORGE_EXAMPLE_WIFI_RETRIES) {
            ++g_retries;
            request_connect();
        } else {
            fail_attempt();
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_retries = 0;
        xEventGroupSetBits(g_events, kConnectedBit);
    }
}

// lwIP and the default event loop, whether or not there is a network to join:
// app_main starts the control surface on a board with nothing stored too, and
// the server's first socket fails an lwIP assertion if nothing has
// initialised the stack.
void start_stack() {
    if (g_stack) {
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    g_stack = true;
}

// The station interface, the driver and the handlers, at the first attempt
// rather than at boot, so a build with nothing to join never touches the
// radio.
void start_driver() {
    if (g_driver) {
        return;
    }
    g_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    g_events = xEventGroupCreate();
    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    g_driver = true;
}

// A station an attempt started stops before the next attempt configures it,
// since the driver refuses a new configuration while the station is still
// connecting, and when an attempt that associated is given up. The wait is
// for STA_STOP to be handled, after which on_event takes no disconnect for a
// retry until the next STA_START.
bool stop_station() {
    if (!g_started) {
        return true;
    }
    xEventGroupClearBits(g_events, kStoppedBit);
    const esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        std::printf("error: could not stop the wifi station to try another network (%s)\n",
                    esp_err_to_name(err));
        return false;
    }
    g_started = false;
    const EventBits_t bits =
        xEventGroupWaitBits(g_events, kStoppedBit, pdFALSE, pdFALSE, kStopWait);
    if ((bits & kStoppedBit) == 0) {
        std::printf("warning: the wifi station stopped without saying so; trying anyway\n");
    }
    return true;
}

// One attempt at a network, blocking until the board holds an address there
// or has given up on it.
bool join(const char* ssid, const char* password) {
    start_driver();
    if (!stop_station()) {
        return false;
    }
    xEventGroupClearBits(g_events, kConnectedBit | kFailedBit | kAssociatedBit);

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), ssid, sizeof(config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(config.sta.password), password,
                 sizeof(config.sta.password) - 1);
    // Refused rather than fatal. The network came from NVS or an Improv
    // client, and an abort here would reboot into the same stored network and
    // abort again.
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        std::printf("error: the wifi driver refused the network '%s' (%s)\n", ssid,
                    esp_err_to_name(err));
        return false;
    }
    // Before the call, so that a start that fails partway is still stopped
    // at the next attempt.
    g_started = true;
    err = esp_wifi_start();
    if (err != ESP_OK) {
        std::printf("error: could not start the wifi station (%s)\n", esp_err_to_name(err));
        return false;
    }
    // Modem sleep off, which a sink on mains power can afford. With it on, the
    // access point holds what it sends the board until the board next wakes,
    // up to a beacon interval later, while what the board sends leaves at
    // once. A Sendspin clock exchange reads that as an offset that moves by
    // milliseconds from one exchange to the next, and a server's read-ahead
    // that arrives while the board sleeps can overflow the access point's
    // queue for it, which stalls the stream's start on retransmissions. Set
    // after every start, the first and every retry's.
    if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
        std::printf("network: could not turn modem sleep off; clock and stream timing will suffer\n");
    }

    // Until the attempt joins or gives up, and for at most kAddressWait from
    // each association. A disconnect that the retries turn into a new
    // association starts the wait again.
    const EventBits_t done = kConnectedBit | kFailedBit;
    EventBits_t bits = 0;
    do {
        bits = xEventGroupWaitBits(g_events, done | kAssociatedBit, pdFALSE, pdFALSE, portMAX_DELAY);
        if ((bits & done) == 0) {
            xEventGroupClearBits(g_events, kAssociatedBit);
            bits = xEventGroupWaitBits(g_events, done | kAssociatedBit, pdFALSE, pdFALSE,
                                       kAddressWait);
        }
    } while ((bits & done) == 0 && (bits & kAssociatedBit) != 0);
    if ((bits & kConnectedBit) != 0) {
        return true;
    }
    if ((bits & kFailedBit) == 0) {
        std::printf("error: '%s' gave the board no address in %u s\n", ssid, kAddressWaitSeconds);
        // Not left associated, where an address that came later would find
        // nobody waiting for it.
        (void)stop_station();
    } else if (const esp_err_t refused = g_connect_error; refused != ESP_OK) {
        std::printf("error: the wifi driver would not connect to '%s' (%s)\n", ssid,
                    esp_err_to_name(refused));
    } else {
        std::printf("error: could not associate with '%s'\n", ssid);
    }
    return false;
}

}  // namespace

bool network_up() {
    const std::lock_guard lock(g_mutex);
    if (g_up) {
        return true;
    }
    // The network the BOARD was told to join, which is the Kconfig one until
    // something stores another (settings.hpp). NVS itself - which WiFi's own
    // calibration data also needs - is initialised there, before this runs.
    settings_load();
    start_stack();
    // Read again on every call, so a call after a failed one tries what has
    // been stored since: that is how Improv moves a board onto the network it
    // was just given.
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
    if (!join(ssid, password)) {
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
