#pragma once

// The FLOAT variant of src/ac4core's explicit-instantiation scalar. See the
// double variant under src/internal/scalar/double/ for what this seam is and
// why it is not iclforge::ac3::internal::decode_scalar_t's mechanism.
//
// float: the ESP32-P4 and -S3, which have a single-precision FPU
// (planning/ac4.md, "The ESP32"). ICLFORGE_DECODE_SCALAR=float's build compiles
// src/ac4core with -Wdouble-promotion as an error (src/ac4core/CMakeLists.txt),
// so a float value implicitly widening to double anywhere in these kernels is
// a build failure rather than a silent, unmeasured cost - the same promise
// iclforge::ac3's own float decode path makes.

namespace iclforge::ac4::detail {

using Real = float;

}  // namespace iclforge::ac4::detail

// The float variant of AC4CORE_ALSO_AT_DOUBLE (see the double variant under
// src/internal/scalar/double/): Real is float here, so the encoder's double
// instantiations are extra, and this passes them through.
#define AC4CORE_ALSO_AT_DOUBLE(...) __VA_ARGS__
