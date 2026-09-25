#pragma once

#include <span>
#include <vector>

#include "pcm/snf_random.hpp"
#include "syntax/asf.hpp"
#include "syntax/context.hpp"

// The audio spectral frontend's reconstruction, ETSI TS 103 190-1 V1.4.1
// clause 5.1, from what D1's sf_data() reading kept (syntax/asf.hpp):
// quantisation reconstruction and scaling (5.1.3, Pseudocode 21), spectral
// noise fill (5.1.4, Pseudocodes 22 and 23) and ungrouping (5.1.5,
// Pseudocode 25).
//
// Lines stay in bitstream order - by window group, then scale factor band,
// then window, then line, the order quant_spec holds them in - until
// ungroup(), so that a scale factor band of a group is one contiguous run,
// as the stereo processing of clause 5.3 wants it.

namespace ac4::detail {

// scaled_spec for one track, in bitstream order: sign(q) |q|^(4/3) times
// 2^((sf - 100) / 4), then the noise fill when b_snf_data_exists. `noise` is
// the generator Pseudocode 23 draws from, advanced by every line it fills.
// Fails for a scale factor outside 0 to 255, which the note under Table A.1's
// formula says is not a valid one.
[[nodiscard]] ParseResult reconstruct_track(const SfInfo& info, const SfData& data, RandGenState& noise,
                                            std::vector<double>& scaled);

// The length in lines of each window of the frame, in order: one full block
// for a long frame, otherwise num_windows blocks, each of its group's
// transform length. Fails when they do not add up to the frame's length.
[[nodiscard]] ParseResult window_lengths(const SubstreamContext& ctx, const AsfPsyInfo& psy,
                                         std::vector<int>& lengths);

// Pseudocode 25: bitstream order to window order, each window's lines
// ascending, zero above max_sfb. `lengths` is window_lengths()'s result and
// `spec_reord` receives their sum.
void ungroup(const SubstreamContext& ctx, const AsfPsyInfo& psy, const SfData& data, std::span<const int> lengths,
             std::span<const double> scaled, std::vector<double>& spec_reord);

}  // namespace ac4::detail
