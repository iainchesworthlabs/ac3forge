# hearth-testserver (planning/hearth-reference-player.md, B3 and B4): src/sendspin's server host
# as a program that dials players, pairs them, sends them settings, plays one programme to them as a
# group and reports what they said about it. Until the engine plays to boards, this is what drives
# them, from a computer and in CI. It can run a test sink of its own, the reference a board's levels
# are held to, so it links the sink's library. Never installed or packaged. It reads a board's
# /status with cpp-httplib, configured as src/sendspin configures it (iclforge_sendspin_httplib).
add_executable(hearth-testserver main.cpp server_store.cpp)
target_include_directories(hearth-testserver PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(hearth-testserver PRIVATE cxx_std_23)
target_link_libraries(hearth-testserver PRIVATE hearth_testsink iclforge_sendspin_httplib iclforge::warnings)
if(WIN32)
    target_link_libraries(hearth-testserver PRIVATE ws2_32)
endif()
