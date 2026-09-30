# ---------------------------------------------------------------------------
# iclforge::objects - the object-audio model: an object's position and its path over time (the
# keyframe scene, its JSON, text and OSC readers), the placement an encoder is handed each frame, and
# the Object Audio Metadata payload of ETSI TS 103 420 clause 5 (build and parse) and the EMDF
# container it travels in. Codec-blind: E-AC-3 JOC and the Atmos encoder that use it stay in
# iclforge::ac3.
# ---------------------------------------------------------------------------
# The minimum-footprint profile compiles what it needs into one archive (src/ac3/minimal.cmake),
# so no library is built here for it.
if(ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER)
    return()
endif()

iclforge_add_library(objects
    SOURCES
        src/emdf.cpp
        src/motion.cpp
        src/oamd.cpp
        src/scene.cpp
        src/scene_json.cpp
        src/scene_osc.cpp
        src/scene_text.cpp
    DEPENDS base dsp
    PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    LINK_PRIVATE
        "$<BUILD_INTERFACE:iclforge::tracy>"
        "$<BUILD_INTERFACE:iclforge::fmt_private>")
