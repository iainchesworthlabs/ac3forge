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
idf_component_register(
    SRCS "src/player.cpp" "src/control.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer heap esp_http_server
    EMBED_FILES "ui/ac3forge_ui.html" "ui/ac3forge_ui.js")

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

# The decode-critical sources at -O2. src/forge/minimal.cmake lists them, with
# what each one measured, and the probe under apps/baremetal/platform/esp32s3/
# has always been built this way - so the timing docs/platforms/esp32.md
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
# layout, the copies into the output task's ring and the calls into the sink
# run there, six times a frame. Measured on a board with the example's sinks
# alongside it: 1.5 ms a frame less at twelve slots, for 4,400 bytes of flash
# (planning/esp32-714-realtime.md, decision 11).
if(AC3FORGE_MINIMAL_HOT_O2)
    set_source_files_properties(src/player.cpp PROPERTIES COMPILE_OPTIONS "-O2")
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
