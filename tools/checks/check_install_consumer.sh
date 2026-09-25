#!/usr/bin/env bash
#
# Installed-package consumer check for ac3forge's C API.
#
# tests/capi and the C examples compile against the build tree, where the generated headers sit
# under <build>/src/capi/generated whether or not an install rule copies them. That is how
# `cmake --install` shipped an ac3forge_c/ac3forge.h nobody could include: it #includes
# ac3forge_c/version.h, which is generated, and only the source include/ directory and export.h
# were installed. This is the check that was missing. For each build directory given it installs
# the tree, then configures, builds and runs a C program (tools/checks/install_consumer) against
# the installed prefix with find_package(ac3forge), the way a downstream project would.
#
# The build tree hides a second thing. Every executable in it links {fmt}, so an installed static
# archive whose objects call a {fmt} function they do not define still links there, while a
# consumer of the installed archive stopped at `undefined reference to fmt::v12::vprint`. One
# consumer also pulls only the archive members its own calls reach: the C program never reaches
# mp4's HLS and DASH writers, which call {fmt} as well. So before that program is built, every
# installed static archive is linked whole into one executable by the C++ driver of the compiler
# that built the tree. The C++ runtime and libm are the driver's to supply, and whatever the
# linker still reports as undefined is a symbol the package uses and does not provide.
# --whole-archive is the GNU and lld spelling, so this runs where the CI leg that calls it does,
# on Linux.
#
# It installs the `library` and `libruntime` components, the two that make up the ac3forge-dev-*
# packages (cmake/Packaging.cmake), so the CLI and GUI stay out and a tree that is already built
# takes seconds. Each build directory is one AC3FORGE_INSTALL_BOTH_LINKAGES/BUILD_SHARED_LIBS
# combination and needs AC3FORGE_BUILD_CAPI=ON; pass several to check several. Every exported C
# API target is linked and run, static and shared; install_consumer/CMakeLists.txt says how.
#
# Usage:  ./tools/checks/check_install_consumer.sh <build-dir>...
# Exit:   0 = every tree's archives linked whole and its consumer built and ran, 2 = bad
#         invocation, anything else = a step failed (its own output is above).

set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "Usage: $0 <build-dir>..." >&2
    exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# One value out of a build tree's CMakeCache.txt, whatever its type annotation (the compilers are
# FILEPATH in one tree and STRING in another).
cache_value() {
    sed -n "s/^$2:[A-Z]*=//p" "$1/CMakeCache.txt" | head -n 1
}

index=0
for build in "$@"; do
    index=$((index + 1))
    prefix="$scratch/$index/prefix"
    consumer_build="$scratch/$index/consumer"

    echo "=== $build"
    cmake --install "$build" --prefix "$prefix" --component library
    cmake --install "$build" --prefix "$prefix" --component libruntime

    if [[ ! -d "$prefix/include/ac3forge_c" ]]; then
        echo "::error::$build installed no include/ac3forge_c; was it configured with AC3FORGE_BUILD_CAPI=OFF?" >&2
        exit 1
    fi
    echo "--- installed include/ac3forge_c:"
    ls -1 "$prefix/include/ac3forge_c"

    # The compilers that built the libraries, not whichever cc and c++ a machine offers.
    c_compiler="$(cache_value "$build" CMAKE_C_COMPILER)"
    cxx_compiler="$(cache_value "$build" CMAKE_CXX_COMPILER)"

    archives=("$prefix"/lib/lib*_static.a)
    if [[ -e "${archives[0]}" ]]; then
        echo "--- every installed static archive, linked whole: ${archives[*]##*/}"
        printf 'int main() { return 0; }\n' > "$scratch/$index/closed.cpp"
        if ! "${cxx_compiler:-c++}" "$scratch/$index/closed.cpp" -o "$scratch/$index/closed" \
                -Wl,--whole-archive "${archives[@]}" -Wl,--no-whole-archive; then
            echo "::error::$build: an installed static archive uses a symbol that nothing in the package defines (listed above), and a consumer linking it stops at the same place - see cmake/Fmt.cmake" >&2
            exit 1
        fi
    fi

    compilers=()
    if [[ -n "$c_compiler" ]]; then
        compilers+=("-DCMAKE_C_COMPILER=$c_compiler")
    fi
    if [[ -n "$cxx_compiler" ]]; then
        compilers+=("-DCMAKE_CXX_COMPILER=$cxx_compiler")
    fi

    cmake -S "$root/tools/checks/install_consumer" -B "$consumer_build" \
        -DCMAKE_PREFIX_PATH="$prefix" \
        ${compilers[@]+"${compilers[@]}"}
    cmake --build "$consumer_build"
    ctest --test-dir "$consumer_build" --output-on-failure
done
