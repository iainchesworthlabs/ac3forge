# vcpkg port for iclforge - installs the library only (iclforge::ac3, plus iclforge::matroska,
# iclforge::mp4, iclforge::mpegts, iclforge::c, the AC-4 libraries, iclforge::iab and iclforge::iamf as
# opt-in features, off unless asked for, since each adds public targets), never the CLI, GUI,
# Hearth, tests, examples or fuzz harnesses - upstream's own ICLFORGE_BUILD_CLI/GUI/HEARTH/TESTS/
# EXAMPLES/FUZZERS options make that a plain OFF each, no patching needed. iclforge::adm (the
# ADM/BW64 reader) and iclforge::admbridge have no feature here: ac3adm needs Boost and, even though
# both are now installed/exported by upstream (shared-only - see cmake/InstallLibrary.cmake's
# ICLFORGE_BUILD_ADM block upstream), this port keeps ICLFORGE_BUILD_ADM=OFF below rather than
# adding an "adm" feature - out of scope for this port until there's a real need for it.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO iainchesworthlabs/ac3forge
    REF "v${VERSION}"
    SHA512 b28a4de6884a952007140f4766db4c28fc7892abed374472df0decbf5d6e9eda5f3cc12170b2ba7b6df9b9e10d22f99a790258a465e7b432e73b4759bb151d35
    HEAD_REF main
)

# One vcpkg feature <-> one ICLFORGE_BUILD_<NAME> CMake option, OFF where the feature is not
# asked for: upstream defaults ICLFORGE_BUILD_AC4/IAB/IAMF (and the container writers) ON.
vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        matroska ICLFORGE_BUILD_MATROSKA
        mp4      ICLFORGE_BUILD_MP4
        mpegts   ICLFORGE_BUILD_MPEGTS
        capi     ICLFORGE_BUILD_CAPI
        ac4      ICLFORGE_BUILD_AC4
        iab      ICLFORGE_BUILD_IAB
        iamf     ICLFORGE_BUILD_IAMF
)

# DERIVED_VERSION_OVERRIDE: upstream derives its version via `git describe`, which finds nothing
# in a tarball checkout and falls back to "0.0.0-dev" - thread the real tag through instead.
#
# ICLFORGE_BUILD_HEARTH defaults ON upstream and builds Hearth, an application (apps/hearth) and a
# library nothing installs (src/sendspin), with dependencies this port does not declare
# (cpp-httplib, mdns, mbedTLS, libFLAC, Opus); upstream also refuses it beside
# ICLFORGE_BUILD_AC4=OFF, the ac4 feature's absence.
#
# ICLFORGE_BUILD_ADM/ICLFORGE_ENABLE_TRACY are already OFF by upstream's own default; pinned
# explicitly so a future default change upstream can't silently pull an undeclared dependency
# into this port. ICLFORGE_WITH_ALSA/ICLFORGE_WITH_PIPEWIRE default to AUTO upstream and would
# otherwise probe the build machine's ambient ALSA/PipeWire installs even though this
# library-only build never builds, links or installs iclforge::audio at all.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DICLFORGE_BUILD_CLI=OFF
        -DICLFORGE_BUILD_GUI=OFF
        -DICLFORGE_BUILD_HEARTH=OFF
        -DICLFORGE_BUILD_TESTS=OFF
        -DICLFORGE_BUILD_EXAMPLES=OFF
        -DICLFORGE_BUILD_FUZZERS=OFF
        -DICLFORGE_FETCH_CATCH2=OFF
        -DICLFORGE_INSTALL_BOTH_LINKAGES=OFF
        -DICLFORGE_BUILD_ADM=OFF
        -DICLFORGE_ENABLE_TRACY=OFF
        -DICLFORGE_WITH_ALSA=OFF
        -DICLFORGE_WITH_PIPEWIRE=OFF
        "-DDERIVED_VERSION_OVERRIDE=v${VERSION}"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME iclforge CONFIG_PATH lib/cmake/iclforge)

vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
