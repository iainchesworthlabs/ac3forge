# ---------------------------------------------------------------------------
# ac3::sendspin - the Sendspin protocol for Hearth (planning/hearth-reference-player.md,
# A4): the server half ac3hearth's engine uses, and the player half the test sink and
# the ESP32 sinks use, with the _ac3forge_player@v1 role on both
# (planning/hearth-sendspin-extension.md).
#
# Two layers. The core needs nothing: JSON, base64url, the transport-mode framing, the
# audio chunk formats, the handshake-phase and core messages, pairing tokens, admission
# between servers, and the transport seam with its in-memory pair. The rest takes its dependencies from vcpkg's "hearth"
# feature on a computer: mbedTLS for the crypto seam over the PSA Crypto API and the Noise,
# PSK, CPace and pairing code above it, and cpp-httplib for the WebSocket transport.
# AC3FORGE_SENDSPIN_CORE_ONLY builds the core alone, which is what fuzz/run.sh does so the
# fuzz build keeps no vcpkg dependency.
#
# Written for the player half's hardest host, an ESP32 inside an HTTP server task:
# no thread_local, no recursion over input, no large stack frames, and allocations
# bounded by limits the caller sets (see the plan's constraints on src/sendspin). The
# WebSocket transport over cpp-httplib is the exception, since only a computer builds it.
#
# Always STATIC and never installed, like ac3::audio: no export annotations, and not
# part of the ac3forge-dev package.
# ---------------------------------------------------------------------------
option(AC3FORGE_SENDSPIN_CORE_ONLY
    "Build only ac3::sendspin's dependency-free core, leaving out the parts that need vcpkg's \"hearth\" feature"
    OFF)

# Sendspin's reference time filter, vendored unmodified (third_party/time-filter/VENDORED.md).
# A target of its own so the project's warning set, which upstream's code was not written
# against, does not apply to it; its headers reach consumers as SYSTEM includes for the same
# reason.
add_library(ac3sendspin_time_filter STATIC third_party/time-filter/sendspin_time_filter.cpp)
target_include_directories(ac3sendspin_time_filter SYSTEM
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/time-filter>")
target_compile_features(ac3sendspin_time_filter PUBLIC cxx_std_17)
set_target_properties(ac3sendspin_time_filter PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(ac3sendspin STATIC)
add_library(ac3::sendspin ALIAS ac3sendspin)

target_sources(ac3sendspin
    PRIVATE
        src/arbiter.cpp
        src/base64.cpp
        src/base64url.cpp
        src/chunks.cpp
        src/clock_sync.cpp
        src/discovery.cpp
        src/frames.cpp
        src/handshake.cpp
        src/json.cpp
        src/messages.cpp
        src/pairing_messages.cpp
        src/pairing_token.cpp
        src/suite.cpp
        src/transport_memory.cpp)

if(NOT AC3FORGE_SENDSPIN_CORE_ONLY)
    find_package(MbedTLS CONFIG REQUIRED)
    find_package(httplib CONFIG REQUIRED)
    # mdns is one C header. Its package's target asks for a C99 compiler, which this C++-only
    # project does not enable, so the header is found directly, as the port's usage says.
    find_path(AC3FORGE_MDNS_INCLUDE_DIR "mdns.h" REQUIRED)
    # The seams' backends: one directory per backend, chosen here rather than by a
    # preprocessor branch. crypto/psa/ is the PSA Crypto API, which mbedTLS 3.6 and 4 both
    # provide; transport/httplib/ is the WebSocket transport over cpp-httplib, and
    # discovery/mdns/ DNS-SD over mjansson's mdns, both for a computer (a board brings its
    # own over esp_http_server and espressif/mdns).
    target_sources(ac3sendspin
        PRIVATE
            src/channel.cpp
            src/cpace.cpp
            src/crypto/psa/crypto.cpp
            src/discovery/mdns/mdns.cpp
            src/discovery/mdns/packets.cpp
            src/handshake_session.cpp
            src/hmac.cpp
            src/noise.cpp
            src/pairing.cpp
            src/pairing_flow.cpp
            src/player_session.cpp
            src/psk.cpp
            src/server_session.cpp
            src/session_driver.cpp
            src/transport/httplib/websocket.cpp)
    # The listening socket's options and the mDNS sockets' system calls, which differ by
    # platform (listen_socket.hpp, discovery/mdns/platform.hpp).
    if(WIN32)
        target_sources(ac3sendspin
            PRIVATE
                src/discovery/mdns/platform_windows.cpp
                src/transport/httplib/listen_socket_windows.cpp)
        target_link_libraries(ac3sendspin PRIVATE iphlpapi ws2_32)
    else()
        target_sources(ac3sendspin
            PRIVATE
                src/discovery/mdns/platform_posix.cpp
                src/transport/httplib/listen_socket_posix.cpp)
    endif()
    target_include_directories(ac3sendspin SYSTEM PRIVATE "${AC3FORGE_MDNS_INCLUDE_DIR}")
    target_link_libraries(ac3sendspin PRIVATE MbedTLS::mbedcrypto httplib::httplib)
    # cpp-httplib is configured by macros, which reach every file here that includes it.
    # Code elsewhere in the same program that included cpp-httplib would need the same
    # values, since its inline functions would otherwise differ between the two.
    #   - A Sendspin WebSocket message is one Noise transport message or one handshake-phase
    #     JSON text, never over 65535 bytes (messaging.md, Binary Messages). A peer that
    #     announces a longer one is disconnected before anything is allocated for it.
    #   - close() waits for the peer's answering Close frame when no reader is waiting for
    #     it. Upstream's 5 s would be what Listener::stop() waits out for a peer gone quiet.
    target_compile_definitions(ac3sendspin
        PRIVATE
            CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH=65535
            CPPHTTPLIB_WEBSOCKET_CLOSE_TIMEOUT_SECOND=1)
endif()

target_include_directories(ac3sendspin
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

target_compile_features(ac3sendspin PUBLIC cxx_std_23)

target_link_libraries(ac3sendspin PUBLIC ac3sendspin_time_filter)
target_link_libraries(ac3sendspin PRIVATE ac3::warnings ac3::coverage)

set_target_properties(ac3sendspin PROPERTIES POSITION_INDEPENDENT_CODE ON)
