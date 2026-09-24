#pragma once

#include <array>
#include <span>

#include "syntax/asf.hpp"
#include "syntax/context.hpp"

// Stereo processing, ETSI TS 103 190-1 V1.4.1 clause 5.3: Pseudocode 59's
// parameters a, b, c and d for each window group and scale factor band of a
// chparam_info(), and the 2 x 2 matrix of clause 5.3.3.2 that makes two
// output tracks of two input tracks,
//
//   O0 = a I0 + b I1,   O1 = c I0 + d I1.
//
// Pseudocode 59 is printed with a block that belongs to no branch: after the
// sap_mode 1 and 2 branches, an `if (sap_used) ... else ...` pair that would
// overwrite them, followed by the sap_mode 3 `else`. The block is read as a
// stray copy of the one inside the sap_mode 3 branch (src/ac4dec/ERRATA.md,
// "Pseudocode 59's stray block").

namespace ac4::detail {

struct StereoParameters {
    // a, b, c, d per group and band, as Pseudocode 59 sets them.
    std::array<std::array<std::array<double, 4>, kMaxSfb>, kMaxWindows> abcd{};
};

// Pseudocode 59 for one chparam_info() under the sf_info() it was read with.
[[nodiscard]] StereoParameters stereo_parameters(const SubstreamContext& ctx, const SfInfo& info,
                                                 const ChparamInfo& chparam);

// The matrix, band by band, on two tracks' lines in bitstream order (see
// pcm/asf_reconstruct.hpp); `layout` is either track's SfData, whose band
// offsets both share under one sf_info(). Bands at or above a group's max_sfb
// hold no lines of either track and are left as they are.
void apply_stereo(const SfInfo& info, const SfData& layout, const StereoParameters& parameters,
                  std::span<double> track0, std::span<double> track1);

}  // namespace ac4::detail
