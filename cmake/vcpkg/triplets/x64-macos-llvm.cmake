# Overlay triplet: x64 macOS (Intel), LLVM (clang).
#
# Sibling of arm64-macos-llvm.cmake - see that file for the arm64 half. Both
# exist now that DR8's macOS universal binaries are real: GitHub's
# macos-15-intel hosted runner is genuine native Intel hardware (not Rosetta),
# so this triplet builds a real, not cross-compiled, x86_64 half for
# .github/workflows/_build.yml's package-macos-universal job to lipo-merge
# with the arm64 half into one universal ac3cli/ac3gui bundle.
#
# Linkage policy: dynamic runtime, static dependency libraries - see
# x64-windows-msvc.cmake for the reasoning, which is the same here.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Chainload policy: not set here. See x64-linux-gcc.cmake.
#
# macOS carries one extra wrinkle: macos.llvm.toolchain.cmake prefers LLVM's own
# libc++ over the SDK's, while vcpkg will build the ports against the SDK's.
# Both are libc++ so the ABI matches, but this is the platform where the
# assumption is thinnest - unlike the arm64 triplet, this one IS exercised by
# real CI (macos-llvm-x64, on macos-15-intel), so a real ABI mismatch here
# would show up as a real build/link failure rather than staying an
# unconfirmed risk.
