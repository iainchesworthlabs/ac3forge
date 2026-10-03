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
# namespace iclforge::ac4::detail.
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
# tools/checks/check_install_consumer.sh and iclforge_install_pkgconfig()
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
        src/dsp/transform_tables.cpp
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
# D14a: "AC-4 joins ICLFORGE_DECODE_SCALAR"). The same cache variable
# src/ac3/CMakeLists.txt declares and resolves its own decode_scalar_t from -
# declared again here with the identical default/STRINGS (CMake's set(...
# CACHE ...) only takes effect the first time it runs in a configure, so
# whichever of the two subdirectories add_subdirectory() reaches first is the
# one that actually sets it) so that src/ac4core configures correctly even in
# a tree that builds AC-4 without iclforge::ac3.
#
# "fixed" is iclforge::internal::Fixed32, the ESP32-C6's and -C3's (planning/ac4.md,
# D14d).
set(ICLFORGE_DECODE_SCALAR "double" CACHE STRING
    "Scalar the decoder carries coefficients in: double (default), float, or fixed (Q7.24 in an int32, for parts with no FPU)")
set_property(CACHE ICLFORGE_DECODE_SCALAR PROPERTY STRINGS double float fixed)
if(ICLFORGE_DECODE_SCALAR STREQUAL "float")
    set(_ac4core_scalar_dir "float")
elseif(ICLFORGE_DECODE_SCALAR STREQUAL "fixed")
    set(_ac4core_scalar_dir "fixed")
else()
    set(_ac4core_scalar_dir "double")
endif()

# AC4CORE_ALSO_AT_DOUBLE: the encoder (src/ac4enc) calls straight into a handful
# of these same kernels (Mdct, QmfAnalysis, QmfSynthesis, Resampler,
# ajoc::Reconstruction, aspx::generate_high_band) always at double - decision
# 34 (planning/ac4.md), the encoder never runs on an ESP32 and has no float
# or fixed-point tier of its own, so it does not follow ICLFORGE_DECODE_SCALAR.
# iclforge::ac3 avoids this by compiling its encoder's and decoder's DSP
# separately (ICLFORGE_ENCODE_SCALAR beside ICLFORGE_DECODE_SCALAR,
# src/ac3/CMakeLists.txt); iclforge_ac4core is one static library both ac4dec and
# ac4enc link (this file's own header comment), so when Real is not already
# double, each of those kernels' .cpp files also explicitly instantiates
# <double>. That is done through a macro the per-scalar iclforge/ac4core/detail/real.hpp
# defines (src/internal/scalar/<double|float>/, chosen just above): it passes
# its argument through where Real is float and drops it where Real is already
# double (a second explicit instantiation of the same specialization would be
# ill-formed), so no source file asks which build it is with a preprocessor
# conditional (tools/checks/check_platform_macros.ps1's rule). The default,
# double, build is unchanged: one instantiation, as it always was.

# PUBLIC, not PRIVATE: ac4core's own PUBLIC headers (aspx/hf_generator.hpp,
# acpl/acpl.hpp, ajcc/ajcc.hpp, ajoc/ajoc.hpp, dsp/qmf.hpp, dsp/mdct.hpp,
# dsp/fft.hpp, dsp/resampler.hpp, dsp/synthesis.hpp) #include
# "iclforge/ac4core/detail/real.hpp" themselves, and src/ac4dec and src/ac4enc compile
# against those headers through iclforge::ac4core as a COMPILE_ONLY dependency (their
# own CMakeLists.txt) - a consumer that only ever compiles against a target,
# never links it, still needs that target's PUBLIC usage requirements, this
# include directory among them, or its own translation units cannot resolve
# the header ac4core's PUBLIC header just asked for.
target_include_directories(iclforge_ac4core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/variants/scalar-${_ac4core_scalar_dir}>")

# Which "iclforge/ac4core/detail/profiling.hpp" the kernels' AC4_ZONE_SCOPED_N() markers see
# (planning/ac4.md, D14b): none/, where they expand to nothing, or stage_timers/,
# where they call the two functions the application supplies, as iclforge::ac3's
# own zone markers do under ICLFORGE_STAGE_TIMERS - which the root allows only in
# the minimum-footprint profile, so no other build ever takes the second. A
# directory choice, the way the scalar just above is, for the platform-tree rule
# (tools/checks/check_platform_macros.ps1). PUBLIC for the same reason as the
# scalar's: the decoder's own translation units carry markers too, and compile
# against this target without linking it.
if(ICLFORGE_STAGE_TIMERS)
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
# iclforge::arithmetic (src/arithmetic): Fixed32, MantExp and the project's own
# float transcendentals, decision 31's "no copies". PUBLIC: the core's public
# headers include them (dsp/scalar_traits.hpp, dsp/resampler.hpp, and the
# fixed variant's detail/real.hpp), so the decoder and the encoder, which
# compile against those headers, need the include path too.
target_link_libraries(iclforge_ac4core
    PRIVATE
        "$<BUILD_INTERFACE:iclforge::warnings>"
        "$<BUILD_INTERFACE:iclforge::coverage>"
    PUBLIC
        "$<BUILD_INTERFACE:iclforge::arithmetic>")

# The kernels of a frame's transform and filter work at -O3 under the profile's
# ICLFORGE_MINIMAL_HOT_O2 (named for the AC-3 and E-AC-3 decoders' -O2 in
# src/ac3/minimal.cmake, whose switch this is). A size-optimised build is what
# the profile is; these are the files a stage timer showed the optimiser paying
# for, and it costs flash and not SRAM. On the ESP32-P4's in-order core -O3 beat
# -O2 on these by 2.7 ms a 5.1 frame (it unrolls the filter and transform loops
# and interleaves their independent multiplies, which hides the latency of the
# core's floating-point unit that a rolled loop exposes) for 9 KB of flash; the
# arithmetic is the same operation for operation, which the board's PCM hashes
# checked (docs/platforms/bare-metal/esp32-p4.md, planning/ac4.md D14e). The
# decoder's own files (src/ac4dec) are at -O2 in its CMakeLists.txt: -O3 gained
# nothing there and cost 73 KB.
if(ICLFORGE_MINIMAL_DECODER AND ICLFORGE_MINIMAL_HOT_O2)
    set_source_files_properties(
        src/dsp/fft.cpp
        src/dsp/mdct.cpp
        src/dsp/qmf.cpp
        src/dsp/resampler.cpp
        src/dsp/synthesis.cpp
        src/aspx/hf_generator.cpp
        src/acpl/acpl.cpp
        PROPERTIES COMPILE_OPTIONS "-O3")
endif()

# The float tables of the sample rate converter are built by the compiler, in
# src/dsp/resampler.cpp and in no other translation unit and only in a build whose
# scalar is float, and the Q1.30 tables alike only in one whose scalar is fixed
# (planning/ac4.md, D14d) (dsp/resampler_design.hpp: the three ratios the decoder uses, 1001/960
# the long one, 47,094 coefficients each designed with a Kaiser window and a sinc,
# planning/ac4.md, D14a5). A compiler's limit on what its constant evaluator may do
# in one constant expression is set for small computations and this is not one: the
# three tables take 6 to 10 million steps in MSVC, 10.6 million in Clang and 76 to 78
# million operations in GCC 14 and 15 (32 million in GCC 16, which counts them
# differently), and the defaults are 100,000 steps, 1,048,576 steps and 33,554,432
# operations. Each compiler has its own flag for it, and the limits below are three to
# eleven times what the tables need. The evaluation makes the file take 5 to 14 seconds
# longer to compile, by compiler: clang-cl 5.9 s where it took 0.8, GCC 16 6.0 where 0.8,
# GCC 14 6.1 where 1.1, ESP-IDF's RISC-V GCC 15 8.4 where 1.5, Clang 22 8.8 where 1.0 and
# MSVC 14.7 where 0.7 (planning/ac4.md, D14a5); a double build pays nothing. The
# ESP-IDF component reaches this file through the root CMakeLists.txt, so the same
# limits apply there. APPEND, since the block above sets this file's compile options
# as well when it applies.
if(_ac4core_scalar_dir STREQUAL "float" OR _ac4core_scalar_dir STREQUAL "fixed")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(_ac4core_constexpr_limit "/constexpr:steps67108864")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
           CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # clang-cl accepts /constexpr:steps and ignores it, with a warning; its own
        # option goes through the /clang: escape, as -ffp-contract=off does.
        set(_ac4core_constexpr_limit "/clang:-fconstexpr-steps=67108864")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_ac4core_constexpr_limit "-fconstexpr-steps=67108864")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(_ac4core_constexpr_limit "-fconstexpr-ops-limit=268435456")
    endif()
    # src/dsp/transform_tables.cpp narrows the inverse transform's tables (dsp/transform_tables.hpp)
    # to the tier's scalar the same way: about 13,000 doubles, well inside the same limits.
    if(_ac4core_constexpr_limit)
        set_property(SOURCE src/dsp/resampler.cpp src/dsp/transform_tables.cpp APPEND PROPERTY
            COMPILE_OPTIONS "${_ac4core_constexpr_limit}")
    endif()
endif()

# In the minimum-footprint profile (ICLFORGE_MINIMAL_AC4, planning/ac4.md, D14b):
# the profile's own compile options - no exceptions, no RTTI, a section for each
# function and datum - PUBLIC, as iclforge::ac3_minimal links them, since a
# consumer has to share whether a call can throw; and code that is not
# position-independent, which a static archive linked into one firmware image
# has no use for and which costs a RISC-V part an indirection to each global.
# The archive is nothing an install or an export sees in that profile.
if(ICLFORGE_MINIMAL_DECODER)
    target_link_libraries(iclforge_ac4core PUBLIC iclforge::minimal_profile)
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
