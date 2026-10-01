# ---------------------------------------------------------------------------
# Tracy.cmake
#
# Defines an INTERFACE target `iclforge::tracy` that, when ICLFORGE_ENABLE_TRACY is
# on, links Tracy's client library and defines ICLFORGE_TRACY_ENABLED, which
# the iclforge/base/detail/profiling.hpp variant selected by src/ac3/CMakeLists.txt
# (src/base/variants/profiling-tracy_enabled/, chosen over its tracy_disabled/
# sibling by an include-dir switch, not an #ifdef) uses to turn ICLFORGE_ZONE_SCOPED()
# etc. into real Tracy zones instead of no-ops. Off by default, matching
# Coverage.cmake's shape: normal dev/CI builds pay no instrumentation cost and
# do not even need Tracy present.
#
# Tracy itself is an OPT-IN vcpkg manifest feature ("profiling" in
# vcpkg.json), not a base dependency - configure with
# -DVCPKG_MANIFEST_FEATURES=profiling to make it resolvable at all. The
# cli-tools feature is what this investigation actually needs: `capture` and
# `csvexport` record and dump a trace headlessly, without the GUI profiler.
# See docs/platforms/android.md's performance-investigation notes for how
# this was used to find the encode_frame() hotspot.
# ---------------------------------------------------------------------------

option(ICLFORGE_ENABLE_TRACY "Enable Tracy profiler instrumentation" OFF)

add_library(iclforge_tracy INTERFACE)
add_library(iclforge::tracy ALIAS iclforge_tracy)

if(ICLFORGE_ENABLE_TRACY)
    find_package(Tracy CONFIG REQUIRED)
    target_link_libraries(iclforge_tracy INTERFACE Tracy::TracyClient)
    target_compile_definitions(iclforge_tracy INTERFACE ICLFORGE_TRACY_ENABLED)
endif()
