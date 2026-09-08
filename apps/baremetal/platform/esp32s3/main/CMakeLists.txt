# The ESP32-S3 application: IDF's app_main plus its clock, and the REAL probe -
# apps/baremetal/probe.cpp or encode_probe.cpp, the same files the host and
# arm-none-eabi shapes build, checking the same fixtures against the same
# levels and the same hashes.
#
# The probe is reached by relative path rather than copied. Three directories
# up: main -> esp32s3 -> platform -> baremetal. It is listed as a source here
# rather than added to the ac3forge component because it is the APPLICATION -
# the component is the library, and an integrator pulling ac3forge into their
# own project wants the codec, not this probe.
#
# WHICH probe follows AC3FORGE_ESP_PROFILE, set by the project above this and
# read by the ac3forge component to build the matching half of the library.
# Both define ac3probe::run() rather than main(), so main.cpp calls whichever
# was built without knowing which it is - the same seam apps/baremetal's own
# CMakeLists.txt uses for the two hosted shapes.
if(AC3FORGE_ESP_PROFILE STREQUAL "encoder")
    set(AC3FORGE_ESP_PROBE_SRC "../../../encode_probe.cpp")
else()
    set(AC3FORGE_ESP_PROBE_SRC "../../../probe.cpp")
endif()

# REQUIRES is spelled out even though `main` would otherwise depend on every
# component in the build automatically. Naming them is the point: this
# application is a footprint probe, and "it links whatever IDF happens to
# provide" is not a property a footprint probe should have. Each entry below is
# here because a header in main.cpp needs it -
#
#   ac3forge       the decoder under test
#   esp_timer      esp_timer_get_time(), ac3probe::now_us()'s source
#   esp_hw_support esp_cpu.h, for esp_cpu_get_cycle_count()
#   esp_rom        esp_rom_sys.h, for the busy-wait esp_rom_delay_us()
#   freertos       vTaskDelay() and the task app_main runs in
#
# - and removing any one of them should break the build rather than silently
# widen it.
idf_component_register(
    SRCS "main.cpp"
         "${AC3FORGE_ESP_PROBE_SRC}"
    INCLUDE_DIRS ""
    REQUIRES ac3forge esp_timer esp_hw_support esp_rom freertos)

# probe.hpp and the two fixture headers all sit beside the probes.
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
