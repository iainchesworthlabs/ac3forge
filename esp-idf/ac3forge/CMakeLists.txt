# ---------------------------------------------------------------------------
# ac3::forge_minimal as an ESP-IDF component - a THIN wrapper, not a second
# copy of ac3forge's build graph, and deliberately the same shape as
# apps/android/app/src/main/cpp/CMakeLists.txt: pre-seed the repo root's
# option()s, then add_subdirectory() the root so the library is built from the
# SAME source-of-truth target definitions every other platform uses. A change
# to src/forge/minimal.cmake's source list is picked up here automatically,
# with nothing to keep in sync by hand.
#
# The alternative - re-listing minimal.cmake's 24 sources in an
# idf_component_register(SRCS ...) - was rejected for exactly that reason: two
# copies of a source list drift, and the drift is silent until a link error on
# a target nobody builds locally.
#
# PLACEMENT. Top level, beside python/, rust/ and js/ - the repo's existing
# convention of one directory per binding, and an ESP-IDF component is one.
# It used to live inside apps/baremetal/platform/esp32s3/, which made it the
# probe's private business; an integrator running `idf.py add-dependency`, or an
# ESPHome external component calling add_idf_component(), needs it somewhere it
# can be pointed at without dragging a test application along.
#
# Consumers add the DIRECTORY ABOVE this one to EXTRA_COMPONENT_DIRS - IDF wants
# the directory that CONTAINS components, not the component itself:
#
#     set(EXTRA_COMPONENT_DIRS "/path/to/ac3forge/esp-idf")
#
# The probe application under apps/baremetal/platform/esp32s3/ is now just
# another consumer of this, which is the point: if it stops working the way an
# integrator's project would, its own build says so.
# ---------------------------------------------------------------------------

# Two things are registered here, and the library is neither of them. The
# codec's sources and include directories come from ac3::forge_minimal,
# propagated by the PUBLIC link at the bottom of this file (its own include
# directories are PUBLIC, behind $<BUILD_INTERFACE>, which is exactly the
# in-build case this is). What this component adds of its own is the layer
# that cannot live in the library because it is made of FreeRTOS: the player
# (include/ac3forge/player.hpp, src/player.cpp - a fetch task, a decode task and
# the ring between them, over a source seam and a sink seam) and the
# planar-to-interleaved conversion every sink shares (ac3forge/interleave.hpp,
# tested on the host by tests/io/test_interleave.cpp, and on its way into the
# library - see planning/esp32-player.md). freertos, esp_timer and heap are what
# the player's own sources include.
#
# The control surface (include/ac3forge/control.hpp, src/control.cpp) is here
# too, on esp_http_server, with the web page it serves: ui/ac3forge_ui.html and
# ui/ac3forge_ui.js, embedded as they are and sent from flash
# (planning/esp32-device-ui.md). IDF names an embedded file's symbols after its
# base name, hence the component's name on both. Like the rest of this archive
# they reach a firmware only when something in it uses Control.
#
# The Sendspin player (CONFIG_AC3FORGE_SENDSPIN, this directory's Kconfig) adds
# its sources only when a project turns it on. The condition reads a CONFIG_
# value, which is unset during IDF's requirements pass - where the source list
# does not matter - and set when the sources are compiled. REQUIRES does not
# wait for it: mbedtls and nvs_flash are listed for every project, since a
# REQUIRES that depended on the configuration would resolve in the pass that
# cannot see it (examples/hearth_sink/main/CMakeLists.txt found the same).
#
# src/tcp_arrivals.cpp is built whatever the configuration: it defines the
# lwIP hook a project installs from lwip_hooks/ac3forge_lwip_hooks.h, which
# lwIP then needs to link. How it finds a socket's IPv4 peer depends on
# whether lwIP has IPv6 (src/peer/ipv4_peer.hpp).
set(AC3FORGE_COMPONENT_SRCS "src/player.cpp" "src/control.cpp" "src/tcp_arrivals.cpp")
if(CONFIG_LWIP_IPV6)
    list(APPEND AC3FORGE_COMPONENT_SRCS "src/peer/dual_stack/ipv4_peer.cpp")
else()
    list(APPEND AC3FORGE_COMPONENT_SRCS "src/peer/ipv4_only/ipv4_peer.cpp")
endif()
if(CONFIG_AC3FORGE_SENDSPIN)
    list(APPEND AC3FORGE_COMPONENT_SRCS
        "src/burst_player.cpp"
        "src/sendspin_host.cpp"
        "src/sendspin_store.cpp")
endif()
idf_component_register(
    SRCS ${AC3FORGE_COMPONENT_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "lwip_hooks"
    REQUIRES freertos esp_timer heap esp_http_server mbedtls nvs_flash
    PRIV_REQUIRES lwip
    EMBED_FILES "ui/ac3forge_ui.html" "ui/ac3forge_ui.js")

# ac3forge_lwip_ip4_input (src/tcp_arrivals.cpp) is called only from lwIP's own
# ip4.c, once a project installs the hook (lwip_hooks/ac3forge_lwip_hooks.h) -
# nothing in this component's own sources calls it, so a linker that pulls
# object files from this archive on an as-needed basis has no reason to keep
# that translation unit unless something forces it, and drops it whenever
# nothing else in the same build happens to reference tcp_arrivals.cpp's other
# symbols (as when CONFIG_AC3FORGE_SENDSPIN is off: undefined reference to
# ac3forge_lwip_ip4_input from ip4.c, 2026-09-22). PRIV_REQUIRES lwip above is
# unconditional, so liblwip.a - and this component's own tcp_arrivals.cpp.obj -
# are always part of the link; this just makes the linker keep the one symbol
# a hook, once installed, needs to find.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=ac3forge_lwip_ip4_input")

# How the interleave converts a float sample to an integer slot is chosen by
# part, as a header directory: conversion/float/ where the CPU has a
# floating-point unit, conversion/bits/ where it does not. Each holds
# ac3forge/slot_conversion.hpp, naming the conversion interleave.hpp's
# interleaves default to. On a part without an FPU each float operation is a
# call into the software floating-point routines, four a sample, and the bits
# form computes the same integers with integer operations; on a part with one,
# the float form is the few instructions it looks like. The choice is read from
# CONFIG_SOC_CPU_HAS_FPU, the capability ESP-IDF's own soc component picks the
# RISC-V float ABI by.
#
# Below the registration rather than in its INCLUDE_DIRS. IDF's requirements
# pass also runs this file, before any sdkconfig exists, and its
# idf_component_register returns: everything above the registration runs there
# with the flag unset on every part, so the directory it recorded for the
# component would be conversion/bits/ on an ESP32-S3 too. Nothing below the
# registration runs in that pass. PUBLIC, as INCLUDE_DIRS would be, because
# interleave.hpp is a header the sinks in a consuming project include.
if(CONFIG_SOC_CPU_HAS_FPU)
    target_include_directories(${COMPONENT_LIB} PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/conversion/float")
else()
    target_include_directories(${COMPONENT_LIB} PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/conversion/bits")
endif()

# Where the library's own CMake lives, which depends on how this component was
# obtained.
#
#   IN THE REPO, at esp-idf/ac3forge/, the root is two up: ac3forge -> esp-idf
#   -> root. This is the case the probe app and both examples build.
#
#   PACKED, as an archive from the component registry, the library sits in lib/
#   INSIDE this directory: a component tarball is rooted here and cannot
#   reference anything above itself, so tools/packaging/pack_esp_component.py
#   stages a copy of what the profile needs there before packing.
#
#   lib/ rather than beside this file, because both trees have a CMakeLists.txt
#   at their root - this wrapper and the library's own project - and staging
#   them into one directory replaces the first with the second.
#
# Tested for rather than configured, because the answer is a property of the
# tree this file finds itself in and a consumer should not have to know which
# kind they have.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/src/forge/CMakeLists.txt")
    set(AC3FORGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/lib")
else()
    get_filename_component(AC3FORGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
if(NOT EXISTS "${AC3FORGE_ROOT}/src/forge/CMakeLists.txt")
    message(FATAL_ERROR
        "ac3forge: no library sources at '${AC3FORGE_ROOT}'. In a checkout this component "
        "lives at esp-idf/ac3forge/; packed, it carries its own lib/ - see "
        "tools/packaging/pack_esp_component.py.")
endif()

# WHICH DIRECTION. Decode by default - that is what a player wants, and what
# most of this port has been about; AC3FORGE_ESP_PROFILE=encoder builds the
# other half. Never both: the root refuses the combination, for the measured
# reason that no two of decode / AC-3 encode / E-AC-3 encode fit in this part's
# internal SRAM at once (233,546 / 201,770 / 243,770 bytes of peak against
# 277,400 free).
#
# A consuming project sets it before the component is scanned, in its top-level
# CMakeLists.txt above the include of project.cmake:
#
#     set(AC3FORGE_ESP_PROFILE encoder)
#
# It is a STRING rather than the two booleans it expands to so that the illegal
# state - both on, or both off - cannot be spelled at all from outside.
set(AC3FORGE_ESP_PROFILE "decoder" CACHE STRING
    "Which half of the minimum-footprint profile to build: decoder or encoder")
set_property(CACHE AC3FORGE_ESP_PROFILE PROPERTY STRINGS decoder encoder)
if(AC3FORGE_ESP_PROFILE STREQUAL "encoder")
    set(AC3FORGE_MINIMAL_ENCODER ON CACHE BOOL "" FORCE)
    set(AC3FORGE_MINIMAL_DECODER OFF CACHE BOOL "" FORCE)
elseif(AC3FORGE_ESP_PROFILE STREQUAL "decoder")
    set(AC3FORGE_MINIMAL_DECODER ON CACHE BOOL "" FORCE)
    set(AC3FORGE_MINIMAL_ENCODER OFF CACHE BOOL "" FORCE)
else()
    message(FATAL_ERROR
        "AC3FORGE_ESP_PROFILE is 'decoder' or 'encoder' (got '${AC3FORGE_ESP_PROFILE}')")
endif()

# Everything else the profile excludes. Pre-seeded before the root is pulled in:
# option() only sets a cache variable if one is not already present, so FORCEing
# these first means the root CMakeLists.txt's own option(...) calls see the
# values already set and leave them alone.
#
# This is the same fourteen the root's own guard (CMakeLists.txt:230) refuses
# the profile alongside. They are spelled out rather than looped because the
# guard names them individually too, and a mismatch between the two lists should
# be visible in a diff.
set(AC3FORGE_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_GUI OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_MATROSKA OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_MP4 OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_MPEGTS OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_IAB OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_IAMF OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_CAPI OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_ADM OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(AC3FORGE_BUILD_AC4 OFF CACHE BOOL "" FORCE)

# src/sendspin's player half, for the Sendspin player: the core and the
# player's session, pairing and Noise, with its crypto seam over ESP-IDF's own
# mbedTLS (the PSA Crypto API, which v6.1's mbedTLS 4 provides). The WebSocket
# and mDNS a computer takes from vcpkg are the component's and the example's
# own here, over esp_http_server and espressif/mdns. AC3FORGE_BUILD_HEARTH
# alone would also build apps/hearth, which the root skips when the player
# half is all there is.
if(CONFIG_AC3FORGE_SENDSPIN)
    set(AC3FORGE_BUILD_HEARTH ON CACHE BOOL "" FORCE)
    set(AC3FORGE_SENDSPIN_PLAYER_ONLY ON CACHE BOOL "" FORCE)
    set(AC3FORGE_SENDSPIN_MBEDTLS_TARGET "idf::mbedtls" CACHE STRING "" FORCE)
else()
    set(AC3FORGE_BUILD_HEARTH OFF CACHE BOOL "" FORCE)
endif()

# The decode-critical sources at -O2. src/forge/minimal.cmake lists them, with
# what each one measured, and the probe under apps/baremetal/platform/esp32s3/
# has always been built this way - so the timing docs/platforms/bare-metal/esp32-s3.md
# quotes is this build, and a player built from this component without it
# decodes slower than those figures. The cost is flash, not SRAM. Not FORCEd:
# a project that needs the flash back says so before project(),
#
#     set(AC3FORGE_MINIMAL_HOT_O2 OFF CACHE BOOL "")
#
# or on the command line, `idf.py -DAC3FORGE_MINIMAL_HOT_O2=OFF build`.
set(AC3FORGE_MINIMAL_HOT_O2 ON CACHE BOOL
    "Decode-critical sources at -O2; see src/forge/minimal.cmake")

# The player's own source with them, under the same switch: the render onto the
# layout and the calls into the sink run there, six times a frame, and the
# header-only renderer compiles into it. Measured on a board with the example's sinks
# alongside it: 1.5 ms a frame less at twelve slots, for 4,400 bytes of flash
# (planning/esp32-714-realtime.md, decision 11).
if(AC3FORGE_MINIMAL_HOT_O2)
    set_source_files_properties(src/player.cpp PROPERTIES COMPILE_OPTIONS "-O2")
endif()

# WHICH ARITHMETIC. idf_component.yml says the decode arithmetic follows the
# part: single precision where there is a floating-point unit, the fixed-point
# tier where there is not (planning/arithmetic-tiers.md). This is where a
# project that has not chosen gets that. src/forge/minimal.cmake builds float32
# for every value except `fixed`, including no value at all, so without this a
# project for a part with no FPU decodes in float, where every floating-point
# operation is a compiled subroutine.
#
# Whether the part has an FPU comes from SOC_CPU_HAS_FPU, one of the capability
# symbols ESP-IDF's soc component writes into every target's sdkconfig.
# ESP-IDF's own components/soc/project_include.cmake reads the same symbol to
# give a RISC-V core -mabi=ilp32f, so the arithmetic chosen here and the ABI
# the compiler is given come from one place, and a part added to
# idf_component.yml's targets needs nothing added here. In ESP-IDF v6.1 the
# ESP32, S3, P4, H4 and S31 set it, and no other target does.
#
# Not FORCEd, and skipped entirely when the project has chosen already - above
# its project(), as apps/baremetal/platform/esp32c3/ does, or on the command
# line:
#
#     idf.py -DAC3FORGE_DECODE_SCALAR=float build
#
# DEFINED rather than set(CACHE) alone, because a normal variable set above
# project() is a choice too. The order that makes this work: the project's
# CMakeLists.txt sets its variables above project(); project() loads sdkconfig
# and then adds the components, which runs this file; and minimal.cmake reads
# the value inside the add_subdirectory() below. ESP-IDF's earlier requirements
# pass includes this file as well, and stops at idf_component_register().
if(NOT DEFINED AC3FORGE_DECODE_SCALAR)
    if(CONFIG_SOC_CPU_HAS_FPU)
        set(AC3FORGE_DECODE_SCALAR "float" CACHE STRING
            "Decode arithmetic: float (this part has an FPU) or fixed")
    else()
        set(AC3FORGE_DECODE_SCALAR "fixed" CACHE STRING
            "Decode arithmetic: fixed (this part has no FPU) or float")
    endif()
    set_property(CACHE AC3FORGE_DECODE_SCALAR PROPERTY STRINGS fixed float)
endif()

# Tells apps/baremetal/CMakeLists.txt that an external build system owns the
# application. Without it that file would see CMAKE_SYSTEM_NAME "Generic" -
# which ESP-IDF's own toolchain.cmake sets, exactly as the arm-none-eabi one
# does - and build the Cortex-M3 probe: its reset vector, its __aeabi_read_tp
# TLS shim and its mps2-an385 linker script, none of which mean anything here,
# into a second executable IDF has no way to flash.
set(AC3FORGE_BAREMETAL_PLATFORM "esp32s3" CACHE STRING "" FORCE)

# The binary directory name is arbitrary but must be given explicitly: with a
# source directory this far outside CMAKE_SOURCE_DIR, CMake cannot derive one
# by mirroring the relative path the way it does for an in-tree subdirectory.
add_subdirectory("${AC3FORGE_ROOT}" ac3forge_build)

# PUBLIC, not INTERFACE: the component now has sources of its own that include
# the library's headers, and so does everything that includes the component's.
target_link_libraries(${COMPONENT_LIB} PUBLIC ac3::forge_minimal)
if(CONFIG_AC3FORGE_SENDSPIN)
    target_link_libraries(${COMPONENT_LIB} PUBLIC ac3::sendspin)
endif()

# The burst player's source under the same switch as player.cpp: it renders,
# routes and meters every block of a Sendspin stream.
if(CONFIG_AC3FORGE_SENDSPIN AND AC3FORGE_MINIMAL_HOT_O2)
    set_source_files_properties(src/burst_player.cpp PROPERTIES COMPILE_OPTIONS "-O2")
endif()
