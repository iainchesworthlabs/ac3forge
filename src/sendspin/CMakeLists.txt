# ---------------------------------------------------------------------------
# ac3::sendspin - the Sendspin protocol for Hearth (planning/hearth-reference-player.md,
# A4): the server half ac3hearth's engine uses, and the player half the test sink and
# the ESP32 sinks use, with the _ac3forge_player@v1 role on both
# (planning/hearth-sendspin-extension.md).
#
# Two layers. The core needs nothing: JSON, base64url, the transport-mode framing, the
# audio chunk formats and the handshake-phase messages. The rest - the crypto seam over
# the PSA Crypto API, Noise and PSK identities - needs mbedTLS, which a computer build
# takes from vcpkg's "hearth" feature. AC3FORGE_SENDSPIN_CORE_ONLY builds the core
# alone, which is what fuzz/run.sh does so the fuzz build keeps no vcpkg dependency.
#
# Written for the player half's hardest host, an ESP32 inside an HTTP server task:
# no thread_local, no recursion over input, no large stack frames, and allocations
# bounded by limits the caller sets (see the plan's constraints on src/sendspin).
#
# Always STATIC and never installed, like ac3::audio: no export annotations, and not
# part of the ac3forge-dev package.
# ---------------------------------------------------------------------------
option(AC3FORGE_SENDSPIN_CORE_ONLY
    "Build only ac3::sendspin's dependency-free core, leaving out the parts that need vcpkg's \"hearth\" feature"
    OFF)

add_library(ac3sendspin STATIC)
add_library(ac3::sendspin ALIAS ac3sendspin)

target_sources(ac3sendspin
    PRIVATE
        src/base64url.cpp
        src/chunks.cpp
        src/frames.cpp
        src/handshake.cpp
        src/json.cpp
        src/pairing_token.cpp
        src/suite.cpp)

if(NOT AC3FORGE_SENDSPIN_CORE_ONLY)
    find_package(MbedTLS CONFIG REQUIRED)
    # The crypto seam's backend: one directory per backend, chosen here rather than by
    # a preprocessor branch. psa/ is the PSA Crypto API, which mbedTLS 3.6 and 4 both
    # provide.
    target_sources(ac3sendspin
        PRIVATE
            src/cpace.cpp
            src/crypto/psa/crypto.cpp
            src/hmac.cpp
            src/noise.cpp
            src/pairing.cpp
            src/psk.cpp)
    target_link_libraries(ac3sendspin PRIVATE MbedTLS::mbedcrypto)
endif()

target_include_directories(ac3sendspin
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

target_compile_features(ac3sendspin PUBLIC cxx_std_23)

target_link_libraries(ac3sendspin PRIVATE ac3::warnings ac3::coverage)

set_target_properties(ac3sendspin PROPERTIES POSITION_INDEPENDENT_CODE ON)
