#pragma once

// The type the ENCODERS run their analysis front end in, in the FLOAT variant.
// See the double variant under src/internal/scalar/encode/float64/ for what
// this seam is and why it is its own axis.
//
// float. Measured on an ESP32-S3-DevKitC-1 at 240 MHz on 2026-09-10, with the
// front end in double: the forward transform was 71.9 ms and transient
// detection 57.4 ms of a 201.4 ms AC-3 5.1 frame - 64% of it - and the same
// two stages 128 ms of a 352 ms E-AC-3 5.1 frame, every operation of both a
// call into the mask ROM's software floating point on that single-precision
// FPU. The decoder's own float conversion took its transform from that kind
// of cost to 3.4 ms for six channels (docs/platforms/esp32.md). The
// coefficients the transform produces are widened to double on their way into
// the rest of the encoder, which is unchanged by this variant.

namespace ac3::internal {

using encode_scalar_t = float;

}  // namespace ac3::internal
