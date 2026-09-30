# ---------------------------------------------------------------------------
# iclforge::ac4core - what the AC-4 decoder (src/ac4dec) and the AC-4 encoder compute
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
#
# Installed and exported as iclforge::ac4core wherever the static decoder and encoder
# are (cmake/InstallLibrary.cmake): libac4dec_static.a and libac4enc_static.a
# call into it and do not contain it, as no archive contains what it links, so
# their exported targets name it as a link-only dependency and a consumer links
# it with them. The archive is libac4core_static.a, with the suffix every
# static archive the package installs has, by which
# tools/checks/check_install_consumer.sh and ac3forge_install_pkgconfig()
# (cmake/PkgConfig.cmake) tell an archive. It has no headers to install.
# ---------------------------------------------------------------------------
add_library(iclforge_ac4core STATIC)
add_library(iclforge::ac4core ALIAS iclforge_ac4core)

target_sources(iclforge_ac4core
    PRIVATE
        src/acpl/acpl.cpp
        src/ajcc/ajcc.cpp
        src/ajoc/ajoc.cpp
        src/aspx/frequency_tables.cpp
        src/aspx/hf_generator.cpp
        src/dsp/fft.cpp
        src/dsp/kbd.cpp
        src/dsp/mdct.cpp
        src/dsp/qmf.cpp
        src/dsp/resampler.cpp
        src/dsp/synthesis.cpp
        src/tables/huffman_codes.cpp
        src/tables/huffman_tables.cpp
        src/tables/isf_tables.cpp
        src/tables/noise_tables.cpp
        src/tables/qmf_tables.cpp
        src/tables/sfb_tables.cpp)

target_include_directories(iclforge_ac4core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

target_compile_features(iclforge_ac4core PUBLIC cxx_std_23)

# The scalar ac4core's kernels are explicitly instantiated at (planning/ac4.md,
# D14a: "AC-4 joins AC3FORGE_DECODE_SCALAR"). The same cache variable
# src/ac3/CMakeLists.txt declares and resolves its own decode_scalar_t from -
# declared again here with the identical default/STRINGS (CMake's set(...
# CACHE ...) only takes effect the first time it runs in a configure, so
# whichever of the two subdirectories add_subdirectory() reaches first is the
# one that actually sets it) so that src/ac4core configures correctly even in
# a tree that builds AC-4 without iclforge::ac3.
#
# "fixed" maps to the double directory rather than its own for now: AC-4's
# fixed-point tier (planning/ac4.md, D14d) has not instantiated these kernels
# at ac3::internal::Fixed32 yet, so there is nothing for a fixed32/ directory
# here to hold; a build configured AC3FORGE_DECODE_SCALAR=fixed still needs
# iclforge_ac4core to produce something, and double is the correct fallback (it is
# what every non-float build already used before this option existed).
set(AC3FORGE_DECODE_SCALAR "double" CACHE STRING
    "Scalar the decoder carries coefficients in: double (default), float, or fixed (Q7.24 in an int32, for parts with no FPU)")
set_property(CACHE AC3FORGE_DECODE_SCALAR PROPERTY STRINGS double float fixed)
if(AC3FORGE_DECODE_SCALAR STREQUAL "float")
    set(_ac4core_scalar_dir "float")
else()
    set(_ac4core_scalar_dir "double")
endif()

# AC4CORE_ALSO_AT_DOUBLE: the encoder (src/ac4enc) calls straight into a handful
# of these same kernels (Mdct, QmfAnalysis, QmfSynthesis, Resampler,
# ajoc::Reconstruction, aspx::generate_high_band) always at double - decision
# 34 (planning/ac4.md), the encoder never runs on an ESP32 and has no float
# or fixed-point tier of its own, so it does not follow AC3FORGE_DECODE_SCALAR.
# iclforge::ac3 avoids this by compiling its encoder's and decoder's DSP
# separately (AC3FORGE_ENCODE_SCALAR beside AC3FORGE_DECODE_SCALAR,
# src/ac3/CMakeLists.txt); iclforge_ac4core is one static library both ac4dec and
# ac4enc link (this file's own header comment), so when Real is not already
# double, each of those kernels' .cpp files also explicitly instantiates
# <double>. That is done through a macro the per-scalar ac4/detail/real.hpp
# defines (src/internal/scalar/<double|float>/, chosen just above): it passes
# its argument through where Real is float and drops it where Real is already
# double (a second explicit instantiation of the same specialization would be
# ill-formed), so no source file asks which build it is with a preprocessor
# conditional (tools/checks/check_platform_macros.ps1's rule). The default,
# double, build is unchanged: one instantiation, as it always was.

# PUBLIC, not PRIVATE: ac4core's own PUBLIC headers (aspx/hf_generator.hpp,
# acpl/acpl.hpp, ajcc/ajcc.hpp, ajoc/ajoc.hpp, dsp/qmf.hpp, dsp/mdct.hpp,
# dsp/fft.hpp, dsp/resampler.hpp, dsp/synthesis.hpp) #include
# "ac4/detail/real.hpp" themselves, and src/ac4dec and src/ac4enc compile
# against those headers through iclforge::ac4core as a COMPILE_ONLY dependency (their
# own CMakeLists.txt) - a consumer that only ever compiles against a target,
# never links it, still needs that target's PUBLIC usage requirements, this
# include directory among them, or its own translation units cannot resolve
# the header ac4core's PUBLIC header just asked for.
target_include_directories(iclforge_ac4core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/variants/scalar-${_ac4core_scalar_dir}>")

# Which "ac4/detail/profiling.hpp" the kernels' AC4_ZONE_SCOPED_N() markers see
# (planning/ac4.md, D14b): none/, where they expand to nothing, or stage_timers/,
# where they call the two functions the application supplies, as iclforge::ac3's
# own zone markers do under AC3FORGE_STAGE_TIMERS - which the root allows only in
# the minimum-footprint profile, so no other build ever takes the second. A
# directory choice, the way the scalar just above is, for the platform-tree rule
# (tools/checks/check_platform_macros.ps1). PUBLIC for the same reason as the
# scalar's: the decoder's own translation units carry markers too, and compile
# against this target without linking it.
if(AC3FORGE_STAGE_TIMERS)
    set(_ac4core_profiling_dir "stage_timers")
else()
    set(_ac4core_profiling_dir "none")
endif()
target_include_directories(iclforge_ac4core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/variants/profiling-${_ac4core_profiling_dir}>")

# -Wdouble-promotion as an error on the float build (planning/ac4.md, D14a):
# an implicit float-to-double promotion anywhere in these kernels is exactly
# the unmeasured cost a single-precision-FPU target (the ESP32-P4/S3) cannot
# afford, so it is a build failure rather than a silent one. GCC/Clang only -
# MSVC has no equivalent warning, and clang-cl parses MSVC-style arguments so
# a bare -Wdouble-promotion there would need the same /clang: escape the
# top-level CMakeLists.txt's -ffp-contract=off does; ac4core's touched files
# are checked under upstream clang-cl too (tu_compile.ps1), so the portable
# warning set is not skipped, only this one MSVC-shaped spelling of it.
if(_ac4core_scalar_dir STREQUAL "float" AND
   (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
    (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT
     CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")))
    target_compile_options(iclforge_ac4core PRIVATE -Wdouble-promotion -Werror=double-promotion)
endif()

# $<BUILD_INTERFACE:...> for the reason src/ac4/CMakeLists.txt gives.
#
# iclforge::arithmetic (src/arithmetic): Fixed32 and the project's own float
# transcendentals, decision 31's "no copies" - not yet called from here (no
# kernel in this tree names Fixed32 or a scalar_log2/scalar_exp today), wired
# in ahead of the fixed-point tier (D14d) and of hf_generator's dB gains
# eventually needing a cross-platform-identical log10/pow (this PR's report
# names that second one as a known gap, not yet closed).
target_link_libraries(iclforge_ac4core
    PRIVATE
        "$<BUILD_INTERFACE:ac3::warnings>"
        "$<BUILD_INTERFACE:ac3::coverage>"
        "$<BUILD_INTERFACE:iclforge::arithmetic>")

# The kernels of a frame's transform and filter work at -O2 under the profile's
# AC3FORGE_MINIMAL_HOT_O2, as src/ac3/minimal.cmake does for the AC-3 and
# E-AC-3 decoders' (docs/platforms/bare-metal/esp32-p4.md has what it bought on
# a board). A size-optimised build is what the profile is; these are the files a
# stage timer showed the optimiser paying for, and it costs flash and not SRAM.
# The decoder's own files (src/ac4dec) gained nothing measurable from it and
# stay at -Os. The option is that file's own, declared before this directory is
# added.
if(AC3FORGE_MINIMAL_DECODER AND AC3FORGE_MINIMAL_HOT_O2)
    set_source_files_properties(
        src/dsp/fft.cpp
        src/dsp/mdct.cpp
        src/dsp/qmf.cpp
        src/dsp/resampler.cpp
        src/dsp/synthesis.cpp
        src/aspx/hf_generator.cpp
        src/acpl/acpl.cpp
        PROPERTIES COMPILE_OPTIONS "-O2")
endif()

# In the minimum-footprint profile (AC3FORGE_MINIMAL_AC4, planning/ac4.md, D14b):
# the profile's own compile options - no exceptions, no RTTI, a section for each
# function and datum - PUBLIC, as iclforge::ac3_minimal links them, since a
# consumer has to share whether a call can throw; and code that is not
# position-independent, which a static archive linked into one firmware image
# has no use for and which costs a RISC-V part an indirection to each global.
# The archive is nothing an install or an export sees in that profile.
if(AC3FORGE_MINIMAL_DECODER)
    target_link_libraries(iclforge_ac4core PUBLIC ac3::minimal_profile)
    set(_ac4core_pic OFF)
else()
    set(_ac4core_pic ON)
endif()
set_target_properties(iclforge_ac4core PROPERTIES
    POSITION_INDEPENDENT_CODE ${_ac4core_pic}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "iclforge_ac4core_static"
    EXPORT_NAME "core")
