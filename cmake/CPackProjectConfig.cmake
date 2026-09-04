# ---------------------------------------------------------------------------
# Included by cpack itself once per generator during a multi-generator run
# (CPACK_PROJECT_CONFIG_FILE, wired up in Packaging.cmake) - CPACK_GENERATOR
# is set to the single generator currently being packaged when this runs, so
# variables here apply to that pass only, not the whole cpack invocation.
#
# Why this file exists: CPack's own default component-grouping mode
# (ONE_PER_GROUP - nothing in Packaging.cmake overrides it for archives) is
# what makes the archive generators (ZIP/TGZ) split into one independent
# runtime/dev download each, given Packaging.cmake's own GROUP assignments
# (runtime stays ungrouped; library+libruntime share GROUP "dev") - the
# deliberate design those generators use. But that grouping mode is global
# CPack state, not archive-specific, and DragNDrop (macOS) reads the exact
# same default - confirmed on real macOS CI during v0.3.0-beta.1's dry run,
# where it kept splitting into a -runtime.dmg and a -library.dmg even with
# CPACK_DMG_COMPONENT_INSTALL explicitly OFF. That variable alone does not
# override the grouping mode here. Forcing monolithic installation just for
# DragNDrop's own pass restores the one-dmg-bundles-everything shape this
# project has always intended for it, without touching the archive
# generators' split.
# ---------------------------------------------------------------------------

if(CPACK_GENERATOR STREQUAL "DragNDrop")
    set(CPACK_MONOLITHIC_INSTALL ON)
endif()

# DEB/RPM: the opposite override from DragNDrop above. Packaging.cmake's
# CPACK_COMPONENT_LIBRARY_GROUP/CPACK_COMPONENT_LIBRUNTIME_GROUP "dev" merges
# library+libruntime into one archive for ZIP/TGZ - correct there, but DEB/
# RPM want those same two components to stay three independent .deb/.rpm
# files (runtime, libac3forge0, libac3forge-dev/ac3forge-devel), which is the
# entire reason they're split into their own CPack components in the first
# place (see cmake/InstallLibrary.cmake). IGNORE - one package per component,
# not per group - restores that for exactly these two generators' own passes,
# without touching the group-based merge every other generator still uses.
if(CPACK_GENERATOR STREQUAL "DEB" OR CPACK_GENERATOR STREQUAL "RPM")
    set(CPACK_COMPONENTS_GROUPING IGNORE)
endif()

# NSIS: the Windows installer keeps carrying exactly what it carries today.
# CPACK_NSIS_COMPONENT_INSTALL is off (see Packaging.cmake), which makes that
# generator monolithic - it installs every component in CPACK_COMPONENTS_ALL
# into one installer - so the Desktop Atmos Demo would otherwise land inside
# the ac3cli/ac3gui installer the moment AC3FORGE_BUILD_WINDEMO is on for a
# packaging build. It ships as its own archive instead while its driver is
# test-signed and the demo is a demonstration rather than a product; the
# intended end state is the opposite (the installer installs the demo and
# its signed driver, and removes them on uninstall), and this is the one
# line to delete when that lands.
if(CPACK_GENERATOR STREQUAL "NSIS")
    list(REMOVE_ITEM CPACK_COMPONENTS_ALL windemo)
endif()
