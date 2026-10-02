#pragma once

#include <array>
#include <span>
#include <vector>

#include "iclforge/ac4core/detail/real.hpp"
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

namespace iclforge::ac4::detail {

// 2^((sf - 100) / 4) for every scale factor, 0 to 255, at the decoder's scalar: what
// std::pow(2.0, 0.25 * (sf - 100)) gives, rounded to Real, made once instead of once
// for every scale factor band of every frame. The same call with the same argument
// gives the same value on every platform's C library as it always did, so the
// output moves by no bit; the call costs a hundred operations of the compiler's
// software `double` on a part whose FPU is single precision, and a stream of 5.1
// makes 250 of them a frame (planning/ac4.md, D14e).
using ScaleFactorGains = std::array<Real, 256>;
[[nodiscard]] ScaleFactorGains scale_factor_gains();

// scaled_spec for one track, in bitstream order: sign(q) |q|^(4/3) times
// 2^((sf - 100) / 4), then the noise fill when b_snf_data_exists. `noise` is
// the generator Pseudocode 23 draws from, advanced by every line it fills.
// Fails for a scale factor outside 0 to 255, which the note under Table A.1's
// formula says is not a valid one. `sf_gain` is scale_factor_gains()'s. The
// lines are the values times 2^-exponent: at double and float the exponent is
// 0, and at Fixed32 the track's own, which puts its largest line in [1/2, 1)
// (planning/ac4.md, D14d); that tier forms its gains itself and its table is
// empty.
[[nodiscard]] ParseResult reconstruct_track(const SfInfo& info, const SfData& data,
                                            const ScaleFactorGains& sf_gain, RandGenState& noise,
                                            std::vector<Real>& scaled, int& exponent);

// The length in lines of each window of the frame, in order: one full block
// for a long frame, otherwise num_windows blocks, each of its group's
// transform length. Fails when they do not add up to the frame's length.
[[nodiscard]] ParseResult window_lengths(const SubstreamContext& ctx, const AsfPsyInfo& psy,
                                         std::vector<int>& lengths);

// Pseudocode 25: bitstream order to window order, each window's lines
// ascending, zero above max_sfb. `lengths` is window_lengths()'s result and
// `spec_reord` receives their sum.
void ungroup(const SubstreamContext& ctx, const AsfPsyInfo& psy, const SfData& data, std::span<const int> lengths,
             std::span<const Real> scaled, std::vector<Real>& spec_reord);

// ungroup() for a frame of one long block, in one group of one window: the lines of `scaled` are in
// window order already, so the spectrum is `scaled` with zeros after its last band, and the buffer
// changes hands (`scaled` takes the spectrum's old one) in place of two passes over 8 KB a channel,
// one to zero `spec_reord` and one to copy. False, with nothing changed, for any other frame, which
// ungroup() takes.
[[nodiscard]] bool ungroup_in_place(const SubstreamContext& ctx, const AsfPsyInfo& psy,
                                    const SfData& data, std::span<const int> lengths,
                                    std::vector<Real>& scaled, std::vector<Real>& spec_reord);

}  // namespace iclforge::ac4::detail
