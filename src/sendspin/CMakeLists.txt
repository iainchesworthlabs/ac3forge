# ---------------------------------------------------------------------------
# ac3::sendspin - the Sendspin protocol for Hearth (planning/hearth-reference-player.md,
# A4): the server half ac3hearth's engine uses, and the player half the test sink and
# the ESP32 sinks use, with the _ac3forge_player@v1 role on both
# (planning/hearth-sendspin-extension.md).
#
# Two layers. The core needs nothing: JSON, base64url, the transport-mode framing, the
# audio chunk formats, the handshake-phase and core messages, pairing tokens, admission
# between servers, PCM, and the transport seam with its in-memory pair. The rest takes its
# dependencies from vcpkg's "hearth" feature on a computer: mbedTLS for the crypto seam over
# the PSA Crypto API and the Noise, PSK, CPace and pairing code above it, cpp-httplib for the
# WebSocket transport, mdns for discovery, and libFLAC and Opus for player@v1's codecs.
# AC3FORGE_SENDSPIN_CORE_ONLY builds the core alone, which is what fuzz/run.sh does so the
# fuzz build keeps no vcpkg dependency.
#
# AC3FORGE_SENDSPIN_PLAYER_ONLY builds the core and the player half, which is what a board
# runs (planning/hearth-reference-player.md, B3): the Noise responder, pairing, the clock and
# the player session, with the crypto seam over whichever mbedTLS the board's SDK provides,
# named by AC3FORGE_SENDSPIN_MBEDTLS_TARGET. The ESP-IDF component sets both
# (esp-idf/ac3forge/CMakeLists.txt) and brings its own WebSocket and mDNS. No vcpkg.
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
option(AC3FORGE_SENDSPIN_PLAYER_ONLY
    "Build ac3::sendspin's core and player half only, for a board, with the crypto seam over AC3FORGE_SENDSPIN_MBEDTLS_TARGET"
    OFF)
set(AC3FORGE_SENDSPIN_MBEDTLS_TARGET "" CACHE STRING
    "With AC3FORGE_SENDSPIN_PLAYER_ONLY, the CMake target that provides mbedTLS's PSA Crypto API (idf::mbedtls on ESP-IDF); empty to find MbedTLS as a package")
if(AC3FORGE_SENDSPIN_CORE_ONLY AND AC3FORGE_SENDSPIN_PLAYER_ONLY)
    message(FATAL_ERROR "AC3FORGE_SENDSPIN_CORE_ONLY and AC3FORGE_SENDSPIN_PLAYER_ONLY are exclusive: "
                        "the player half is the core and more")
endif()

# A board's build is position-dependent, as the minimum-footprint library beside it is; a
# computer's links these archives into executables and shared test binaries alike.
if(AC3FORGE_SENDSPIN_PLAYER_ONLY)
    set(_ac3_sendspin_pic OFF)
else()
    set(_ac3_sendspin_pic ON)
endif()

# Sendspin's reference time filter, vendored unmodified (third_party/time-filter/VENDORED.md).
# A target of its own so the project's warning set, which upstream's code was not written
# against, does not apply to it; its headers reach consumers as SYSTEM includes for the same
# reason.
add_library(ac3sendspin_time_filter STATIC third_party/time-filter/sendspin_time_filter.cpp)
target_include_directories(ac3sendspin_time_filter SYSTEM
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/time-filter>")
target_compile_features(ac3sendspin_time_filter PUBLIC cxx_std_17)
set_target_properties(ac3sendspin_time_filter PROPERTIES POSITION_INDEPENDENT_CODE ${_ac3_sendspin_pic})

add_library(ac3sendspin STATIC)
add_library(ac3::sendspin ALIAS ac3sendspin)

target_sources(ac3sendspin
    PRIVATE
        src/ac3forge_player.cpp
        src/arbiter.cpp
        src/base64.cpp
        src/base64url.cpp
        src/chunks.cpp
        src/codec/pcm.cpp
        src/clock_sync.cpp
        src/discovery.cpp
        src/frames.cpp
        src/handshake.cpp
        src/json.cpp
        src/messages.cpp
        src/pairing_messages.cpp
        src/pairing_token.cpp
        src/state_roles.cpp
        src/stream_roles.cpp
        src/suite.cpp
        src/transport_memory.cpp)

# The player half beyond the core: everything a player needs that takes the crypto seam, and
# nothing that needs an operating system's sockets or threads. crypto/psa/ is the seam's one
# backend, over the PSA Crypto API, which mbedTLS 3.6 and 4 both provide.
set(_ac3_sendspin_player_sources
    src/channel.cpp
    src/cpace.cpp
    src/handshake_session.cpp
    src/hmac.cpp
    src/noise.cpp
    src/pairing.cpp
    src/pairing_flow.cpp
    src/player_session.cpp
    src/psk.cpp)
set(_ac3_sendspin_crypto_sources src/crypto/psa/crypto.cpp)

if(AC3FORGE_SENDSPIN_PLAYER_ONLY)
    target_sources(ac3sendspin PRIVATE ${_ac3_sendspin_player_sources})
    # The backend is an archive of its own there, compiled against the SDK's mbedTLS without
    # the project's warning set: the SDK's headers, and the C library headers they bring, were
    # not written against -Wpedantic, and only this file includes them.
    add_library(ac3sendspin_crypto STATIC ${_ac3_sendspin_crypto_sources})
    target_include_directories(ac3sendspin_crypto PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(ac3sendspin_crypto PRIVATE cxx_std_23)
    if(AC3FORGE_SENDSPIN_MBEDTLS_TARGET)
        target_link_libraries(ac3sendspin_crypto PRIVATE ${AC3FORGE_SENDSPIN_MBEDTLS_TARGET})
    else()
        find_package(MbedTLS CONFIG REQUIRED)
        target_link_libraries(ac3sendspin_crypto PRIVATE MbedTLS::mbedcrypto)
    endif()
    target_link_libraries(ac3sendspin PRIVATE ac3sendspin_crypto)
    # A board's library is built with the minimum-footprint profile's options, where there is
    # one: no exceptions, no RTTI, a section per function. The board's code shares them.
    if(TARGET ac3::minimal_profile)
        target_link_libraries(ac3sendspin PUBLIC ac3::minimal_profile)
        target_link_libraries(ac3sendspin_time_filter PRIVATE ac3::minimal_profile)
        target_link_libraries(ac3sendspin_crypto PRIVATE ac3::minimal_profile)
    endif()
elseif(NOT AC3FORGE_SENDSPIN_CORE_ONLY)
    find_package(MbedTLS CONFIG REQUIRED)
    find_package(httplib CONFIG REQUIRED)
    find_package(FLAC CONFIG REQUIRED)
    find_package(Opus CONFIG REQUIRED)
    # mdns is one C header. Its package's target asks for a C99 compiler, which this C++-only
    # project does not enable, so the header is found directly, as the port's usage says.
    find_path(AC3FORGE_MDNS_INCLUDE_DIR "mdns.h" REQUIRED)
    # The seams' other backends: one directory per backend, chosen here rather than by a
    # preprocessor branch. transport/httplib/ is the WebSocket transport over cpp-httplib, and
    # discovery/mdns/ DNS-SD over mjansson's mdns, both for a computer (a board brings its
    # own over esp_http_server and espressif/mdns).
    target_sources(ac3sendspin
        PRIVATE
            ${_ac3_sendspin_player_sources}
            ${_ac3_sendspin_crypto_sources}
            src/codec/codec.cpp
            src/codec/flac/flac.cpp
            src/codec/opus/opus.cpp
            src/discovery/mdns/mdns.cpp
            src/discovery/mdns/packets.cpp
            src/server_host.cpp
            src/server_session.cpp
            src/server_store.cpp
            src/session_driver.cpp
            src/transport/httplib/websocket.cpp)
    # The listening socket's options and the mDNS sockets' system calls, which differ by
    # platform (listen_socket.hpp, discovery/mdns/platform.hpp), and the Windows Firewall
    # exception a listening or mDNS socket needs on Windows only (ac3/sendspin/firewall.hpp).
    if(WIN32)
        target_sources(ac3sendspin
            PRIVATE
                src/discovery/mdns/platform_windows.cpp
                src/platform/firewall_windows.cpp
                src/transport/httplib/listen_socket_windows.cpp)
        target_link_libraries(ac3sendspin PRIVATE iphlpapi ole32 oleaut32 shell32 ws2_32)
    else()
        target_sources(ac3sendspin
            PRIVATE
                src/discovery/mdns/platform_posix.cpp
                src/platform/firewall_posix.cpp
                src/transport/httplib/listen_socket_posix.cpp)
    endif()
    target_include_directories(ac3sendspin SYSTEM PRIVATE "${AC3FORGE_MDNS_INCLUDE_DIR}")
    # cpp-httplib is configured by macros, which must have the same values in every file of a
    # program that includes it, since its inline functions would otherwise differ between them.
    # So every target that includes it links ac3sendspin_httplib rather than httplib::httplib:
    # this library, the test server, and the tests of the transport.
    #   - A Sendspin WebSocket message is one Noise transport message or one handshake-phase
    #     JSON text, never over 65535 bytes (messaging.md, Binary Messages). A peer that
    #     announces a longer one is disconnected before anything is allocated for it.
    #   - close() waits for the peer's answering Close frame when no reader is waiting for
    #     it. Upstream's 5 s would be what Listener::stop() waits out for a peer gone quiet.
    add_library(ac3sendspin_httplib INTERFACE)
    target_link_libraries(ac3sendspin_httplib INTERFACE httplib::httplib)
    target_compile_definitions(ac3sendspin_httplib
        INTERFACE
            CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH=65535
            CPPHTTPLIB_WEBSOCKET_CLOSE_TIMEOUT_SECOND=1)
    target_link_libraries(ac3sendspin PRIVATE MbedTLS::mbedcrypto ac3sendspin_httplib FLAC::FLAC Opus::opus)
endif()

target_include_directories(ac3sendspin
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

target_compile_features(ac3sendspin PUBLIC cxx_std_23)

target_link_libraries(ac3sendspin PUBLIC ac3sendspin_time_filter)
target_link_libraries(ac3sendspin PRIVATE ac3::warnings ac3::coverage)

set_target_properties(ac3sendspin PROPERTIES POSITION_INDEPENDENT_CODE ${_ac3_sendspin_pic})
