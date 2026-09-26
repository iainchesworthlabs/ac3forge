#pragma once

#include <array>
#include <span>
#include <vector>

#include "frame/metadata.hpp"

// Dialogue enhancement's parameters (ETSI TS 103 190-1 V1.4.1 clause 5.7.8).
// The channel-independent method (5.7.8.7) raises channel m by g p m in each
// band: for p to raise the dialogue d in m by g d, p is the dialogue's share
// of the channel, <d, m> / <m, m>, per band of Table 173, on Table 209's
// scale; with de_ms_proc_flag, of L and R's Mid. The cross-channel method
// (5.7.8.8) is de_cross_parameters()'s.

namespace ac4::detail {

// Table 209's channel-independent value nearest `p`, as its index: 0 to 1.5
// in steps of 0.1, 1.75, 2, then 2.5 to 9 in steps of 0.5.
[[nodiscard]] int de_parameter_index(double p) noexcept;

// Table 209's value of an index.
[[nodiscard]] double de_parameter_value(int index) noexcept;

// One channel's parameters for a frame, from the long-block spectra of the
// channel and of the dialogue in it (Analysis::transform's lines,
// `frame_length` of them), band by band: Table 173's bands are QMF subbands,
// frame_length / 64 lines each. A band with no energy takes 0. With
// kMid, called on L and R's Mids.
[[nodiscard]] std::array<int, kDeBands> de_parameters(std::span<const double> channel,
                                                      std::span<const double> dialogue,
                                                      int frame_length) noexcept;

// Table 172's mixing coefficient nearest `c`, as its index.
[[nodiscard]] int de_mix_index(double c) noexcept;

// The cross-channel method's parameters for a frame (clause 5.7.8.8, which
// raises the channels m by g r p^T m): r, the dialogue's panning, from its
// energy in each channel, quantised to Table 172 with the last coefficient
// the energy the others leave; and per band, p the least-squares mix of the
// channels onto the dialogue's projection on r, on Table 210's scale. The
// spectra as de_parameters() takes them, two or three channels.
[[nodiscard]] DeFrameParameters de_cross_parameters(std::span<const std::vector<double>> channels,
                                                    std::span<const std::vector<double>> dialogue,
                                                    int frame_length);

}  // namespace ac4::detail
