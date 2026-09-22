# The ESP32-C3 application: IDF's app_main plus its clock, and the REAL probe -
# apps/baremetal/probe.cpp or encode_probe.cpp, the same files the host, the
# arm-none-eabi leg and the S3 build, checking the same fixtures against the
# same levels and the same hashes. The esp32s3 sibling's copy of this file
# carries the reasoning; only the paths and the target differ.
if(AC3FORGE_ESP_PROFILE STREQUAL "encoder")
    set(AC3FORGE_ESP_PROBE_SRC "../../../encode_probe.cpp")
else()
    set(AC3FORGE_ESP_PROBE_SRC "../../../probe.cpp")
endif()

idf_component_register(
    SRCS "main.cpp"
         "${AC3FORGE_ESP_PROBE_SRC}"
         "../../../stage_timers.cpp"
    INCLUDE_DIRS ""
    REQUIRES ac3forge esp_timer esp_hw_support esp_rom freertos)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

# What this part can actually give the decode. The allocator reports 249,180
# bytes free, and that is NOT the number: this heap is regioned, its largest
# block is 114,688, and the 7.1.4 fixture - a 238,094-byte peak - failed on a
# 6,144-byte request with the total still showing eleven kilobytes spare. So
# the budget is what the part was observed to manage rather than what it says
# it has, and a fixture above it is skipped and named (probe.cpp's
# over_budget) instead of aborting the run.
#
# Raising it is a measurement, not an edit: run the leg, see which fixture
# fails and where, and move this to just under it.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    AC3FORGE_PROBE_HEAP_BUDGET_BYTES=230000)
