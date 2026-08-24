# ---------------------------------------------------------------------------
# ac3cli - the command-line front end over ac3::forge.
# ---------------------------------------------------------------------------
add_executable(ac3cli main.cpp support.cpp usage.cpp json.cpp multi_source.cpp ../common/fmp4_folder_writer.cpp ../common/recording_sink.cpp commands/containers.cpp commands/audio_io.cpp commands/live_audio.cpp commands/synth.cpp commands/atmos.cpp commands/encode.cpp commands/decode.cpp commands/analysis.cpp commands/probe.cpp commands/stream_tools.cpp)

# ../common/recording_sink.cpp + fmp4_folder_writer.cpp: the take writer
# 'record' and 'live' stream their output through (roadmap IO9), shared
# verbatim with the GUI rather than reimplemented - see
# apps/common/recording_sink.hpp's own comment for why they sit there and not
# in either app. Compiled straight in; apps/common has no library target,
# matching how tests/CMakeLists.txt already compiles the same files into
# ac3tests.
target_include_directories(ac3cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")

# ac3cli's platform-specific code: putting stdin/stdout into binary mode for
# the "-" stdin/stdout file-argument convention (see platform/stdio_binary.hpp
# for why Windows needs a real call and POSIX doesn't). Selected here rather
# than behind an #ifdef in main.cpp, the same platform-tree pattern ac3::audio
# uses for WASAPI/ALSA (src/audio/CMakeLists.txt) - tools/checks/check_platform_macros.ps1
# enforces there being no third option.
if(WIN32)
    target_sources(ac3cli PRIVATE platform/windows/stdio_binary.cpp)
else()
    target_sources(ac3cli PRIVATE platform/posix/stdio_binary.cpp)
endif()

# main.cpp's 'atmos-adm' command (roadmap B1 phase 3) needs ac3adm::ac3adm/ac3::admbridge - this
# project's one optional, non-default library dependency (AC3FORGE_BUILD_ADM, default OFF - see
# the root CMakeLists.txt's own option() for why: libadm's Boost dependency). Unlike
# matroska::matroska/mp4::mp4/mpegts::mpegts below (default ON, and a FATAL_ERROR guard in the
# root CMakeLists.txt refuses to build this target at all without them), ac3cli builds and works
# perfectly well with this flag off - it just lacks the one command, refused with a clear reason at
# dispatch time (main.cpp's Needs::kAdm/unmet()) rather than missing from the binary's usage
# listing entirely.
#
# main.cpp itself never names ac3adm::ac3adm/ac3::admbridge or includes either of their headers -
# not even behind a preprocessor guard, since tools/checks/check_platform_macros.ps1 (CI-enforced)
# refuses ANY #if/#ifdef/#ifndef anywhere under src/, feature flags included, not just platform
# ones (that script's own header comment). So the same platform-tree shape the WIN32/else split
# above already uses for stdio_binary.cpp applies here too, for a library-linked-or-not axis
# instead of an OS one: adm/atmos_adm.hpp declares the narrow, ac3adm/admbridge-type-free seam
# main.cpp calls unconditionally (ac3cli::load_adm_atmos_source(), ac3cli::adm_capability()), and
# exactly one of the two files below actually implements it - never both, never neither - so
# ac3cli links successfully and behaves correctly either way with zero preprocessor conditionals
# anywhere in this target's sources. See adm/atmos_adm.hpp's own top comment for the full
# reasoning, including why this replaced an earlier #ifdef-based design once
# tools/checks/check_platform_macros.ps1 was actually run against it.
if(AC3FORGE_BUILD_ADM)
    target_sources(ac3cli PRIVATE adm/enabled/atmos_adm.cpp)
    target_link_libraries(ac3cli PRIVATE ac3adm::ac3adm ac3::admbridge)
else()
    target_sources(ac3cli PRIVATE adm/disabled/atmos_adm.cpp)
endif()

# ac3::coverage (roadmap VX15) instruments this target's OWN sources. Linking
# an instrumented library is not enough and never was: --coverage is
# target-scoped, so before this line every .cpp under apps/cli compiled
# uninstrumented and produced no .gcno at all - a gcovr filter for apps/cli
# came back with zero files rather than a low percentage. It is an INTERFACE
# target that adds nothing when AC3FORGE_ENABLE_COVERAGE is off (the default),
# so this costs every other configuration nothing.
target_link_libraries(ac3cli
    PRIVATE
        ac3::forge
        ac3::audio
        ac3::signing
        matroska::matroska
        mp4::mp4
        mpegts::mpegts
        ac3::warnings
        ac3::fmt
        ac3::coverage)

# ac3::forge, matroska::matroska, mp4::mp4 and mpegts::mpegts are link-only here, but
# installed and packaged in their own right elsewhere - see
# cmake/InstallLibrary.cmake and cmake/Packaging.cmake's `library` CPack
# component, and docs/library/index.md for how a third party consumes them
# via find_package(ac3forge). ac3::audio stays link-only and unpackaged
# always - a CLI/GUI implementation detail, not part of the distributed
# library.
#
# mp4::mp4/mpegts::mpegts are only ever unavailable when AC3FORGE_BUILD_MP4=OFF/
# AC3FORGE_BUILD_MPEGTS=OFF, which the root CMakeLists.txt's own FATAL_ERROR guards
# already refuse to combine with AC3FORGE_BUILD_CLI=ON - so unlike ac3::audio's
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
# neighbours), and src/ac3adm/CMakeLists.txt has an identical GCC-15 instance
# already scoped to just that target - this is the same category surfacing on
# GCC 16 instead, in this one file. Scoped to the file rather than widening
# either of those guards: nothing else in this project has hit it, so
# silencing it everywhere would risk hiding a genuine one elsewhere.
set_source_files_properties(commands/stream_tools.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-null-dereference>")

include(GNUInstallDirs)

# COMPONENT runtime: without one, CPack files this under its own
# "Unspecified" component, inconsistent once component-based packaging is
# on for the library (see cmake/InstallLibrary.cmake, cmake/Packaging.cmake).
install(TARGETS ac3cli
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime
    BUNDLE DESTINATION "." COMPONENT runtime)

# ---------------------------------------------------------------------------
# The man page and the four shell completions (roadmap IO8).
#
# GENERATED, not written: `ac3cli man` and `ac3cli completions <shell>` render
# both from main.cpp's own command table (see apps/cli/usage.cpp), so a new
# command or option reaches the man page, all four completion scripts and the
# --help text at once and cannot be forgotten in one of them. That is the
# whole reason to generate rather than check in five hand-maintained files.
#
# The cost of generating from the binary is that the binary has to run, which
# a cross build cannot do - the linux-gcc-arm64/linux-llvm-arm64 presets build
# aarch64 objects on an x86_64 host, and Emscripten builds no ac3cli at all.
# There, these files are simply not produced and not installed: a packaging
# run on the target arch (or a native build of the same source) produces
# them, and a cross-built .deb without a man page is a smaller loss than a
# build that fails outright or a checked-in file that silently goes stale.
# The status message says which happened rather than leaving it to be
# discovered from a missing file.
if(CMAKE_CROSSCOMPILING)
    message(STATUS "  CLI man page   : skipped (cross build cannot run ac3cli)")
else()
    set(AC3CLI_DOC_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${AC3CLI_DOC_DIR}")

    # DEPENDS ac3cli, not just $<TARGET_FILE:ac3cli>: the command table lives
    # in the binary, so the artefacts have to be regenerated whenever it is
    # relinked, not only when the file name changes.
    add_custom_command(
        OUTPUT "${AC3CLI_DOC_DIR}/ac3cli.1"
        COMMAND "${CMAKE_COMMAND}"
                "-DAC3_RUN_COMMAND=$<TARGET_FILE:ac3cli>;man"
                "-DAC3_RUN_OUTPUT=${AC3CLI_DOC_DIR}/ac3cli.1"
                -P "${PROJECT_SOURCE_DIR}/cmake/RunToFile.cmake"
        DEPENDS ac3cli
        COMMENT "Generating ac3cli.1 from ac3cli's own command table"
        VERBATIM)

    # <shell> -> the file name that shell's completion loader expects. bash
    # and zsh look their file up by a fixed name (the command / _command);
    # fish and PowerShell want a suffixed script. PowerShell has no
    # convention-driven search path at all, so its script goes under the
    # project's own share directory for the user to dot-source from $PROFILE
    # - which is what `ac3cli completions powershell` itself says.
    set(AC3CLI_COMPLETION_SHELLS bash zsh fish powershell)
    set(AC3CLI_COMPLETION_FILES ac3cli _ac3cli ac3cli.fish ac3cli.ps1)
    set(AC3CLI_COMPLETION_DESTS
        "${CMAKE_INSTALL_DATAROOTDIR}/bash-completion/completions"
        "${CMAKE_INSTALL_DATAROOTDIR}/zsh/site-functions"
        "${CMAKE_INSTALL_DATAROOTDIR}/fish/vendor_completions.d"
        "${CMAKE_INSTALL_DATAROOTDIR}/ac3forge/completions")

    set(AC3CLI_GENERATED_DOCS "${AC3CLI_DOC_DIR}/ac3cli.1")
    foreach(index RANGE 3)
        list(GET AC3CLI_COMPLETION_SHELLS ${index} shell)
        list(GET AC3CLI_COMPLETION_FILES ${index} completion_file)
        list(GET AC3CLI_COMPLETION_DESTS ${index} completion_dest)
        add_custom_command(
            OUTPUT "${AC3CLI_DOC_DIR}/${completion_file}"
            COMMAND "${CMAKE_COMMAND}"
                    "-DAC3_RUN_COMMAND=$<TARGET_FILE:ac3cli>;completions;${shell}"
                    "-DAC3_RUN_OUTPUT=${AC3CLI_DOC_DIR}/${completion_file}"
                    -P "${PROJECT_SOURCE_DIR}/cmake/RunToFile.cmake"
            DEPENDS ac3cli
            COMMENT "Generating ${shell} completions for ac3cli"
            VERBATIM)
        list(APPEND AC3CLI_GENERATED_DOCS "${AC3CLI_DOC_DIR}/${completion_file}")
        install(FILES "${AC3CLI_DOC_DIR}/${completion_file}"
            DESTINATION "${completion_dest}" COMPONENT runtime)
    endforeach()

    # ALL, so a plain `cmake --build` produces them: they are install()'d
    # unconditionally below, and an install of a target that was never built
    # is a hard CMake error rather than a missing file.
    add_custom_target(ac3cli_docs ALL DEPENDS ${AC3CLI_GENERATED_DOCS})

    install(FILES "${AC3CLI_DOC_DIR}/ac3cli.1"
        DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT runtime)
endif()
