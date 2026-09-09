// An SD card, over SDMMC, through FATFS.
//
// COMPILED BY CI, NOT RUN. QEMU has no SD host, so nothing here is exercised
// without a board - which is why it is deliberately small: the less that lives
// behind an unrunnable seam, the less can be wrong in it. What CI does
// establish is that it builds against the current IDF, which is the failure
// that would otherwise be found by an integrator rather than by us.
//
// SDMMC rather than SPI. The ESP32-S3 has an SD host peripheral and 4-bit mode
// is several times faster than SPI, which matters less for one AC-3 stream
// (448 kbit/s is nothing) than it does for the CPU: SPI mode bit-bangs through
// the driver, and this player would rather spend its cycles decoding.

#include "byte_source.hpp"

#include <cstdio>

#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace player {
namespace {

constexpr const char* kMountPoint = "/sdcard";

sdmmc_card_t* g_card = nullptr;
std::FILE* g_file = nullptr;
std::size_t g_length = 0;

}  // namespace

bool source_open() {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    // Not formatted on failure. A card that will not mount is a card with
    // something on it that this cannot read, and formatting it is not a
    // recovery - it is the destruction of whatever the user was trying to play.
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 2;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = CONFIG_AC3FORGE_EXAMPLE_SD_BUS_WIDTH;
    slot_config.clk = static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_SD_CLK_GPIO);
    slot_config.cmd = static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_SD_CMD_GPIO);
    slot_config.d0 = static_cast<gpio_num_t>(CONFIG_AC3FORGE_EXAMPLE_SD_D0_GPIO);
    // The card's own pull-ups are usually absent on breakout wiring, and the
    // symptom is a card that enumerates intermittently rather than one that
    // fails cleanly.
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    if (esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot_config, &mount_config, &g_card) !=
        ESP_OK) {
        std::printf("error: could not mount an SD card at %s\n", kMountPoint);
        return false;
    }

    const char* path = CONFIG_AC3FORGE_EXAMPLE_SD_PATH;
    g_file = std::fopen(path, "rb");
    if (g_file == nullptr) {
        std::printf("error: %s is not on the card\n", path);
        return false;
    }
    // The length, which the framer needs so it does not read past the audio.
    // A file has one, unlike a partition.
    if (std::fseek(g_file, 0, SEEK_END) == 0) {
        const auto end = std::ftell(g_file);
        g_length = end > 0 ? static_cast<std::size_t>(end) : 0;
        std::rewind(g_file);
    }
    std::printf("source: sd %s, %lu bytes\n", path, static_cast<unsigned long>(g_length));
    return true;
}

std::size_t source_read(std::span<std::byte> dst) {
    if (g_file == nullptr) {
        return 0;
    }
    return std::fread(dst.data(), 1, dst.size(), g_file);
}

bool source_rewind() {
    if (g_file == nullptr) {
        return false;
    }
    std::rewind(g_file);
    return true;
}

const char* source_name() { return "sd"; }

std::size_t source_length() { return g_length; }

}  // namespace player
