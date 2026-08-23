# ---------------------------------------------------------------------------
# Fmt.cmake
#
# Defines an INTERFACE target `ac3::fmt` wrapping {fmt} (`fmt::fmt`) - the
# formatting library this project uses everywhere instead of std::format/
# std::print. NDK r26's bundled libc++ (LLVM 17) does not implement <format>
# at all unless the compiler is invoked with -fexperimental-library, which
# nothing in this project's Android build passes (see
# docs/platforms/android.md); {fmt} has no such gap, since it depends on
# nothing libc++ gates behind that flag.
#
# Unlike Tracy.cmake, this is NOT opt-in: {fmt} is a base dependency, always
# resolved, the same way Catch2 is for AC3FORGE_BUILD_TESTS builds (see
# tests/CMakeLists.txt, whose find-then-FetchContent-fallback shape this
# mirrors). Desktop builds get it from vcpkg (see vcpkg.json's base
# "dependencies"); the Android app build never wires vcpkg's toolchain in at
# all (apps/android/app/src/main/cpp/CMakeLists.txt has no vcpkg/VCPKG
# reference anywhere), so it silently takes the FetchContent fallback below -
# {fmt} is a plain CMake/C++ library and builds cleanly under the NDK
# toolchain with no further plumbing needed, unlike bolting vcpkg's own
# Android triplet chainloading on for this one dependency.
# ---------------------------------------------------------------------------

# Matches packaging/vcpkg-port/ac3forge/vcpkg.json's own fmt dependency and
# packaging/conan/conanfile.py's pinned requirement, so all three routes
# build against the same code. fmt's own git tags carry no "v" prefix
# (unlike Catch2's), hence GIT_TAG "${AC3FORGE_FMT_VERSION}" below, not
# "v${AC3FORGE_FMT_VERSION}".
set(AC3FORGE_FMT_VERSION 12.2.0)

option(AC3FORGE_FETCH_FMT "Fetch {fmt} from source via FetchContent when no local copy is found" ON)

find_package(fmt CONFIG QUIET)

if(NOT fmt_FOUND)
    if(NOT AC3FORGE_FETCH_FMT)
        message(FATAL_ERROR
            "{fmt} was not found and AC3FORGE_FETCH_FMT is OFF.\n"
            "Supply it with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake "
            "or -DCMAKE_PREFIX_PATH=<prefix>, or allow the download by setting "
            "AC3FORGE_FETCH_FMT=ON.")
    endif()

    message(STATUS
        "{fmt} not found locally; fetching v${AC3FORGE_FMT_VERSION} "
        "(-DAC3FORGE_FETCH_FMT=OFF to require a local copy)")

    include(FetchContent)
    FetchContent_Declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG "${AC3FORGE_FMT_VERSION}"
        GIT_SHALLOW TRUE
        # SYSTEM keeps fmt's headers out of reach of /W4 /WX, matching how
        # the installed package presents itself; EXCLUDE_FROM_ALL keeps its
        # own install rules out of ours (see tests/CMakeLists.txt's Catch2
        # fallback for the identical reasoning).
        SYSTEM
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(fmt)

    # fmt's own CMakeLists does not set POSITION_INDEPENDENT_CODE on its
    # `fmt` target, and a plain FetchContent build defaults to whatever the
    # ambient (unset) value is - fine for a static-only consumer, but
    # forge_shared (src/forge/CMakeLists.txt) links every dependency,
    # including this one, into a real .so/.dll. Confirmed the hard way: the
    # WASM leg and the manylinux wheel build (neither wires vcpkg's toolchain
    # in, so both take this fallback) both failed linking libac3forge.so with
    # "relocation ... can not be used when making a shared object; recompile
    # with -fPIC" pointing straight at fmt's own object file. Desktop builds
    # never hit this: vcpkg's fmt port already builds PIC-correct for
    # whichever linkage its triplet asks for.
    set_target_properties(fmt PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

add_library(ac3_fmt INTERFACE)
add_library(ac3::fmt ALIAS ac3_fmt)
target_link_libraries(ac3_fmt INTERFACE fmt::fmt)

# vcpkg's fmtConfig.cmake does not mark its own include directories SYSTEM,
# so without this, warnings inside fmt's own headers get promoted to errors
# by ac3::warnings' /W4 /WX (see cmake/CompilerWarnings.cmake) the moment any
# first-party target links ac3::fmt - the same class of problem the SYSTEM
# keyword above already heads off for the FetchContent fallback path (CMake's
# FetchContent SYSTEM support marks that path automatically; find_package()
# has no equivalent, hence this explicit re-marking here).
get_target_property(AC3FORGE_FMT_INCLUDE_DIRS fmt::fmt INTERFACE_INCLUDE_DIRECTORIES)
if(AC3FORGE_FMT_INCLUDE_DIRS)
    target_include_directories(ac3_fmt SYSTEM INTERFACE ${AC3FORGE_FMT_INCLUDE_DIRS})
endif()

# fmt/base.h trips MSVC's C4702 (unreachable code) on this toolset even
# though its headers are genuinely external (vcpkg's own toolchain already
# marks its whole installed include tree -external:W0, on top of the SYSTEM
# marking just above) - the same "third-party header code trips a warning
# our SYSTEM-headers rule does not actually save us from" situation
# cmake/CompilerWarnings.cmake's own AC3_WARNINGS_OFF_FLAG comment already
# documents for Qt's generated qmlcachegen code, and the identical scoped
# fix: disable only this one diagnostic, only for MSVC, wherever ac3::fmt is
# linked, rather than weakening /W4 for anything else.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(ac3_fmt INTERFACE /wd4702)
endif()
