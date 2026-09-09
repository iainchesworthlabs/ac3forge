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

#include "source/file_common.hpp"

namespace player {
namespace {

constexpr const char* kMountPoint = "/sdcard";

sdmmc_card_t* g_card = nullptr;

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

    // Everything past the mount is ../file_common.hpp, shared with
    // source/fatfs/ - which mounts the same filesystem from FLASH and
    // therefore runs under QEMU. That is what gives this file's read path
    // coverage it could not otherwise have: only the SDMMC host above goes
    // untested, and that is Espressif's driver rather than ours.
    const char* path = CONFIG_AC3FORGE_EXAMPLE_SD_PATH;
    if (!file_source::open(path)) {
        return false;
    }
    std::printf("source: sd %s, %lu bytes\n", path,
                static_cast<unsigned long>(file_source::length()));
    return true;
}

std::size_t source_read(std::span<std::byte> dst) { return file_source::read(dst); }

bool source_rewind() { return file_source::rewind(); }

const char* source_name() { return "sd"; }

std::size_t source_length() { return file_source::length(); }

}  // namespace player
