# vcpkg port for ac3forge - installs the library only (ac3::forge, plus matroska::matroska,
# mp4::mp4, mpegts::mpegts and ac3::forge_c as opt-in features), never the CLI, GUI, tests,
# examples or fuzz harnesses - upstream's own AC3FORGE_BUILD_CLI/GUI/TESTS/EXAMPLES/FUZZERS
# options make that a plain OFF each, no patching needed. ac3adm::ac3adm (the ADM/BW64 reader)
# and ac3::admbridge have no feature here: ac3adm needs Boost and, even though both are now
# installed/exported by upstream (shared-only - see cmake/InstallLibrary.cmake's
# AC3FORGE_BUILD_ADM block upstream), this port keeps AC3FORGE_BUILD_ADM=OFF below rather than
# adding an "adm" feature - out of scope for this port until there's a real need for it.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO iainchesworthlabs/ac3forge
    REF "v${VERSION}"
    SHA512 3faa29f76ffa9ff506a7e08c0e8b3287d5bab1c1d20bbe5c13c0c6a3cba06cff9d525186dbd034eb187d30206a6aec9d9e01f627b927d1088c9caa849e43d040
    HEAD_REF main
)

# One vcpkg feature <-> one AC3FORGE_BUILD_<NAME> CMake option.
vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        matroska AC3FORGE_BUILD_MATROSKA
        mp4      AC3FORGE_BUILD_MP4
        mpegts   AC3FORGE_BUILD_MPEGTS
        capi     AC3FORGE_BUILD_CAPI
)

# DERIVED_VERSION_OVERRIDE: upstream derives its version via `git describe`, which finds nothing
# in a tarball checkout and falls back to "0.0.0-dev" - thread the real tag through instead.
#
# AC3FORGE_BUILD_ADM/AC3FORGE_ENABLE_TRACY are already OFF by upstream's own default; pinned
# explicitly so a future default change upstream can't silently pull an undeclared dependency
# into this port. AC3FORGE_WITH_ALSA/AC3FORGE_WITH_PIPEWIRE default to AUTO upstream and would
# otherwise probe the build machine's ambient ALSA/PipeWire installs even though this
# library-only build never builds, links or installs ac3::audio at all.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DAC3FORGE_BUILD_CLI=OFF
        -DAC3FORGE_BUILD_GUI=OFF
        -DAC3FORGE_BUILD_TESTS=OFF
        -DAC3FORGE_BUILD_EXAMPLES=OFF
        -DAC3FORGE_BUILD_FUZZERS=OFF
        -DAC3FORGE_FETCH_CATCH2=OFF
        -DAC3FORGE_INSTALL_BOTH_LINKAGES=OFF
        -DAC3FORGE_BUILD_ADM=OFF
        -DAC3FORGE_ENABLE_TRACY=OFF
        -DAC3FORGE_WITH_ALSA=OFF
        -DAC3FORGE_WITH_PIPEWIRE=OFF
        "-DDERIVED_VERSION_OVERRIDE=v${VERSION}"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME ac3forge CONFIG_PATH lib/cmake/ac3forge)

vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
