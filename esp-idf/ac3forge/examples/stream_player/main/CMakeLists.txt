# The sink is a DIRECTORY CMake picks, not a flag the player branches on - the
# same rule the library follows for its own platform choices. See
# main/audio_sink.hpp.
#
#   i2s   the real one, and the default
#   null  counts frames and returns; what CI builds, because qemu-system-xtensa
#         has no I2S and the real sink's first write would never return
# From Kconfig, not a -D cache variable: this file is evaluated in IDF's
# separate requirements-expansion process, which does not inherit cache
# variables from the project above it. See main/Kconfig.projbuild.
if(CONFIG_AC3FORGE_EXAMPLE_SINK_NULL)
    set(AC3FORGE_SINK_DIR "sink/null")
else()
    set(AC3FORGE_SINK_DIR "sink/i2s")
endif()

# esp_driver_i2s is required either way. The null build does not call it, but a
# component's REQUIRES is resolved before the source list is, and listing it
# conditionally would make the two builds differ in a second place for no gain.
idf_component_register(
    SRCS "stream_player.cpp"
         "${AC3FORGE_SINK_DIR}/audio_sink.cpp"
    INCLUDE_DIRS "."
    REQUIRES ac3forge esp_driver_i2s esp_partition esp_timer freertos)

# How long the audio actually is. The partition is 256 KB and the sample is
# 10,752 bytes, so without this the player reads a quarter of a megabyte of
# erased flash every lap and the accumulator dutifully skips all of it looking
# for a sync word - which "works", and is 251,392 bytes of resynchronisation
# reported on every pass.
#
# A real source knows its length: Content-Length, a file size, a directory
# entry. A raw partition does not, so the build supplies it - which is the
# honest version of the same fact rather than a heuristic like "stop at the
# first run of 0xFF".
file(SIZE "${CMAKE_CURRENT_SOURCE_DIR}/../stream/sample.ac3" AC3FORGE_STREAM_BYTES)
target_compile_definitions(${COMPONENT_LIB}
    PRIVATE "AC3FORGE_STREAM_BYTES=${AC3FORGE_STREAM_BYTES}")
