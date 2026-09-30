# ---------------------------------------------------------------------------
# iclforge::iec61937 - IEC 61937 burst packing and unpacking: an access unit of AC-3, E-AC-3 or AC-4
# disguised as PCM for an S/PDIF or HDMI sink, and the detector that finds one in a capture. The
# packers take the frame's facts as arguments; the library links no codec.
# ---------------------------------------------------------------------------
# The minimum-footprint profile compiles what it needs into one archive (src/ac3/minimal.cmake),
# so no library is built here for it.
if(ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER)
    return()
endif()

iclforge_add_library(iec61937
    SOURCES
        src/iec61937.cpp
    DEPENDS base
    PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    LINK_PRIVATE
        "$<BUILD_INTERFACE:iclforge::tracy>")
