# ---------------------------------------------------------------------------
# iclforge::render - from what a decoder produced to what the speakers want: the room's layout and its
# text form, routing and per-output trim and delay, the identify tone, the bed and object renderer and
# the panning math under it. Mostly headers; spatial.cpp holds the panner.
# It links no codec: a decoder hands it PcmBlocks (iclforge/render/pcm_block.hpp).
# ---------------------------------------------------------------------------
# The minimum-footprint profile compiles what it needs into one archive (src/ac3/minimal.cmake),
# so no library is built here for it.
if(ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER)
    return()
endif()

iclforge_add_library(render
    SOURCES
        src/spatial.cpp
    DEPENDS base objects
    PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    LINK_PRIVATE
        "$<BUILD_INTERFACE:iclforge::tracy>")
