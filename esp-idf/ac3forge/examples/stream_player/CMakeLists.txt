# ---------------------------------------------------------------------------
# ac3forge's streaming player example: decode AC-3 out of a flash partition.
#
#   . $IDF_PATH/export.sh
#   idf.py set-target esp32s3
#   idf.py -p <PORT> flash monitor
#
# `idf.py flash` writes stream/sample.ac3 into the `audio` partition as well as
# the application - see the flash-to-partition call below - so there is nothing
# to copy by hand.
#
# The sibling of i2s_player, differing in one thing: where the audio comes from.
# That one decodes a bitstream linked into its own image; this one never holds
# more than 16 KB of the stream at a time. See main/stream_player.cpp.
#
# WITHOUT A BOARD:
#
#   SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci" idf.py build
#   idf.py qemu
#
# which is the first of the four configurations CI runs under QEMU (the others
# overlay sdkconfig.ci-tdm, sdkconfig.ci-render and sdkconfig.ci-http).
# sdkconfig.ci selects the capture sink - the same calls and the same audio as
# a real sink, converted with the same code and checked, with no peripheral
# behind it; see main/audio_sink.hpp.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

get_filename_component(AC3FORGE_ESP_COMPONENTS "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(EXTRA_COMPONENT_DIRS "${AC3FORGE_ESP_COMPONENTS}")
set(AC3FORGE_ESP_PROFILE "decoder" CACHE STRING "")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ac3forge_stream_player LANGUAGES C CXX)

# The audio, into the partition named in partitions.csv. Done here rather than
# through a separate esptool invocation so `idf.py flash` puts everything the
# application needs on the part in one step - a demo whose second half has to be
# flashed by hand is a demo that gets reported as broken.
esptool_py_flash_to_partition(flash "audio" "${CMAKE_CURRENT_SOURCE_DIR}/stream/sample.ac3")

# And the same bytes again as a FAT filesystem, for source/fatfs/.
#
# Not duplication for its own sake: source/sd/ cannot run under QEMU, which has
# no SD host, but FATFS does not care what block device it sits on. Mounting the
# same filesystem from flash runs the identical VFS, FATFS and stdio path that
# the SD source uses (main/source/file_common.hpp), so the file layer gets
# exercised on target and only the SDMMC host stays untested.
#
# read_only, because nothing writes to it and a read-only image needs no wear
# levelling - which keeps the mount to esp_vfs_fat_spiflash_mount_ro and the
# partition to its nominal size.
fatfs_create_rawflash_image(storage "${CMAKE_CURRENT_SOURCE_DIR}/stream" FLASH_IN_PROJECT)
