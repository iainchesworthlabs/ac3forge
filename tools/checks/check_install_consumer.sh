#!/usr/bin/env bash
#
# Installed-package consumer check for ac3forge's C API and its AC-4 decoder.
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
# API target is linked and run, static and shared; install_consumer/CMakeLists.txt says how. So is
# every exported AC-4 decoder target, in a C++ program that decodes a committed stream
# (install_consumer/consumer_ac4.cpp), and a tree built with AC3FORGE_BUILD_AC4=ON whose package
# exports none fails.
#
# The same prefix is then used the way a Makefile, Meson or autotools build uses it, through its
# .pc files (cmake/PkgConfig.cmake), which are all such a build has. The C program is linked with
# the C compiler that configured the tree (cc when the tree has none) and the flags pkg-config
# prints, and nothing else. A prefix whose ac3forge_c.pc names the static archive, the shape a
# vcpkg or Conan package has (AC3FORGE_INSTALL_BOTH_LINKAGES=OFF and BUILD_SHARED_LIBS=OFF), is
# asked with --static, which is what puts Requires.private and Libs.private on the link line.
# Every link here runs with --as-needed, which GCC on Ubuntu does by default and clang does not:
# it drops an -lm that comes before the archive calling it, so a .pc that lists its libraries in
# the wrong order fails under either compiler. Each .pc that names an archive is also linked whole
# into an empty C program on its own, which finds what one component's file lacks even when no
# consumer reaches that archive. The AC-4 program is built the same way from ac4dec.pc, by the C++
# compiler that configured the tree, where the package has one. It needs pkg-config, or whatever
# $PKG_CONFIG names.
#
# Usage:  ./tools/checks/check_install_consumer.sh <build-dir>...
# Exit:   0 = every tree's archives linked whole, its consumers built and ran and its .pc files
#         linked, 2 = bad invocation, anything else = a step failed (its own output is above).

set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "Usage: $0 <build-dir>..." >&2
    exit 2
fi
if ! command -v "${PKG_CONFIG:-pkg-config}" > /dev/null; then
    echo "::error::${PKG_CONFIG:-pkg-config} is not installed, and the pkg-config consumer needs it" >&2
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

# pkg-config confined to the .pc files of one prefix (pc_dir, set per build directory below), so a
# copy of ac3forge installed on the machine is never the one that gets read.
pc() {
    PKG_CONFIG_LIBDIR="$pc_dir" "${PKG_CONFIG:-pkg-config}" "$@"
}

# The prefix, consumed through pkg-config alone. $1 = the prefix, $2 = a scratch directory, $3 =
# the C compiler that configured the tree, or empty for cc, $4 = its C++ compiler, or empty for c++.
pkg_config_check() {
    local prefix="$1" work="$2" cc="${3:-${CC:-cc}}" cxx="${4:-${CXX:-c++}}"
    local -a flags libs whole static=() ac4_static=()
    local pc_file name flag libdir

    pc_file="$(find "$prefix" -name ac3forge_c.pc -print -quit)"
    if [[ -z "$pc_file" ]]; then
        echo "::error::$prefix installed no ac3forge_c.pc" >&2
        return 1
    fi
    pc_dir="$(dirname "$pc_file")"

    # A Libs line that names the archive is a static-only install, and --static is what makes
    # pkg-config read the private fields that say what the archive needs.
    case " $(pc --libs-only-l ac3forge_c) " in
        *" -lac3forge_c_static "*) static=(--static) ;;
        *) ;;
    esac

    libdir="$(pc --variable=libdir ac3forge_c)"
    read -r -a flags <<< "$(pc ${static[@]+"${static[@]}"} --cflags --libs ac3forge_c)"
    echo "--- $cc consumer.c, flags from: pkg-config ${static[*]:+${static[*]} }--cflags --libs ac3forge_c"
    echo "    ${flags[*]}"
    if ! "$cc" -std=c11 "$root/tools/checks/install_consumer/consumer.c" -o "$work/pc_consumer" \
            -Wl,--as-needed -Wl,-rpath,"$libdir" "${flags[@]}"; then
        echo "::error::the flags pkg-config prints for ac3forge_c do not link the C consumer (the linker's complaint is above) - see cmake/PkgConfig.cmake" >&2
        return 1
    fi
    "$work/pc_consumer"

    # The AC-4 decoder through ac4dec.pc, where the package has one, in the same way. Its Requires
    # line brings ac4.pc, and a static-only install's Requires.private the core's archive.
    if [[ -f "$pc_dir/ac4dec.pc" ]]; then
        case " $(pc --libs-only-l ac4dec) " in
            *" -lac4dec_static "*) ac4_static=(--static) ;;
            *) ;;
        esac
        libdir="$(pc --variable=libdir ac4dec)"
        read -r -a flags <<< "$(pc ${ac4_static[@]+"${ac4_static[@]}"} --cflags --libs ac4dec)"
        echo "--- $cxx consumer_ac4.cpp, flags from: pkg-config ${ac4_static[*]:+${ac4_static[*]} }--cflags --libs ac4dec"
        echo "    ${flags[*]}"
        if ! "$cxx" -std=c++23 "$root/tools/checks/install_consumer/consumer_ac4.cpp" \
                -o "$work/pc_consumer_ac4" -Wl,--as-needed -Wl,-rpath,"$libdir" "${flags[@]}"; then
            echo "::error::the flags pkg-config prints for ac4dec do not link the AC-4 consumer (the linker's complaint is above) - see cmake/PkgConfig.cmake" >&2
            return 1
        fi
        "$work/pc_consumer_ac4" "$root/tests/golden/external-baseline/ac4-51-film-96/dee.ac4"
    fi

    # One .pc at a time, every archive it names linked whole into a program that calls nothing.
    printf 'int main(void) { return 0; }\n' > "$work/empty.c"
    for pc_file in "$pc_dir"/*.pc; do
        name="$(basename "$pc_file" .pc)"
        case " $(pc --libs-only-l "$name") " in
            *" -l"*"_static "*) ;;
            *) continue ;;
        esac
        read -r -a libs <<< "$(pc --static --libs "$name")"
        whole=()
        for flag in "${libs[@]}"; do
            case "$flag" in
                -l*_static) whole+=("-Wl,--whole-archive" "$flag" "-Wl,--no-whole-archive") ;;
                *) whole+=("$flag") ;;
            esac
        done
        echo "--- $name, its archives linked whole: pkg-config --static --libs $name"
        echo "    ${libs[*]}"
        if ! "$cc" "$work/empty.c" -o "$work/whole_$name" -Wl,--as-needed "${whole[@]}"; then
            echo "::error::pkg-config --static --libs $name does not link its archive on its own (the linker's complaint is above) - see cmake/PkgConfig.cmake" >&2
            return 1
        fi
    done
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

    # AC3FORGE_EXPECT_AC4: a tree that built the AC-4 libraries has to have installed them.
    expect_ac4="$(cache_value "$build" AC3FORGE_BUILD_AC4)"
    cmake -S "$root/tools/checks/install_consumer" -B "$consumer_build" \
        -DCMAKE_PREFIX_PATH="$prefix" \
        -DAC3FORGE_EXPECT_AC4="${expect_ac4:-OFF}" \
        ${compilers[@]+"${compilers[@]}"}
    cmake --build "$consumer_build"
    ctest --test-dir "$consumer_build" --output-on-failure

    pkg_config_check "$prefix" "$scratch/$index" "$c_compiler" "$cxx_compiler"
done
