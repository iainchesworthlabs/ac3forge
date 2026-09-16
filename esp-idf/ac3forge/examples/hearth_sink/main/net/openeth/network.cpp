// The OpenCores Ethernet MAC qemu-system-xtensa emulates. See ../../network.hpp.
//
// Not a device on any board, and the driver says so if asked to run on one.
// `idf.py qemu` attaches the model to the host with `-nic user,model=open_eth`:
// a user-mode network in which the guest takes 10.0.2.15 by DHCP and the host
// is 10.0.2.2, so a server on the host is reachable with no firewall opening -
// the connection arrives over the host's own loopback.
//
// A generic PHY, because that is the one PHY driver this IDF ships inside
// esp_eth (the vendor ones moved to the component registry), and QEMU's model
// answers the IEEE 802.3 registers a generic PHY reads. Its address is left to
// autodetection rather than asserted; there is nothing to reset.

#include "network.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace player {
namespace {

// Where the driver stands. It is started once: a second default event loop is
// an error, which ESP_ERROR_CHECK makes a reboot, and a MAC that could not be
// created will not appear on a later call.
enum class Driver { not_started, started, unavailable };

// app_main asks at boot and the HTTP source asks again when it opens, so the
// callers take turns. Held for everything from here down to g_netif.
std::mutex g_mutex;
Driver g_driver = Driver::not_started;
EventGroupHandle_t g_events = nullptr;
esp_netif_t* g_netif = nullptr;
// Up once, however many callers ask. Read from any task.
std::atomic<bool> g_up{false};
constexpr int kGotIpBit = BIT0;

void on_got_ip(void*, esp_event_base_t, std::int32_t, void* data) {
    const auto* event = static_cast<const ip_event_got_ip_t*>(data);
    std::printf("network: openeth " IPSTR " via " IPSTR "\n", IP2STR(&event->ip_info.ip),
                IP2STR(&event->ip_info.gw));
    xEventGroupSetBits(g_events, kGotIpBit);
}

// False, having said why, when there is no MAC to drive - which is any board.
bool start_driver() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ESP_ETH_PHY_ADDR_AUTO;
    phy_config.reset_gpio_num = -1;
    esp_eth_mac_t* mac = esp_eth_mac_new_openeth(&mac_config);
    esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phy_config);
    if (mac == nullptr || phy == nullptr) {
        std::printf("error: could not create the openeth MAC or its PHY - is this QEMU?\n");
        return false;
    }
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = nullptr;
    if (esp_eth_driver_install(&eth_config, &eth) != ESP_OK) {
        std::printf("error: could not install the openeth driver\n");
        return false;
    }

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    g_netif = esp_netif_new(&netif_config);
    ESP_ERROR_CHECK(esp_netif_attach(g_netif, esp_eth_new_netif_glue(eth)));

    g_events = xEventGroupCreate();
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, nullptr));
    ESP_ERROR_CHECK(esp_eth_start(eth));
    return true;
}

}  // namespace

bool network_up() {
    const std::lock_guard lock(g_mutex);
    if (g_up) {
        return true;
    }
    if (g_driver == Driver::not_started) {
        g_driver = start_driver() ? Driver::started : Driver::unavailable;
    }
    if (g_driver != Driver::started) {
        return false;
    }

    // QEMU's DHCP answers well inside a second. Bounded so a model that does
    // not answer says so rather than hanging a CI job to its timeout. The
    // client goes on asking after a call gives up, so a later call waits for
    // the same address again rather than starting anything a second time.
    const auto bits =
        xEventGroupWaitBits(g_events, kGotIpBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if ((bits & kGotIpBit) == 0) {
        std::printf("error: openeth got no address in 30 s\n");
        return false;
    }
    g_up = true;
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
