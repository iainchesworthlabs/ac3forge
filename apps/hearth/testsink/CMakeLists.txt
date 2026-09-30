# hearth-testsink (planning/hearth-reference-player.md, The test sink): src/sendspin's player
# half as a program. The sink itself is a static library, so the loopback tests in iclforge-tests can run
# sinks in-process; the executable adds the command line. Never installed or packaged.
add_library(hearth_testsink STATIC burst_output.cpp sink.cpp store.cpp wav_output.cpp)
target_include_directories(hearth_testsink PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(hearth_testsink PUBLIC cxx_std_23)
# iclforge::ac4dec and iclforge::ac4 for the extension role's AC-4 bursts (planning/ac4.md, D11): the
# decoder, and the inspector's sync-frame scan in front of it.
target_link_libraries(hearth_testsink PUBLIC iclforge::sendspin iclforge::ac3 iclforge::ac4dec iclforge::ac4)
# iclforge::coverage: this library's sources are also linked straight into iclforge-tests (tests/CMakeLists.txt),
# and need their own instrumentation for the same reason apps/hearth/engine's own link does -
# --coverage is compile-time, not inherited from whatever later links this archive.
target_link_libraries(hearth_testsink PRIVATE iclforge::warnings iclforge::coverage)

add_executable(hearth-testsink main.cpp)
target_link_libraries(hearth-testsink PRIVATE hearth_testsink iclforge::warnings)
