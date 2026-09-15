# ---------------------------------------------------------------------------
# ac3::sendspin - the Sendspin protocol for Hearth (planning/hearth-reference-player.md,
# A4): the server half ac3hearth's engine uses, and the player half the test sink and
# the ESP32 sinks use, with the _ac3forge_player@v1 role on both
# (planning/hearth-sendspin-extension.md).
#
# What is here so far is the part with no dependency at all: JSON, base64url, the
# transport-mode framing and the audio chunk formats. Noise, CPace, the time filter,
# the WebSocket and mDNS seams and the sessions follow, each with the dependency it
# needs behind vcpkg's "hearth" feature.
#
# Written for the player half's hardest host, an ESP32 inside an HTTP server task:
# no thread_local, no recursion over input, no large stack frames, and allocations
# bounded by limits the caller sets (see the plan's constraints on src/sendspin).
#
# Always STATIC and never installed, like ac3::audio: no export annotations, and not
# part of the ac3forge-dev package.
# ---------------------------------------------------------------------------
add_library(ac3sendspin STATIC)
add_library(ac3::sendspin ALIAS ac3sendspin)

target_sources(ac3sendspin
    PRIVATE
        src/base64url.cpp
        src/chunks.cpp
        src/frames.cpp
        src/json.cpp)

target_include_directories(ac3sendspin
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

target_compile_features(ac3sendspin PUBLIC cxx_std_23)

target_link_libraries(ac3sendspin PRIVATE ac3::warnings ac3::coverage)

set_target_properties(ac3sendspin PROPERTIES POSITION_INDEPENDENT_CODE ON)
