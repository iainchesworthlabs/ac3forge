# Overlay triplet: arm64 macOS (Apple Silicon), LLVM (clang).
#
# Sibling: x64-macos-llvm.cmake, added for DR8's macOS universal binaries once
# GitHub's macos-15-intel hosted runner made a real (not cross-compiled) x64
# leg possible - see that file's own header.
#
# Linkage policy: dynamic runtime, static dependency libraries - see
# x64-windows-msvc.cmake for the reasoning, which is the same here.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Chainload policy: not set here. See x64-linux-gcc.cmake.
#
# macOS carries one extra wrinkle: macos.llvm.toolchain.cmake prefers LLVM's own
# libc++ over the SDK's, while vcpkg will build the ports against the SDK's.
# Both are libc++ so the ABI matches, but this is the platform where the
# assumption is thinnest - and it is untested, since this project has no macOS
# host to configure on (CI is real hardware, but nobody has inspected the
# resulting binary's ABI by hand).
