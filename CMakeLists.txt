cmake_minimum_required(VERSION 3.28...4.3)

# Derive the version from the nearest reachable v* git tag before project() -
# see the file for the fallback/override rules. This makes a pushed tag the
# single source of truth: nothing in this tree needs bumping by hand anymore.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitVersionDerivation.cmake")

project(ac3forge
    VERSION ${DERIVED_VERSION_SEMVER}
    DESCRIPTION "Clean-room AC-3 (ATSC A/52) encoder and decoder with a spatial object layer"
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

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# One place for runtime artefacts so the Qt app finds its plugins and the CLI
# and tests sit beside it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# Empty unless -DAC3FORGE_SANITIZERS=<comma-list> is set (see the *-asan-ubsan
# presets in CMakePresets.json); a plain configure is unaffected.
set(AC3FORGE_SANITIZERS "" CACHE STRING "Comma-separated -fsanitize= value, e.g. address,undefined")
include(Sanitizers)

# ac3::coverage, off unless -DAC3FORGE_ENABLE_COVERAGE=ON (see the
# linux-gcc-coverage preset in CMakePresets.json); a plain configure is unaffected.
include(Coverage)

# ac3::tracy, off unless -DAC3FORGE_ENABLE_TRACY=ON (and vcpkg's "profiling"
# manifest feature resolved - see vcpkg.json); a plain configure is unaffected.
include(Tracy)

# ac3::fmt - {fmt}, used everywhere instead of std::format/std::print (see
# cmake/Fmt.cmake). Unlike Tracy above, this is a base dependency: always
# resolved, never opt-in.
include(Fmt)

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
#    allocation with it. See ROADMAP VX11, which this pins the policy half
#    of.
#  - It is invisible. Nothing in the source says a contraction happened, so
#    the difference cannot be reviewed, only measured after the fact.
#  - It would break the SIMD seam's correctness argument outright.
#    src/forge/src/internal/arch/*/ac3/internal/arch/simd.hpp maps every
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
# Project-wide via add_compile_options rather than on ac3::forge alone: the
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
option(AC3FORGE_BUILD_CLI "Build the ac3cli command-line application" ON)
option(AC3FORGE_BUILD_GUI "Build the Qt6 Quick GUI application" ON)
option(AC3FORGE_BUILD_TESTS "Build the unit test suite" ON)
# Catch2 is the only build-time dependency. Turn this off to insist it comes
# from a package manager instead of the network (see tests/CMakeLists.txt).
option(AC3FORGE_FETCH_CATCH2 "Download Catch2 when no local copy is found" ON)
option(AC3FORGE_BUILD_EXAMPLES "Build the documented library examples" ON)
# libFuzzer harnesses over ac3::forge's untrusted-input entry points (see
# fuzz/). Off by default: it needs upstream Clang (GCC and MSVC do not ship
# libFuzzer) and instruments ac3forge itself with ASan+UBSan, which no other
# configuration wants paid for. Use fuzz/run.sh rather than this option
# directly - it configures a dedicated build/fuzz directory with the right
# compiler.
option(AC3FORGE_BUILD_FUZZERS "Build libFuzzer harnesses (Clang only; see fuzz/)" OFF)
# Every other library component (matroska::matroska, mp4::mp4, mpegts::mpegts) gets its own
# AC3FORGE_BUILD_<NAME> option, defaulting ON so nothing changes for today's direct/CPack builds.
# This is what lets a vcpkg port expose a component as an opt-in feature - see
# cmake/InstallLibrary.cmake's matching install()/export() gate.
option(AC3FORGE_BUILD_MATROSKA "Build the standalone Matroska container writer (src/matroska)" ON)
option(AC3FORGE_BUILD_MP4 "Build the standalone MP4/ISOBMFF container writer (src/mp4)" ON)
option(AC3FORGE_BUILD_MPEGTS "Build the standalone MPEG-TS container writer (src/mpegts)" ON)

# ac3iab::ac3iab (src/ac3iab) - roadmap item IM1 phase 1: a standalone reader for SMPTE
# ST 2098-2's Immersive Audio Bitstream. Same zero-third-party-dependency shape as the three
# container writers above (unlike ac3adm, see its own option() below), so it defaults ON the
# same way; unlike them, nothing in apps/cli, apps/gui or examples/ links it yet (roadmap IM1's
# phase 3 is what will eventually give it a consumer), so there is no matching FATAL_ERROR
# guard here - tests/CMakeLists.txt only adds ac3iab's own test file and links ac3iab::ac3iab
# when this option is on, the same conditional shape it already uses for ac3adm::ac3adm/
# AC3FORGE_BUILD_ADM.
option(AC3FORGE_BUILD_IAB "Build the standalone Immersive Audio Bitstream reader (src/ac3iab)" ON)

# iamf::iamf (src/iamf) - roadmap item IM3 phase 1: a standalone OBU/ISOBMFF writer for AOM's
# IAMF (Immersive Audio Model and Formats) v1.1.0, emitting `ipcm` substreams for a channel-based
# 7.1.4 element. Same zero-third-party-dependency shape as ac3iab immediately above (unlike
# ac3adm, see its own option() below), so it defaults ON the same way and for the same reason;
# also like ac3iab, nothing in apps/cli, apps/gui or examples/ links it unconditionally yet (only
# examples/mux_iamf.cpp does, guarded on this option) - see src/iamf/CMakeLists.txt's own header
# comment.
option(AC3FORGE_BUILD_IAMF "Build the standalone IAMF OBU/ISOBMFF writer (src/iamf)" ON)

# Roadmap PF7: the minimum-footprint decoder profile. Not a "build X too"
# option like the ones above - it REPLACES what src/forge builds with a single
# decode-only static library for a target with a few hundred kilobytes of RAM
# and no operating system (see cmake/MinimalDecoder.cmake and
# docs/building.md). Off by default, and the guard below refuses it alongside
# anything that needs the full library, so an ordinary configure is unaffected
# and a half-configured one fails here rather than deep inside a subdirectory.
option(AC3FORGE_MINIMAL_DECODER
    "Build ONLY the decode-only, exception-free, table-trimmed ac3::forge_minimal (roadmap PF7)"
    OFF)

# apps/cli, apps/gui, tests/ and examples/ all link matroska::matroska, mp4::mp4 and
# mpegts::mpegts unconditionally - none of them are written to cope with any of the three being
# absent. AC3FORGE_BUILD_MATROSKA=OFF, AC3FORGE_BUILD_MP4=OFF or AC3FORGE_BUILD_MPEGTS=OFF only
# make sense with all four also OFF (this is the shape a vcpkg build without the matching
# feature actually uses - mp4/mpegts have no vcpkg feature of their own yet, see
# cmake/InstallLibrary.cmake); catch the unsupported combination here with a clear message
# rather than letting it fail deep inside one of those subdirectories with a "target not found"
# error.
if(NOT AC3FORGE_BUILD_MATROSKA AND (AC3FORGE_BUILD_CLI OR AC3FORGE_BUILD_GUI OR AC3FORGE_BUILD_TESTS OR AC3FORGE_BUILD_EXAMPLES))
    message(FATAL_ERROR "AC3FORGE_BUILD_MATROSKA=OFF requires AC3FORGE_BUILD_CLI, "
        "AC3FORGE_BUILD_GUI, AC3FORGE_BUILD_TESTS and AC3FORGE_BUILD_EXAMPLES to all be OFF too - "
        "they all link matroska::matroska unconditionally.")
endif()
if(NOT AC3FORGE_BUILD_MP4 AND (AC3FORGE_BUILD_CLI OR AC3FORGE_BUILD_GUI OR AC3FORGE_BUILD_TESTS OR AC3FORGE_BUILD_EXAMPLES))
    message(FATAL_ERROR "AC3FORGE_BUILD_MP4=OFF requires AC3FORGE_BUILD_CLI, "
        "AC3FORGE_BUILD_GUI, AC3FORGE_BUILD_TESTS and AC3FORGE_BUILD_EXAMPLES to all be OFF too - "
        "they all link mp4::mp4 unconditionally.")
endif()
if(NOT AC3FORGE_BUILD_MPEGTS AND (AC3FORGE_BUILD_CLI OR AC3FORGE_BUILD_GUI OR
                                  AC3FORGE_BUILD_TESTS OR AC3FORGE_BUILD_EXAMPLES))
    message(FATAL_ERROR "AC3FORGE_BUILD_MPEGTS=OFF requires AC3FORGE_BUILD_CLI, "
        "AC3FORGE_BUILD_GUI, AC3FORGE_BUILD_TESTS and AC3FORGE_BUILD_EXAMPLES to all be OFF too - "
        "they all link mpegts::mpegts unconditionally.")
endif()

# AC3FORGE_MINIMAL_DECODER's own version of the three guards above. Everything listed here
# either links the full ac3::forge (CLI, GUI, tests, examples, the C API) or is a module the
# profile deliberately leaves out (the container writers, the ADM reader); none of them build
# against a decode-only archive, and several would not even configure for a bare-metal target.
# Name every offender at once rather than failing on the first, so a caller assembling the
# option set gets one message instead of six configures.
if(AC3FORGE_MINIMAL_DECODER)
    set(_ac3_minimal_conflicts "")
    foreach(_opt IN ITEMS AC3FORGE_BUILD_CLI AC3FORGE_BUILD_GUI AC3FORGE_BUILD_TESTS
                          AC3FORGE_BUILD_EXAMPLES AC3FORGE_BUILD_MATROSKA AC3FORGE_BUILD_MP4
                          AC3FORGE_BUILD_MPEGTS AC3FORGE_BUILD_IAB AC3FORGE_BUILD_IAMF
                          AC3FORGE_BUILD_CAPI AC3FORGE_BUILD_ADM AC3FORGE_BUILD_PYTHON
                          AC3FORGE_BUILD_FUZZERS)
        if(${_opt})
            list(APPEND _ac3_minimal_conflicts "${_opt}")
        endif()
    endforeach()
    if(_ac3_minimal_conflicts)
        list(JOIN _ac3_minimal_conflicts "=OFF, " _ac3_minimal_message)
        message(FATAL_ERROR
            "AC3FORGE_MINIMAL_DECODER=ON needs ${_ac3_minimal_message}=OFF - none of them build "
            "against a decode-only archive. The config-*-minimal presets in CMakePresets.json "
            "already set them; see docs/building.md's minimum-footprint decoder profile.")
    endif()
    unset(_ac3_minimal_conflicts)
    unset(_ac3_minimal_message)
endif()

# ac3::minimal_profile - the compile/link options the profile above adds. Not
# beside include(Sanitizers)/include(Coverage)/include(Tracy) up top because,
# unlike those three, this one is driven by an option() declared in this
# section rather than by a bare cache variable, and a module cannot read an
# option that has not been declared yet. No-op when the profile is off.
include(MinimalDecoder)

# ac3adm::ac3adm (src/ac3adm) is a READER, not a writer like the three container modules above -
# roadmap item B1 phase 1 ("ADM BWF reader feeding the JOC encoder", see ROADMAP.md), parsing
# BW64/RF64 + the Audio Definition Model rather than muxing a container. Unlike
# matroska/mp4/mpegts, it defaults OFF rather than ON and is genuinely additive: nothing in
# apps/cli, apps/gui, tests/ or examples/ links it unconditionally, so every one of those builds
# identically whether this is on or off. The reason for the different default is its
# dependency footprint - see src/ac3adm/CMakeLists.txt's own header comment - libadm needs
# several Boost header libraries, resolved through a dedicated opt-in vcpkg feature
# (-DVCPKG_MANIFEST_FEATURES=adm, see vcpkg.json) that a normal build never pulls in.
option(AC3FORGE_BUILD_ADM "Build the standalone BW64/RF64 + ADM parser (src/ac3adm) - needs Boost, see vcpkg.json's \"adm\" feature" OFF)

# ac3forge_c (src/capi) - roadmap item F1: the C API over ac3::forge's encode/decode core. Depends
# on nothing but ac3::forge_static (see src/capi/CMakeLists.txt's own header comment on why it
# always embeds the static codec rather than tracking BUILD_SHARED_LIBS), so - unlike
# AC3FORGE_BUILD_ADM - there is no extra dependency footprint to opt out of; default ON matches
# matroska/mp4/mpegts above, not ADM.
option(AC3FORGE_BUILD_CAPI "Build the C API over the encode/decode core (src/capi)" ON)

# Roadmap F2: the pybind11 extension module (python/) - off by default like AC3FORGE_BUILD_ADM
# above, and for the same reason: nothing under src/, apps/cli, apps/gui, tests/ or examples/ links
# it or is affected by it either way. A normal C++ build (and every existing CI leg) is unchanged
# with this OFF. Turned ON by python/pyproject.toml's own cmake.define when scikit-build-core
# drives the configure for `pip install`/cibuildwheel - see python/CMakeLists.txt.
option(AC3FORGE_BUILD_PYTHON "Build the pybind11 Python extension module (python/)" OFF)

# enable_testing() has to run before ANY add_subdirectory() that might contain
# an add_test() call, not merely before tests/ itself - CTest silently drops
# an add_test() registered from a directory processed earlier in the same
# configure pass (confirmed the hard way: apps/gui/tests/'s ac3gui_qml_tests_*
# entries were invisible to `ctest -N` until this moved here). Calling it unconditionally
# is harmless when AC3FORGE_BUILD_TESTS is OFF - nothing calls add_test() then.
if(AC3FORGE_BUILD_TESTS)
    enable_testing()
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
# The library holds the entire codec and spatial engine and is GUI-free.
add_subdirectory(src/forge)

# Live capture, monitor playback and IEC 61937 passthrough - the only
# platform-specific code in the project. After src/forge (PRIVATE-links
# ac3::forge for the IEC 61937 framer); before apps/cli/apps/gui/tests, which
# link it alongside ac3::forge for their live-audio commands/features. Not
# linked by examples/ - none of them touch live audio.
#
# Skipped entirely under Emscripten: src/audio/CMakeLists.txt hard-fails
# configure when none of WIN32/LINUX/APPLE/ANDROID is set (it has no browser
# platform directory, and never will - a WASM build gets live audio from the
# Web Audio API in JS instead, not from this library), and nothing a WASM
# configure builds needs it anyway (AC3FORGE_BUILD_CLI/GUI/TESTS are all OFF
# there, and examples/wasm_decode_demo does not link ac3::audio).
#
# Skipped under AC3FORGE_MINIMAL_DECODER for a different reason than under
# Emscripten but with the same effect: a bare-metal target has no audio
# device, no backend directory would match it, and the profile's whole point
# is that nothing outside the decode path is compiled.
if(NOT EMSCRIPTEN AND NOT AC3FORGE_MINIMAL_DECODER)
    add_subdirectory(src/audio)
endif()

# The EMDF Atmos object signer (ac3::signing). Clean-room and dependency-free:
# the HMAC construction and authenticated-region layout are in-tree, and the
# only secret - the key - is supplied by the operator at runtime, never
# embedded, so unlike the old quarantine overlay this is committed and built by
# default. After src/forge so it can reuse the codec's parsing primitives; before
# apps/cli so ac3cli can link it.
# Not under AC3FORGE_MINIMAL_DECODER: object SIGNING is a production-side
# concern, and verification is opt-in even in a full build - neither is on a
# decode path a set-top box has to run.
if(NOT AC3FORGE_MINIMAL_DECODER)
    add_subdirectory(src/signing)
endif()

# A standalone container writer, kept apart from the codec on purpose: it
# depends on nothing of ac3::forge and knows nothing about AC-3. Gated by
# AC3FORGE_BUILD_MATROSKA (default ON) so it can be opted out of - a vcpkg
# port maps this straight to its own "matroska" feature.
if(AC3FORGE_BUILD_MATROSKA)
    add_subdirectory(src/matroska)
endif()

# A second standalone container writer, same shape and same reasoning as
# src/matroska above - see src/mp4/CMakeLists.txt's own header. Gated by
# AC3FORGE_BUILD_MP4 (default ON, see the FATAL_ERROR guard above).
if(AC3FORGE_BUILD_MP4)
    add_subdirectory(src/mp4)
endif()

# A third standalone container writer, same shape as src/matroska above but
# for MPEG-2 Transport Stream rather than Matroska. Gated by AC3FORGE_BUILD_MPEGTS
# (default ON, see the option() and its FATAL_ERROR guard above) so it can be
# opted out of the same way matroska is expected to become opt-out-able too.
if(AC3FORGE_BUILD_MPEGTS)
    add_subdirectory(src/mpegts)
endif()

# A standalone reader, not a writer like the three container modules above - roadmap item IM1
# phase 1 ("IAB (SMPTE ST 2098-2) reader", see ROADMAP.md). Gated by AC3FORGE_BUILD_IAB
# (default ON, see the option() above).
if(AC3FORGE_BUILD_IAB)
    add_subdirectory(src/ac3iab)
endif()

# A standalone writer, not a reader like ac3iab above - roadmap item IM3 phase 1 (an IAMF
# OBU/ISOBMFF writer, see ROADMAP.md). Gated by AC3FORGE_BUILD_IAMF (default ON, see the option()
# above).
if(AC3FORGE_BUILD_IAMF)
    add_subdirectory(src/iamf)
endif()

# A standalone BW64/RF64 + Audio Definition Model parser - reader, not writer, but the same
# "depends on nothing of ac3::forge" shape as the three container writers above. Gated by
# AC3FORGE_BUILD_ADM (default OFF, unlike the three above - see the option() itself for why).
if(AC3FORGE_BUILD_ADM)
    add_subdirectory(src/ac3adm)

    # ac3::admbridge - roadmap item B1 phase 2: maps ac3adm's parsed ADM graph onto
    # ac3::oba::AtmosEncoder. Depends on BOTH ac3::forge (added above) and ac3adm::ac3adm (just
    # added), so it is meaningless without AC3FORGE_BUILD_ADM - gated by the same flag rather
    # than a new option of its own; see src/admbridge/CMakeLists.txt's own header comment for
    # the full reasoning, including why this is a separate module from either side.
    add_subdirectory(src/admbridge)
endif()

# ac3forge_c (roadmap F1) - after src/forge so ac3::forge_static exists to embed, gated by its own
# AC3FORGE_BUILD_CAPI option (see the option() above) the same way matroska/mp4/mpegts are gated by
# theirs.
if(AC3FORGE_BUILD_CAPI)
    add_subdirectory(src/capi)
endif()

# install()/find_package(ac3forge) support for ac3::forge, matroska::matroska,
# mp4::mp4 and mpegts::mpegts - after every add_subdirectory() call above (needs their targets to
# exist), before include(Packaging) (CPack's library component packages exactly what gets
# install()'d here). Deliberately not ac3::audio, and deliberately not ac3adm::ac3adm either -
# see cmake/InstallLibrary.cmake's own header comment for the former; ac3adm is left out of the
# installed package for the same "FetchContent'd third-party code, not this project's own" reason
# it stays out of docs/library/index.md's zero-dependency claim (see src/ac3adm/CMakeLists.txt's
# header comment) plus the added complexity of re-exporting libbw64/libadm's own transitive
# targets through this package - AC3FORGE_BUILD_ADM users consume ac3adm::ac3adm from an in-tree
# add_subdirectory() build, not from a `find_package(ac3forge)` installed one, for now. ac3::admbridge
# (src/admbridge) is excluded for the same reason - it transitively depends on ac3adm::ac3adm.
# Not under AC3FORGE_MINIMAL_DECODER: the exported package describes
# ac3::forge_static/ac3::forge_shared, neither of which exists in that
# profile, and an embedded integrator consumes ac3::forge_minimal from an
# in-tree add_subdirectory() build rather than from find_package().
if(NOT AC3FORGE_MINIMAL_DECODER)
    include(InstallLibrary)
endif()

# The pybind11 extension module - after ac3::forge exists (it links straight against it, the
# same way examples/ does), independent of everything below: it needs none of CLI/GUI/tests/
# examples and isn't consumed via find_package(ac3forge), so it doesn't need to come before or
# after InstallLibrary for any reason other than "ac3::forge must already be a target".
if(AC3FORGE_BUILD_PYTHON)
    add_subdirectory(python)
endif()

if(AC3FORGE_BUILD_CLI)
    add_subdirectory(apps/cli)
endif()

if(AC3FORGE_BUILD_GUI)
    add_subdirectory(apps/gui)
endif()

if(AC3FORGE_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# After the tests block: the examples register themselves as tests, which needs
# enable_testing() to have run.
if(AC3FORGE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(AC3FORGE_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

# The browser decode demo (roadmap F3, docs/platforms/wasm.md) - a platform
# app, same shape and reasoning as apps/android/'s Shield app: a small
# demo consuming ac3::forge unmodified, kept out of both src/ (not library
# code) and examples/ (not a single-API-call snippet backing a
# docs/library/*.md page, see CONTRIBUTING.md). Unlike apps/android/,
# which is a wholly separate Gradle/NDK build reached from the other
# direction, this platform has no non-CMake build system to work around, so
# it is a normal add_subdirectory() gated on EMSCRIPTEN (set by
# cmake/toolchains/wasm.emscripten.toolchain.cmake) rather than an
# AC3FORGE_BUILD_* option - there is nothing to opt in or out of on a host
# that isn't targeting WASM in the first place.
if(EMSCRIPTEN)
    add_subdirectory(apps/wasm)
endif()

# The minimum-footprint decoder probe (roadmap PF7, docs/building.md). Gated on
# the profile rather than on an option of its own, for the same reason
# apps/wasm is gated on EMSCRIPTEN: outside that profile there is no
# ac3::forge_minimal for it to link, so there is nothing to opt in or out of.
if(AC3FORGE_MINIMAL_DECODER)
    add_subdirectory(apps/baremetal)
endif()

# After every target's install() rules (apps/cli, apps/gui) have been declared.
# Nothing to package under AC3FORGE_MINIMAL_DECODER - no application is built
# and nothing is installed (see include(InstallLibrary) above).
if(NOT AC3FORGE_MINIMAL_DECODER)
    include(Packaging)
endif()

message(STATUS "ac3forge ${PROJECT_VERSION_FULL}")
message(STATUS "  Build CLI      : ${AC3FORGE_BUILD_CLI}")
message(STATUS "  Build GUI      : ${AC3FORGE_BUILD_GUI}")
message(STATUS "  Build tests    : ${AC3FORGE_BUILD_TESTS}")
message(STATUS "  Build examples : ${AC3FORGE_BUILD_EXAMPLES}")
message(STATUS "  Build Python   : ${AC3FORGE_BUILD_PYTHON}")
# Which src/audio/src/backend/ directory was compiled, and so whether 'record'
# and 'play' work in this build at all - see the platform tree in
# src/audio/CMakeLists.txt. 'posix' is the one that means no.
message(STATUS "  Audio backend  : ${AC3FORGE_AUDIO_BACKEND}")
# Which src/forge/src/internal/arch/ directory the codec's vector kernels
# were compiled from (ROADMAP PF5). 'generic' is the portable scalar
# reference; -DAC3FORGE_SIMD=generic forces it anywhere.
message(STATUS "  SIMD kernels   : ${AC3FORGE_SIMD_RESOLVED}")
