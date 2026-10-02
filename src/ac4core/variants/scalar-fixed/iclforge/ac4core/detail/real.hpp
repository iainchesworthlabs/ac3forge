#pragma once

#include "iclforge/arithmetic/fixed32.hpp"

// The FIXED-POINT variant of src/ac4core's explicit-instantiation scalar. See the
// double variant under variants/scalar-double/ for what this seam is and why it is
// not iclforge::ac3::internal::decode_scalar_t's mechanism.
//
// Fixed32: Q7.24 in a 32-bit integer, for the ESP32-C6 and -C3, which have no
// floating-point unit (planning/ac4.md, D14d). A block exponent travels with each
// transform block and each QMF slot where a value at one absolute scale would lose
// a small value's bits; the kernels that need one take it through the overload sets
// of dsp/real_functions.hpp, and the floating tiers' exponents are zero.

namespace iclforge::ac4::detail {

using Real = iclforge::internal::Fixed32;

}  // namespace iclforge::ac4::detail

// Real is Fixed32 here, so the encoder's double instantiations are extra, as in the
// float variant, and this passes them through.
#define AC4CORE_ALSO_AT_DOUBLE(...) __VA_ARGS__
