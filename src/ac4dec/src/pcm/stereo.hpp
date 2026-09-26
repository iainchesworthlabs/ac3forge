#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

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
    // a, b, c, d per group and band, as Pseudocode 59 sets them; 1, 0, 0, 1
    // in a band the chparam_info() does not cover, which it leaves as it is.
    std::array<std::array<std::array<double, 4>, kMaxSfb>, kMaxWindows> abcd{};
};

// What a chparam_info() parameterises: a 2 x 2 step (Pseudocode 59), or one of
// the prediction gains a'_j of ETSI TS 103 190-2 V1.3.1 clause 5.2.3.2 step 5,
// which Table 20 applies as O1 = a'_j I0 + I1, the step (1, 0, a'_j, 1). a'_j is
// the band's sap_gain where the chparam_info() is full SAP (sap_mode 3) and
// sends the band's coefficient, and 0 in every other band and mode
// (src/ac4dec/ERRATA.md, "Table 20's prediction gains").
enum class StereoUse : std::uint8_t { kPair, kPrediction };

// Pseudocode 59 for one chparam_info() under the sf_info() it was read with,
// as `use` takes it.
[[nodiscard]] StereoParameters stereo_parameters(const SubstreamContext& ctx, const SfInfo& info,
                                                 const ChparamInfo& chparam,
                                                 StereoUse use = StereoUse::kPair);

// The matrix, band by band, on two tracks' lines in bitstream order (see
// pcm/asf_reconstruct.hpp); `layout` is either track's SfData, whose band
// offsets both share under one sf_info() without b_dual_maxsfb, or the one
// align_tracks() gives them. Bands at or above a group's max_sfb hold no lines
// of either track and are left as they are.
void apply_stereo(const SfInfo& info, const SfData& layout, const StereoParameters& parameters,
                  std::span<double> track0, std::span<double> track1);

// b_dual_maxsfb, which the channel pair's ASPX_ACPL_1 sends (Table 22): the
// second track has max_sfb_side bands in a group where the first has max_sfb,
// and each track holds only its own bands, group after group, so the two do
// not share band offsets. Both are laid out afresh with the larger count in
// each group, zero in a band a track does not send; `common` takes that count
// and those offsets (max_sfb and sect_sfb_offset alone), for apply_stereo()
// and ungroup(). chparam_info() covers the first track's bands (get_max_sfb(),
// clause 4.3.6.2), and leaves the rest as they are.
void align_tracks(const SubstreamContext& ctx, const AsfPsyInfo& psy, const SfData& first,
                  const SfData& second, std::vector<double>& track0, std::vector<double>& track1,
                  SfData& common);

}  // namespace ac4::detail
