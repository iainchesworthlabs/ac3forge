# The ESP32-C6 application: IDF's app_main plus its clock, the REAL probe -
# apps/baremetal/probe.cpp or encode_probe.cpp, the same files every other leg
# builds - and the network load main/Kconfig.projbuild chooses. The esp32s3
# sibling's copy of this file carries the reasoning for the first two.
if(AC3FORGE_ESP_PROFILE STREQUAL "encoder")
    set(AC3FORGE_ESP_PROBE_SRC "../../../encode_probe.cpp")
else()
    set(AC3FORGE_ESP_PROBE_SRC "../../../probe.cpp")
endif()

# The network load is a DIRECTORY, like the stream_player example's sinks and
# sources: net/none/ has no network code at all, net/wifi/ joins an access
# point and reads a TCP stream. One of them is compiled.
if(CONFIG_AC3FORGE_PROBE_NETWORK_WIFI)
    set(AC3FORGE_PROBE_NETWORK_DIR "net/wifi")
else()
    set(AC3FORGE_PROBE_NETWORK_DIR "net/none")
endif()

# The WiFi components are listed for both loads. IDF expands REQUIRES in an
# earlier pass than the one that reads CONFIG_ values, so a conditional list
# resolves to its default branch while the source list resolves correctly (see
# the stream_player example's main/CMakeLists.txt). Listing them changes
# nothing the `none` load runs: nothing it compiles calls into them.
#
#   ac3forge       the decoder under test
#   esp_timer      esp_timer_get_time(), ac3probe::now_us()'s source
#   esp_hw_support esp_cpu.h, for esp_cpu_get_cycle_count()
#   esp_rom        esp_rom_sys.h, for the busy-wait esp_rom_delay_us()
#   freertos       vTaskDelay() and the task app_main runs in
#   esp_wifi esp_netif esp_event nvs_flash lwip   the WiFi load
idf_component_register(
    SRCS "main.cpp"
         "${AC3FORGE_ESP_PROBE_SRC}"
         "../../../stage_timers.cpp"
         "${AC3FORGE_PROBE_NETWORK_DIR}/network_load.cpp"
    INCLUDE_DIRS "."
    REQUIRES ac3forge esp_timer esp_hw_support esp_rom freertos
             esp_wifi esp_netif esp_event nvs_flash lwip)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

# What this part can give the decode, from main/Kconfig.projbuild. Zero, the
# default, attempts every fixture; a run that aborts on an allocation says which
# fixture needs a budget below it. See docs/platforms/bare-metal/esp32-c6.md
# for the values measured with each network load.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    "AC3FORGE_PROBE_HEAP_BUDGET_BYTES=${CONFIG_AC3FORGE_PROBE_HEAP_BUDGET_BYTES}")
