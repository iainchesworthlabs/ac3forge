# ---------------------------------------------------------------------------
# iclforge::dsp - the signal-processing pieces more than one library uses: the radix-2 FFT kernel, the
# 64-band complex QMF bank, the offline sample-rate converter and the second-order filter sections.
# It knows no codec. The transforms that belong to one codec (AC-3's 512-sample MDCT, AC-4's KBD
# windows) stay with that codec; folding the duplicated FFTs and QMF banks of ac3 and iclforge_ac4core into this
# library is a later phase (planning/layout.md, "What stays out").
# ---------------------------------------------------------------------------
# The minimum-footprint profile compiles what it needs into one archive (src/ac3/minimal.cmake),
# so no library is built here for it.
if(AC3FORGE_MINIMAL_DECODER OR AC3FORGE_MINIMAL_ENCODER)
    return()
endif()

iclforge_add_library(dsp
    SOURCES
        src/biquad.cpp
        src/fft.cpp
        src/qmf.cpp
        src/resampler.cpp
    DEPENDS base
    PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    LINK_PRIVATE
        "$<BUILD_INTERFACE:ac3::tracy>"
        "$<BUILD_INTERFACE:iclforge::arithmetic>")
