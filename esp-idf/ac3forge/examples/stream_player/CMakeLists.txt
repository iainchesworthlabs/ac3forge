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
# which is what CI does. sdkconfig.ci selects the null sink - a real sink, same
# calls and same buffers, with no peripheral behind it; see main/audio_sink.hpp.
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
