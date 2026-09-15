# ac3hearth-testsink (planning/hearth-reference-player.md, The test sink): src/sendspin's player
# half as a program. The sink itself is a static library, so the loopback tests in ac3tests can run
# sinks in-process; the executable adds the command line. Never installed or packaged.
add_library(ac3hearth_testsink STATIC burst_output.cpp sink.cpp store.cpp wav_output.cpp)
target_include_directories(ac3hearth_testsink PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ac3hearth_testsink PUBLIC cxx_std_23)
target_link_libraries(ac3hearth_testsink PUBLIC ac3::sendspin ac3::forge)
target_link_libraries(ac3hearth_testsink PRIVATE ac3::warnings)

add_executable(ac3hearth-testsink main.cpp)
target_link_libraries(ac3hearth-testsink PRIVATE ac3hearth_testsink ac3::warnings)
