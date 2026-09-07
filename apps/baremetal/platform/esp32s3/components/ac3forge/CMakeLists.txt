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
# NOTE ON PLACEMENT. This lives inside the probe app for now. The eventual
# publishable component (an integrator running `idf.py add-dependency`, or the
# ESPHome external component calling add_idf_component()) wants it somewhere
# reusable instead - see docs/platforms/esp32.md. Moving it is a relocation,
# not a rewrite: nothing below depends on where it sits except AC3FORGE_ROOT.
# ---------------------------------------------------------------------------

# Registered with no sources and no include directories of its own: both come
# from ac3::forge_minimal, propagated by the INTERFACE link at the bottom of
# this file (its own include directories are PUBLIC, behind $<BUILD_INTERFACE>,
# which is exactly the in-build case this is). A component registered with no
# SRCS gets an INTERFACE ${COMPONENT_LIB} - hence INTERFACE below rather than
# PUBLIC, which CMake refuses on an interface target.
idf_component_register()

# apps/baremetal/platform/esp32s3/components/ac3forge -> repo root is six up:
# ac3forge -> components -> esp32s3 -> platform -> baremetal -> apps -> root.
get_filename_component(AC3FORGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

# Pre-seed the root's option()s before pulling it in: option() only sets a
# cache variable if one is not already present, so FORCE-ing these first means
# the root CMakeLists.txt's own option(...) calls see these values already set
# and leave them alone.
#
# This is the same fourteen the root's own guard (CMakeLists.txt:230) refuses
# the profile alongside, plus the profile switch itself. They are spelled out
# rather than looped because the guard names them individually too, and a
# mismatch between the two lists should be visible in a diff.
set(AC3FORGE_MINIMAL_DECODER ON CACHE BOOL "" FORCE)
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

target_link_libraries(${COMPONENT_LIB} INTERFACE ac3::forge_minimal)
