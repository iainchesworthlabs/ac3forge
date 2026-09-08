#pragma once

#include <array>
#include <type_traits>

#include "ac3/core/mdct.hpp"
#include "ac3/internal/decode_scalar.hpp"
#include "ac3/internal/profile.hpp"

// The §7.9.4 inverse pair, selected by the scalar type the decoder stores its
// coefficients in (ac3::internal::decode_scalar_t, roadmap PF7's float32 gap).
// Shared by both decoders - decoder.cpp's AC-3 and eac3_decoder.cpp's Annex E -
// because both make exactly this choice at exactly this point.
//
// It is a TEMPLATE for one specific reason, and not for generality: `if
// constexpr` only discards the untaken branch inside a template. In an ordinary
// function both arms are still fully type-checked, so the double-only call
// would fail to compile in a float32 build even though it could never run -
// which is the trap src/internal/cpu/minimal/cpu_features.cpp's header records
// ("the discarded branch of a non-template is still semantically checked and
// its callees still ODR-used").
//
// The two arms differ only in the `fast` argument, which the float32 inverses
// do not take: the direct form is the spec's own evaluation and stays double,
// and a profile carrying float32 coefficients has already refused
// fast_imdct=false with kUnsupported long before reaching here.

namespace ac3::internal {

template <typename Scalar>
void inverse_transform_into(const std::array<Scalar, 256>& coeffs, std::array<Scalar, 512>& x,
                            bool short_block, bool fast) {
    if constexpr (std::is_same_v<Scalar, float>) {
        (void)fast;
        if (short_block) {
            imdct256_pair_windowed(coeffs, x);
        } else {
            imdct512_windowed(coeffs, x);
        }
    } else {
        if (short_block) {
            imdct256_pair_windowed(coeffs, x, fast);
        } else {
            imdct512_windowed(coeffs, x, fast);
        }
    }
}

}  // namespace ac3::internal
