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
#
# A second target, ac3::fmt_private, is defined at the end of this file. The
# libraries installed as static archives (ac3::forge, mp4::mp4) link it in place
# of ac3::fmt.
# ---------------------------------------------------------------------------

# Matches packaging/vcpkg-port/ac3forge/vcpkg.json's own fmt dependency and
# packaging/conan/conanfile.py's pinned requirement, so all three routes
# build against the same code. fmt's own git tags carry no "v" prefix
# (unlike Catch2's), hence GIT_TAG "${AC3FORGE_FMT_VERSION}" below, not
# "v${AC3FORGE_FMT_VERSION}".
set(AC3FORGE_FMT_VERSION 12.2.0)

# The oldest {fmt} a local copy may be. fmt/base.h, which cpu_features.cpp and
# most of apps/cli include, first shipped in 11.0.0, but no 11.0.x release
# builds this tree with Clang 22: apps/common/fmp4_folder_writer.cpp formats a
# system_clock time_point with "{:%FT%TZ}", and write_floating_seconds() in
# 11.0.x's fmt/chrono.h then fails with "call to consteval function
# 'fmt::basic_format_string<...>' ... is not a constant expression". 11.1.0 is
# the first release the whole tree compiles against.
set(AC3FORGE_FMT_MINIMUM_VERSION 11.1.0)

option(AC3FORGE_FETCH_FMT "Fetch {fmt} from source via FetchContent when no local copy is found" ON)

# Without a version, find_package() takes whatever {fmt} it finds, and an older
# one fails the build at the first #include <fmt/base.h> instead of here.
# Ubuntu 26.04's libfmt-dev is 10.1.1 (its CMake package reports 10.1.0); a
# fuzz/run.sh configure on a machine with it installed picked it up and stopped
# compiling src/forge/src/internal/cpu/cpu_features.cpp. With the minimum,
# find_package() passes over a copy like that, including one an existing build
# directory has already cached in fmt_DIR, and the fallback below applies.
#
# A minimum still accepts newer major versions. fmt writes its version file with
# AnyNewerVersion compatibility, vcpkg's port installs fmt's own, and
# ConanCenter's recipe sets cmake_config_version_compat to AnyNewerVersion over
# CMakeDeps' SameMajorVersion default, so 12.2.0 satisfies it by every route.
find_package(fmt ${AC3FORGE_FMT_MINIMUM_VERSION} CONFIG QUIET)

if(NOT fmt_FOUND)
    # find_package() lists each copy it turned down; naming them keeps a machine
    # with an old libfmt-dev installed from being told only "not found". The
    # same file can be listed more than once - through a cached fmt_DIR, and
    # through a merged-/usr distro's /lib -> /usr/lib symlink - hence REAL_PATH
    # and REMOVE_DUPLICATES.
    set(_ac3_fmt_skipped "")
    foreach(_ac3_fmt_config _ac3_fmt_version IN ZIP_LISTS fmt_CONSIDERED_CONFIGS fmt_CONSIDERED_VERSIONS)
        file(REAL_PATH "${_ac3_fmt_config}" _ac3_fmt_real_config)
        list(APPEND _ac3_fmt_skipped "\n  skipped {fmt} ${_ac3_fmt_version} (${_ac3_fmt_real_config})")
    endforeach()
    list(REMOVE_DUPLICATES _ac3_fmt_skipped)
    list(JOIN _ac3_fmt_skipped "" _ac3_fmt_skipped)
    unset(_ac3_fmt_real_config)

    if(NOT AC3FORGE_FETCH_FMT)
        message(FATAL_ERROR
            "{fmt} ${AC3FORGE_FMT_MINIMUM_VERSION} or newer was not found and AC3FORGE_FETCH_FMT is OFF."
            "${_ac3_fmt_skipped}\n"
            "Supply it with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake "
            "or -DCMAKE_PREFIX_PATH=<prefix>, or allow the download by setting "
            "AC3FORGE_FETCH_FMT=ON.")
    endif()

    message(STATUS
        "{fmt} ${AC3FORGE_FMT_MINIMUM_VERSION} or newer not found locally; fetching v${AC3FORGE_FMT_VERSION} "
        "(-DAC3FORGE_FETCH_FMT=OFF to require a local copy)${_ac3_fmt_skipped}")
    unset(_ac3_fmt_skipped)

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

# ac3::fmt_private - a private copy of {fmt}, compiled into the object files of a library that is
# installed as a static archive: ac3::forge (src/forge/CMakeLists.txt) and mp4::mp4
# (src/mp4/CMakeLists.txt).
#
# An archive is not linked. Each function its objects call without defining stays an undefined
# reference until a consumer's own link, and the installed package names no {fmt} for that link to
# find. Linking ac3::fmt privately, as those two libraries did, therefore left an installed
# libac3forge_static.a and libmp4_static.a with undefined fmt::v12::vformat and fmt::v12::vprint
# references, and a program linking ac3::forge_c_static from `cmake --install`'s output stopped at
# "undefined reference to fmt::v12::vprint". A shared library takes {fmt} in at its own link, so
# only the static variants showed it.
#
# Declaring the dependency on the exported targets instead (find_dependency(fmt) plus a link item)
# does not hold for every way this file finds {fmt}. The FetchContent copy is an ordinary target
# that is in no export set, so install(EXPORT) refuses it and the configure step fails. The {fmt}
# a consumer finds also has to be this build's major version, which is part of every symbol name
# (fmt::v12::), so a consumer whose only copy is another major version, such as Ubuntu 26.04's
# libfmt-dev 10.1.1, still ends at an undefined reference.
#
# FMT_HEADER_ONLY makes {fmt}'s definitions part of each including translation unit, so the archive
# carries them and no fmt:: symbol is left undefined. $<COMPILE_ONLY:> keeps what ac3::fmt supplies
# for compiling (include path, SYSTEM marking, /wd4702) and drops the link: no {fmt} library
# reaches a link line, and a shared {fmt} adds no NEEDED entry to libac3forge.so.
#
# The definitions go in their own inline namespace, fmt::ac3_private, through FMT_BEGIN_NAMESPACE
# and FMT_END_NAMESPACE, the hooks {fmt} provides for embedding it in a library. Without that the
# archive holds weak definitions of fmt::v12::vprint and vformat, and a weak definition in an
# archive member satisfies every other object's reference to the same name: ac3tests, which
# compiles cpu_features.cpp a second time, had the archive's cpu_features.cpp.o pulled in for
# fmt::v12::vprint and stopped at a duplicate ac3::internal::cpu::has_avx2, in the static build and
# in the BUILD_SHARED_LIBS=ON pass alike. A member pulled in without a clash would put a copy of
# this library's code in a test binary that is meant to run against libac3forge.so. The private
# copy answers to this library's own code alone, and a consumer's {fmt}, of any version, never
# binds to it.
#
# Every translation unit of these libraries gets the definitions, whichever fmt header it
# includes: with FMT_HEADER_ONLY, fmt/base.h ends by including fmt/format.h (11.1.0 through
# 12.2.0). A library that goes back to linking ac3::fmt privately is what
# tools/checks/check_install_consumer.sh finds, by linking every installed archive whole.
# Applications, tests and examples keep linking ac3::fmt: they are executables, so no later link
# is left to resolve anything. The FMT_*_NAMESPACE definitions contain spaces and braces, which
# the Ninja generator (Linux and Windows) and the Unix Makefiles generator (Linux) quote
# correctly; every preset here uses Ninja, and no other generator has been tried.
add_library(ac3_fmt_private INTERFACE)
add_library(ac3::fmt_private ALIAS ac3_fmt_private)
target_link_libraries(ac3_fmt_private INTERFACE "$<COMPILE_ONLY:ac3::fmt>")
target_compile_definitions(ac3_fmt_private INTERFACE
    FMT_HEADER_ONLY=1
    "FMT_BEGIN_NAMESPACE=namespace fmt { inline namespace ac3_private {"
    "FMT_END_NAMESPACE=} }")
