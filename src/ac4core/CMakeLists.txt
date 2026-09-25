# ---------------------------------------------------------------------------
# ac4::core - what the AC-4 decoder (src/ac4dec) and the AC-4 encoder compute
# alike, built and tested once (planning/ac4.md, "Where the libraries live"):
# the tables generated from the ETSI attachments, and the transforms.
#
# A static library of position-independent code with hidden symbols, linked
# PRIVATE by the libraries that use it: each shared library carries the part
# it uses, a static build links it once, and the core has no public headers
# and no ABI of its own. Its headers sit under src/ beside its sources, and
# the libraries that link it include them by the path from there
# ("tables/huffman_tables.hpp", "dsp/mdct.hpp"). Everything in it is in
# namespace ac4::detail.
#
# The syntax is not here: the decoder's reader and the encoder's writer are
# separate transcriptions of the syntax tables, in opposite directions.
# ---------------------------------------------------------------------------
add_library(ac4core STATIC)
add_library(ac4::core ALIAS ac4core)

target_sources(ac4core
    PRIVATE
        src/acpl/acpl.cpp
        src/aspx/frequency_tables.cpp
        src/aspx/hf_generator.cpp
        src/dsp/fft.cpp
        src/dsp/kbd.cpp
        src/dsp/mdct.cpp
        src/dsp/qmf.cpp
        src/dsp/synthesis.cpp
        src/tables/huffman_codes.cpp
        src/tables/huffman_tables.cpp
        src/tables/noise_tables.cpp
        src/tables/qmf_tables.cpp
        src/tables/sfb_tables.cpp)

target_include_directories(ac4core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")

target_compile_features(ac4core PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> for the reason src/ac4/CMakeLists.txt gives.
target_link_libraries(ac4core
    PRIVATE
        "$<BUILD_INTERFACE:ac3::warnings>"
        "$<BUILD_INTERFACE:ac3::coverage>")

set_target_properties(ac4core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "ac4core")
