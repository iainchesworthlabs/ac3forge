# The Windows Desktop Atmos Demo (roadmap UX11): docs/platforms/windows-demo.md.
#
# engine/ is the headless part - what the UI drives and the spikes proved -
# split in two the way the CLI's own platform/ directory is: the pure modules
# (slot plan, bed mixer, placement, output policy) have no Windows in them and
# are ALSO compiled into ac3tests on every platform (tests/CMakeLists.txt);
# the modules under engine/platform/windows/ talk to the session manager, the
# policy-config interface and the shell, and only ever build here. The rest
# of engine/ (taps, output stage, signing hook, the frame loop) is written
# against ac3::audio and ac3::signing and would compile anywhere, but only
# makes sense here, so it rides the same option.

add_library(ac3windemo_engine STATIC
    engine/slots.cpp
    engine/bed_mixer.cpp
    engine/placement.cpp
    engine/output_policy.cpp
    engine/tap_pool.cpp
    engine/output_stage.cpp
    engine/signing_hook.cpp
    engine/engine.cpp)
add_library(ac3::windemo_engine ALIAS ac3windemo_engine)
target_include_directories(ac3windemo_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/engine")
target_compile_features(ac3windemo_engine PUBLIC cxx_std_23)
target_link_libraries(ac3windemo_engine
    PUBLIC ac3::forge ac3::audio ac3::signing
    PRIVATE ac3::warnings)

# The platform half, selected the way apps/cli/CMakeLists.txt selects its
# platform/ directory. This whole directory is already WIN32-only (the root
# CMakeLists.txt's guard), so the branch has one arm today; it is a branch so
# that a second platform is a directory, not a conditional in a source file.
if(WIN32)
    target_sources(ac3windemo_engine PRIVATE
        engine/platform/windows/session_monitor.cpp
        engine/platform/windows/default_device.cpp
        engine/platform/windows/foreground.cpp)
    target_compile_definitions(ac3windemo_engine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    # COM for the session manager and policy config, the shell for the
    # full-screen query and the Settings fallback.
    target_link_libraries(ac3windemo_engine PRIVATE ole32 shell32)
endif()

# The console runner: Phase 2's exit criterion, and the thing the UI replaces.
add_executable(ac3windemo runner/main.cpp)
target_link_libraries(ac3windemo PRIVATE ac3::windemo_engine ac3::warnings)
target_compile_definitions(ac3windemo PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(ac3windemo PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
