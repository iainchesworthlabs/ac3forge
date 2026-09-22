#pragma once

// The type the ENCODERS run their analysis front end in - the transient
// detector, the block gather, the analysis window and the forward transform -
// in the DOUBLE variant, which every ordinary build resolves to
// (AC3FORGE_ENCODE_SCALAR=double, the default). The coefficients the
// transform hands the rest of the encoder are double in either variant; what
// this seam selects is the arithmetic in front of them.
//
// A second axis beside src/internal/scalar/{float32,float64}/'s decode_scalar_t
// rather than a second alias in those headers, for the reason that seam was
// itself split from the profile: which scalar a build's decoder carries and
// which its encoders' front end runs in are independent choices, and a full
// build - CLI, tests, oracles - configured with one of them float and the other
// double is what makes either claim checkable against the other. The
// minimum-footprint profile picks float32 for both, unconditionally.
//
// double is the default because the fifteen bitstream hashes in
// tests/golden/bitstream-hashes.json, the quality trend and every encoder
// oracle are stated in terms of this arithmetic. The float variant produces a
// different, equally valid bitstream: the same recipe with float rounding,
// which moves a transient decision or a quantised coefficient now and then.

namespace ac3::internal {

using encode_scalar_t = double;

}  // namespace ac3::internal
