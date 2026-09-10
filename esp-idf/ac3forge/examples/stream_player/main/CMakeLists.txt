# The sink is a DIRECTORY CMake picks, not a flag the player branches on - the
# same rule the library follows for its own platform choices. See
# main/audio_sink.hpp.
#
#   i2s      stereo on the I2S peripheral, and the default
#   tdm      the I2S peripheral in TDM mode, up to sixteen slots
#   capture  the i2s or tdm sink's conversion, checked, with no peripheral
#            behind it: what CI runs, because qemu-system-xtensa has no I2S
#            and the other two sinks' first write would never return
#   null     counts blocks and returns; CI builds it and does not run it
# From Kconfig, not a -D cache variable: this file is evaluated in IDF's
# separate requirements-expansion process, which does not inherit cache
# variables from the project above it. See main/Kconfig.projbuild.
if(CONFIG_AC3FORGE_EXAMPLE_SINK_NULL)
    set(AC3FORGE_SINK_DIR "sink/null")
elseif(CONFIG_AC3FORGE_EXAMPLE_SINK_CAPTURE)
    set(AC3FORGE_SINK_DIR "sink/capture")
elseif(CONFIG_AC3FORGE_EXAMPLE_SINK_TDM)
    set(AC3FORGE_SINK_DIR "sink/tdm")
else()
    set(AC3FORGE_SINK_DIR "sink/i2s")
endif()

# And where the bytes come from - the mirror of the same idea, see
# main/byte_source.hpp. CI runs the partition, fatfs and http sources under
# QEMU, http over the emulated Ethernet below; sd needs an SDMMC host, which
# QEMU does not have, so CI only builds it.
if(CONFIG_AC3FORGE_EXAMPLE_SOURCE_SD)
    set(AC3FORGE_SOURCE_DIR "source/sd")
elseif(CONFIG_AC3FORGE_EXAMPLE_SOURCE_FATFS)
    set(AC3FORGE_SOURCE_DIR "source/fatfs")
elseif(CONFIG_AC3FORGE_EXAMPLE_SOURCE_HTTP)
    set(AC3FORGE_SOURCE_DIR "source/http")
else()
    set(AC3FORGE_SOURCE_DIR "source/partition")
endif()

# The HTTP source has a seam of its own underneath it: which network. WiFi on a
# board; the OpenCores Ethernet MAC under QEMU, which is what lets CI run this
# source end to end. See main/source/http/network.hpp. Only the HTTP source
# compiles a network at all - the other sources have no use for one, and a
# file that is compiled and never called is one more thing to keep building.
set(AC3FORGE_NETWORK_SRCS "")
if(CONFIG_AC3FORGE_EXAMPLE_SOURCE_HTTP)
    if(CONFIG_AC3FORGE_EXAMPLE_NETWORK_OPENETH)
        set(AC3FORGE_NETWORK_SRCS "source/http/net/openeth/network.cpp")
    else()
        set(AC3FORGE_NETWORK_SRCS "source/http/net/wifi/network.cpp")
    endif()
endif()

# esp_driver_i2s is required either way. The null build does not call it, but a
# component's REQUIRES is resolved before the source list is, and listing it
# conditionally would make the two builds differ in a second place for no gain.
# EVERY component every sink and source might need, unconditionally, whichever
# pair this build selected.
#
# Not laziness - a measured constraint. IDF expands REQUIRES in a separate,
# earlier pass than the one that evaluates the source list, and CONFIG_ values
# are not reliably set in it: a conditional REQUIRES silently resolved to the
# default branch while the SOURCES resolved correctly, so an SD build compiled
# source/sd/byte_source.cpp against a component list that did not include
# esp_driver_sdmmc and failed on a missing header. Listing them all makes the
# builds differ in exactly one place, which is the directory each seam picks.
#
# sdmmc as well as esp_driver_sdmmc: sdmmc_cmd.h lives in the former and
# sdmmc_host.h in the latter. fatfs carries esp_vfs_fat.h. esp_eth for the
# QEMU network under the HTTP source, listed for every build for the reason
# above.
idf_component_register(
    SRCS "stream_player.cpp"
         "${AC3FORGE_SINK_DIR}/audio_sink.cpp"
         "${AC3FORGE_SOURCE_DIR}/byte_source.cpp"
         ${AC3FORGE_NETWORK_SRCS}
    INCLUDE_DIRS "." "source/http"
    REQUIRES ac3forge esp_driver_i2s esp_timer freertos
             esp_partition
             esp_driver_sdmmc sdmmc fatfs wear_levelling
             esp_wifi esp_eth esp_http_client esp_netif esp_event nvs_flash)

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
# Defined for every build even though only the partition source reads it: an SD
# file and an HTTP body report their own length, and a definition that appears
# and disappears with the source choice is one more way the builds can differ.
