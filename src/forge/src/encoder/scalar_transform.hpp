#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "ac3/core/eac3_tools.hpp"
#include "ac3/core/mdct.hpp"
#include "ac3/internal/encode_scalar.hpp"

// The forward transforms the two encoders run, in whichever scalar the build
// carries the time domain AND the coefficient store in
// (ac3/internal/encode_scalar.hpp). One overload set rather than an
// `if constexpr` at each call site: the double forms take a `fast` flag and a
// batched entry point the float forms do not have, so the two shapes differ in
// more than a type, and a call site written against one shape does not compile
// against the other.
//
// The double overloads are what every ordinary build calls, and they call
// exactly what the encoders called before this header existed - the same
// functions with the same arguments, so the fifteen bitstream hashes in
// tests/golden/bitstream-hashes.json do not move.
//
// The float overloads ignore `fast`. The direct-form transform is double only
// (its four (k, n) tables are 1.9 MB), and under the minimum-footprint profile,
// the only build whose scalar is float, it is a stub that asserts and
// zero-fills (src/core/transform/stub/) - so `fast_mdct = false` was never a
// choice there, and honouring it here would mean widening the windowed block
// to call a function that cannot answer.

namespace ac3::encoder_detail {

// One long block: 512 windowed samples to 256 coefficients, in the scalar of
// both - the store is the same scalar as the window (encode_scalar_t).
inline void forward_long(std::span<const double, 512> windowed, std::span<double, 256> coeffs,
                         bool fast) {
    mdct512_forward(windowed, coeffs, fast);
}

inline void forward_long(std::span<const float, 512> windowed, std::span<float, 256> coeffs,
                         bool /*fast*/) {
    mdct512_forward(windowed, coeffs);
}

// Four long blocks at once (ROADMAP PF5 phase 4c). The double form is the
// batched kernel; the float one is four calls, which is what the float batch
// entry point in mdct.cpp is as well.
inline void forward_long_batch4(std::span<const double, 512> w0, std::span<const double, 512> w1,
                                std::span<const double, 512> w2, std::span<const double, 512> w3,
                                std::span<double, 256> c0, std::span<double, 256> c1,
                                std::span<double, 256> c2, std::span<double, 256> c3) {
    mdct512_forward_batch4(w0, w1, w2, w3, c0, c1, c2, c3);
}

inline void forward_long_batch4(std::span<const float, 512> w0, std::span<const float, 512> w1,
                                std::span<const float, 512> w2, std::span<const float, 512> w3,
                                std::span<float, 256> c0, std::span<float, 256> c1,
                                std::span<float, 256> c2, std::span<float, 256> c3) {
    forward_long(w0, c0, true);
    forward_long(w1, c1, true);
    forward_long(w2, c2, true);
    forward_long(w3, c3, true);
}

// A block-switched block (§7.9.2): the two half-block transforms, whose 128
// coefficients each the caller interleaves into the store.
inline void forward_short(std::span<const double, 512> windowed, std::span<double, 128> first,
                          std::span<double, 128> second, bool fast) {
    mdct256_forward_first(windowed.first<256>(), first, fast);
    mdct256_forward_second(windowed.last<256>(), second, fast);
}

inline void forward_short(std::span<const float, 512> windowed, std::span<float, 128> first,
                          std::span<float, 128> second, bool /*fast*/) {
    mdct256_forward_first(windowed.first<256>(), first);
    mdct256_forward_second(windowed.last<256>(), second);
}

// §3.5.5's enhanced-coupling analysis spectrum, which the E-AC-3 encoder
// runs over its own shared channel to fit per-band amplitudes and angles.
// The double form takes the fast flag, the float form - the one the float
// decoder runs - has no direct-form alternative to choose.
inline void ecpl_spectrum(std::span<const double, 256> prev, std::span<const double, 256> curr,
                          std::span<const double, 256> next, std::span<double, 256> real_out,
                          std::span<double, 256> imag_out, bool fast) {
    eac3::ecpl_channel_spectrum(prev, curr, next, real_out, imag_out, fast);
}

inline void ecpl_spectrum(std::span<const float, 256> prev, std::span<const float, 256> curr,
                          std::span<const float, 256> next, std::span<float, 256> real_out,
                          std::span<float, 256> imag_out, bool /*fast*/) {
    eac3::ecpl_channel_spectrum(prev, curr, next, real_out, imag_out);
}

}  // namespace ac3::encoder_detail
