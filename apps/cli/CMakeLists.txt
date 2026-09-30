# ---------------------------------------------------------------------------
# forge - the command-line front end over iclforge::ac3.
# ---------------------------------------------------------------------------
add_executable(forge main.cpp support.cpp usage.cpp json.cpp multi_source.cpp ../common/fmp4_folder_writer.cpp ../common/recording_sink.cpp ../common/container_input.cpp ../common/stream_playback.cpp ../common/ac4_object_render.cpp ../common/ac4_encode_core.cpp ../common/ac4_objects_core.cpp commands/containers.cpp commands/audio_io.cpp commands/live_audio.cpp commands/synth.cpp commands/atmos.cpp commands/encode.cpp commands/ac4_encode.cpp commands/ac4_encode_objects.cpp commands/decode.cpp commands/analysis.cpp commands/probe.cpp commands/stream_tools.cpp)

# ../common/recording_sink.cpp + fmp4_folder_writer.cpp: the take writer
# 'record' and 'live' stream their output through (wide-layout record/live paths), shared
# verbatim with the GUI rather than reimplemented - see
# apps/common/recording_sink.hpp's own comment for why they sit there and not
# in either app. Compiled straight in; apps/common has no library target,
# matching how tests/CMakeLists.txt already compiles the same files into
# iclforge-tests. ../common/container_input.cpp (container readers (mkv/mp4/ts)) is the same shape,
# for turning a container file into an elementary stream instead, and so is
# ../common/stream_playback.cpp, the decoder choice and end-of-stream flush
# 'monitor' and 'spatial' play a stream with, ../common/ac4_encode_core.cpp,
# the steps of 'ac4-encode' forge-gui's AC-4 encode takes too, and
# ../common/ac4_objects_core.cpp, the object slots and the AC-4 objects' steps
# 'atmos-encode', 'atmos-adm' and 'atmos-iab' take, which forge-gui's AC-4 objects take too.
target_include_directories(forge PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")

# forge's platform-specific code, two seams' worth: putting stdin/stdout into
# binary mode for the "-" stdin/stdout file-argument convention (see
# platform/stdio_binary.hpp for why Windows needs a call there and POSIX
# does not), and putting the console on UTF-8 so the section signs in this
# CLI's specification citations render (platform/console_encoding.hpp for what
# was going wrong and why a redirect is untouched by the fix). Selected here
# rather than behind an #ifdef in main.cpp, the same platform-tree pattern
# iclforge::audio uses for WASAPI/ALSA (src/audio/CMakeLists.txt) -
# tools/checks/check_platform_macros.ps1 enforces there being no third option.
#
# WIN32_LEAN_AND_MEAN/NOMINMAX are here rather than #define'd above the
# <windows.h> include for the same reason and by the same rule: that script's
# header comment says header-configuration defines belong in
# target_compile_definitions, and src/audio/CMakeLists.txt is the example it
# names. PRIVATE and inside the WIN32 arm, so no other configuration sees a
# definition that means nothing to it.
if(WIN32)
    target_sources(forge PRIVATE platform/windows/stdio_binary.cpp
                                  platform/windows/console_encoding.cpp)
    target_compile_definitions(forge PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    target_sources(forge PRIVATE platform/posix/stdio_binary.cpp
                                  platform/posix/console_encoding.cpp)
endif()

# ../common/probe_json.cpp: probe's JSON stream summary and its vocabulary,
# which Hearth's media information writes too (apps/hearth/engine), compiled
# in the same way as the apps/common files above.
target_sources(forge PRIVATE ../common/probe_json.cpp)

# main.cpp's 'atmos-adm' command (ADM BWF reader phase 3) needs iclforge::adm/iclforge::admbridge - this
# project's one optional, non-default library dependency (ICLFORGE_BUILD_ADM, default OFF - see
# the root CMakeLists.txt's own option() for why: libadm's Boost dependency). Unlike
# iclforge::matroska/iclforge::mp4/iclforge::mpegts below (default ON, and a FATAL_ERROR guard in the
# root CMakeLists.txt refuses to build this target at all without them), forge builds and works
# perfectly well with this flag off - it just lacks the one command, refused with a clear reason at
# dispatch time (main.cpp's Needs::kAdm/unmet()) rather than missing from the binary's usage
# listing entirely.
#
# main.cpp itself never names iclforge::adm/iclforge::admbridge or includes either of their headers -
# not even behind a preprocessor guard, since tools/checks/check_platform_macros.ps1 (CI-enforced)
# refuses ANY #if/#ifdef/#ifndef anywhere under src/, feature flags included, not just platform
# ones (that script's own header comment). So the same platform-tree shape the WIN32/else split
# above already uses for stdio_binary.cpp applies here too, for a library-linked-or-not axis
# instead of an OS one: adm/atmos_adm.hpp declares the narrow, ac3adm/admbridge-type-free seam
# main.cpp calls unconditionally (forge_cli::load_adm_atmos_source(), forge_cli::adm_capability()), and
# exactly one of the two files below actually implements it - never both, never neither - so
# forge links successfully and behaves correctly either way with zero preprocessor conditionals
# anywhere in this target's sources. See adm/atmos_adm.hpp's own top comment for the full
# reasoning, including why this replaced an earlier #ifdef-based design once
# tools/checks/check_platform_macros.ps1 was actually run against it.
# 'atmos-iab' (IAB reader phase 3) rides the exact same ICLFORGE_BUILD_ADM gate as 'atmos-adm'
# above, for the reason adm/atmos_iab.hpp's own top comment gives: iclforge::iab by itself
# (ICLFORGE_BUILD_IAB, default ON) has nothing that can drive AtmosEncoder - only
# iclforge::admbridge's own build_iab() does that, and admbridge only exists when ICLFORGE_BUILD_ADM
# turned it on. adm/enabled/atmos_iab.cpp additionally needs iclforge::iab directly (to sniff and
# parse the input container), already linked unconditionally elsewhere in this target - see the
# root CMakeLists.txt's own ICLFORGE_BUILD_ADM-requires-ICLFORGE_BUILD_IAB guard for why that
# combination cannot actually diverge.
if(ICLFORGE_BUILD_ADM)
    target_sources(forge PRIVATE adm/enabled/atmos_adm.cpp adm/enabled/decode_adm.cpp
                                  adm/enabled/atmos_iab.cpp)
    target_link_libraries(forge PRIVATE iclforge::adm iclforge::admbridge)
else()
    target_sources(forge PRIVATE adm/disabled/atmos_adm.cpp adm/disabled/decode_adm.cpp
                                  adm/disabled/atmos_iab.cpp)
endif()

# iclforge::coverage (coverage floors) instruments this target's OWN sources. Linking
# an instrumented library is not enough and never was: --coverage is
# target-scoped, so before this line every .cpp under apps/cli compiled
# uninstrumented and produced no .gcno at all - a gcovr filter for apps/cli
# came back with zero files rather than a low percentage. It is an INTERFACE
# target that adds nothing when ICLFORGE_ENABLE_COVERAGE is off (the default),
# so this costs every other configuration nothing.
target_link_libraries(forge
    PRIVATE
        iclforge::ac3
        iclforge::audio
        iclforge::signing
        iclforge::matroska
        iclforge::mp4
        iclforge::mpegts
        iclforge::ac4
        iclforge::ac4dec
        iclforge::ac4enc
        iclforge::warnings
        iclforge::fmt
        iclforge::coverage)

# iclforge::ac3, iclforge::matroska, iclforge::mp4 and iclforge::mpegts are link-only here, but
# installed and packaged in their own right elsewhere - see
# cmake/InstallLibrary.cmake and cmake/Packaging.cmake's `library` CPack
# component, and docs/library/index.md for how a third party consumes them
# via find_package(iclforge). iclforge::audio stays link-only and unpackaged
# always - a CLI/GUI implementation detail, not part of the distributed
# library.
#
# iclforge::mp4/iclforge::mpegts are only ever unavailable when ICLFORGE_BUILD_MP4=OFF/
# ICLFORGE_BUILD_MPEGTS=OFF, which the root CMakeLists.txt's own FATAL_ERROR guards
# already refuse to combine with ICLFORGE_BUILD_CLI=ON - so unlike iclforge::audio's
# Needs::kCapture/kPassthrough/kMonitor runtime-availability checks (main.cpp), the
# `mp4`/`ts` commands below need no such check: this target is simply never configured
# at all in a build where either would be missing.
# -Wnull-dereference false positive, GNU only: SampleQueue::reset()'s
# std::vector<std::vector<float>>::assign(channels, {}) call, inlined at -O3
# (Release; not seen at the -debug preset's lower optimization) through
# several layers of libstdc++'s own vector internals (bits/stl_vector.h's
# _M_fill_assign/_M_erase_at_end), which GCC misattributes as a real null
# dereference of a pointer subtraction its own optimizer introduced -
# confirmed by actually building config-linux-gcc (not just -debug) on the
# CI-pinned GCC 16 - reachable from a plain WSL2 GCC 15.2.0 build too, and
# seen for real in CI's own build logs (CodeQL, adm-validate, and the plain
# Linux GCC/GCC-arm64 legs all hit it). `channels` is source_channels,
# always > 0 for a stream that scanned successfully - nothing here is
# genuinely null-unsafe.
#
# cmake/CompilerWarnings.cmake documents this exact false-positive category
# for GCC < 15 against libstdc++ internals reached from this project's own
# code (see its own comment, and gcc.gnu.org/bugzilla PR 108757 and
# neighbours), and src/adm/CMakeLists.txt has an identical GCC-15 instance
# already scoped to just that target - this is the same category surfacing on
# GCC 16 instead, in this one file. Scoped to the file rather than widening
# either of those guards: nothing else in this project has hit it, so
# silencing it everywhere would risk hiding a genuine one elsewhere.
set_source_files_properties(commands/stream_tools.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-null-dereference>")

# The same post-inlining misattribution as stream_tools.cpp's
# -Wnull-dereference case just above, a different diagnostic from the same
# root cause: run_demux's AC-4 re-wrap lambda (on_mp4_sample, the MP4 branch)
# builds `framed` with reserve() then a nested `put` lambda's push_back()
# loop (commands/containers.cpp ~901-907). At -O3 (Release; not seen at the
# -debug preset's lower optimization - confirmed building config-linux-gcc-debug
# clean with the same compiler) GCC inlines the whole call chain - through
# std::_Function_handler<...>::_M_invoke, the lambda itself, and
# vector<byte>::push_back's _M_realloc_append - down into the growth guard's
# own destructor (_Guard_alloc::~_Guard_alloc, bits/stl_vector.h), and
# misattributes ITS deallocate() call as freeing a pointer at a nonzero
# offset. Confirmed by actually building config-linux-gcc (Release) with a
# plain WSL2 g++ 15.2.0: the diagnostic's full inlining chain bottoms out at
# containers.cpp:901 (reserve) and :904 (push_back), nowhere else in this
# file. `framed` is an ordinary local std::vector<std::byte>, never aliased
# and never manually freed - nothing here is genuinely unsafe. Not seen on
# CI's pinned GCC 16 (main builds this file clean there), so scoped to the
# file the same way stream_tools.cpp's instance is rather than version-gated
# in cmake/CompilerWarnings.cmake - a GCC-version condition earns its keep
# there for a warning several files hit; this is one lambda in one file.
set_source_files_properties(commands/containers.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-free-nonheap-object>")

include(GNUInstallDirs)

# COMPONENT runtime: without one, CPack files this under its own
# "Unspecified" component, inconsistent once component-based packaging is
# on for the library (see cmake/InstallLibrary.cmake, cmake/Packaging.cmake).
install(TARGETS forge
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime
    BUNDLE DESTINATION "." COMPONENT runtime)

# ---------------------------------------------------------------------------
# The man page and the four shell completions (CLI shell completions).
#
# GENERATED, not written: `forge man` and `forge completions <shell>` render
# both from main.cpp's own command table (see apps/cli/usage.cpp), so a new
# command or option reaches the man page, all four completion scripts and the
# --help text at once and cannot be forgotten in one of them. That is the
# whole reason to generate rather than check in five hand-maintained files.
#
# The cost of generating from the binary is that the binary has to run, so the
# question this guard has to answer is "can this host execute a binary built
# for this target". That is a host/target comparison, and NOT
# CMAKE_CROSSCOMPILING, which is a much broader flag: CMakeDetermineSystem sets
# it TRUE whenever CMAKE_SYSTEM_NAME arrives from a toolchain file rather than
# from the host, whether or not the target differs from the host at all. Every
# Linux and macOS preset here chainloads a toolchain file that sets
# CMAKE_SYSTEM_NAME unconditionally (cmake/toolchains/linux.gcc, linux.llvm,
# macos.llvm), so testing CMAKE_CROSSCOMPILING skipped these files on every
# native Linux and macOS build too - both .tar.gz, both .deb, both .rpm and the
# universal .dmg shipped without a man page or completions, and only the
# Homebrew formula (which passes no toolchain file, so its own test still found
# them) got them at all. The Windows toolchains set only
# CMAKE_SYSTEM_PROCESSOR, which is why Windows never showed the symptom.
#
# Comparing the two pairs directly answers the real question and keeps the
# genuine cases out: Emscripten (Emscripten vs Windows/Linux/Darwin, and no
# forge built there anyway) and any future host-arch/target-arch mismatch,
# such as an x86_64 box configuring for aarch64. The two arm64 Linux presets
# are NOT that case in CI - they run on native aarch64 runners
# (.github/workflows/_build.yml pins them to ubuntu-24.04-arm), so they now
# produce and ship these files like every other native leg.
#
# Where the host cannot run the target's binary, the files are not produced and
# not installed: a packaging run on the target arch (or a native build of the
# same source) produces them, and a cross-built .deb without a man page is a
# smaller loss than a build that fails outright or a checked-in file that
# silently goes stale. The status message says which happened rather than
# leaving it to be discovered from a missing file.
if(NOT "${CMAKE_HOST_SYSTEM_NAME}" STREQUAL "${CMAKE_SYSTEM_NAME}"
        OR NOT "${CMAKE_HOST_SYSTEM_PROCESSOR}" STREQUAL "${CMAKE_SYSTEM_PROCESSOR}")
    message(STATUS "  CLI man page   : skipped (a ${CMAKE_HOST_SYSTEM_NAME}/${CMAKE_HOST_SYSTEM_PROCESSOR} "
                   "host cannot run a ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR} forge)")
else()
    # The other half of "the status message says which happened": a
    # configure log that says nothing at all is how the skip above went
    # unnoticed on Linux and macOS for as long as it did.
    message(STATUS "  CLI man page   : generated (with bash/zsh/fish/PowerShell completions)")

    set(ICLFORGE_CLI_DOC_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${ICLFORGE_CLI_DOC_DIR}")

    # DEPENDS forge, not just $<TARGET_FILE:forge>: the command table lives
    # in the binary, so the artefacts have to be regenerated whenever it is
    # relinked, not only when the file name changes.
    add_custom_command(
        OUTPUT "${ICLFORGE_CLI_DOC_DIR}/forge.1"
        COMMAND "${CMAKE_COMMAND}"
                "-DAC3_RUN_COMMAND=$<TARGET_FILE:forge>;man"
                "-DAC3_RUN_OUTPUT=${ICLFORGE_CLI_DOC_DIR}/forge.1"
                -P "${PROJECT_SOURCE_DIR}/cmake/RunToFile.cmake"
        DEPENDS forge
        COMMENT "Generating forge.1 from forge's own command table"
        VERBATIM)

    # <shell> -> the file name that shell's completion loader expects. bash
    # and zsh look their file up by a fixed name (the command / _command);
    # fish and PowerShell want a suffixed script. PowerShell has no
    # convention-driven search path at all, so its script goes under the
    # project's own share directory for the user to dot-source from $PROFILE
    # - which is what `forge completions powershell` itself says.
    set(ICLFORGE_CLI_COMPLETION_SHELLS bash zsh fish powershell)
    set(ICLFORGE_CLI_COMPLETION_FILES forge _forge forge.fish forge.ps1)
    set(ICLFORGE_CLI_COMPLETION_DESTS
        "${CMAKE_INSTALL_DATAROOTDIR}/bash-completion/completions"
        "${CMAKE_INSTALL_DATAROOTDIR}/zsh/site-functions"
        "${CMAKE_INSTALL_DATAROOTDIR}/fish/vendor_completions.d"
        "${CMAKE_INSTALL_DATAROOTDIR}/iclforge/completions")

    set(ICLFORGE_CLI_GENERATED_DOCS "${ICLFORGE_CLI_DOC_DIR}/forge.1")
    foreach(index RANGE 3)
        list(GET ICLFORGE_CLI_COMPLETION_SHELLS ${index} shell)
        list(GET ICLFORGE_CLI_COMPLETION_FILES ${index} completion_file)
        list(GET ICLFORGE_CLI_COMPLETION_DESTS ${index} completion_dest)
        add_custom_command(
            OUTPUT "${ICLFORGE_CLI_DOC_DIR}/${completion_file}"
            COMMAND "${CMAKE_COMMAND}"
                    "-DAC3_RUN_COMMAND=$<TARGET_FILE:forge>;completions;${shell}"
                    "-DAC3_RUN_OUTPUT=${ICLFORGE_CLI_DOC_DIR}/${completion_file}"
                    -P "${PROJECT_SOURCE_DIR}/cmake/RunToFile.cmake"
            DEPENDS forge
            COMMENT "Generating ${shell} completions for forge"
            VERBATIM)
        list(APPEND ICLFORGE_CLI_GENERATED_DOCS "${ICLFORGE_CLI_DOC_DIR}/${completion_file}")
        install(FILES "${ICLFORGE_CLI_DOC_DIR}/${completion_file}"
            DESTINATION "${completion_dest}" COMPONENT runtime)
    endforeach()

    # ALL, so a plain `cmake --build` produces them: they are install()'d
    # unconditionally below, and an install of a target that was never built
    # is a hard CMake error rather than a missing file.
    add_custom_target(forge_docs ALL DEPENDS ${ICLFORGE_CLI_GENERATED_DOCS})

    install(FILES "${ICLFORGE_CLI_DOC_DIR}/forge.1"
        DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT runtime)
endif()
