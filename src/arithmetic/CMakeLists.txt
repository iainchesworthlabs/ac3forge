# ---------------------------------------------------------------------------
# ac3::arithmetic - the scalar building blocks the decoder's arithmetic tiers
# share: Fixed32 (ac3/internal/fixed32.hpp, the Q7.24 fixed-point tier's
# type) and the project's own cross-platform-identical float transcendentals
# (ac3/internal/scalar_math.hpp). Header-only, so there is nothing to
# compile here - just a target that carries the include path.
#
# Why this is its own target rather than living inside ac3::forge, where both
# files used to be (src/forge/src/core/{fixed32,scalar_math}.hpp): src/ac4core
# needs Fixed32 too, for the same reason ac3::forge does (a fixed-point tier
# with no floating-point unit, planning/ac4.md's D14), and a decoder that
# linked ac3::forge to get it would bring the whole AC-3/E-AC-3 codec into
# every AC-4 build (src/ac4dec/CMakeLists.txt's own header explains why AC-4
# links none of it). Copying the two files into src/ac4core instead was the
# other option and was rejected: planning/ac4.md's decision 31 is explicit
# that a copied Fixed32 would drift from the original. So both consumers
# depend on this one small target instead - "no copies" made structural
# rather than a rule to remember.
#
# Not installed and not exported: like ac3::warnings and the other interface
# targets forge_objects links $<BUILD_INTERFACE:...>-wrapped, this is
# in-tree build plumbing, not part of what a downstream consumer of the
# installed package needs to reach.
# ---------------------------------------------------------------------------
add_library(ac3_arithmetic INTERFACE)
add_library(ac3::arithmetic ALIAS ac3_arithmetic)

target_include_directories(ac3_arithmetic INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

target_compile_features(ac3_arithmetic INTERFACE cxx_std_23)
