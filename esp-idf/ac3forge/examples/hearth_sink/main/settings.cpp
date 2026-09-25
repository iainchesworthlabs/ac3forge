// The board's own settings, in NVS. See settings.hpp for why they are not in
// the image.

#include "settings.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>

#include "esp_err.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace player {
namespace {

// One namespace, opened per write and closed again: these are written by a
// person pressing something, not in a loop, and a handle held open for the
// life of the run is a handle to leak on any path that fails.
constexpr const char* kNamespace = "hearth_sink";
constexpr const char* kKeyName = "name";
constexpr const char* kKeySsid = "ssid";
constexpr const char* kKeyPassword = "password";
constexpr const char* kKeySlotBits = "slot_bits";
constexpr const char* kKeySecondLine = "second_line";

Settings g_settings;
bool g_loaded = false;

void copy_into(std::span<char> field, std::string_view text) {
    const std::size_t room = field.size() - 1;
    const std::size_t taken = std::min(text.size(), room);
    std::memcpy(field.data(), text.data(), taken);
    field[taken] = '\0';
}

// "hearth-a1b2c3" from the low three bytes of the board's MAC: two boards from
// the same box answer to different names without anyone naming them.
void default_name(std::span<char> field) {
    std::array<std::uint8_t, 6> mac{};
    if (!board_mac(mac)) {
        copy_into(field, "hearth");
        return;
    }
    std::array<char, kMaxNameBytes + 1> text{};
    (void)std::snprintf(text.data(), text.size(), "hearth-%02x%02x%02x", mac[3], mac[4], mac[5]);
    copy_into(field, text.data());
}

// A string from NVS into a fixed field, leaving it alone when the key is
// absent (a board that has never been told) or too long for the field (a
// partition written by a different build).
void read_string(nvs_handle_t handle, const char* key, std::span<char> field) {
    std::size_t length = field.size();
    if (nvs_get_str(handle, key, field.data(), &length) != ESP_OK) {
        return;
    }
    field[field.size() - 1] = '\0';
}

bool write_string(const char* key, std::string_view value, std::span<char> field) {
    if (value.size() > field.size() - 1) {
        std::printf("settings: that value is %u bytes, longer than the %u %s holds\n",
                    static_cast<unsigned>(value.size()),
                    static_cast<unsigned>(field.size() - 1), key);
        return false;
    }
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        std::printf("settings: could not open NVS to write %s\n", key);
        return false;
    }
    // A fixed-size local rather than the caller's view: the value is not
    // NUL-terminated on its own, and nvs_set_str wants a C string.
    std::array<char, kMaxPasswordBytes + 1> text{};
    const std::size_t taken = std::min(value.size(), text.size() - 1);
    std::memcpy(text.data(), value.data(), taken);
    text[taken] = '\0';
    const esp_err_t err = nvs_set_str(handle, key, text.data());
    if (err == ESP_OK) {
        (void)nvs_commit(handle);
        copy_into(field, std::string_view(text.data(), taken));
    } else {
        std::printf("settings: NVS refused %s (%s)\n", key, esp_err_to_name(err));
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool write_int(const char* key, std::int32_t value) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        std::printf("settings: could not open NVS to write %s\n", key);
        return false;
    }
    const esp_err_t err = nvs_set_i32(handle, key, value);
    if (err == ESP_OK) {
        (void)nvs_commit(handle);
    } else {
        std::printf("settings: NVS refused %s (%s)\n", key, esp_err_to_name(err));
    }
    nvs_close(handle);
    return err == ESP_OK;
}

}  // namespace

void settings_load() {
    if (g_loaded) {
        return;
    }
    g_loaded = true;

    // WiFi's own calibration data lives in NVS too, and network_up() used to
    // be what initialised it. Doing it here means the settings are readable on
    // a board whose network never comes up at all.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // The image's own answers first, so a board with nothing stored behaves
    // exactly as it did before there was anywhere to store anything. The
    // network is the exception: its Kconfig pair exists only in a build that
    // compiled the WiFi seam, so the fallback to it lives there
    // (source/http/net/wifi/network.cpp) and an unprovisioned board simply
    // has no network here.
    default_name(g_settings.name);
    g_settings.slot_bits = CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS;
    g_settings.second_line = CONFIG_AC3FORGE_EXAMPLE_I2S_SECOND_LINE != 0;

    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        // Nothing has ever been written: not a failure, just a new board.
        std::printf("settings: nothing stored; name %s, %d-bit slots, %s line\n",
                    g_settings.name.data(), g_settings.slot_bits,
                    g_settings.second_line ? "a second" : "no second");
        return;
    }
    read_string(handle, kKeyName, g_settings.name);
    read_string(handle, kKeySsid, g_settings.ssid);
    read_string(handle, kKeyPassword, g_settings.password);
    std::int32_t stored = 0;
    if (nvs_get_i32(handle, kKeySlotBits, &stored) == ESP_OK) {
        // Checked here as well as where it is written, and said out loud when
        // it fails: a width that is neither 16 nor 32 reaches line_ceiling(),
        // which answers "no such width", and plan_sink then refuses to open
        // the sink at all. On a board that boots unattended that would be
        // silence with the reason on a console nobody is reading, so a bad
        // stored value costs a line and the build's own width stands.
        if (stored == 16 || stored == 32) {
            g_settings.slot_bits = static_cast<int>(stored);
        } else {
            std::printf("settings: stored slot width %ld is not 16 or 32; keeping %d\n",
                        static_cast<long>(stored), g_settings.slot_bits);
        }
    }
    if (nvs_get_i32(handle, kKeySecondLine, &stored) == ESP_OK) {
        g_settings.second_line = stored != 0;
    }
    nvs_close(handle);

    // The password is never printed, here or anywhere: a console capture goes
    // into CI artifacts and into this repository's issues.
    std::printf("settings: name %s, network '%s'%s, %d-bit slots, %s line\n",
                g_settings.name.data(), g_settings.ssid.data(),
                g_settings.password[0] != '\0' ? " (password stored)" : "", g_settings.slot_bits,
                g_settings.second_line ? "a second" : "no second");
}

const Settings& settings() { return g_settings; }

bool board_mac(std::span<std::uint8_t, 6> mac) {
    // Ask before reading, not read and fall back: esp_read_mac() logs an error
    // for a MAC type the target has none of, and a P4 would print one on every
    // boot for a MAC it was never going to have. ESP-IDF's MAC table
    // (components/esp_hw_support/mac_addr.c) lists the station MAC only where
    // SOC_WIFI_SUPPORTED is set, and the P4's WiFi is a second chip's;
    // esp_mac_addr_len_get() gives 0 for a type the table lacks, and says
    // nothing. A station MAC is the base MAC unchanged (generate_mac), so the
    // base MAC is what a chip without a radio has in its place.
    const esp_mac_type_t type =
        esp_mac_addr_len_get(ESP_MAC_WIFI_STA) != 0 ? ESP_MAC_WIFI_STA : ESP_MAC_BASE;
    return esp_read_mac(mac.data(), type) == ESP_OK;
}

bool settings_set_name(std::string_view name) {
    if (name.empty()) {
        std::printf("settings: a name cannot be empty\n");
        return false;
    }
    return write_string(kKeyName, name, g_settings.name);
}

bool settings_set_network(std::string_view ssid, std::string_view password) {
    if (ssid.empty()) {
        std::printf("settings: an SSID cannot be empty\n");
        return false;
    }
    if (!write_string(kKeySsid, ssid, g_settings.ssid)) {
        return false;
    }
    return write_string(kKeyPassword, password, g_settings.password);
}

bool settings_set_slot_bits(int bits) {
    if (bits != 16 && bits != 32) {
        std::printf("settings: %d-bit slots is not a width (16 or 32)\n", bits);
        return false;
    }
    if (!write_int(kKeySlotBits, bits)) {
        return false;
    }
    g_settings.slot_bits = bits;
    return true;
}

bool settings_set_second_line(bool wired) {
    if (!write_int(kKeySecondLine, wired ? 1 : 0)) {
        return false;
    }
    g_settings.second_line = wired;
    return true;
}

bool settings_forget() {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        std::printf("settings: could not open NVS to clear it\n");
        return false;
    }
    const esp_err_t err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        std::printf("settings: NVS refused the erase (%s)\n", esp_err_to_name(err));
        return false;
    }
    g_loaded = false;
    settings_load();
    return true;
}

}  // namespace player
