# ac3hearth-testsink (planning/hearth-reference-player.md, The test sink): src/sendspin's player
# half as a program. The sink itself is a static library, so the loopback tests in ac3tests can run
# sinks in-process; the executable adds the command line. Never installed or packaged.
add_library(ac3hearth_testsink STATIC burst_output.cpp sink.cpp store.cpp wav_output.cpp)
target_include_directories(ac3hearth_testsink PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ac3hearth_testsink PUBLIC cxx_std_23)
# ac4::decoder and ac4::ac4 for the extension role's AC-4 bursts (planning/ac4.md, D11): the
# decoder, and the inspector's sync-frame scan in front of it.
target_link_libraries(ac3hearth_testsink PUBLIC ac3::sendspin ac3::forge ac4::decoder ac4::ac4)
# ac3::coverage: this library's sources are also linked straight into ac3tests (tests/CMakeLists.txt),
# and need their own instrumentation for the same reason apps/hearth/engine's own link does -
# --coverage is compile-time, not inherited from whatever later links this archive.
target_link_libraries(ac3hearth_testsink PRIVATE ac3::warnings ac3::coverage)

add_executable(ac3hearth-testsink main.cpp)
target_link_libraries(ac3hearth-testsink PRIVATE ac3hearth_testsink ac3::warnings)
