#pragma once

#include <cmath>
#include <type_traits>

#include "iclforge/arithmetic/mant_exp.hpp"
#include "iclforge/arithmetic/scalar_math.hpp"

// The transcendentals the QMF-domain tools of src/ac4dec call at the decoder's scalar, where the
// answer reaches the output and a C library's last bit would be heard on a platform that had
// another one (planning/ac4.md, D14a4).
//
// At Real = double they are libm's own calls, exactly as the tools made them (`std::pow`, and
// `iclforge::internal::scalar_exp2`, which is `std::exp2` there), so the double build's output is
// the bytes it was. At Real = float they are the project's own functions (src/arithmetic's
// `scalar_exp2` and `scalar_log2`): plain float multiplies and adds, the same float on the x86-64
// host, the Cortex-M3 leg and the ESP32s, where the C libraries' `powf` and `exp2f` differ in the
// last bit on some inputs. In the fixed-point tier they take a MantExp, a mantissa and a power of
// two (dsp/scalar_traits.hpp's Energy), and are that type's integer log2 and exp2. They live in
// this target, and not in the decoder's, because src/ac4dec includes from src/ac4core and
// src/ac4core from src/arithmetic (tools/checks/layering.json).

namespace iclforge::ac4::detail::dsp {

// 2^x. At float the argument is clamped to the range of a float's exponent, where the C library's
// exp2f would overflow to infinity or underflow to zero.
template <typename Value>
[[nodiscard]] inline Value exp2_of(Value x) noexcept {
    return iclforge::internal::scalar_exp2(x);
}

// x^e for a constant exponent e > 0, and 0 for an x that is not positive, as std::pow gives for a
// zero. At float and MantExp it is 2^(e log2 x), the accuracy of the two functions (a few ulps).
template <typename Value>
[[nodiscard]] inline Value pow_of(Value x, Value e) noexcept {
    if constexpr (std::is_same_v<Value, double>) {
        return std::pow(x, e);
    } else {
        return x > Value{} ? iclforge::internal::scalar_exp2(e * iclforge::internal::scalar_log2(x))
                           : Value{};
    }
}

// The square root of an energy: std::sqrt at double and float, MantExp's at the fixed tier.
template <typename Value>
[[nodiscard]] inline Value sqrt_of(Value x) noexcept {
    if constexpr (std::is_floating_point_v<Value>) {
        return std::sqrt(x);
    } else {
        return iclforge::internal::scalar_sqrt(x);
    }
}

}  // namespace iclforge::ac4::detail::dsp
