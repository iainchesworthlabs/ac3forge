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
# It installs the `library` and `libruntime` components, the two that make up the ac3forge-dev-*
# packages (cmake/Packaging.cmake), so the CLI and GUI stay out and a tree that is already built
# takes seconds. Each build directory is one AC3FORGE_INSTALL_BOTH_LINKAGES/BUILD_SHARED_LIBS
# combination and needs AC3FORGE_BUILD_CAPI=ON; pass several to check several. Which of the
# exported C API targets are linked and run, and which only compiled, is decided in
# install_consumer/CMakeLists.txt.
#
# Usage:  ./tools/checks/check_install_consumer.sh <build-dir>...
# Exit:   0 = every tree's consumer built and ran, 2 = bad invocation, anything else = a step
#         failed (its own output is above).

set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "Usage: $0 <build-dir>..." >&2
    exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

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

    cmake -S "$root/tools/checks/install_consumer" -B "$consumer_build" \
        -DCMAKE_PREFIX_PATH="$prefix"
    cmake --build "$consumer_build"
    ctest --test-dir "$consumer_build" --output-on-failure
done
