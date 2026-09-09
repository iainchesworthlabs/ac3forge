// A FAT volume in FLASH, read exactly as the SD card is read.
//
// This is the mock that makes the file path testable. source/sd/ cannot run
// under QEMU - there is no SD host - but FATFS does not care what block device
// it sits on, so mounting the same filesystem from a flash partition exercises
// the identical VFS, FATFS and stdio code through ../file_common.hpp. What is
// NOT exercised is the SDMMC host, which is Espressif's driver rather than
// ours; everything we wrote runs.
//
// The volume is built at BUILD time and flashed with the application - see the
// project CMakeLists - so there is nothing to prepare on the host and no image
// to keep in the repository.

#include "byte_source.hpp"

#include <cstdio>

#include "esp_vfs_fat.h"

#include "source/file_common.hpp"

namespace player {
namespace {

constexpr const char* kMountPoint = "/audio";
wl_handle_t g_wl = WL_INVALID_HANDLE;

}  // namespace

bool source_open() {
    esp_vfs_fat_mount_config_t config = {};
    // Not formatted on failure: an unreadable volume here means the build did
    // not produce the image, and silently formatting would turn a build fault
    // into an empty filesystem and a confusing "file not found".
    config.format_if_mount_failed = false;
    config.max_files = 2;
    config.allocation_unit_size = CONFIG_WL_SECTOR_SIZE;

    const auto err = esp_vfs_fat_spiflash_mount_ro(kMountPoint, "storage", &config);
    if (err != ESP_OK) {
        std::printf("error: could not mount the 'storage' partition read-only (%d)\n",
                    static_cast<int>(err));
        return false;
    }

    const char* path = CONFIG_AC3FORGE_EXAMPLE_FATFS_PATH;
    if (!file_source::open(path)) {
        return false;
    }
    std::printf("source: fatfs %s, %lu bytes (flash-backed)\n", path,
                static_cast<unsigned long>(file_source::length()));
    return true;
}

std::size_t source_read(std::span<std::byte> dst) { return file_source::read(dst); }

bool source_rewind() { return file_source::rewind(); }

const char* source_name() { return "fatfs"; }

std::size_t source_length() { return file_source::length(); }

}  // namespace player
