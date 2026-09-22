# ac3hearth-testserver (planning/hearth-reference-player.md, B3 and B4): src/sendspin's server host
# as a program that dials players, pairs them, sends them settings, plays one programme to them as a
# group and reports what they said about it. Until the engine plays to boards, this is what drives
# them, from a computer and in CI. It can run a test sink of its own, the reference a board's levels
# are held to, so it links the sink's library. Never installed or packaged. It reads a board's
# /status with cpp-httplib, configured as src/sendspin configures it (ac3sendspin_httplib).
add_executable(ac3hearth-testserver main.cpp server_store.cpp)
target_include_directories(ac3hearth-testserver PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(ac3hearth-testserver PRIVATE cxx_std_23)
target_link_libraries(ac3hearth-testserver PRIVATE ac3hearth_testsink ac3sendspin_httplib ac3::warnings)
if(WIN32)
    target_link_libraries(ac3hearth-testserver PRIVATE ws2_32)
endif()
