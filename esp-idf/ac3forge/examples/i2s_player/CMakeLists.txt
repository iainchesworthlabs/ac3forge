# ---------------------------------------------------------------------------
# ac3forge's I2S player example: decode AC-3 and put it out of a DAC.
#
#   . $IDF_PATH/export.sh
#   idf.py set-target esp32s3
#   idf.py -p <PORT> flash monitor
#
# Wire an I2S DAC to the three pins `idf.py menuconfig` shows under
# "ac3forge I2S player" and listen. See README.md.
#
# This is what an integrator's own project looks like: two lines to find the
# component, and then nothing else about ac3forge in the build at all.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

# Three directories up: i2s_player -> examples -> ac3forge, then one more to
# esp-idf/, which is the directory CONTAINING components. An integrator who has
# pulled this in with `idf.py add-dependency` does not need this line - the
# managed component is already on the path - and would write their project
# without it.
get_filename_component(AC3FORGE_ESP_COMPONENTS "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(EXTRA_COMPONENT_DIRS "${AC3FORGE_ESP_COMPONENTS}")

# Decode. The default, spelled out because an example should show the knob
# exists rather than rely on which way it happens to point.
set(AC3FORGE_ESP_PROFILE "decoder" CACHE STRING "")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ac3forge_i2s_player LANGUAGES C CXX)
