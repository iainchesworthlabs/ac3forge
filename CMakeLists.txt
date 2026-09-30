cmake_minimum_required(VERSION 3.28...4.3)

# Derive the version from the nearest reachable v* git tag before project() -
# see the file for the fallback/override rules. This makes a pushed tag the
# single source of truth: nothing in this tree needs bumping by hand anymore.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitVersionDerivation.cmake")

project(iclforge
    VERSION ${DERIVED_VERSION_SEMVER}
    DESCRIPTION "Clean-room AC-3 (ATSC A/52), E-AC-3 and AC-4 encoder and decoder with a spatial object layer"
    HOMEPAGE_URL "https://github.com/iainchesworthlabs/ac3forge"
    LANGUAGES CXX)

# CMake's project() VERSION field can only hold M.M.P, so a tag's prerelease
# suffix (e.g. the "-beta.1" in v0.2.0-beta.1) is carried separately here.
set(PROJECT_VERSION_PRERELEASE "${DERIVED_VERSION_PRERELEASE}" CACHE STRING "Prerelease label (e.g. beta.2)" FORCE)
set(PROJECT_VERSION_FULL "${DERIVED_VERSION_FULL}" CACHE STRING "Full semver version (M.M.P[-prerelease])" FORCE)

# vcpkg.json's "version" is a fixed placeholder, deliberately never kept in
# sync with PROJECT_VERSION: it is only read by vcpkg's manifest resolver
# before any CMake configure step runs, and this project is never published
# to a vcpkg registry, so the field has no real meaning to synchronise.
# Matches R:\aqualink-automate, whose vcpkg.json version sits equally static.

# ---------------------------------------------------------------------------
# Global language settings: C++23 only, no compiler extensions.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# No source in this tree is a C++20 module, but CMake 3.28+ scans every C++20-or-later target
# for them under Ninja and the Visual Studio generators. The scan is not free: each changed file
# re-collates its target's module map, and a re-collated map marks every object in the target
# dirty, so editing one test file recompiled all of ac3tests (measured: 224 objects, 3m17s on
# 4 cores, instead of one object and a link). Turn it back on here if a module is ever added.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# One place for runtime artefacts so the Qt app finds its plugins and the CLI
# and tests sit beside it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# Empty unless -DICLFORGE_SANITIZERS=<comma-list> is set (see the *-asan-ubsan
# presets in CMakePresets.json); a plain configure is unaffected.
set(ICLFORGE_SANITIZERS "" CACHE STRING "Comma-separated -fsanitize= value, e.g. address,undefined")
include(Sanitizers)

# iclforge::coverage, off unless -DICLFORGE_ENABLE_COVERAGE=ON (see the
# linux-gcc-coverage preset in CMakePresets.json); a plain configure is unaffected.
include(Coverage)

# iclforge::tracy, off unless -DICLFORGE_ENABLE_TRACY=ON (and vcpkg's "profiling"
# manifest feature resolved - see vcpkg.json); a plain configure is unaffected.
include(Tracy)

# iclforge::fmt used to be resolved here, beside Tracy. It is now further down,
# after ICLFORGE_MINIMAL_DECODER is declared, because the minimum-footprint
# profile must not resolve it at all - see the include(Fmt) below.

# Sources carry spec citations with non-ASCII glyphs (section marks, degrees,
# arrows). Tell MSVC both the source and execution charsets are UTF-8 so it
# neither mis-decodes them nor warns; harmless everywhere else.
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# ---------------------------------------------------------------------------
# Floating-point contraction: OFF, everywhere, deliberately.
#
# GCC and Clang both let the optimiser fuse `a * b + c` into a single
# fused-multiply-add by default (-ffp-contract=fast for GCC, =on for Clang),
# which keeps ONE EXTRA rounding step's worth of precision compared with
# evaluating the multiply and the add separately. That is a perfectly
# reasonable default for numerical code and the wrong one here, for three
# reasons that all point the same way:
#
#  - It is architecture-dependent. FMA is a base ARMv8-A instruction, so
#    every arm64 and Apple-silicon leg contracts; x86-64 has no FMA below
#    AVX2 and this project passes no -march=, so no x86 leg does. The same
#    source therefore computes different numbers on different legs purely
#    because of what the ISA happens to offer - and the gold-reference gate
#    has been recording exactly that: `linux-gcc-arm64`, `linux-llvm-arm64`
#    and `macos-llvm` score ~61.8 dB where every x86 leg scores ~67.8 dB, a
#    gap of 6.02 dB, which is not a vague "floating-point differences"
#    number but precisely one AC-3 exponent step (§7.2.2.2's PSD units are
#    128 per exponent, one exponent is 6.02 dB). A last-bit difference in an
#    MDCT coefficient that straddles a power of two moves
#    exponent_from_fixed()'s leading-zero count by one, and a whole band's
#    allocation with it. See cross-platform bitstream reproducibility policy for the half this option pins.
#  - It is invisible. Nothing in the source says a contraction happened, so
#    the difference cannot be reviewed, only measured after the fact.
#  - It would break the SIMD seam's correctness argument outright.
#    src/arithmetic/arch/*/ac3/internal/arch/simd.hpp maps every
#    operation to exactly one IEEE-754 add/subtract/multiply so that a
#    vectorised kernel is bit-identical to the scalar loop it replaced. That
#    holds only if the scalar loop is not silently getting a fused form the
#    intrinsics cannot express (and NEON's vmulq_f64 + vsubq_f64 pair CAN be
#    re-fused by the compiler, so "use intrinsics" is not by itself a
#    defence).
#
# Cost, measured: none on x86-64, where the flag is a no-op because baseline
# x86-64 has no FMA instruction to emit. On aarch64 it gives up FMLA in the
# transform inner loops, and buys agreement with every other leg for it -
# see docs/building.md's "Floating-point contraction".
#
# MSVC has no -ffp-contract; /fp:precise is its equivalent statement of
# intent, and is also already its default. It is spelled out so the policy is
# stated on every compiler rather than three of four. Like the x86-64 case
# above it is a no-op in practice today: without an /arch: switch MSVC
# targets baseline SSE2, which has no FMA either.
#
# clang-cl needs the /clang: escape. It reports CXX_COMPILER_ID "Clang" like
# upstream Clang but parses MSVC-style arguments, and a bare
# -ffp-contract=off there is not an error - it is a WARNING ("unknown
# argument ignored in clang-cl"), which -Werror then turns into a failed
# build on the windows-llvm leg while quietly meaning the policy was never
# applied at all. Same split as cmake/CompilerWarnings.cmake's, and resolved
# the same way: at configure time, where CMAKE_CXX_COMPILER_FRONTEND_VARIANT
# is available, rather than in a generator expression that cannot see it.
#
# Project-wide via add_compile_options rather than on iclforge::ac3 alone: the
# benchmarks and the tests' own scalar reference kernels have to be compiled
# under the same rules as the code they measure and check.
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(AC3_FP_CONTRACT_FLAG "/fp:precise")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
       CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(AC3_FP_CONTRACT_FLAG "/clang:-ffp-contract=off")
else()
    set(AC3_FP_CONTRACT_FLAG "-ffp-contract=off")
endif()
add_compile_options("${AC3_FP_CONTRACT_FLAG}")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(ICLFORGE_BUILD_CLI "Build the ac3cli command-line application" ON)
option(ICLFORGE_BUILD_GUI "Build the Qt6 Quick GUI application" ON)
option(ICLFORGE_BUILD_CRUCIBLE "Build AC3Forge Crucible (Windows, Linux and macOS)" OFF)
option(ICLFORGE_BUILD_TESTS "Build the unit test suite" ON)
# Catch2 is the only build-time dependency. Turn this off to insist it comes
# from a package manager instead of the network (see tests/CMakeLists.txt).
option(ICLFORGE_FETCH_CATCH2 "Download Catch2 when no local copy is found" ON)
option(ICLFORGE_BUILD_EXAMPLES "Build the documented library examples" ON)
# libFuzzer harnesses over iclforge::ac3's untrusted-input entry points (see
# fuzz/). Off by default: it needs upstream Clang (GCC and MSVC do not ship
# libFuzzer) and instruments iclforge itself with ASan+UBSan, which no other
# configuration wants paid for. Use fuzz/run.sh rather than this option
# directly - it configures a dedicated build/fuzz directory with the right
# compiler.
option(ICLFORGE_BUILD_FUZZERS "Build libFuzzer harnesses (Clang only; see fuzz/)" OFF)
# Every other library component (iclforge::matroska, iclforge::mp4, iclforge::mpegts, and the AC-4,
# IAB and IAMF libraries below) gets its own ICLFORGE_BUILD_<NAME> option, defaulting ON so
# nothing changes for today's direct/CPack builds. This is what lets the vcpkg port and the Conan
# recipe (packaging/) expose each as an opt-in feature or option, off unless asked for - see
# cmake/InstallLibrary.cmake's matching install()/export() gate.
option(ICLFORGE_BUILD_MATROSKA "Build the standalone Matroska container writer (src/matroska)" ON)
option(ICLFORGE_BUILD_MP4 "Build the standalone MP4/ISOBMFF container writer (src/mp4)" ON)
option(ICLFORGE_BUILD_MPEGTS "Build the standalone MPEG-TS container writer (src/mpegts)" ON)
# AC-4 bitstream inspector: the AC-4 sync-frame/TOC/presentation/substream-group bitstream
# inspector (src/ac4). Same "own option, defaults ON" treatment as the three
# above - apps/cli's `probe` command links it unconditionally.
option(ICLFORGE_BUILD_AC4 "Build the standalone AC-4 bitstream inspector, decoder and encoder (src/ac4, src/ac4core, src/ac4dec, src/ac4enc)" ON)

# iclforge::iab (src/iab) - IAB reader, phase 1: a standalone reader for SMPTE
# ST 2098-2's Immersive Audio Bitstream. Like the three container writers above, it needs no
# opt-in third-party library (unlike ac3adm, see its own option() below), so it defaults ON the
# same way; unlike them, nothing in apps/cli, apps/gui or examples/ links it yet (IAB reader's
# phase 3 is what will eventually give it a consumer), so there is no matching FATAL_ERROR
# guard here - tests/CMakeLists.txt only adds ac3iab's own test file and links iclforge::iab
# when this option is on, the same conditional shape it already uses for iclforge::adm/
# ICLFORGE_BUILD_ADM.
option(ICLFORGE_BUILD_IAB "Build the standalone Immersive Audio Bitstream reader (src/iab)" ON)

# iclforge::iamf (src/iamf) - IAMF writer, phase 1: a standalone OBU/ISOBMFF writer for AOM's
# IAMF (Immersive Audio Model and Formats) v1.1.0, emitting `ipcm` substreams for a channel-based
# 7.1.4 element. Same zero-third-party-dependency shape as ac3iab immediately above (unlike
# ac3adm, see its own option() below), so it defaults ON the same way and for the same reason;
# also like ac3iab, nothing in apps/cli, apps/gui or examples/ links it unconditionally yet (only
# examples/mux_iamf.cpp does, guarded on this option) - see src/iamf/CMakeLists.txt's own header
# comment.
option(ICLFORGE_BUILD_IAMF "Build the standalone IAMF OBU/ISOBMFF writer (src/iamf)" ON)

# The minimum-footprint decoder profile. Not a "build X too"
# option like the ones above - it REPLACES what src/ac3 builds with a single
# decode-only static library for a target with a few hundred kilobytes of RAM
# and no operating system (see cmake/MinimalDecoder.cmake and
# docs/building.md). Off by default, and the guard below refuses it alongside
# anything that needs the full library, so an ordinary configure is unaffected
# and a half-configured one fails here rather than deep inside a subdirectory.
option(ICLFORGE_MINIMAL_DECODER
    "Build ONLY the decode-only, exception-free, table-trimmed iclforge::ac3_minimal (minimum-footprint decoder profile)"
    OFF)

# The same profile pointed the other way: encode only, no decoder.
#
# Mutually exclusive with the decoder above, and that is a measurement rather
# than a simplification. On the ESP32-S3, which has 277,400 bytes of internal
# SRAM free once the image is placed, the three shapes peak at:
#
#   decode (including Atmos objects)   233,546
#   AC-3 encode                        201,770
#   E-AC-3 encode                      243,770
#
# Each fits alone. No two fit at once - the two encoders together reach 440,420,
# and decode alongside E-AC-3 encode would be worse. A build that offered both
# would be offering something the part cannot run, so the option refuses it
# rather than letting the failure arrive as out_of_memory on a device.
#
# Sequential use is a different question and is fine: tear one down, build the
# other. It is holding both at once that does not fit.
option(ICLFORGE_MINIMAL_ENCODER
    "Build ONLY the encode-only, exception-free iclforge::ac3_minimal (minimum-footprint encoder profile)"
    OFF)

if(ICLFORGE_MINIMAL_DECODER AND ICLFORGE_MINIMAL_ENCODER)
    message(FATAL_ERROR
        "ICLFORGE_MINIMAL_DECODER and ICLFORGE_MINIMAL_ENCODER are mutually exclusive - see "
        "the measured peaks beside the option above. Pick one; a target that needs both in "
        "sequence can build one library and rebuild, but neither part this profile serves can "
        "hold both at once.")
endif()

# The AC-4 decoder inside the same profile (planning/ac4.md, D14b): the
# inspector (src/ac4), the core (src/ac4core) and the decoder (src/ac4dec) as
# static archives built with the profile's own compile options, and no encoder -
# the encoder is built for no ESP32 part (decision 34). It is an option of its
# own rather than a use of ICLFORGE_BUILD_AC4, which the guard below refuses in
# the profile because it also builds src/ac4enc, the applications and the
# tests. Off by default, and when off the profile builds exactly what it did
# before: esp-idf/iclforge/CMakeLists.txt sets it from CONFIG_ICLFORGE_AC4.
option(ICLFORGE_MINIMAL_AC4
    "Minimum-footprint decoder profile only: also build the AC-4 inspector, core and decoder (no encoder)"
    OFF)
if(ICLFORGE_MINIMAL_AC4 AND NOT ICLFORGE_MINIMAL_DECODER)
    message(FATAL_ERROR
        "ICLFORGE_MINIMAL_AC4 adds the AC-4 decoder to the minimum-footprint decoder profile, "
        "and needs ICLFORGE_MINIMAL_DECODER=ON (a full build has ICLFORGE_BUILD_AC4).")
endif()

# Where a frame's time goes, on a target that cannot run Tracy. Routes the
# library's ICLFORGE_ZONE_SCOPED_N() markers - the same ones ICLFORGE_ENABLE_TRACY
# turns into Tracy zones - to two functions the APPLICATION supplies (see
# src/base/variants/profiling-stage_timers/), so a bare-metal probe can
# report per-stage microseconds beside its per-frame ones. Minimum-footprint
# profile only: it is that profile's targets that have no other profiler, and
# apps/baremetal/stage_timers.cpp is the one implementation of the two
# functions. Off by default, and when off the markers compile to nothing
# exactly as before, so a timed build and a plain one differ only in the
# library's include path.
option(ICLFORGE_STAGE_TIMERS
    "Minimum-footprint profile only: route zone markers to an application-supplied stage timer"
    OFF)
if(ICLFORGE_STAGE_TIMERS AND NOT (ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER))
    message(FATAL_ERROR
        "ICLFORGE_STAGE_TIMERS is only meaningful with ICLFORGE_MINIMAL_DECODER or "
        "ICLFORGE_MINIMAL_ENCODER - the ordinary library has Tracy (ICLFORGE_ENABLE_TRACY) for "
        "this, and nothing in it defines the two functions the stage-timer backend calls.")
endif()

# apps/cli, apps/gui, tests/ and examples/ all link iclforge::matroska, iclforge::mp4 and
# iclforge::mpegts unconditionally - none of them are written to cope with any of the three being
# absent. ICLFORGE_BUILD_MATROSKA=OFF, ICLFORGE_BUILD_MP4=OFF or ICLFORGE_BUILD_MPEGTS=OFF only
# make sense with all four also OFF (this is the shape a vcpkg or Conan build without the
# matching feature or option actually uses, see cmake/InstallLibrary.cmake and packaging/);
# catch the unsupported combination here with a clear message
# rather than letting it fail deep inside one of those subdirectories with a "target not found"
# error.
if(NOT ICLFORGE_BUILD_MATROSKA AND (ICLFORGE_BUILD_CLI OR ICLFORGE_BUILD_GUI OR ICLFORGE_BUILD_TESTS OR ICLFORGE_BUILD_EXAMPLES))
    message(FATAL_ERROR "ICLFORGE_BUILD_MATROSKA=OFF requires ICLFORGE_BUILD_CLI, "
        "ICLFORGE_BUILD_GUI, ICLFORGE_BUILD_TESTS and ICLFORGE_BUILD_EXAMPLES to all be OFF too - "
        "they all link iclforge::matroska unconditionally.")
endif()
if(NOT ICLFORGE_BUILD_MP4 AND (ICLFORGE_BUILD_CLI OR ICLFORGE_BUILD_GUI OR ICLFORGE_BUILD_TESTS OR ICLFORGE_BUILD_EXAMPLES))
    message(FATAL_ERROR "ICLFORGE_BUILD_MP4=OFF requires ICLFORGE_BUILD_CLI, "
        "ICLFORGE_BUILD_GUI, ICLFORGE_BUILD_TESTS and ICLFORGE_BUILD_EXAMPLES to all be OFF too - "
        "they all link iclforge::mp4 unconditionally.")
endif()
if(NOT ICLFORGE_BUILD_MPEGTS AND (ICLFORGE_BUILD_CLI OR ICLFORGE_BUILD_GUI OR
                                  ICLFORGE_BUILD_TESTS OR ICLFORGE_BUILD_EXAMPLES))
    message(FATAL_ERROR "ICLFORGE_BUILD_MPEGTS=OFF requires ICLFORGE_BUILD_CLI, "
        "ICLFORGE_BUILD_GUI, ICLFORGE_BUILD_TESTS and ICLFORGE_BUILD_EXAMPLES to all be OFF too - "
        "they all link iclforge::mpegts unconditionally.")
endif()
if(NOT ICLFORGE_BUILD_AC4 AND (ICLFORGE_BUILD_CLI OR ICLFORGE_BUILD_GUI OR ICLFORGE_BUILD_TESTS))
    message(FATAL_ERROR "ICLFORGE_BUILD_AC4=OFF requires ICLFORGE_BUILD_CLI, "
        "ICLFORGE_BUILD_GUI and ICLFORGE_BUILD_TESTS to all be OFF too - they all link iclforge::ac4 "
        "unconditionally.")
endif()
# Hearth's applications, which a board's build leaves out: a board takes src/sendspin's player
# half alone (ICLFORGE_SENDSPIN_PLAYER_ONLY, set by esp-idf/iclforge/CMakeLists.txt, with the
# minimum-footprint profile and no AC-4), and the two guards below are about the applications.
if(ICLFORGE_BUILD_HEARTH AND NOT ICLFORGE_SENDSPIN_PLAYER_ONLY)
    set(_ac3_hearth_apps ON)
else()
    set(_ac3_hearth_apps OFF)
endif()
if(NOT ICLFORGE_BUILD_AC4 AND _ac3_hearth_apps)
    message(FATAL_ERROR "ICLFORGE_BUILD_AC4=OFF requires ICLFORGE_BUILD_HEARTH to be OFF too, "
        "unless ICLFORGE_SENDSPIN_PLAYER_ONLY is set - Hearth's engine reads an AC-4 item's table "
        "of contents with iclforge::ac4, and its presentations and metadata with iclforge::ac4dec.")
endif()

# ICLFORGE_MINIMAL_DECODER's own version of the three guards above. Everything listed here
# either links the full iclforge::ac3 (CLI, GUI, tests, examples, the C API) or is a module the
# profile deliberately leaves out (the container writers, the ADM reader); none of them build
# against a decode-only archive, and several would not even configure for a bare-metal target.
# Name every offender at once rather than failing on the first, so a caller assembling the
# option set gets one message instead of six configures.
if(ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER)
    set(_ac3_minimal_conflicts "")
    foreach(_opt IN ITEMS ICLFORGE_BUILD_CLI ICLFORGE_BUILD_GUI ICLFORGE_BUILD_TESTS
                          ICLFORGE_BUILD_EXAMPLES ICLFORGE_BUILD_MATROSKA ICLFORGE_BUILD_MP4
                          ICLFORGE_BUILD_MPEGTS ICLFORGE_BUILD_IAB ICLFORGE_BUILD_IAMF
                          ICLFORGE_BUILD_CAPI ICLFORGE_BUILD_ADM ICLFORGE_BUILD_PYTHON
                          ICLFORGE_BUILD_FUZZERS ICLFORGE_BUILD_AC4)
        if(${_opt})
            list(APPEND _ac3_minimal_conflicts "${_opt}")
        endif()
    endforeach()
    # Hearth's applications link the full iclforge::ac3; a board's Sendspin player half does not.
    if(_ac3_hearth_apps)
        list(APPEND _ac3_minimal_conflicts "ICLFORGE_BUILD_HEARTH")
    endif()
    if(_ac3_minimal_conflicts)
        list(JOIN _ac3_minimal_conflicts "=OFF, " _ac3_minimal_message)
        message(FATAL_ERROR
            "The minimum-footprint profile needs ${_ac3_minimal_message}=OFF - none of them build "
            "against a decode-only or encode-only archive. The config-*-minimal presets in CMakePresets.json "
            "already set them; see docs/building.md's minimum-footprint decoder profile.")
    endif()
    unset(_ac3_minimal_conflicts)
    unset(_ac3_minimal_message)
endif()
unset(_ac3_hearth_apps)

# iclforge::minimal_profile - the compile/link options the profile above adds. Not
# beside include(Sanitizers)/include(Coverage)/include(Tracy) up top because,
# unlike those three, this one is driven by an option() declared in this
# section rather than by a bare cache variable, and a module cannot read an
# option that has not been declared yet. No-op when the profile is off.
include(MinimalDecoder)

# iclforge::fmt - {fmt}, used everywhere instead of std::format/std::print (see
# cmake/Fmt.cmake). A base dependency for the ordinary build: always resolved,
# never opt-in, unlike Tracy above.
#
# NOT for the minimum-footprint profile, which links it nowhere: no source in
# src/ac3/minimal.cmake's list mentions fmt, and neither iclforge_ac3_minimal nor
# apps/baremetal's probe lists iclforge::fmt among its libraries. The profile is in
# fact built to avoid it - src/internal/cpu/minimal/cpu_features.cpp exists as
# a whole separate translation unit rather than a branch precisely because the
# shared one reports through fmt, "which this profile does not carry and whose
# formatted-output machinery it cannot afford" (that file's own header).
#
# Resolving it anyway was not free. Neither config-arm-none-eabi-minimal nor
# the ESP-IDF build wires vcpkg in, so find_package(fmt CONFIG QUIET) missed
# and cmake/Fmt.cmake's FetchContent fallback cloned fmt from GitHub at
# CONFIGURE time - a network dependency, on every fresh configure, for a
# library neither build links. Confirmed on the first ESP32-S3 configure,
# which produced a build/_deps/fmt-build for nothing.
#
# This sits here rather than beside Tracy for the same reason
# include(MinimalDecoder) does, stated directly above: the option it reads is
# declared in this section, and a module cannot read an option that does not
# exist yet. Everything that consumes iclforge::fmt is added by add_subdirectory()
# further down, so resolving it here is still early enough.
if(NOT ICLFORGE_MINIMAL_DECODER AND NOT ICLFORGE_MINIMAL_ENCODER)
    include(Fmt)
endif()

# iclforge::adm (src/adm) is a READER, not a writer like the three container modules above -
# ADM BWF reader, phase 1, parsing
# BW64/RF64 + the Audio Definition Model rather than muxing a container. Unlike
# matroska/mp4/mpegts, it defaults OFF rather than ON and is genuinely additive: nothing in
# apps/cli, apps/gui, tests/ or examples/ links it unconditionally, so every one of those builds
# identically whether this is on or off. The reason for the different default is its
# dependency footprint - see src/adm/CMakeLists.txt's own header comment - libadm needs
# several Boost header libraries, resolved through a dedicated opt-in vcpkg feature
# (-DVCPKG_MANIFEST_FEATURES=adm, see vcpkg.json) that a normal build never pulls in.
option(ICLFORGE_BUILD_ADM "Build the standalone BW64/RF64 + ADM parser (src/adm) - needs Boost, see vcpkg.json's \"adm\" feature" OFF)

# iclforge_c (src/capi) - the C API over iclforge::ac3's encode/decode core. Depends
# on nothing but iclforge::ac3_static (see src/capi/CMakeLists.txt's own header comment on why it
# always embeds the static codec rather than tracking BUILD_SHARED_LIBS), so - unlike
# ICLFORGE_BUILD_ADM - there is no extra dependency footprint to opt out of; default ON matches
# matroska/mp4/mpegts above, not ADM.
option(ICLFORGE_BUILD_CAPI "Build the C API over the encode/decode core (src/capi)" ON)

# Python on PyPI: the pybind11 extension module (python/) - off by default like ICLFORGE_BUILD_ADM
# above, and for the same reason: nothing under src/, apps/cli, apps/gui, tests/ or examples/ links
# it or is affected by it either way. A normal C++ build (and every existing CI leg) is unchanged
# with this OFF. Turned ON by python/pyproject.toml's own cmake.define when scikit-build-core
# drives the configure for `pip install`/cibuildwheel - see python/CMakeLists.txt.
option(ICLFORGE_BUILD_PYTHON "Build the pybind11 Python extension module (python/)" OFF)

# Hearth, the desktop reference player (planning/hearth-reference-player.md): src/sendspin and
# apps/hearth (the engine, the window, the test sink and server). ON by default: every leg of
# build-and-test's own matrix (_build.yml) has built and tested it since A7, so a plain local
# configure now matches what CI already proves rather than needing -DICLFORGE_BUILD_HEARTH=ON by
# hand to see the same targets. Its dependencies (cpp-httplib, mdns, mbedTLS, libFLAC, Opus) still
# come through vcpkg's own opt-in "hearth" feature rather than the base manifest - CMakePresets.json's
# "core" fragment selects it by default for every hosted preset, the same way this option now
# defaults ON, but the minimum-footprint decoder profile, the Emscripten/WASM demo and the Windows
# Crucible-coverage leg turn BOTH back off explicitly (see their own entries in CMakePresets.json)
# because none of the three can build Hearth at all - no OS for the minimum-footprint profile, no
# vcpkg toolchain under Emscripten, and a deliberately Crucible-only scope for that coverage leg.
# "Every CI leg" above means build-and-test's matrix specifically, not every job in the repo's CI:
# _ci-core.yml's own standalone jobs are not part of that matrix and do not all request the
# "hearth" vcpkg feature, so a leg that narrows VCPKG_MANIFEST_FEATURES away from it (adm-validate's
# own "adm"-only feature list is the one example found so far) needs this same explicit OFF or its
# plain add_subdirectory(src/sendspin) below fails resolving mbedTLS/cpp-httplib/libFLAC/Opus/mdns
# that were never installed - found the hard way once this option's default flipped to ON.
option(ICLFORGE_BUILD_HEARTH "Build Hearth: src/sendspin and apps/hearth - see vcpkg.json's \"hearth\" feature, selected by default via CMakePresets.json's core fragment" ON)

# enable_testing() has to run before ANY add_subdirectory() that might contain
# an add_test() call, not merely before tests/ itself - CTest silently drops
# an add_test() registered from a directory processed earlier in the same
# configure pass (confirmed the hard way: apps/gui/tests/'s ac3gui_qml_tests_*
# entries were invisible to `ctest -N` until this moved here). Calling it unconditionally
# is harmless when ICLFORGE_BUILD_TESTS is OFF - nothing calls add_test() then.
if(ICLFORGE_BUILD_TESTS)
    enable_testing()
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
# The scalar building blocks iclforge::ac3 and src/ac4core (below) both use -
# Fixed32 and the cross-platform-identical float transcendentals
# (planning/ac4.md, decision 31). Header-only and dependency-free, so it goes
# first; both consumers link it PRIVATE.
include(IclforgeLibrary)
add_subdirectory(src/arithmetic)

# The codec-blind libraries the AC-3 codec builds on, each linking only the ones above it:
# base (bit I/O, the speaker vocabulary, the kernels' build variants), dsp, objects (the object model
# and the OAMD payload), render and iec61937. Then the codec, which links all of them. None of the
# five links a codec; AC-4 (src/ac4*) links none of them yet.
add_subdirectory(src/base)
add_subdirectory(src/dsp)
add_subdirectory(src/objects)
add_subdirectory(src/render)
add_subdirectory(src/iec61937)
add_subdirectory(src/ac3)

# Live capture, monitor playback and IEC 61937 passthrough - the only
# platform-specific code in the project. After src/ac3 (PRIVATE-links
# iclforge::ac3 for the IEC 61937 framer); before apps/cli/apps/gui/tests, which
# link it alongside iclforge::ac3 for their live-audio commands/features. Not
# linked by examples/ - none of them touch live audio.
#
# Skipped entirely under Emscripten: src/audio/CMakeLists.txt hard-fails
# configure when none of WIN32/LINUX/APPLE/ANDROID is set (it has no browser
# platform directory, and never will - a WASM build gets live audio from the
# Web Audio API in JS instead, not from this library), and nothing a WASM
# configure builds needs it anyway (ICLFORGE_BUILD_CLI/GUI/TESTS are all OFF
# there, and examples/wasm_decode_demo does not link iclforge::audio).
#
# Skipped under ICLFORGE_MINIMAL_DECODER for a different reason than under
# Emscripten but with the same effect: a bare-metal target has no audio
# device, no backend directory would match it, and the profile's whole point
# is that nothing outside the decode path is compiled.
if(NOT EMSCRIPTEN AND NOT ICLFORGE_MINIMAL_DECODER AND NOT ICLFORGE_MINIMAL_ENCODER)
    add_subdirectory(src/audio)
endif()

# The EMDF Atmos object signer (iclforge::signing). Clean-room and dependency-free:
# the HMAC construction and authenticated-region layout are in-tree, and the
# only secret - the key - is supplied by the operator at runtime, never
# embedded, so unlike the old quarantine overlay this is committed and built by
# default. After src/ac3 so it can reuse the codec's parsing primitives; before
# apps/cli so ac3cli can link it.
# Not under ICLFORGE_MINIMAL_DECODER: object SIGNING is a production-side
# concern, and verification is opt-in even in a full build - neither is on a
# decode path a set-top box has to run.
if(NOT ICLFORGE_MINIMAL_DECODER AND NOT ICLFORGE_MINIMAL_ENCODER)
    add_subdirectory(src/signing)
endif()

# A standalone container writer, kept apart from the codec on purpose: it
# depends on nothing of iclforge::ac3 and knows nothing about AC-3. Gated by
# ICLFORGE_BUILD_MATROSKA (default ON) so it can be opted out of - a vcpkg
# port maps this straight to its own "matroska" feature.
if(ICLFORGE_BUILD_MATROSKA)
    add_subdirectory(src/matroska)
endif()

# A second standalone container writer, same shape and same reasoning as
# src/matroska above - see src/mp4/CMakeLists.txt's own header. Gated by
# ICLFORGE_BUILD_MP4 (default ON, see the FATAL_ERROR guard above).
if(ICLFORGE_BUILD_MP4)
    add_subdirectory(src/mp4)
endif()

# A third standalone container writer, same shape as src/matroska above but
# for MPEG-2 Transport Stream rather than Matroska. Gated by ICLFORGE_BUILD_MPEGTS
# (default ON, see the option() and its FATAL_ERROR guard above) so it can be
# opted out of the same way matroska is expected to become opt-out-able too.
if(ICLFORGE_BUILD_MPEGTS)
    add_subdirectory(src/mpegts)
endif()

# A fourth standalone module, same "depends on nothing of iclforge::ac3" shape
# as the three container writers above - the AC-4 bitstream inspector, and the
# AC-4 decoder beside it, which links the inspector and
# nothing of iclforge::ac3 either, and the AC-4 encoder (src/ac4enc). src/ac4core
# is what the decoder and the encoder compute alike (tables and transforms),
# linked privately by both.
# Gated together by ICLFORGE_BUILD_AC4 (default ON, see the option() and its
# FATAL_ERROR guard above).
if(ICLFORGE_BUILD_AC4)
    add_subdirectory(src/ac4)
    add_subdirectory(src/ac4core)
    add_subdirectory(src/ac4dec)
    add_subdirectory(src/ac4enc)
elseif(ICLFORGE_MINIMAL_AC4)
    # The minimum-footprint profile's share of the above: what a decode reads
    # and no encoder (ICLFORGE_MINIMAL_AC4, beside the profile's options). The
    # three directories check ICLFORGE_MINIMAL_DECODER themselves for what a
    # bare-metal archive cannot have: a shared library, position-independent
    # code, and exceptions.
    add_subdirectory(src/ac4)
    add_subdirectory(src/ac4core)
    add_subdirectory(src/ac4dec)
    # -Wno-psabi: GCC's arm-none-eabi says, once a translation unit and for every
    # std::span passed by value, that its passing changed in GCC 7.1. A note about an
    # ABI nothing here links against, hundreds of them in the log of a leg whose
    # output is the numbers.
    foreach(_ac3_ac4_target IN ITEMS iclforge_ac4_objects iclforge_ac4core iclforge_ac4dec_objects)
        target_compile_options(${_ac3_ac4_target} PRIVATE "$<$<CXX_COMPILER_ID:GNU>:-Wno-psabi>")
    endforeach()
    unset(_ac3_ac4_target)
endif()

# A standalone reader, not a writer like the three container modules above - roadmap item IM1
# phase 1 ("IAB (SMPTE ST 2098-2) reader"). Gated by ICLFORGE_BUILD_IAB
# (default ON, see the option() above).
if(ICLFORGE_BUILD_IAB)
    add_subdirectory(src/iab)
endif()

# A standalone writer, not a reader like iab above - IAMF writer, phase 1 (an IAMF
# OBU/ISOBMFF writer). Gated by ICLFORGE_BUILD_IAMF (default ON, see the option()
# above).
if(ICLFORGE_BUILD_IAMF)
    add_subdirectory(src/iamf)
endif()

# A standalone BW64/RF64 + Audio Definition Model parser - reader, not writer, but the same
# "depends on nothing of iclforge::ac3" shape as the three container writers above. Gated by
# ICLFORGE_BUILD_ADM (default OFF, unlike the three above - see the option() itself for why).
if(ICLFORGE_BUILD_ADM)
    # IAB reader bridge, phase 3: iclforge::admbridge's own IAB mapping (src/admbridge/src/iab_bridge.cpp)
    # names ac3iab::IABitstreamFrame in a public header, so it needs the src/iab add_subdirectory()
    # above to have actually run. ICLFORGE_BUILD_IAB defaults ON, unlike ICLFORGE_BUILD_ADM, so this
    # is the one combination that can silently fail to configure without an explicit guard - every
    # other ICLFORGE_BUILD_ADM consequence below only ever depends on flags that are already
    # unconditionally true (iclforge::ac3) or already checked by this same if() (iclforge::adm).
    if(NOT ICLFORGE_BUILD_IAB)
        message(FATAL_ERROR "ICLFORGE_BUILD_ADM=ON requires ICLFORGE_BUILD_IAB=ON (default ON) - "
                             "iclforge::admbridge's IAB mapping (IAB reader phase 3) depends on iclforge::iab")
    endif()

    add_subdirectory(src/adm)

    # iclforge::admbridge - ADM BWF → JOC bridge, phase 2: maps ac3adm's parsed ADM graph onto
    # ac3::oba::AtmosEncoder. Depends on iclforge::ac3 (added above), iclforge::adm (just added) and
    # (IAB reader phase 3) iclforge::iab (added earlier, guarded above), so it is meaningless
    # without ICLFORGE_BUILD_ADM - gated by the same flag rather than a new option of its own; see
    # src/admbridge/CMakeLists.txt's own header comment for the full reasoning, including why this
    # is a separate module from every side it bridges.
    add_subdirectory(src/admbridge)
endif()

# iclforge_c (C API) - after src/ac3 so iclforge::ac3_static exists to embed, gated by its own
# ICLFORGE_BUILD_CAPI option (see the option() above) the same way matroska/mp4/mpegts are gated by
# theirs.
if(ICLFORGE_BUILD_CAPI)
    add_subdirectory(src/capi)
endif()

# iclforge::sendspin (planning/hearth-sendspin-extension.md) - gated by ICLFORGE_BUILD_HEARTH, the
# option above. Never installed or exported, like iclforge::audio.
if(ICLFORGE_BUILD_HEARTH)
    add_subdirectory(src/sendspin)
endif()

# install()/find_package(iclforge) support for iclforge::ac3, iclforge::matroska,
# iclforge::mp4 and iclforge::mpegts - after every add_subdirectory() call above (needs their targets to
# exist), before include(Packaging) (CPack's library component packages exactly what gets
# install()'d here). Deliberately not iclforge::audio - see cmake/InstallLibrary.cmake's own header
# comment for why. iclforge::adm and iclforge::admbridge ARE installed/exported here (ICLFORGE_BUILD_ADM
# block) but SHARED ONLY, unlike every other component - ac3adm PRIVATE-embeds the third-party
# libbw64/libadm (FetchContent'd, never installed/exported by this project in their own right,
# see src/adm/CMakeLists.txt's header comment), which only a self-contained SHARED library can
# absorb without either re-exporting them or shipping a STATIC archive with genuinely unresolved
# symbols. admbridge follows suit since it transitively depends on iclforge::adm_shared.
# Not under ICLFORGE_MINIMAL_DECODER: the exported package describes
# iclforge::ac3_static/iclforge::ac3_shared, neither of which exists in that
# profile, and an embedded integrator consumes iclforge::ac3_minimal from an
# in-tree add_subdirectory() build rather than from find_package().
if(NOT ICLFORGE_MINIMAL_DECODER AND NOT ICLFORGE_MINIMAL_ENCODER)
    include(InstallLibrary)
endif()

# The pybind11 extension module - after iclforge::ac3 exists (it links straight against it, the
# same way examples/ does), independent of everything below: it needs none of CLI/GUI/tests/
# examples and isn't consumed via find_package(iclforge), so it doesn't need to come before or
# after InstallLibrary for any reason other than "iclforge::ac3 must already be a target".
if(ICLFORGE_BUILD_PYTHON)
    add_subdirectory(python)
endif()

if(ICLFORGE_BUILD_CLI)
    add_subdirectory(apps/cli)
endif()

if(ICLFORGE_BUILD_GUI)
    add_subdirectory(apps/gui)
endif()

# Hearth's applications, the test sink so far (planning/hearth-reference-player.md). Not in a
# board's build, which takes src/sendspin's player half alone (ICLFORGE_SENDSPIN_PLAYER_ONLY,
# set by esp-idf/iclforge/CMakeLists.txt) and has no iclforge::ac3 for the applications to link.
if(ICLFORGE_BUILD_HEARTH AND NOT ICLFORGE_SENDSPIN_PLAYER_ONLY)
    add_subdirectory(apps/hearth)
endif()

# AC3Forge Crucible (docs/crucible/design/promotion.md).
# In-tree like the GUI rather than wrapping the root the way the Android app
# does, because it shares this toolchain. All three desktop platforms now have
# a half under apps/crucible/engine/platform/ and apps/crucible/ui/platform/,
# so all three are allowed in; the guard names them rather than being dropped,
# because a platform with no arm in apps/crucible/CMakeLists.txt would
# configure and then fail to link, which is what this list has always been for.
#
# macOS was excluded here until 2026-09-06 and is now included on the strength
# of the code compiling and nothing more: no Mac has run any part of this
# application, and docs/crucible/design/promotion.md's "What cannot be verified, and
# why" table still says so. The engine core is tested everywhere regardless
# (tests/crucible).
if(ICLFORGE_BUILD_CRUCIBLE AND (WIN32 OR APPLE OR LINUX))
    add_subdirectory(apps/crucible)
endif()

if(ICLFORGE_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# After the tests block: the examples register themselves as tests, which needs
# enable_testing() to have run.
if(ICLFORGE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ICLFORGE_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

# The browser decode demo (docs/platforms/wasm.md) - a platform
# app, same shape and reasoning as apps/android/'s Shield app: a small
# demo consuming iclforge::ac3 unmodified, kept out of both src/ (not library
# code) and examples/ (not a single-API-call snippet backing a
# docs/library/*.md page, see CONTRIBUTING.md). Unlike apps/android/,
# which is a wholly separate Gradle/NDK build reached from the other
# direction, this platform has no non-CMake build system to work around, so
# it is a normal add_subdirectory() gated on EMSCRIPTEN (set by
# cmake/toolchains/wasm.emscripten.toolchain.cmake) rather than an
# ICLFORGE_BUILD_* option - there is nothing to opt in or out of on a host
# that isn't targeting WASM in the first place.
if(EMSCRIPTEN)
    add_subdirectory(apps/wasm)
endif()

# The minimum-footprint probe, decoder or encoder (docs/building.md). Gated on
# the profile rather than on an option of its own, for the same reason
# apps/wasm is gated on EMSCRIPTEN: outside that profile there is no
# iclforge::ac3_minimal for it to link, so there is nothing to opt in or out of.
if(ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER)
    # Guarded on existence because this tree is also PACKED, as the ESP-IDF
    # component archive (tools/packaging/pack_esp_component.py), and that
    # archive carries the library without the probe that measures it - a
    # consumer wants the decoder, not a footprint harness. In a checkout the
    # directory is always there and this is a no-op.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/baremetal/CMakeLists.txt")
        add_subdirectory(apps/baremetal)
    endif()
endif()

# Forge's own LICENSE.txt and NOTICES.txt, and the install() rules that put
# them in the `runtime` component (apps/notices/notices.cmake). After both
# application subdirectories, because it needs ICLFORGE_BUILD_GUI settled and
# ICLFORGE_GUI_QT_VERSION exported; before include(Packaging), because CPack
# packages the install rules it declares. Gated on the two applications
# because every section of that file describes them: a library-only configure
# would otherwise generate a notice naming binaries its packages do not
# contain. The library components want notices of their own too - {fmt} is
# compiled into the library as well as into the applications
# (src/ac3/CMakeLists.txt compiles it in through iclforge::fmt_private, which
# puts it in the static archive as well as the shared library), so
# libiclforge0 and libiclforge-dev still have the missing-copyright gap the
# runtime package had until now - and that is separate work, over install
# rules in cmake/InstallLibrary.cmake rather than these, not started here.
if(ICLFORGE_BUILD_CLI OR ICLFORGE_BUILD_GUI)
    include("${CMAKE_CURRENT_SOURCE_DIR}/apps/notices/notices.cmake")
endif()

# After every target's install() rules (apps/cli, apps/gui) have been declared.
# Nothing to package under ICLFORGE_MINIMAL_DECODER - no application is built
# and nothing is installed (see include(InstallLibrary) above).
if(NOT ICLFORGE_MINIMAL_DECODER AND NOT ICLFORGE_MINIMAL_ENCODER)
    include(Packaging)
endif()

message(STATUS "iclforge ${PROJECT_VERSION_FULL}")
message(STATUS "  Build CLI      : ${ICLFORGE_BUILD_CLI}")
message(STATUS "  Build GUI      : ${ICLFORGE_BUILD_GUI}")
message(STATUS "  Build tests    : ${ICLFORGE_BUILD_TESTS}")
message(STATUS "  Build examples : ${ICLFORGE_BUILD_EXAMPLES}")
message(STATUS "  Build Python   : ${ICLFORGE_BUILD_PYTHON}")
# Which src/audio/src/backend/ directory was compiled, and so whether 'record'
# and 'play' work in this build at all - see the platform tree in
# src/audio/CMakeLists.txt. 'posix' is the one that means no.
message(STATUS "  Audio backend  : ${ICLFORGE_AUDIO_BACKEND}")
# Which of apps/crucible/engine/platform/linux/'s two X11 readers Crucible
# compiled (docs/crucible/design/promotion.md, "X11 full-screen detection"): 'xcb'
# reads the active window through libxcb; 'none' says so at runtime and
# leaves the full-screen rule off. Linux only, since only Linux has the
# choice; the Linux Crucible CI leg asserts on this line.
if(ICLFORGE_BUILD_CRUCIBLE AND LINUX)
    message(STATUS "  Crucible X11   : ${ICLFORGE_CRUCIBLE_X11_BACKEND}")
endif()
# Which src/arithmetic/arch/ directory the codec's vector kernels
# were compiled from (SIMD kernels). 'generic' is the portable scalar
# reference; -DICLFORGE_SIMD=generic forces it anywhere.
message(STATUS "  SIMD kernels   : ${ICLFORGE_SIMD_RESOLVED}")
