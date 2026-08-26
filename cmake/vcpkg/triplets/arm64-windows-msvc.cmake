# Overlay triplet: arm64 Windows, MSVC (roadmap DR8).
#
# Linkage policy: dynamic CRT, static dependency libraries - see
# x64-windows-msvc.cmake for the reasoning, which is the same here. ac3forge
# takes only test/tooling packages from vcpkg (Catch2), so linking them
# statically keeps ac3tests.exe self-contained; the CRT stays dynamic because
# the prebuilt Qt kits the GUI links against (where one is available) are
# built that way.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Chainload policy: not set here, same as x64-windows-msvc.cmake and for the
# identical reason - VCPKG_CHAINLOAD_TOOLCHAIN_FILE is set once, per concrete
# configure preset (CMakePresets.json), not repeated in a triplet. vcpkg
# builds Catch2 with its own default toolset rather than the chainloaded one;
# on Windows that is what you want, since a port driving its own build system
# needs the unmodified MSVC environment, and vcpkg's own toolset selection
# already resolves an ARM64-capable compiler on an ARM64 target triplet.
#
# No VCPKG_ENV_PASSTHROUGH here, same reasoning as x64-windows-msvc.cmake.
