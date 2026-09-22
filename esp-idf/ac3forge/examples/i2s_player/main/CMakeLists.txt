# fixture.hpp is reached by relative path rather than copied: five directories
# up to the repo root, then apps/baremetal. It is the same 5.1 AC-3 stream the
# probe checks against known levels, so if this example plays the wrong thing
# the probe says so first.
get_filename_component(AC3FORGE_FIXTURE_DIR
                       "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../apps/baremetal" ABSOLUTE)

idf_component_register(
    SRCS "i2s_player.cpp"
    INCLUDE_DIRS ""
    REQUIRES ac3forge esp_driver_i2s esp_timer freertos)

target_include_directories(${COMPONENT_LIB} PRIVATE "${AC3FORGE_FIXTURE_DIR}")
