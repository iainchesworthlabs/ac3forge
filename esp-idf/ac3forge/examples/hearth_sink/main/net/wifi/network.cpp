// WiFi station: the network a board has. See ../../network.hpp.

#include "network.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
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
// How long network_up() waits for an address after each association. The
// quick retries in on_event bound how long associating takes, but DHCP asks
// forever, so without this a network that accepts the board and never gives it
// an address would hold the caller for good: Improv's task in the middle of an
// answer, or app_main at boot before Improv has started, with that network
// stored. The station stays associated, and an address that comes after the
// wait still counts.
constexpr unsigned kAddressWaitSeconds = 30;
constexpr TickType_t kAddressWait = pdMS_TO_TICKS(kAddressWaitSeconds * 1000U);

// How many times the station tries again at once after a failure, before a
// caller waiting in network_up() is told the network failed. A try at a
// network that is not there takes a scan's time, about 2.4 s on a board, so
// the default five end about 15 s after the first try.
constexpr int kQuickRetries = CONFIG_AC3FORGE_EXAMPLE_WIFI_RETRIES;
// After those the station waits before each try, longer each time up to the
// last, and goes on trying for as long as it runs. An access point that
// restarts is gone for 30 s to two minutes, and a board on the same socket
// boots long before it after a power cut, so giving up would leave either
// board off the network until someone restarted it. The last wait bounds how
// long the board takes to rejoin once the network is back; a scan every 15 s
// or so costs a board on mains power nothing.
constexpr std::array<std::uint32_t, 5> kRetryWaitSeconds{1, 2, 4, 8, 15};
// What the retry timer posts when a wait is over, so that the retry happens on
// the event loop's task like everything else here.
const esp_event_base_t kRetryEvent = "HEARTH_WIFI_RETRY";
constexpr std::int32_t kRetryDue = 0;
// How soon the timer posts again if the event loop's queue was full.
constexpr std::uint64_t kRepostMicros = 100U * 1000U;

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
// Runs out when a retry's wait is over. Made with the driver, and started and
// stopped by the event loop's task alone.
esp_timer_handle_t g_retry_timer = nullptr;

// Read and written by the event loop's task alone, in the order the driver
// posted its events, so nothing a stopped station set off can touch them once
// the next start has been handled.
//
// Tries in a row that failed since the station started or last held an
// address: a disconnect, or a connect the driver refused.
int g_failures = 0;
// The station is trying to be on its network, from its STA_START until it
// stops. Nothing gives up in between.
bool g_trying = false;
// A retry is waiting on g_retry_timer. Cleared when it is taken, and when the
// station joins or stops, so that a kRetryDue posted before then is let go.
bool g_retry_due = false;
// The station has held an address since it started.
bool g_held = false;
// It held one and has lost its network since: what the next address is
// reported as, and since when.
bool g_dropped = false;
std::int64_t g_dropped_us = 0;
// The network the station last associated with, for the console.
std::array<char, 33> g_ssid{};
// Why the driver refused a connect on the spot, after which no disconnect
// comes to say why. Set by the event loop's task before kFailedBit and read
// after it.
std::atomic<esp_err_t> g_connect_error{ESP_OK};

// Set by join() while it waits for the station, so that the event loop's task
// leaves saying where the board is to it. Written under g_mutex.
std::atomic<bool> g_waiting{false};
// The board holds an address on its network now: set when one comes, cleared
// when the station loses its network or its address, or stops. Read from any
// task.
std::atomic<bool> g_joined{false};

void on_retry_timer(void*) {
    // Posted rather than done here, so that only the event loop's task ever
    // reads or writes the retry state above. A full queue is asked again
    // shortly, and a post that arrives after the station stopped or joined is
    // let go there.
    if (esp_event_post(kRetryEvent, kRetryDue, nullptr, 0, 0) != ESP_OK) {
        (void)esp_timer_start_once(g_retry_timer, kRepostMicros);
    }
}

// The next try, once the quick retries are spent.
void retry_later() {
    const std::size_t step = std::min(static_cast<std::size_t>(g_failures - kQuickRetries - 1),
                                      kRetryWaitSeconds.size() - 1);
    g_retry_due = true;
    (void)esp_timer_stop(g_retry_timer);
    if (esp_timer_start_once(g_retry_timer, std::uint64_t{kRetryWaitSeconds[step]} * 1000000U) !=
        ESP_OK) {
        g_retry_due = false;
        std::printf("error: the wifi station could not wait to try again, so it has stopped trying\n");
    }
}

void request_connect() {
    const esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        return;
    }
    // Refused on the spot, so no disconnect comes to retry from. A caller
    // waiting in network_up() hears why now, and the next try waits, since
    // one made at once would most likely be refused too. A refusal because
    // the station is stopping is let go when its STA_STOP is handled.
    g_connect_error = err;
    xEventGroupSetBits(g_events, kFailedBit);
    g_failures = std::max(g_failures, kQuickRetries) + 1;
    retry_later();
}

// A try failed: the station lost its network, or has not reached it yet.
void on_failure() {
    ++g_failures;
    if (g_failures <= kQuickRetries) {
        request_connect();
        return;
    }
    if (g_failures == kQuickRetries + 1) {
        // The quick retries are spent, and a caller waiting in network_up()
        // hears so now. The station goes on trying either way.
        xEventGroupSetBits(g_events, kFailedBit);
    }
    retry_later();
}

void on_event(void*, esp_event_base_t base, std::int32_t id, void* data) {
    if (base == kRetryEvent) {
        if (g_retry_due && g_trying) {
            g_retry_due = false;
            request_connect();
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // Each start counts its own failures: one that follows a failed
        // attempt starts with every quick retry, not with none left.
        g_failures = 0;
        g_connect_error = ESP_OK;
        g_trying = true;
        g_held = false;
        g_dropped = false;
        request_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        g_trying = false;
        g_retry_due = false;
        (void)esp_timer_stop(g_retry_timer);
        g_joined = false;
        xEventGroupSetBits(g_events, kStoppedBit);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const auto* event = static_cast<const wifi_event_sta_connected_t*>(data);
        const std::size_t length = std::min<std::size_t>(event->ssid_len, g_ssid.size() - 1);
        std::memcpy(g_ssid.data(), event->ssid, length);
        g_ssid[length] = '\0';
        xEventGroupSetBits(g_events, kAssociatedBit);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // Nothing to retry once the station has stopped. The disconnect a
        // stop brings can also land just before STA_STOP; the retry it starts
        // is refused, since the station has stopped, and STA_STOP lets go of
        // the wait that refusal sets up.
        if (!g_trying) {
            return;
        }
        g_joined = false;
        if (g_held && !g_dropped) {
            const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(data);
            g_dropped = true;
            g_dropped_us = esp_timer_get_time();
            std::printf("network: lost '%s' (reason %u); rejoining\n", g_ssid.data(),
                        static_cast<unsigned>(event->reason));
        }
        on_failure();
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        // The address went while the station stayed associated, a lease that
        // could not be renewed. DHCP goes on asking, and GOT_IP says when it
        // has one again.
        g_joined = false;
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(data);
        g_failures = 0;
        g_retry_due = false;
        g_held = true;
        g_joined = true;
        // network_up() says where the board is when it is waiting; this says
        // so for an address that came without it.
        if (g_dropped) {
            const auto seconds = static_cast<long>((esp_timer_get_time() - g_dropped_us) / 1000000);
            std::printf("network: back on '%s' after %ld s, address " IPSTR "\n", g_ssid.data(),
                        seconds, IP2STR(&event->ip_info.ip));
        } else if (!g_waiting) {
            std::printf("network: joined '%s', address " IPSTR "\n", g_ssid.data(),
                        IP2STR(&event->ip_info.ip));
        }
        g_dropped = false;
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

// The station interface, the driver, the retry timer and the handlers, at the
// first attempt rather than at boot, so a build with nothing to join never
// touches the radio.
void start_driver() {
    if (g_driver) {
        return;
    }
    g_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    g_events = xEventGroupCreate();
    esp_timer_create_args_t timer{};
    timer.callback = &on_retry_timer;
    timer.name = "wifi_retry";
    ESP_ERROR_CHECK(esp_timer_create(&timer, &g_retry_timer));
    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(kRetryEvent, kRetryDue, &on_event, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    g_driver = true;
}

// A started station stops before it is given another network, since the
// driver refuses a new configuration while the station is still connecting.
// The wait is for STA_STOP to be handled, after which on_event retries
// nothing until the next STA_START.
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

// Starts the station on a network, and blocks until the board holds an
// address there or the quick retries are spent. The station goes on trying
// after a false return.
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
    g_waiting = true;
    err = esp_wifi_start();
    if (err != ESP_OK) {
        g_waiting = false;
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
    // after every start, the first and every later network's. The station's
    // own retries do not restart it, and the setting holds across them.
    if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) {
        std::printf("network: could not turn modem sleep off; clock and stream timing will suffer\n");
    }

    // Until the station joins or its quick retries are spent, and for at most
    // kAddressWait from each association. A disconnect that the retries turn
    // into a new association starts the wait again.
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
    g_waiting = false;
    if ((bits & kConnectedBit) != 0) {
        return true;
    }
    if ((bits & kFailedBit) == 0) {
        // Left associated: an address that comes later still joins the board,
        // and the event loop's task says so.
        std::printf("error: '%s' gave the board no address in %u s\n", ssid, kAddressWaitSeconds);
    } else if (const esp_err_t refused = g_connect_error; refused != ESP_OK) {
        std::printf("error: the wifi driver would not connect to '%s' (%s)\n", ssid,
                    esp_err_to_name(refused));
    } else {
        std::printf("error: could not associate with '%s'\n", ssid);
    }
    std::printf("network: still trying '%s' in the background\n", ssid);
    return false;
}

}  // namespace

bool network_up() {
    const std::lock_guard lock(g_mutex);
    if (g_joined) {
        return true;
    }
    // The network the BOARD was told to join, which is the Kconfig one until
    // something stores another (settings.hpp). NVS itself - which WiFi's own
    // calibration data also needs - is initialised there, before this runs.
    settings_load();
    start_stack();
    // Read again on every call, so a call after a failed one tries what has
    // been stored since: that is how Improv moves a board onto the network it
    // was just given. A board that holds an address returned above, so a
    // network stored while it is on one waits for the next boot.
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
    std::printf("network: wifi station on '%s'%s, address %s\n", ssid,
                stored.ssid[0] != '\0' ? " (stored)" : " (from the build)",
                network_address().c_str());
    return true;
}

bool network_ready() { return g_joined; }

std::string network_address() {
    if (!g_joined || g_netif == nullptr) {
        return {};
    }
    esp_netif_ip_info_t info{};
    // Zero while a lease that could not be renewed waits out ESP-IDF's
    // lost-address timer.
    if (esp_netif_get_ip_info(g_netif, &info) != ESP_OK || info.ip.addr == 0) {
        return {};
    }
    std::array<char, 16> text{};
    (void)std::snprintf(text.data(), text.size(), IPSTR, IP2STR(&info.ip));
    return std::string(text.data());
}

}  // namespace player
